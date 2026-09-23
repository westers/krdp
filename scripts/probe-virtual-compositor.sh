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
    [[ $# == 0 || ( $# == 3 && ( "$1" == --supervised-worker-nvidia || "$1" == --supervised-multi-worker-nvidia || "$1" == --supervised-mixed-worker-nvidia ) ) || ( $# == 1 && ( "$1" == --plasma || "$1" == --plasma-nvidia || "$1" == --plasma-rdp-nvidia || "$1" == --plasma-retention-nvidia || "$1" == --plasma-audio-nvidia || "$1" == --plasma-worker-nvidia || "$1" == --plasma-multi-worker-nvidia || "$1" == --plasma-mixed-worker-nvidia || "$1" == --plasma-multi-create-nvidia || "$1" == --plasma-multi-add-nvidia || "$1" == --plasma-multi-remove-nvidia || "$1" == --plasma-multi-resize-nvidia || "$1" == --plasma-multi-fit-nvidia || "$1" == --plasma-negative-worker-nvidia || "$1" == --plasma-multi-window-nvidia || "$1" == --plasma-multi-input-nvidia || "$1" == --plasma-multi-drag-nvidia || "$1" == --plasma-multi-reposition-nvidia ) ) ]]
    probe_mode="${1:-}"
    rdp_mode=
    probe_timeout=80
    probe_output_count=1
    managed_runtime=
    managed_id=
    layout_hint=
    if [[ "$probe_mode" == --supervised-worker-nvidia || "$probe_mode" == --supervised-multi-worker-nvidia || "$probe_mode" == --supervised-mixed-worker-nvidia ]]; then
        [[ "$probe_mode" == --supervised-worker-nvidia ]] || probe_output_count=2
        [[ "$probe_mode" != --supervised-mixed-worker-nvidia ]] || layout_hint=--multi-mixed
        managed_runtime="$2"
        managed_id="$3"
        [[ "$managed_runtime" == /run/user/"$(id -u)"/krdp-headless.* ]]
        [[ -d "$managed_runtime" && ! -L "$managed_runtime" && -O "$managed_runtime" ]]
        [[ "$(realpath "$managed_runtime")" == "$managed_runtime" && "$(stat -c %a "$managed_runtime")" == 700 ]]
        [[ "$managed_id" =~ ^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$ ]]
        [[ -S "$managed_runtime/worker.sock" && -f "$managed_runtime/worker-token" && ! -L "$managed_runtime/worker-token" ]]
        [[ "$(stat -c %s "$managed_runtime/worker-token")" == 32 ]]
        rdp_mode=--supervised
        probe_mode=--plasma-nvidia
        probe_timeout=120
    fi
    if [[ "$probe_mode" == --plasma-worker-nvidia || "$probe_mode" == --plasma-multi-worker-nvidia || "$probe_mode" == --plasma-mixed-worker-nvidia || "$probe_mode" == --plasma-multi-create-nvidia || "$probe_mode" == --plasma-multi-add-nvidia || "$probe_mode" == --plasma-multi-remove-nvidia || "$probe_mode" == --plasma-multi-resize-nvidia || "$probe_mode" == --plasma-multi-fit-nvidia || "$probe_mode" == --plasma-negative-worker-nvidia || "$probe_mode" == --plasma-multi-window-nvidia || "$probe_mode" == --plasma-multi-input-nvidia || "$probe_mode" == --plasma-multi-drag-nvidia || "$probe_mode" == --plasma-multi-reposition-nvidia ]]; then
        rdp_mode=--worker
        [[ "$probe_mode" != --plasma-multi-worker-nvidia ]] || rdp_mode=--multi-worker
        [[ "$probe_mode" != --plasma-mixed-worker-nvidia ]] || rdp_mode=--multi-mixed
        [[ "$probe_mode" != --plasma-multi-create-nvidia ]] || rdp_mode=--multi-create
        [[ "$probe_mode" != --plasma-multi-add-nvidia ]] || rdp_mode=--multi-add
        [[ "$probe_mode" != --plasma-multi-remove-nvidia ]] || rdp_mode=--multi-remove
        [[ "$probe_mode" != --plasma-multi-resize-nvidia ]] || rdp_mode=--multi-resize
        [[ "$probe_mode" != --plasma-multi-fit-nvidia ]] || rdp_mode=--multi-fit
        [[ "$probe_mode" != --plasma-negative-worker-nvidia ]] || rdp_mode=--multi-negative
        [[ "$probe_mode" != --plasma-multi-window-nvidia ]] || rdp_mode=--multi-window
        [[ "$probe_mode" != --plasma-multi-input-nvidia ]] || rdp_mode=--multi-input
        [[ "$probe_mode" != --plasma-multi-drag-nvidia ]] || rdp_mode=--multi-drag
        [[ "$probe_mode" != --plasma-multi-reposition-nvidia ]] || rdp_mode=--multi-reposition
        [[ "$rdp_mode" == --worker ]] || probe_output_count=2
        probe_mode=--plasma-nvidia
        probe_timeout=120
    fi
    if [[ "$probe_mode" == --plasma-rdp-nvidia || "$probe_mode" == --plasma-retention-nvidia || "$probe_mode" == --plasma-audio-nvidia ]]; then
        # Disposable acceptance listener, never the installed console service.
        if ss -H -ltn 'sport = :3394' | grep -q .; then
            echo 'Test port3394 already occupied; refusing probe.' >&2
            exit 1
        fi
        rdp_mode=--rdp
        probe_timeout=150
        if [[ "$probe_mode" == --plasma-retention-nvidia ]]; then
            rdp_mode=--retention
            probe_timeout=240
        fi
        if [[ "$probe_mode" == --plasma-audio-nvidia ]]; then
            rdp_mode=--audio
            probe_timeout=240
        fi
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
    probe_runtime="$managed_runtime"
    if [[ -z "$probe_runtime" ]]; then
        probe_runtime=$(mktemp -d "/run/user/$(id -u)/krdp-headless.XXXXXX")
    fi
    mkdir "$probe_runtime/config" "$probe_runtime/cache" "$probe_runtime/state" "$probe_runtime/data"
    cp -r "$repo_path/scripts/virtual-probe-config/." "$probe_runtime/config/"
    if [[ "$rdp_mode" == --worker || "$rdp_mode" == --multi-worker || "$rdp_mode" == --multi-mixed || "$rdp_mode" == --multi-create || "$rdp_mode" == --multi-add || "$rdp_mode" == --multi-remove || "$rdp_mode" == --multi-resize || "$rdp_mode" == --multi-fit || "$rdp_mode" == --multi-negative || "$rdp_mode" == --multi-window || "$rdp_mode" == --multi-input || "$rdp_mode" == --multi-drag || "$rdp_mode" == --multi-reposition || "$rdp_mode" == --supervised ]]; then
        mkdir -p "$probe_runtime/data/applications"
        cp "$repo_path/build/server/org.kde.krdpvirtualprobe.desktop" "$probe_runtime/data/applications/org.kde.krdpconsoleworker.desktop"
        if [[ "$rdp_mode" == --multi-create ]]; then
            cp "$repo_path/build/server/org.kde.krdpvirtualmonitorprobe.desktop" "$probe_runtime/data/applications/"
        fi
    elif [[ -n "$rdp_mode" ]]; then
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
        PIPEWIRE_RUNTIME_DIR="$probe_runtime" PIPEWIRE_REMOTE=pipewire-0 \
        PULSE_RUNTIME_PATH="$probe_runtime/pulse" PULSE_SERVER="unix:$probe_runtime/pulse/native" \
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
        -- bash "$script_path" --inside-private-bus "$probe_mode" "$rdp_mode" "$managed_id" "$probe_output_count" "$layout_hint" \
        >"$probe_runtime/probe.log" 2>&1
fi
[[ "$XDG_RUNTIME_DIR" == /run/user/"$(id -u)"/krdp-headless.* ]]
expected_outputs=${5:-1}
[[ $expected_outputs == 1 || ( $expected_outputs == 2 && ( ${3:-} == --multi-worker || ${3:-} == --multi-mixed || ${3:-} == --multi-create || ${3:-} == --multi-add || ${3:-} == --multi-remove || ${3:-} == --multi-resize || ${3:-} == --multi-fit || ${3:-} == --multi-negative || ${3:-} == --multi-window || ${3:-} == --multi-input || ${3:-} == --multi-drag || ${3:-} == --multi-reposition || ${3:-} == --supervised ) ) ]]
layout_mode=${3:-}
if [[ $layout_mode == --supervised && ${6:-} == --multi-mixed ]]; then layout_mode=--multi-mixed; fi
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
kwin_wayland_wrapper "${compat_args[@]}" --virtual --width 1280 --height 720 --output-count "$expected_outputs" \
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
jq -e --argjson count "$expected_outputs" '.outputs | length == $count' "$XDG_RUNTIME_DIR/outputs.json"
jq -e 'all(.outputs[]; .enabled == true and .size.width == 1280 and .size.height == 720)' "$XDG_RUNTIME_DIR/outputs.json"
if [[ $expected_outputs == 2 ]]; then
    mapfile -t output_names < <(jq -r '.outputs[].name' "$XDG_RUNTIME_DIR/outputs.json")
    [[ ${#output_names[@]} == 2 && ${output_names[0]} =~ ^Virtual-[0-9]+$ && ${output_names[1]} =~ ^Virtual-[0-9]+$ ]]
    if [[ ${3:-} == --multi-negative ]]; then
        env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland timeout 10 kscreen-doctor \
            "output.${output_names[0]}.position.-1280,-100" "output.${output_names[1]}.position.0,0"
    elif [[ $layout_mode == --multi-mixed || $layout_mode == --multi-input ]]; then
        env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland timeout 10 kscreen-doctor \
            "output.${output_names[0]}.scale.1.25" "output.${output_names[0]}.position.0,0" \
            "output.${output_names[1]}.position.1024,100"
    else
        env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland timeout 10 kscreen-doctor \
            "output.${output_names[0]}.position.0,0" "output.${output_names[1]}.position.1280,0"
    fi
    env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland timeout 10 kscreen-doctor -j >"$XDG_RUNTIME_DIR/outputs.json"
    if [[ ${3:-} == --multi-negative ]]; then
        # This KWin build normalizes the workspace origin after accepting a
        # negative position. Record the actual readback, not the request.
        jq -e '(.outputs | map(.pos.x) | sort) == [0,1280] and (.outputs | map(.pos.y) | sort) == [0,100]' "$XDG_RUNTIME_DIR/outputs.json"
        echo 'Private KWin normalized requested negative origin to 0,0'
    elif [[ $layout_mode == --multi-mixed || $layout_mode == --multi-input ]]; then
        jq -e '(.outputs | map(.pos.x) | sort) == [0,1024] and any(.outputs[]; .scale == 1.25)' "$XDG_RUNTIME_DIR/outputs.json"
    else
        jq -e '(.outputs | map(.pos.x) | sort) == [0,1280]' "$XDG_RUNTIME_DIR/outputs.json"
    fi
fi
timeout 5 pw-dump >"$XDG_RUNTIME_DIR/graph.json"
jq -e '[.[] | select(.type == "PipeWire:Interface:Device")] | length == 0' "$XDG_RUNTIME_DIR/graph.json"
gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.ListNames
echo 'Private Wayland compositor ready at 1280x720'
if [[ "${3:-}" == --supervised ]]; then
    env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland \
        "$repo_path/build/bin/krdp-console-worker" --virtual-session "$4" --uid "$(id -u)" \
        --socket "$XDG_RUNTIME_DIR/worker.sock" --token-fd 0 --desktop-media \
        <"$XDG_RUNTIME_DIR/worker-token" >"$XDG_RUNTIME_DIR/managed-worker.log" 2>&1 &
    # The supervisor owns the namespace, not the capture worker or RDP client.
    # A transport/worker disconnect must not terminate the Plasma desktop.
    while kill -0 "$plasma_pid" && kill -0 "$wrapper_pid"; do sleep 1; done
    exit 1
fi
if [[ "${3:-}" == --worker || "${3:-}" == --multi-worker || "${3:-}" == --multi-mixed || "${3:-}" == --multi-create || "${3:-}" == --multi-add || "${3:-}" == --multi-remove || "${3:-}" == --multi-resize || "${3:-}" == --multi-fit || "${3:-}" == --multi-negative || "${3:-}" == --multi-window || "${3:-}" == --multi-input || "${3:-}" == --multi-drag || "${3:-}" == --multi-reposition ]]; then
    if [[ "${3:-}" == --multi-create ]]; then
        env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland \
            "$repo_path/build/bin/krdp-virtual-monitor-creation-probe" \
            >"$XDG_RUNTIME_DIR/creation-probe.log" 2>&1
    fi
    if [[ "${3:-}" == --multi-window || "${3:-}" == --multi-input || "${3:-}" == --multi-drag ]]; then
        # This window is created in the SAME private compositor as the capture
        # worker. KWin's own move-to-screen API proves that its second output
        # is a real workspace destination, not just an RDP client view.
        env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland \
            konsole --nofork -p LocalTabTitleFormat=KRDP-MONITOR-PROBE \
            -p tabtitle=KRDP-MONITOR-PROBE -e sleep 45 \
            >"$XDG_RUNTIME_DIR/window-app.log" 2>&1 &
        window_pid=$!
        script_file="$repo_path/scripts/probe-retained-window-move.js"
        [[ "${3:-}" != --multi-drag ]] || script_file="$repo_path/scripts/probe-retained-window-drag.js"
        qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript \
            "$script_file" krdp-retained-window-probe
        qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start
        moved=false
        for attempt in {1..100}; do
            kill -0 "$window_pid"
            # KWin may finish moving asynchronously after sendClientToScreen
            # returns, especially when the outputs have different scales.
            if [[ "${3:-}" == --multi-drag ]]; then
                if grep -Eq 'krdp-monitor-drag: before output=Virtual-0' "$XDG_RUNTIME_DIR/kwin.log"; then moved=true; break; fi
            elif grep -Eq 'krdp-monitor-probe: (after output|outputChanged)=Virtual-1' "$XDG_RUNTIME_DIR/kwin.log"; then
                moved=true
                break
            fi
            sleep 0.1
        done
        [[ "$moved" == true ]]
    fi
    probe_args=()
    [[ "${3:-}" != --multi-worker ]] || probe_args=(--multi)
    [[ "${3:-}" != --multi-mixed ]] || probe_args=(--multi-mixed)
    [[ "${3:-}" != --multi-create ]] || probe_args=(--multi)
    [[ "${3:-}" != --multi-add ]] || probe_args=(--multi-add)
    [[ "${3:-}" != --multi-remove ]] || probe_args=(--multi-remove)
    [[ "${3:-}" != --multi-resize ]] || probe_args=(--multi-resize)
    [[ "${3:-}" != --multi-fit ]] || probe_args=(--multi-fit)
    [[ "${3:-}" != --multi-negative ]] || probe_args=(--multi-negative)
    [[ "${3:-}" != --multi-window ]] || probe_args=(--multi)
    [[ "${3:-}" != --multi-input ]] || probe_args=(--multi-input)
    [[ "${3:-}" != --multi-drag ]] || probe_args=(--multi-drag)
    [[ "${3:-}" != --multi-reposition ]] || probe_args=(--multi-reposition)
    "$repo_path/build/bin/krdp-virtual-worker-probe" "${probe_args[@]}" >"$XDG_RUNTIME_DIR/worker-probe.log" 2>&1
    # Stopping capture must leave the separate desktop alive and unchanged.
    kill -0 "$wrapper_pid"
    kill -0 "$plasma_pid"
    if [[ "${3:-}" == --multi-add || "${3:-}" == --multi-remove ]]; then
        # The owned output disappears only after the worker's Wayland client
        # closes; wait for KWin's asynchronous screen removal before asserting.
        for attempt in {1..50}; do
            env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland \
                kscreen-doctor -j >"$XDG_RUNTIME_DIR/outputs-after-worker.json"
            [[ $(jq '.outputs | length' "$XDG_RUNTIME_DIR/outputs-after-worker.json") == "$expected_outputs" ]] && break
            sleep 0.1
        done
    else
        env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland \
            kscreen-doctor -j >"$XDG_RUNTIME_DIR/outputs-after-worker.json"
    fi
    jq -e --argjson count "$expected_outputs" '.outputs | length == $count' "$XDG_RUNTIME_DIR/outputs-after-worker.json"
    if [[ "${3:-}" == --multi-resize ]]; then
        jq -e 'any(.outputs[]; .name == "Virtual-0" and .size.width == 1280 and .size.height == 720 and .scale == 1) and any(.outputs[]; .name == "Virtual-1" and .size.width == 1600 and .size.height == 900 and .scale == 1.25)' "$XDG_RUNTIME_DIR/outputs-after-worker.json"
    elif [[ "${3:-}" == --multi-fit ]]; then
        jq -e 'any(.outputs[]; .name == "Virtual-0" and .size.width == 1600 and .size.height == 900 and .pos.x == 0) and any(.outputs[]; .name == "Virtual-1" and .size.width == 1280 and .size.height == 720 and .pos.x == 1600)' "$XDG_RUNTIME_DIR/outputs-after-worker.json"
    else
        jq -e 'all(.outputs[]; .enabled == true and .size.width == 1280 and .size.height == 720)' "$XDG_RUNTIME_DIR/outputs-after-worker.json"
    fi
fi
if [[ "${3:-}" == --rdp || "${3:-}" == --retention || "${3:-}" == --audio ]]; then
    acceptance_seconds=90
    if [[ "${3:-}" == --audio ]]; then
        acceptance_seconds=180
        ffmpeg -nostdin -hide_banner -loglevel error -f lavfi \
            -i sine=frequency=997:sample_rate=48000:duration=10 -ac 2 "$XDG_RUNTIME_DIR/tone.wav"
        audio_sent=false
    fi
    if [[ "${3:-}" == --retention ]]; then
        # stdin creates an unnamed, unsaved document in the private desktop.
        # This fixture must outlive individual RDP transports, not the bounded
        # probe itself. Never launch against the physical session's bus.
        env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland \
            kate --startanon --stdin >"$XDG_RUNTIME_DIR/retention-app.log" 2>&1 <<EOF &
KRDP unsaved reconnect acceptance
Private desktop identity: ${XDG_RUNTIME_DIR##*/}
This document has never been saved.
EOF
        retention_pid=$!
        acceptance_seconds=180
        printf '%s\n' "$retention_pid" >"$XDG_RUNTIME_DIR/retention-app.pid"
        cat "/proc/$retention_pid/stat" >"$XDG_RUNTIME_DIR/retention-app-start.stat"
    fi
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
    echo "Private desktop RDP listener ready on Sol3394 for $acceptance_seconds seconds"
    for ((attempt=1; attempt<=acceptance_seconds; ++attempt)); do
        kill -0 "$rdp_pid"
        kill -0 "$policy_pid"
        if [[ "${audio_sent:-true}" == false ]] && grep -q 'RDPSND channel initialized after explicit media consent' "$XDG_RUNTIME_DIR/rdp.log"; then
            # Ordinary Pulse playback must route to the private graph default,
            # including a client-requested silent-host sink. No physical target.
            sleep 3
            pactl get-default-sink >"$XDG_RUNTIME_DIR/tone-default-sink.txt"
            timeout 15 paplay "$XDG_RUNTIME_DIR/tone.wav" >"$XDG_RUNTIME_DIR/tone-playback.log" 2>&1
            audio_sent=true
        fi
        if (( attempt % 10 == 0 )); then
            timeout 5 pw-dump >"$XDG_RUNTIME_DIR/rdp-graph-$attempt.json"
            if [[ -n "${retention_pid:-}" ]]; then
                kill -0 "$retention_pid"
                cat "/proc/$retention_pid/stat" >"$XDG_RUNTIME_DIR/retention-app-$attempt.stat"
            fi
        fi
        sleep 1
    done
fi
