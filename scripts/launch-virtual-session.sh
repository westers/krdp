#!/usr/bin/env bash
# Trusted guardian child. Namespace exit tears down its descendants; broker
# loss does not. This is not a sandbox against other applications of the UID.
set -euo pipefail
umask 077
# OPT-075: system bus for the desktop. xdg-dbus-proxy runs HERE, outside the sandbox, as the session user; the sandbox
# gets only the proxy socket (or, in full mode, the real socket) at /run/dbus/system_bus_socket. Defaults below are
# overwritten by the options; a test sources this file (the early return below) and overrides the paths.
system_bus=none polkit_agent=0 cups_socket=0
system_bus_rules=()
system_bus_proxy=/usr/bin/xdg-dbus-proxy
system_bus_real_socket=/run/dbus/system_bus_socket
system_bus_ready_timeout=5
system_bus_bwrap=()
# The same grammar as KRdp::VirtualSystemBus::validProxyArgument().
system_bus_rule_re='^(--(see|talk)=[A-Za-z0-9_.*-]+|--(call|broadcast)=[A-Za-z0-9_.*-]+=[A-Za-z0-9_.*@/-]+)$'
# Sets system_bus_bwrap (extra bwrap options) and system_bus (the effective mode). Never fails the launch for a missing
# proxy or socket: the desktop is created with no system bus and the reason goes to the log. An invalid rule is an error.
farside_system_bus_setup() {
    local runtime=$1 rule proxy_socket ready
    system_bus_bwrap=()
    [[ $system_bus == none || $system_bus == filtered || $system_bus == full ]] || return 1
    if [[ $system_bus != none && ! -S $system_bus_real_socket ]]; then
        echo "Virtual system bus: $system_bus requested, but $system_bus_real_socket does not exist; no system bus" >&2
        system_bus=none
    fi
    if [[ $system_bus == filtered ]]; then
        (( ${#system_bus_rules[@]} <= 200 )) || return 1
        for rule in "${system_bus_rules[@]}"; do [[ $rule =~ $system_bus_rule_re ]] || return 1; done
        if [[ ! -x $system_bus_proxy ]]; then
            echo 'Virtual system bus: filtered requested, but xdg-dbus-proxy is not installed; no system bus' >&2
            system_bus=none
        else
            proxy_socket=$runtime/system_bus_socket
            rm -f -- "${runtime:?}/system_bus_socket"
            # --fd: the proxy writes one byte when ready and stops when the read end (held by this shell and
            # inherited by bwrap and the whole desktop) is closed, so it ends with the desktop and never outlives it.
            exec {system_bus_proxy_fd}< <("$system_bus_proxy" --fd=1 "unix:path=$system_bus_real_socket" "$proxy_socket" --filter \
                "${system_bus_rules[@]}" 2>>"$runtime/system-bus-proxy.log")
            if IFS= read -r -n1 -t "$system_bus_ready_timeout" -u "$system_bus_proxy_fd" ready && [[ -S $proxy_socket ]]; then
                system_bus_bwrap=(--ro-bind "$proxy_socket" /run/dbus/system_bus_socket)
            else
                exec {system_bus_proxy_fd}<&-
                echo 'Virtual system bus: xdg-dbus-proxy did not become ready; no system bus' >&2
                system_bus=none
            fi
        fi
    elif [[ $system_bus == full ]]; then
        system_bus_bwrap=(--ro-bind "$system_bus_real_socket" /run/dbus/system_bus_socket)
    fi
    if [[ $system_bus == none ]]; then
        system_bus_bwrap=()
        return 0
    fi
    system_bus_bwrap+=(--setenv DBUS_SYSTEM_BUS_ADDRESS unix:path=/run/dbus/system_bus_socket --setenv FARSIDE_SYSTEM_BUS "$system_bus")
    [[ $polkit_agent == 1 ]] && system_bus_bwrap+=(--setenv FARSIDE_POLKIT_AGENT 1)
    if [[ $cups_socket == 1 && -S /run/cups/cups.sock ]]; then system_bus_bwrap+=(--ro-bind /run/cups/cups.sock /run/cups/cups.sock); fi
    return 0
}
(return 0 2>/dev/null) && return 0
runtime= profile= session= session_uid= worker= support= width= height= initial_layout= vaapi_driver=auto
allowed_pci=()
while (( $# )); do
    [[ $# -ge 2 ]] || exit 1
    case "$1" in
        --runtime) runtime=$2;; --profile) profile=$2;; --session) session=$2;;
        --uid) session_uid=$2;; --worker) worker=$2;; --support) support=$2;;
        --width) width=$2;; --height) height=$2;; --initial-layout) initial_layout=$2;; --allow-render-pci) allowed_pci+=("$2");;
        --vaapi-driver) vaapi_driver=$2;;
        --system-bus) system_bus=$2;; --system-bus-rule) system_bus_rules+=("$2");;
        --polkit-agent) polkit_agent=$2;; --cups-socket) cups_socket=$2;;
        *) echo 'Unknown namespace-launch option' >&2; exit 1;;
    esac
    shift 2
done
[[ $(id -u) != 0 && $session_uid == "$(id -u)" && $(id -ru) == "$session_uid" ]]
[[ $session =~ ^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$ ]]
[[ $polkit_agent =~ ^[01]$ && $cups_socket =~ ^[01]$ && $system_bus =~ ^(none|filtered|full)$ ]]
[[ $width =~ ^[0-9]{3,4}$ && $height =~ ^[0-9]{3,4}$ ]]
(( 10#$width >= 320 && 10#$width <= 4096 && 10#$height >= 200 && 10#$height <= 4096 && 10#$width % 2 == 0 && 10#$height % 2 == 0 ))
layout_args=()
if [[ -n $initial_layout ]]; then
    [[ ${#initial_layout} -le 4096 ]]
    jq -e --argjson w "$width" --argjson h "$height" '
        type == "array" and length >= 1 and length <= 16
        and .[0].primary == true and (map(select(.primary == true)) | length) == 1
        and .[0].width == $w and .[0].height == $h
        and all(.[]; type == "object" and (keys == ["height", "primary", "scale", "width", "x", "y"])
            and (.x | type) == "number" and .x == (.x | floor) and .x >= 0 and .x <= 32768
            and (.y | type) == "number" and .y == (.y | floor) and .y >= 0 and .y <= 32768
            and (.width | type) == "number" and .width == (.width | floor) and .width >= 320 and .width <= 4096 and .width % 2 == 0
            and (.height | type) == "number" and .height == (.height | floor) and .height >= 200 and .height <= 4096 and .height % 2 == 0
            and (.scale | type) == "number" and .scale >= 1 and .scale <= 4
            and (.primary | type) == "boolean")
    ' <<<"$initial_layout" >/dev/null
    layout_args=("$initial_layout")
fi
[[ $runtime == /run/user/"$session_uid"/farside-virtual/* && $runtime == "${FARSIDE_VIRTUAL_RUNTIME:-}" ]]
[[ $profile == "$HOME/.farside-virtual/sessions/$session" && $profile == "${FARSIDE_VIRTUAL_PROFILE:-}" ]]
for directory in "$runtime" "$profile" "$profile/config" "$profile/data" "$profile/cache" "$profile/state"; do
    [[ -d $directory && -O $directory && $(realpath -e "$directory") == "$directory" && $(stat -c %a "$directory") == 700 ]]
done
[[ -f $runtime/worker-token && ! -L $runtime/worker-token && -O $runtime/worker-token ]]
[[ $(stat -c %a "$runtime/worker-token") == 600 && $(stat -c %s "$runtime/worker-token") == 32 ]]
# Exec in a desktop file must be a single literal path, not desktop-entry syntax.
[[ $worker =~ ^/[A-Za-z0-9_./-]+$ && -x $worker && $(realpath -e "$worker") == "$worker" ]]
[[ $support == /* && $(realpath -e "$support") == "$support" ]]
for required in virtual-session-bus.conf virtual-session-pipewire.conf locked-config/kscreenlockerrc locked-config/kdeglobals; do [[ -f $support/$required ]]; done
[[ -d $support/defaults && ${#allowed_pci[@]} -gt 0 ]]
inner=$(dirname "$(realpath -e "$0")")/virtual-session-desktop.sh
[[ -f $inner ]]

render_bindings=() render_environment=() selected= render_node=
for pci in "${allowed_pci[@]}"; do
    [[ $pci =~ ^[0-9a-f]{4}:[0-9a-f]{2}:[01][0-9a-f]\.[0-7]$ ]] || exit 1
done
for pci in "${allowed_pci[@]}"; do
    render=$(realpath -e "/dev/dri/by-path/pci-$pci-render" 2>/dev/null) || continue
    [[ $render =~ ^/dev/dri/renderD[0-9]+$ && -c $render && -r $render && -w $render ]] || continue
    device=$(realpath -e "/sys/class/drm/${render##*/}/device")
    [[ ${device##*/} == "$pci" ]] || continue
    driver_path=$(realpath -e "$device/driver" 2>/dev/null) || continue
    driver=${driver_path##*/}
    candidate=("$render")
    if [[ $driver == nvidia ]]; then
        minor=$(awk '/^Device Minor:/ {print $3}' "/proc/driver/nvidia/gpus/$pci/information" 2>/dev/null) || continue
        [[ $minor =~ ^[0-9]+$ ]] || continue
        candidate+=("/dev/nvidia$minor" /dev/nvidiactl /dev/nvidia-uvm)
        candidate_environment=(__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json __GLX_VENDOR_LIBRARY_NAME=nvidia)
    elif [[ $driver == amdgpu || $driver == i915 || $driver == xe ]]; then
        candidate_environment=(__EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json __GLX_VENDOR_LIBRARY_NAME=mesa)
    else
        continue
    fi
    permitted=true
    for node in "${candidate[@]}"; do
        [[ -c $node && -r $node && -w $node ]] || permitted=false
    done
    [[ $permitted == true ]] || continue
    for node in "${candidate[@]}"; do render_bindings+=(--dev-bind "$node" "$node"); done
    render_environment=("${candidate_environment[@]}")
    render_node=$render
    selected=$pci
    break
done
[[ -n $selected ]] || { echo 'No permitted render GPU is accessible; no fallback to physical DRM.' >&2; exit 1; }
# AUD-FIX8 B3: the worker's encoder probe tries the granted node first, and gets the
# administrator's VaapiDriverMode (KRDP_VIRTUAL_VAAPI_DRIVER) the way krdpserver applies it.
[[ $vaapi_driver =~ ^(auto|off|radeonsi|iHD|i965)$ ]]
render_environment+=(FARSIDE_RENDER_NODE="$render_node")
case $vaapi_driver in
    auto) ;;
    off) render_environment+=(FARSIDE_AUTO_VAAPI_DRIVER=0);;
    *) render_environment+=(FARSIDE_FORCE_VAAPI_DRIVER="$vaapi_driver" LIBVA_DRIVER_NAME="$vaapi_driver");;
esac

# Seed missing settings only. Never overwrite a retained user's profile.
cp -rn "$support/defaults/." "$profile/config/"
mkdir -p "$profile/data/applications" "$runtime/config-defaults"
[[ $(realpath -e "$profile/data/applications") == "$profile/data/applications" ]]
[[ ! -L $profile/data/applications/io.github.westers.farside.consoleworker.desktop ]]
ln -s /etc/xdg/menus "$runtime/config-defaults/menus"
# FARSIDE-VLOCK: the lock screen cannot authenticate under no_new_privs; lock it off.
cp "$support/locked-config/kscreenlockerrc" "$support/locked-config/kdeglobals" "$runtime/config-defaults/"
printf '[Desktop Entry]\nType=Application\nName=Farside Virtual Capture\nNoDisplay=true\nExec=%s\nX-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1,org_kde_kwin_fake_input\n' "$worker" \
    >"$profile/data/applications/io.github.westers.farside.consoleworker.desktop"
farside_system_bus_setup "$runtime" || { echo 'Invalid system-bus options' >&2; exit 1; }
apparmor_query=()
if [[ -e /sys/kernel/security/apparmor/.access ]]; then
    apparmor_query=(--bind /sys/kernel/security/apparmor/.access /sys/kernel/security/apparmor/.access)
fi
printf 'Selected isolated render GPU %s (%s, VaapiDriverMode %s); desktop %s\n' "$selected" "$render_node" "$vaapi_driver" "$session"
exec env -i PATH=/usr/bin:/bin HOME="$HOME" USER="$(id -un)" LOGNAME="$(id -un)" LANG=C.UTF-8 \
    XDG_RUNTIME_DIR="$runtime" XDG_CONFIG_HOME="$profile/config" XDG_CACHE_HOME="$profile/cache" \
    XDG_STATE_HOME="$profile/state" XDG_DATA_HOME="$profile/data" XDG_CONFIG_DIRS="$runtime/config-defaults" \
    XDG_DATA_DIRS=/usr/local/share:/usr/share XDG_CURRENT_DESKTOP=KDE XDG_MENU_PREFIX=plasma- XDG_SESSION_TYPE=wayland \
    PIPEWIRE_RUNTIME_DIR="$runtime" PIPEWIRE_REMOTE=pipewire-0 PULSE_RUNTIME_PATH="$runtime/pulse" PULSE_SERVER="unix:$runtime/pulse/native" \
    "${render_environment[@]}" \
    bwrap --unshare-pid --unshare-ipc --die-with-parent --new-session --ro-bind / / \
    "${apparmor_query[@]}" --proc /proc --dev /dev --tmpfs /tmp --tmpfs /run \
    "${render_bindings[@]}" --perms 01777 --dir /tmp/.X11-unix \
    --bind "$HOME" "$HOME" --bind "$runtime" "$runtime" \
    "${system_bus_bwrap[@]}" \
    dbus-run-session --config-file="$support/virtual-session-bus.conf" \
    -- /usr/bin/bash "$inner" "$session" "$worker" "$support" "$width" "$height" "${layout_args[@]}" \
    >"$runtime/desktop.log" 2>&1
