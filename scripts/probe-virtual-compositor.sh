#!/usr/bin/env bash
# Explicit Sol-only headless smoke; not an installed session launcher.
set -euo pipefail
if [[ "$(hostname -s)" != sol ]]; then
    echo 'This acceptance probe is restricted to Sol, not the active Hal desktop.' >&2
    exit 1
fi
script_path=$(realpath "$0")
repo_path=$(dirname "$(dirname "$script_path")")
if [[ "${1:-}" != --inside-private-bus ]]; then
    [[ $# == 0 || ( $# == 1 && ( "$1" == --plasma || "$1" == --plasma-nvidia || "$1" == --plasma-rdp-nvidia ) ) ]]
    probe_mode="${1:-}"
    rdp_mode=
    probe_timeout=80
    if [[ "$probe_mode" == --plasma-rdp-nvidia ]]; then
        # Disposable acceptance listener, never the installed console service.
        if ss -H -ltn 'sport = :3394' | grep -q .; then
            echo 'Test port3394 already occupied; refusing probe.' >&2
            exit 1
        fi
        rdp_mode=--rdp
        probe_timeout=150
        probe_mode=--plasma-nvidia
    fi
    render_bindings=()
    render_environment=(LIBGL_ALWAYS_SOFTWARE=1
        __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
        __GLX_VENDOR_LIBRARY_NAME=mesa)
    if [[ "$probe_mode" == --plasma-nvidia ]]; then
        # Explicit opt-in for Sol's verified RTX2070 only. This test never
        # grants device permissions and never exposes a DRM modesetting node.
        render_node=$(realpath /dev/dri/by-path/pci-0000:09:00.0-render)
        [[ "$render_node" =~ ^/dev/dri/renderD[0-9]+$ ]]
        [[ "$(cat /sys/class/drm/"${render_node##*/}"/device/vendor)" == 0x10de ]]
        [[ "$(cat /sys/class/drm/"${render_node##*/}"/device/device)" == 0x1f02 ]]
        grep -Eq '^Device Minor:[[:space:]]+0$' /proc/driver/nvidia/gpus/0000:09:00.0/information
        for device in "$render_node" /dev/nvidia0 /dev/nvidiactl /dev/nvidia-uvm; do
            if [[ ! -c "$device" || ! -r "$device" || ! -w "$device" ]]; then
                echo "GPU probe requires existing read/write permission: $device (no permissions changed)" >&2
                exit 1
            fi
            render_bindings+=(--dev-bind "$device" "$device")
        done
        render_environment=(
            __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json
            __GLX_VENDOR_LIBRARY_NAME=nvidia)
        probe_mode=--plasma
    fi
    probe_runtime=$(mktemp -d "/run/user/$(id -u)/krdp-headless.XXXXXX")
    mkdir "$probe_runtime/config" "$probe_runtime/cache" "$probe_runtime/state" "$probe_runtime/data"
    cp -r "$repo_path/scripts/virtual-probe-config/." "$probe_runtime/config/"
    if [[ -n "$rdp_mode" ]]; then
        mkdir -p "$probe_runtime/data/applications"
        cp "$repo_path/scripts/virtual-probe-rdp.desktop" "$probe_runtime/data/applications/org.kde.krdpserver.desktop"
        (umask 077; openssl rand -hex 24 >"$probe_runtime/rdp-password")
        (umask 077; openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
            -subj /CN=krdp-private-probe -keyout "$probe_runtime/rdp.key" \
            -out "$probe_runtime/rdp.crt" >"$probe_runtime/tls.log" 2>&1)
    fi
    mkdir "$probe_runtime/config-defaults"
    ln -s /etc/xdg/menus "$probe_runtime/config-defaults/menus"
    echo "Headless probe evidence: $probe_runtime"
    apparmor_query=()
    if [[ -e /sys/kernel/security/apparmor/.access ]]; then
        # This world-writable kernel interface accepts permission QUERIES,
        # not profile updates. Preserve D-Bus mediation in a read-only root.
        apparmor_query=(--bind /sys/kernel/security/apparmor/.access /sys/kernel/security/apparmor/.access)
    fi
    # Retain the real HOME identity, but no existing display, session bus, Qt
    # reconnect, session id, manager notification, or inherited plugin settings.
    exec env -i PATH=/usr/bin:/bin HOME="$HOME" USER="$(id -un)" LOGNAME="$(id -un)" \
        LANG=C.UTF-8 XDG_RUNTIME_DIR="$probe_runtime" XDG_CONFIG_HOME="$probe_runtime/config" \
        XDG_CONFIG_DIRS="$probe_runtime/config-defaults" XDG_CURRENT_DESKTOP=KDE XDG_MENU_PREFIX=plasma- \
        XDG_CACHE_HOME="$probe_runtime/cache" XDG_STATE_HOME="$probe_runtime/state" \
        XDG_DATA_HOME="$probe_runtime/data" XDG_DATA_DIRS=/usr/local/share:/usr/share \
        XDG_SESSION_TYPE=wayland "${render_environment[@]}" \
        timeout "$probe_timeout" bwrap --unshare-pid --unshare-ipc --die-with-parent --new-session \
        --ro-bind / / "${apparmor_query[@]}" --proc /proc --dev /dev --tmpfs /tmp --tmpfs /run \
        "${render_bindings[@]}" \
        --perms 01777 --dir /tmp/.X11-unix \
        --bind "$probe_runtime" "$probe_runtime" \
        dbus-run-session --config-file="$repo_path/server/virtual-session-bus.conf" \
        -- bash "$script_path" --inside-private-bus "$probe_mode" "$rdp_mode" \
        >"$probe_runtime/probe.log" 2>&1
fi
[[ "$XDG_RUNTIME_DIR" == /run/user/"$(id -u)"/krdp-headless.* ]]
# Populate this private profile's desktop-service identities before KWin checks
# application permissions (including Spectacle's restricted screenshot API).
kbuildsycoca6 --noincremental >"$XDG_RUNTIME_DIR/service-cache.log" 2>&1
wrapper_pid=
graph_pid=
policy_pid=
rdp_pid=
cleanup() {
    if [[ -n "$rdp_pid" ]]; then
        kill "$rdp_pid" 2>/dev/null || true
        wait "$rdp_pid" 2>/dev/null || true
    fi
    if [[ -n "$wrapper_pid" ]]; then
        # KWinWrapper's destructor terminates/waits for its own KWin child.
        kill "$wrapper_pid" 2>/dev/null || true
        wait "$wrapper_pid" 2>/dev/null || true
    fi
    if [[ -n "$policy_pid" ]]; then
        kill "$policy_pid" 2>/dev/null || true
        wait "$policy_pid" 2>/dev/null || true
    fi
    if [[ -n "$graph_pid" ]]; then
        kill "$graph_pid" 2>/dev/null || true
        wait "$graph_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT
env PIPEWIRE_RUNTIME_DIR="$XDG_RUNTIME_DIR" PIPEWIRE_CONFIG_DIR="$repo_path/server" \
    PIPEWIRE_CONFIG_NAME=virtual-session-pipewire.conf pipewire \
    >"$XDG_RUNTIME_DIR/pipewire.log" 2>&1 &
graph_pid=$!
for attempt in {1..50}; do
    kill -0 "$graph_pid"
    [[ -S "$XDG_RUNTIME_DIR/pipewire-0" ]] && break
    sleep 0.1
done
[[ -S "$XDG_RUNTIME_DIR/pipewire-0" ]]
# AUTOCONNECT capture streams need a session-policy manager to create links.
# The policy-only profile excludes hardware discovery; all sockets and state
# remain inside this probe's private runtime and D-Bus namespace.
env PIPEWIRE_RUNTIME_DIR="$XDG_RUNTIME_DIR" PIPEWIRE_REMOTE=pipewire-0 \
    WIREPLUMBER_CONFIG_DIR=/usr/share/wireplumber wireplumber --profile policy \
    >"$XDG_RUNTIME_DIR/policy.log" 2>&1 &
policy_pid=$!
ready=false
for attempt in {1..50}; do
    kill -0 "$policy_pid"
    timeout 5 pw-dump >"$XDG_RUNTIME_DIR/policy-graph.json"
    if jq -e 'any(.[]; .type == "PipeWire:Interface:Client" and .info.props."application.process.binary" == "wireplumber")' "$XDG_RUNTIME_DIR/policy-graph.json" >/dev/null; then
        ready=true
        break
    fi
    sleep 0.1
done
[[ "$ready" == true ]]
compat_args=()
[[ "${2:-}" != --plasma ]] || compat_args=(--xwayland)
kwin_wayland_wrapper "${compat_args[@]}" --virtual --width 1280 --height 720 --output-count 1 \
    --no-global-shortcuts --no-kactivities >"$XDG_RUNTIME_DIR/kwin.log" 2>&1 &
wrapper_pid=$!
ready=false
for attempt in {1..100}; do
    kill -0 "$wrapper_pid"
    if gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.NameHasOwner org.kde.KWin | grep -q true; then
        ready=true
        break
    fi
    sleep 0.1
done
[[ "$ready" == true ]]
[[ -S "$XDG_RUNTIME_DIR/wayland-0" ]]
gdbus call --session --dest org.kde.KWin --object-path /KWin \
    --method org.kde.KWin.supportInformation >"$XDG_RUNTIME_DIR/kwin-support.txt"
if [[ "${2:-}" == --plasma ]]; then
    # KWin 6.6's screenshot AND screencast plugins require OpenGL. Shell
    # readiness with QPainter cannot establish an RDP-capable desktop.
    if grep -q 'Compositing Type: QPainter' "$XDG_RUNTIME_DIR/kwin-support.txt"; then
        echo 'Capture unavailable: virtual KWin selected QPainter; an isolated OpenGL rendering backend is required.' >&2
        exit 1
    fi
    # The wrapper was started before plasma_session could receive its environment
    # update. Obtain only the display and authority PATH from this private child;
    # never read or print the authority cookie or inspect another session.
    kwin_pid=$(pgrep -P "$wrapper_pid" -x kwin_wayland)
    readarray -d '' -t kwin_args <"/proc/$kwin_pid/cmdline"
    compat_display=
    compat_authority=
    for ((i=0; i+1<${#kwin_args[@]}; ++i)); do
        case "${kwin_args[i]}" in
            --xwayland-display) compat_display="${kwin_args[i+1]}" ;;
            --xwayland-xauthority) compat_authority="${kwin_args[i+1]}" ;;
        esac
    done
    [[ "$compat_display" =~ ^:[0-9]+$ ]]
    [[ "$compat_authority" == "$XDG_RUNTIME_DIR/"* && -f "$compat_authority" ]]
    env DISPLAY="$compat_display" XAUTHORITY="$compat_authority" \
        WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland startplasma-wayland \
        >"$XDG_RUNTIME_DIR/plasma.log" 2>&1 &
    plasma_pid=$!
    ready=false
    for attempt in {1..200}; do
        kill -0 "$plasma_pid"
        if gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
            --method org.freedesktop.DBus.NameHasOwner org.kde.plasmashell | grep -q true; then
            ready=true
            break
        fi
        sleep 0.1
    done
    [[ "$ready" == true ]]
    gdbus call --session --dest org.kde.plasmashell --object-path /PlasmaShell \
        --method org.kde.PlasmaShell.evaluateScript 'print(desktops().length)' \
        >"$XDG_RUNTIME_DIR/desktop-count.txt"
    grep -Eq '[1-9]' "$XDG_RUNTIME_DIR/desktop-count.txt"
    echo 'Plasma shell owns its private bus name and reports a desktop'
    dbus-monitor --session "destination='org.kde.KWin.ScreenShot2'" \
        >"$XDG_RUNTIME_DIR/screenshot-bus.log" 2>&1 &
    env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland \
        timeout 35 spectacle --background --nonotify --fullscreen \
        --output "$XDG_RUNTIME_DIR/desktop.png" >"$XDG_RUNTIME_DIR/screenshot.log" 2>&1
    [[ -s "$XDG_RUNTIME_DIR/desktop.png" ]]
fi
env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland \
    timeout 10 kscreen-doctor -j >"$XDG_RUNTIME_DIR/outputs.json"
jq -e '.outputs | length == 1' "$XDG_RUNTIME_DIR/outputs.json"
jq -e '.outputs[0] | .enabled == true and .size.width == 1280 and .size.height == 720' "$XDG_RUNTIME_DIR/outputs.json"
timeout 5 pw-dump >"$XDG_RUNTIME_DIR/graph.json"
jq -e '[.[] | select(.type == "PipeWire:Interface:Device")] | length == 0' "$XDG_RUNTIME_DIR/graph.json"
gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.ListNames
echo 'Private Wayland compositor ready at 1280x720'
if [[ "${3:-}" == --rdp ]]; then
    env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland \
        LD_LIBRARY_PATH=/opt/krdp-console/lib/x86_64-linux-gnu \
        /opt/krdp-console/bin/krdpserver --plasma --monitor 0 --quality 80 \
        --address 192.168.48.57 --port 3394 -u krdptest \
        --certificate "$XDG_RUNTIME_DIR/rdp.crt" --certificate-key "$XDG_RUNTIME_DIR/rdp.key" \
        -p "$(<"$XDG_RUNTIME_DIR/rdp-password")" \
        >"$XDG_RUNTIME_DIR/rdp.log" 2>&1 &
    rdp_pid=$!
    for attempt in {1..100}; do
        kill -0 "$rdp_pid"
        if ss -H -ltn 'sport = :3394' | grep -q .; then break; fi
        sleep 0.1
    done
    ss -H -ltn 'sport = :3394' | grep -q .
    echo 'Private desktop RDP listener ready on Sol3394 for90 seconds'
    for attempt in {1..90}; do
        kill -0 "$rdp_pid"
        kill -0 "$policy_pid"
        if (( attempt % 10 == 0 )); then
            timeout 5 pw-dump >"$XDG_RUNTIME_DIR/rdp-graph-$attempt.json"
        fi
        sleep 1
    done
fi
