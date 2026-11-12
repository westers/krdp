#!/usr/bin/env bash
# REVIEW-P1: configure a SEPARATE build directory with AddressSanitizer + UBSan for the autotests.
# It only runs `cmake` (configure), never a build, and refuses to touch the repo's normal build/ tree.
#
# Usage:   scripts/configure-sanitizers.sh [--dry-run] [--sanitizers 'address;undefined'] BUILD_DIR
#          then:  cmake --build BUILD_DIR -j16 && ctest --test-dir BUILD_DIR -R '<test>' --output-on-failure
# Example: scripts/configure-sanitizers.sh ~/dev/krdp/.work/build-asan     (.work/ and build*/ are git-ignored)
#
# Environment (same names as scripts/build-kpipewire.sh):
#   KRDP_KPIPEWIRE_PREFIX  private KPipeWire prefix, default $HOME/dev/krdp/.deps/kpipewire. When
#                          KPipeWireConfig.cmake exists under it, KPipeWire_DIR/CMAKE_PREFIX_PATH point there
#                          (as build-kpipewire.sh does); otherwise the system KPipeWire is used. The private
#                          KPipeWire itself is NOT sanitizer-built: only krdp code and its tests are.
#   BUILD_TYPE             default Debug.
#
# How the flags get in: CMakeLists.txt does include(ECMEnableSanitizers); ECM reads the cache variable
# ECM_ENABLE_SANITIZERS (a CMake list) and adds -fsanitize=... -fno-omit-frame-pointer to every target.
#
# Running the sanitized tests (set these in the shell that runs ctest):
#   export ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1:detect_stack_use_after_return=1:check_initialization_order=1
#   export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
#   export LSAN_OPTIONS=suppressions=$PWD/lsan.supp:print_suppressions=0      # optional
# Suppressions: put third-party leaks (Qt, fontconfig, libfreerdp one-time allocations) in an lsan.supp file
# outside the repo (one `leak:<function-or-library>` line each) and name only what you have verified is not
# krdp code; never suppress a krdp symbol to get a green run. UBSan: `UBSAN_OPTIONS=suppressions=...` takes
# `<check>:<file-pattern>` lines (e.g. `alignment:libQt6Core`).
# Notes: tests that link with -Wl,--wrap=... still work under ASan. Do NOT run the PipeWire/KWin/D-Bus
# daemon tests on Hal (hard project rule); run the sanitized suite on Sol.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
dry_run=0
sanitizers='address;undefined'
build_dir=''
while (($# > 0)); do
    case "$1" in
    --dry-run) dry_run=1 ;;
    --sanitizers)
        sanitizers="${2:?--sanitizers needs a value}"
        shift
        ;;
    -h | --help)
        sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    -*)
        echo "unknown option: $1" >&2
        exit 2
        ;;
    *)
        [[ -z "${build_dir}" ]] || { echo "only one BUILD_DIR allowed" >&2; exit 2; }
        build_dir="$1"
        ;;
    esac
    shift
done
[[ -n "${build_dir}" ]] || { echo "usage: $0 [--dry-run] [--sanitizers LIST] BUILD_DIR" >&2; exit 2; }

target=$(realpath -m -- "${build_dir}")
for protected in "${here}/build" "${HOME}/dev/krdp/build" "${here}" "${HOME}/dev/krdp"; do
    if [[ "${target}" == "$(realpath -m -- "${protected}")" ]]; then
        echo "refusing: ${target} is the repo's own tree or normal build directory; pick a separate path" >&2
        exit 1
    fi
done

prefix="${KRDP_KPIPEWIRE_PREFIX:-${HOME}/dev/krdp/.deps/kpipewire}"
kpw_args=()
kpw_note="system KPipeWire (no KPipeWireConfig.cmake under ${prefix})"
if [[ -d "${prefix}" ]]; then
    config_file="$(find "${prefix}" -name KPipeWireConfig.cmake 2>/dev/null | head -1 || true)"
    if [[ -n "${config_file}" ]]; then
        kpw_args=("-DCMAKE_PREFIX_PATH=${prefix}" "-DKPipeWire_DIR=$(dirname "${config_file}")")
        kpw_note="private KPipeWire from ${prefix}"
    fi
fi

cmd=(cmake -S "${here}" -B "${target}"
    "-DCMAKE_BUILD_TYPE=${BUILD_TYPE:-Debug}"
    -DBUILD_TESTING=ON
    "-DECM_ENABLE_SANITIZERS=${sanitizers}"
    "${kpw_args[@]}")

echo "# build dir: ${target}"
echo "# KPipeWire: ${kpw_note}"
printf '%q ' "${cmd[@]}"
echo
if ((dry_run)); then
    echo "# --dry-run: nothing was configured"
    exit 0
fi
"${cmd[@]}"
echo "# configured. Next: cmake --build ${target@Q} -j16"
