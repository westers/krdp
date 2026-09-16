#!/usr/bin/env bash
# Exit 0 iff krdpserver (and the harness) resolve every libKPipeWire* to the private prefix.
set -euo pipefail
prefix="${KRDP_KPIPEWIRE_PREFIX:-$HOME/dev/krdp/.deps/kpipewire}"
status=0
for bin in "$HOME/dev/krdp/build/bin/krdpserver" "$HOME/dev/krdp/build/bin/krdpplasmastreamer"; do
    [ -x "$bin" ] || { echo "missing: $bin"; status=1; continue; }
    while read -r line; do
        case "$line" in
            *"$prefix"*) ;;
            *) echo "NOT PRIVATE: $bin -> $line"; status=1 ;;
        esac
    done < <(ldd "$bin" | grep -E 'libKPipeWire(Record|DmaBuf)?\.so' || true)
done
[ $status -eq 0 ] && echo "OK: all KPipeWire libraries resolve to $prefix"
exit $status
