#!/usr/bin/env bash
# OPT-055 K5.1: compare the LogThrottle/LogEdge/FARSIDE_LOG_THROTTLED logic of krdp's src/LogThrottle.h
# with KPipeWire's src/logthrottle_p.h. Compared: everything from "class LogThrottle" to the end, minus
# the krdp namespace-closing brace, blank lines and whitespace. Exit 0 identical, 1 drift, 77 other copy missing.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
mine="${here}/src/LogThrottle.h"
other="${1:-${KPIPEWIRE_SRC:-$HOME/dev/kpipewire}/src/logthrottle_p.h}"
if [[ ! -f "${other}" ]]; then
    echo "SKIP: ${other} does not exist yet (WP2 creates it); nothing compared" >&2
    exit 77
fi
core() {
    sed -n '/^class LogThrottle/,$p' "$1" | grep -v '^}$' | sed -E 's/[[:space:]]+\\$/ \\/; s/[[:space:]]+/ /g; s/^ //; s/ $//' | grep -v '^$'
}
if diff -u <(core "${other}") <(core "${mine}"); then
    echo "OK: logthrottle logic matches ${other}"
else
    echo "DRIFT: ${mine} and ${other} differ" >&2
    exit 1
fi
