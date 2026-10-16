#!/usr/bin/env bash
# OPT-075: the launcher's system-bus setup, with a FAKE xdg-dbus-proxy (a python script). Nothing here talks to a real bus,
# starts the real proxy or opens a device. Checks: the bwrap arguments per mode, the proxy command line (rules after
# --filter), degradation (proxy missing, socket missing, proxy never ready), grammar refusal, and that the proxy stops
# when the shell's read end of the --fd pipe is closed (the lifetime guarantee: no orphaned helper).
# Usage: VirtualSystemBusLauncherTest.sh <launch-virtual-session.sh> <python3>
set -euo pipefail
launcher=$1 python=$2
scratch=$(mktemp -d)
cleanup() { jobs -p | xargs -r kill 2>/dev/null || true; [[ -f $scratch/fake.pid ]] && kill "$(cat "$scratch/fake.pid")" 2>/dev/null || true; rm -rf -- "${scratch:?}"; }
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

# Fake proxy: records argv and its pid, binds the listening socket, writes the ready byte, then exits when the reader of
# fd 1 goes away (POLLERR on the pipe's write end), exactly like the real --fd contract.
cat >"$scratch/fake-proxy" <<PY
#!$python -I
import os, select, socket, sys
args = sys.argv[1:]
open(os.environ["FAKE_ARGV"], "w").write("\n".join(args) + "\n")
open(os.environ["FAKE_PID"], "w").write(str(os.getpid()))
if os.environ.get("FAKE_MODE") == "silent":
    select.select([], [], [], 30); sys.exit(0)
listener = socket.socket(socket.AF_UNIX)
listener.bind(args[2])
os.write(1, b"x")
poll = select.poll(); poll.register(1, 0)
while True:
    if any(event & (select.POLLERR | select.POLLHUP) for _, event in poll.poll(200)): break
open(os.environ["FAKE_GONE"], "w").write("gone")
PY
chmod 700 "$scratch/fake-proxy"
mkdir -p "$scratch/rt"
"$python" -I - "$scratch/real.sock" <<'PY' &
import socket, sys, time
s = socket.socket(socket.AF_UNIX); s.bind(sys.argv[1]); time.sleep(20)
PY
sleep 0.5
export FAKE_ARGV=$scratch/fake.argv FAKE_PID=$scratch/fake.pid FAKE_GONE=$scratch/fake.gone

# shellcheck source=/dev/null
source "$launcher"
system_bus_real_socket=$scratch/real.sock
system_bus_proxy=$scratch/fake-proxy
runtime=$scratch/rt

# none: nothing.
system_bus=none; farside_system_bus_setup "$runtime"
[[ ${#system_bus_bwrap[@]} == 0 && $system_bus == none ]] || fail 'none must add no bwrap arguments'

# filtered: ro-bind of the PROXY socket, never the real one; env set; proxy argv is socket pair, --filter, rules.
system_bus=filtered; polkit_agent=1; cups_socket=0
system_bus_rules=(--see=org.freedesktop.UPower '--call=org.freedesktop.UPower=org.freedesktop.DBus.Properties.Get@/org/freedesktop/UPower/*' --talk=org.bluez)
farside_system_bus_setup "$runtime"
[[ $system_bus == filtered ]] || fail "filtered degraded unexpectedly"
[[ ${system_bus_bwrap[0]} == --ro-bind && ${system_bus_bwrap[1]} == "$runtime/system_bus_socket" && ${system_bus_bwrap[2]} == /run/dbus/system_bus_socket ]] || fail "bind arguments: ${system_bus_bwrap[*]}"
[[ " ${system_bus_bwrap[*]} " == *" --setenv FARSIDE_POLKIT_AGENT 1 "* && " ${system_bus_bwrap[*]} " == *" --setenv FARSIDE_SYSTEM_BUS filtered "* ]] || fail "environment arguments"
[[ " ${system_bus_bwrap[*]} " != *"$system_bus_real_socket"* ]] || fail 'the real socket must not reach the sandbox in filtered mode'
expected=$'--fd=1\nunix:path='"$system_bus_real_socket"$'\n'"$runtime/system_bus_socket"$'\n--filter\n--see=org.freedesktop.UPower\n--call=org.freedesktop.UPower=org.freedesktop.DBus.Properties.Get@/org/freedesktop/UPower/*\n--talk=org.bluez'
[[ $(cat "$FAKE_ARGV") == "$expected" ]] || fail "proxy argv: $(cat "$FAKE_ARGV")"
pid=$(cat "$FAKE_PID")
kill -0 "$pid" || fail 'proxy is not running'
# Lifetime: closing the shell's read end (as the end of the desktop does) stops the proxy.
exec {system_bus_proxy_fd}<&-
for _ in {1..40}; do [[ -f $FAKE_GONE ]] && break; sleep 0.1; done
[[ -f $FAKE_GONE ]] || fail 'the proxy outlived the closing of its --fd'
for _ in {1..20}; do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
! kill -0 "$pid" 2>/dev/null || fail 'the proxy process is still alive'

# full: the real socket read-only at the standard path, no proxy, no rules.
rm -f "$FAKE_ARGV" "$FAKE_GONE"
system_bus=full; system_bus_rules=(); polkit_agent=0
farside_system_bus_setup "$runtime"
[[ ${system_bus_bwrap[1]} == "$system_bus_real_socket" && ${system_bus_bwrap[2]} == /run/dbus/system_bus_socket ]] || fail 'full must bind the real socket'
[[ ! -e $FAKE_ARGV ]] || fail 'full must not start the proxy'

# Degradation: proxy missing, real socket missing, proxy never ready. Each ends as none with a log line, not an error.
system_bus=filtered; system_bus_proxy=$scratch/absent
system_bus=filtered; system_bus_proxy=$scratch/absent; farside_system_bus_setup "$runtime" 2>"$scratch/log"
[[ $system_bus == none && ${#system_bus_bwrap[@]} == 0 ]] || fail 'missing proxy must degrade to none'
grep -q 'xdg-dbus-proxy is not installed' "$scratch/log" || fail 'missing proxy needs a log line'
system_bus=filtered; system_bus_proxy=$scratch/fake-proxy; system_bus_real_socket=$scratch/nonexistent
farside_system_bus_setup "$runtime" 2>"$scratch/log"
[[ $system_bus == none ]] && grep -q 'does not exist' "$scratch/log" || fail 'missing system socket must degrade to none'
system_bus_real_socket=$scratch/real.sock; system_bus=filtered; system_bus_ready_timeout=1
FAKE_MODE=silent farside_system_bus_setup "$runtime" 2>"$scratch/log"
[[ $system_bus == none ]] && grep -q 'did not become ready' "$scratch/log" || fail 'a proxy that never signals ready must degrade to none'
kill "$(cat "$FAKE_PID")" 2>/dev/null || true
system_bus_ready_timeout=5

# Grammar: an argument that could add an option or smuggle a second one is an error and starts nothing.
for bad in '--filter' '--own=org.example.X' '--talk=org.x y' '--talk=org.x;rm' '--call=org.x=' '--see=' $'--talk=org.x\n--own=y' '--fd=3' 'unix:path=/x' '--talk=a$b'; do
    system_bus=filtered; system_bus_rules=("$bad"); rm -f "$FAKE_ARGV"
    if farside_system_bus_setup "$runtime" 2>/dev/null; then fail "accepted a bad rule: $bad"; fi
    [[ ! -e $FAKE_ARGV ]] || fail "a bad rule started the proxy: $bad"
done
system_bus=bogus
! farside_system_bus_setup "$runtime" 2>/dev/null || fail 'an unknown mode must be refused'
echo 'VirtualSystemBusLauncherTest passed'
