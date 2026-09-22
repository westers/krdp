#!/usr/bin/env bash
# Inside the guardian's PID/mount/bus namespace only. Do not invoke on seat0.
set -euo pipefail
umask 077
[[ $# == 5 && $(id -u) != 0 && $XDG_RUNTIME_DIR == /run/user/"$(id -u)"/krdp-virtual/* ]]
[[ ! -S /run/dbus/system_bus_socket && ! -S /run/user/"$(id -u)"/bus ]]
session=$1 worker=$2 support=$3 width=$4 height=$5
wrapper_pid= graph_pid= policy_pid= plasma_pid= worker_pid=
cleanup() {
    for child in "$worker_pid" "$plasma_pid" "$wrapper_pid" "$policy_pid" "$graph_pid"; do
        if [[ -n $child ]]; then kill "$child" 2>/dev/null || true; fi
    done
}
trap cleanup EXIT
kbuildsycoca6 --noincremental >"$XDG_RUNTIME_DIR/service-cache.log" 2>&1
env PIPEWIRE_CONFIG_DIR="$support" PIPEWIRE_CONFIG_NAME=virtual-session-pipewire.conf pipewire >"$XDG_RUNTIME_DIR/pipewire.log" 2>&1 &
graph_pid=$!
for attempt in {1..50}; do
    kill -0 "$graph_pid"
    [[ -S $XDG_RUNTIME_DIR/pipewire-0 ]] && break
    sleep 0.1
done
[[ -S $XDG_RUNTIME_DIR/pipewire-0 ]]
env WIREPLUMBER_CONFIG_DIR=/usr/share/wireplumber wireplumber --profile policy >"$XDG_RUNTIME_DIR/policy.log" 2>&1 &
policy_pid=$!
ready=false
for attempt in {1..50}; do
    kill -0 "$policy_pid"
    timeout 5 pw-dump >"$XDG_RUNTIME_DIR/policy-graph.json"
    if jq -e 'any(.[]; .type == "PipeWire:Interface:Client" and .info.props."application.process.binary" == "wireplumber")' "$XDG_RUNTIME_DIR/policy-graph.json" >/dev/null; then ready=true; break; fi
    sleep 0.1
done
[[ $ready == true ]]
kwin_wayland_wrapper --xwayland --virtual --width "$width" --height "$height" --output-count 1 \
    --no-global-shortcuts --no-kactivities >"$XDG_RUNTIME_DIR/kwin.log" 2>&1 &
wrapper_pid=$!
ready=false
for attempt in {1..100}; do
    kill -0 "$wrapper_pid"
    if timeout 2 gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.NameHasOwner org.kde.KWin | grep -q true; then ready=true; break; fi
    sleep 0.1
done
[[ $ready == true && -S $XDG_RUNTIME_DIR/wayland-0 ]]
timeout 5 gdbus call --session --dest org.kde.KWin --object-path /KWin \
    --method org.kde.KWin.supportInformation >"$XDG_RUNTIME_DIR/kwin-support.txt"
if grep -q 'Compositing Type: QPainter' "$XDG_RUNTIME_DIR/kwin-support.txt"; then
    echo 'Capture requires an OpenGL compositor; QPainter is not a working fallback.' >&2; exit 1
fi
kwin_pid=$(pgrep -P "$wrapper_pid" -x kwin_wayland)
readarray -d '' -t kwin_args <"/proc/$kwin_pid/cmdline"
compat_display= compat_authority=
for ((i=0; i+1<${#kwin_args[@]}; ++i)); do
    case "${kwin_args[i]}" in
        --xwayland-display) compat_display=${kwin_args[i+1]};;
        --xwayland-xauthority) compat_authority=${kwin_args[i+1]};;
    esac
done
[[ $compat_display =~ ^:[0-9]+$ && $compat_authority == "$XDG_RUNTIME_DIR/"* && -f $compat_authority ]]
env DISPLAY="$compat_display" XAUTHORITY="$compat_authority" WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland \
    startplasma-wayland >"$XDG_RUNTIME_DIR/plasma.log" 2>&1 &
plasma_pid=$!
ready=false
for attempt in {1..200}; do
    kill -0 "$plasma_pid"
    if timeout 2 gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.NameHasOwner org.kde.plasmashell | grep -q true; then ready=true; break; fi
    sleep 0.1
done
[[ $ready == true ]]
env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland timeout 10 kscreen-doctor -j >"$XDG_RUNTIME_DIR/outputs.json"
jq -e --argjson w "$width" --argjson h "$height" '.outputs | length == 1 and .[0].enabled and .[0].size.width == $w and .[0].size.height == $h' "$XDG_RUNTIME_DIR/outputs.json"
timeout 5 pw-dump >"$XDG_RUNTIME_DIR/graph.json"
jq -e '[.[] | select(.type == "PipeWire:Interface:Device")] | length == 0' "$XDG_RUNTIME_DIR/graph.json"
echo 'Private Plasma desktop running; capture readiness still requires authenticated worker frames.'
# Capture may be absent while a broker restarts. Keep Plasma, and start a fresh
# worker when its authenticated endpoint returns. Never restart the desktop.
while kill -0 "$plasma_pid" 2>/dev/null; do
    kill -0 "$wrapper_pid" && kill -0 "$graph_pid" && kill -0 "$policy_pid"
    if [[ -n $worker_pid ]] && ! kill -0 "$worker_pid" 2>/dev/null; then
        wait "$worker_pid" || true
        worker_pid=
    fi
    if [[ -z $worker_pid && -S $XDG_RUNTIME_DIR/worker.sock ]]; then
        env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland "$worker" --virtual-session "$session" --uid "$(id -u)" \
            --socket "$XDG_RUNTIME_DIR/worker.sock" --token-fd 0 --desktop-media \
            <"$XDG_RUNTIME_DIR/worker-token" >>"$XDG_RUNTIME_DIR/worker.log" 2>&1 &
        worker_pid=$!
    fi
    sleep 2
done
wait "$plasma_pid"
