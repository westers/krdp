#!/usr/bin/env bash
# REVIEW-S4: the desktop script's worker restart backoff and log cap. Only the
# marked helper block is extracted and run; the script itself needs the
# guardian's namespace and must never run here.
# Usage: VirtualSessionDesktopSupervisionTest.sh <scripts/virtual-session-desktop.sh>
set -euo pipefail
script=${1:?path to virtual-session-desktop.sh}
work=$(mktemp -d)
trap 'rm -rf "${work:?}"' EXIT

sed -n '/^# BEGIN supervision helpers/,/^# END supervision helpers/p' "$script" >"$work/helpers.sh"
[[ -s $work/helpers.sh ]] || { echo "FAIL: helper block missing"; exit 1; }
# shellcheck disable=SC1091
source "$work/helpers.sh"

fail=0
check() { # description expected actual
    if [[ $2 != "$3" ]]; then echo "FAIL: $1: expected $2, got $3"; fail=1; else echo "ok: $1"; fi
}

# --- restart backoff: 2, 4, 8, 16, 32, 60, 60 seconds for a worker that keeps crashing
SECONDS=100
worker_started=100
delays=()
for _ in 1 2 3 4 5 6 7; do
    note_worker_exit            # worker lived 0 s
    delays+=($((worker_next - SECONDS)))
    SECONDS=$worker_next
    worker_started=$SECONDS
done
check "crash loop delays" "2 4 8 16 32 60 60" "${delays[*]}"

# --- a worker that ran 59 s does not reset, one that ran 60 s does
SECONDS=1000
worker_started=941
note_worker_exit
check "59 s run keeps the grown delay" "60" "$((worker_next - SECONDS))"
SECONDS=1100
worker_started=1040
note_worker_exit
check "60 s run resets to 2 s" "2" "$((worker_next - SECONDS))"
check "next delay after reset" "4" "$worker_backoff"

# --- log cap: over the cap -> .1 generation + truncated live log; under -> untouched
logs=$work/logs
mkdir "$logs"
log_cap=4096
head -c 5000 /dev/zero | tr '\0' a >"$logs/worker.log"
head -c 100 /dev/zero | tr '\0' b >"$logs/kwin.log"
ln -s "$logs/kwin.log" "$logs/plasma.log"
exec 9>>"$logs/worker.log"       # a writer that keeps its descriptor, like the worker
cap_logs "$logs"
check "oversize log truncated" "0" "$(stat -c %s "$logs/worker.log")"
check "previous generation kept" "5000" "$(stat -c %s "$logs/worker.log.1")"
check "small log untouched" "100" "$(stat -c %s "$logs/kwin.log")"
[[ ! -e $logs/kwin.log.1 && ! -e $logs/plasma.log.1 ]] && echo "ok: no rotation below the cap, symlink ignored" \
    || { echo "FAIL: unexpected .1 file"; fail=1; }
printf 'after' >&9
check "writer continues after truncation" "5" "$(stat -c %s "$logs/worker.log")"
exec 9>&-
# a second rotation replaces the single .1 generation
head -c 6000 /dev/zero | tr '\0' c >>"$logs/worker.log"
cap_logs "$logs"
check "only one .1 generation" "6005" "$(stat -c %s "$logs/worker.log.1")"
[[ ! -e $logs/worker.log.2 ]] && echo "ok: no .2 generation" || { echo "FAIL: .2 exists"; fail=1; }
# missing logs are fine under set -e
rm -f "$logs"/*.log
cap_logs "$logs"
echo "ok: missing logs tolerated"
exit "$fail"
