#!/usr/bin/env bash
# Isolated graph admission only; this does not claim app routing/PCM delivery.
set -euo pipefail
config_dir=$(cd "$1" && pwd)
runtime_parent="/run/user/$(id -u)"
if [[ ! -d "$runtime_parent" || ! -O "$runtime_parent" ]]; then
    echo 'No owned logind runtime directory; skipping graph probe'
    exit 77
fi
test_runtime=$(mktemp -d "$runtime_parent/krdp-audio-test.XXXXXX")
graph_pid=
cleanup() {
    if [[ -n "$graph_pid" ]]; then
        kill "$graph_pid" 2>/dev/null || true
        wait "$graph_pid" 2>/dev/null || true
    fi
    # Keep diagnostic logs and graph snapshot for inspection; never recursively
    # delete a runtime directory from a test failure path.
    echo "Audio graph evidence: $test_runtime"
}
trap cleanup EXIT
env XDG_RUNTIME_DIR="$test_runtime" PIPEWIRE_RUNTIME_DIR="$test_runtime" \
    PIPEWIRE_CONFIG_DIR="$config_dir" PIPEWIRE_CONFIG_NAME=virtual-session-pipewire.conf \
    pipewire >"$test_runtime/daemon.log" 2>&1 &
graph_pid=$!
for attempt in {1..50}; do
    kill -0 "$graph_pid"
    [[ -S "$test_runtime/pipewire-0" ]] && break
    sleep 0.1
done
[[ -S "$test_runtime/pipewire-0" ]]
env XDG_RUNTIME_DIR="$test_runtime" PIPEWIRE_RUNTIME_DIR="$test_runtime" \
    PIPEWIRE_REMOTE=pipewire-0 timeout 5 pw-dump >"$test_runtime/graph.json"
jq -e '
  ([.[] | select(.type == "PipeWire:Interface:Node") | .info.props
      | select(."media.class" == "Audio/Sink") | ."node.name"] == ["krdp.virtual-session.audio"])
  and ([.[] | select(.type == "PipeWire:Interface:Device")] | length == 0)
  and ([.[] | .info.props? // {} | select(has("api.alsa.path") or has("api.bluez5.address"))] | length == 0)
' "$test_runtime/graph.json"
echo 'Private graph has one virtual audio sink and no hardware devices'
