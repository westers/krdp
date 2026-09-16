#!/usr/bin/env bash
# Exit 0 iff krdpplasmastreamer still builds and links against the SYSTEM
# KPipeWire (i.e. the private KPipeWire in .deps/kpipewire is optional, not
# a hard requirement for the tree to build).
set -euo pipefail
krdp="${KRDP_SRC:-$HOME/dev/krdp}"
jobs="${JOBS:-16}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

cmake -S "$krdp" -B "$tmp" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=OFF \
    -DKPipeWire_DIR=/usr/lib/x86_64-linux-gnu/cmake/KPipeWire
cmake --build "$tmp" -j"$jobs" --target krdpplasmastreamer

bin="$tmp/bin/krdpplasmastreamer"
if [ ! -x "$bin" ]; then
    echo "FAIL: $bin not built"
    exit 1
fi

if ldd "$bin" | grep libKPipeWire.so | grep -q '/usr/lib/x86_64-linux-gnu/'; then
    echo "OK: krdpplasmastreamer links the system KPipeWire"
    exit 0
fi

echo "FAIL: krdpplasmastreamer does not link /usr/lib/x86_64-linux-gnu/libKPipeWire.so"
ldd "$bin" | grep libKPipeWire.so || true
exit 1
