#!/usr/bin/env bash
# Build the patched KPipeWire (~/dev/kpipewire, branch westers/opt-015) into a private prefix
# and relink KRDP against it. System libkpipewire6 is never touched.
set -euo pipefail
src="${KPIPEWIRE_SRC:-$HOME/dev/kpipewire}"
prefix="${KRDP_KPIPEWIRE_PREFIX:-$HOME/dev/krdp/.deps/kpipewire}"
krdp="${KRDP_SRC:-$HOME/dev/krdp}"
jobs="${JOBS:-16}"

cmake -S "$src" -B "$src/build" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_TESTING=OFF \
    -DKDE_INSTALL_USE_QT_SYS_PATHS=OFF
cmake --build "$src/build" -j"$jobs"
cmake --install "$src/build"

config_dir="$(dirname "$(find "$prefix" -name KPipeWireConfig.cmake | head -1)")"
[ -n "$config_dir" ] || { echo "KPipeWireConfig.cmake not found under $prefix"; exit 1; }

cmake -S "$krdp" -B "$krdp/build" \
    -DCMAKE_PREFIX_PATH="$prefix" \
    -DKPipeWire_DIR="$config_dir"
cmake --build "$krdp/build" -j"$jobs"
"$krdp/scripts/check-kpipewire-link.sh"
