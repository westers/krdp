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
    probe_runtime=$(mktemp -d "/run/user/$(id -u)/krdp-headless.XXXXXX")
    mkdir "$probe_runtime/config" "$probe_runtime/cache" "$probe_runtime/state" "$probe_runtime/data"
    echo "Headless probe evidence: $probe_runtime"
    # Retain the real HOME identity, but no existing display, session bus, Qt
    # reconnect, session id, manager notification, or inherited plugin settings.
    exec env -i PATH=/usr/bin:/bin HOME="$HOME" USER="$(id -un)" LOGNAME="$(id -un)" \
        LANG=C.UTF-8 XDG_RUNTIME_DIR="$probe_runtime" XDG_CONFIG_HOME="$probe_runtime/config" \
        XDG_CACHE_HOME="$probe_runtime/cache" XDG_STATE_HOME="$probe_runtime/state" \
        XDG_DATA_HOME="$probe_runtime/data" XDG_DATA_DIRS=/usr/local/share:/usr/share \
        XDG_SESSION_TYPE=wayland LIBGL_ALWAYS_SOFTWARE=1 \
        dbus-run-session --config-file="$repo_path/server/virtual-session-bus.conf" \
        -- bash "$script_path" --inside-private-bus
fi
[[ "$XDG_RUNTIME_DIR" == /run/user/"$(id -u)"/krdp-headless.* ]]
wrapper_pid=
graph_pid=
cleanup() {
    if [[ -n "$wrapper_pid" ]]; then
        # KWinWrapper's destructor terminates/waits for its own KWin child.
        kill "$wrapper_pid" 2>/dev/null || true
        wait "$wrapper_pid" 2>/dev/null || true
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
kwin_wayland_wrapper --virtual --width 1280 --height 720 --output-count 1 \
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
env WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland \
    timeout 10 kscreen-doctor -j >"$XDG_RUNTIME_DIR/outputs.json"
jq -e '.outputs | length == 1' "$XDG_RUNTIME_DIR/outputs.json"
jq -e '.outputs[0] | .enabled == true and .size.width == 1280 and .size.height == 720' "$XDG_RUNTIME_DIR/outputs.json"
timeout 5 pw-dump >"$XDG_RUNTIME_DIR/graph.json"
jq -e '[.[] | select(.type == "PipeWire:Interface:Device")] | length == 0' "$XDG_RUNTIME_DIR/graph.json"
gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
    --method org.freedesktop.DBus.ListNames
echo 'Private Wayland compositor ready at 1280x720; stopping probe only'
