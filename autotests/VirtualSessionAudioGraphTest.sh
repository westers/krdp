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
policy_pid=
record_pid=
cleanup() {
    for child in "$record_pid" "$policy_pid"; do
        if [[ -n "$child" ]]; then
            kill "$child" 2>/dev/null || true
            wait "$child" 2>/dev/null || true
        fi
    done
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
if [[ "${2:-}" == --pcm ]]; then
    # Caller supplies a disposable private D-Bus session, never the desktop bus.
    [[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ]]
    mkdir "$test_runtime/config" "$test_runtime/state" "$test_runtime/cache"
    private_env=(env XDG_RUNTIME_DIR="$test_runtime" PIPEWIRE_RUNTIME_DIR="$test_runtime"
        PIPEWIRE_REMOTE=pipewire-0 XDG_CONFIG_HOME="$test_runtime/config"
        XDG_STATE_HOME="$test_runtime/state" XDG_CACHE_HOME="$test_runtime/cache")
    "${private_env[@]}" WIREPLUMBER_CONFIG_DIR=/usr/share/wireplumber wireplumber --profile policy \
        >"$test_runtime/policy.log" 2>&1 &
    policy_pid=$!
    # Record the sink monitor, never a microphone or the host graph.
    "${private_env[@]}" timeout 8 pw-cat --record --raw --rate 48000 --channels 2 \
        --format s16 --target krdp.virtual-session.audio \
        --properties '{ stream.capture.sink = true }' "$test_runtime/received.raw" \
        >"$test_runtime/record.log" 2>&1 &
    record_pid=$!
    ffmpeg -nostdin -hide_banner -loglevel error -f lavfi \
        -i sine=frequency=997:sample_rate=48000:duration=2 -ac 2 "$test_runtime/tone.wav"
    "${private_env[@]}" timeout 6 pw-cat --playback --target krdp.virtual-session.audio \
        "$test_runtime/tone.wav"
    # timeout terminates capture after 8s; its 124 status is expected.
    wait "$record_pid" || [[ $? == 124 ]]
    record_pid=
    kill -0 "$policy_pid"
    ffmpeg -nostdin -hide_banner -f s16le -ar 48000 -ac 2 -i "$test_runtime/received.raw" \
        -af volumedetect -f null - 2>"$test_runtime/levels.log"
    # Integer PCM silence can report a finite floor (e.g. -91 dB), not -inf.
    if ! awk '/max_volume:/ { for (i = 1; i < NF; ++i) if ($i == "max_volume:")
        if ($(i+1) ~ /^-?[0-9]+([.][0-9]+)?$/ && $(i+1)+0 > -60) audible=1 }
        END { exit !audible }' "$test_runtime/levels.log"; then
        echo 'Private sink did not deliver non-silent PCM' >&2
        exit 1
    fi
    grep 'max_volume:' "$test_runtime/levels.log"
fi
env XDG_RUNTIME_DIR="$test_runtime" PIPEWIRE_RUNTIME_DIR="$test_runtime" \
    PIPEWIRE_REMOTE=pipewire-0 timeout 5 pw-dump >"$test_runtime/graph.json"
jq -e '
  ([.[] | select(.type == "PipeWire:Interface:Node") | .info.props
      | select(."media.class" == "Audio/Sink") | ."node.name"] == ["krdp.virtual-session.audio"])
  and ([.[] | select(.type == "PipeWire:Interface:Device")] | length == 0)
  and ([.[] | .info.props? // {} | select(has("api.alsa.path") or has("api.bluez5.address"))] | length == 0)
' "$test_runtime/graph.json"
echo 'Private graph has one virtual audio sink and no hardware devices'
