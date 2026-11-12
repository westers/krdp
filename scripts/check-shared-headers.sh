#!/usr/bin/env bash
# REVIEW-P1: drift check for files that are hand-copied between this repo (krdp, the server) and the
# client repo (krdp-client). Read-only: it only reads both trees and prints a unified diff of the
# NORMALISED files for every pair that differs.
#
# Usage:  scripts/check-shared-headers.sh [path-to-krdp-client]
#   client repo path: first argument, else $KRDP_CLIENT_SRC, else ../krdp-client relative to this repo root
#   (from a worktree under .work/ pass the path or set KRDP_CLIENT_SRC).
# Exit:   0 every listed pair is equal after normalisation, 1 at least one pair drifts, 77 client repo absent
#         (CTest treats 77 as a skip when the test sets SKIP_RETURN_CODE 77).
#
# Normalisation applied to BOTH sides before diffing (see docs/shared-headers.md):
#   - the namespace root `KRdp` / `KrdpClient` becomes `@NS@`;
#   - quoted includes lose the client's directory prefix (`core/`, `wayland/`), because the client keeps
#     the same files in subdirectories of src/;
#   - lines that match the pair's allow-list regex (4th column) are dropped. This is for the "Copy of ..."
#     provenance comments and single reworded doc lines; keep every regex as narrow as possible.
#   - trailing whitespace and blank lines at the end of the file are ignored.
# Pairs that are intentionally different implementations are NOT listed here (CodecPolicy.h,
# CodecSelection.h, DeskActivity.h, HostFiles.h vs ClipboardLocalFiles.h): see docs/shared-headers.md.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
client="${1:-${KRDP_CLIENT_SRC:-${here}/../krdp-client}}"
if [[ ! -d "${client}/src" ]]; then
    echo "SKIP: client repo not found at ${client}; nothing compared" >&2
    exit 77
fi

# server path (rel. to this repo) | client path (rel. to the client repo) | allow-list ERE (may be empty)
# The allow-list matches the provenance comment the client adds to each copy.
copy_note='^[[:space:]]*(//|/?\*).*([Cc]opy of|a copy of) .*(krdp|server)'
pairs=$(cat <<EOF
src/ClipboardFileDescriptor.h|src/core/ClipboardFileDescriptor.h|${copy_note}
src/ClipboardFileLimits.h|src/core/ClipboardFileLimits.h|${copy_note}
src/ClipboardFileNames.h|src/core/ClipboardFileNames.h|${copy_note}
src/ClipboardFilePacing.h|src/core/ClipboardFilePacing.h|${copy_note}
src/ClipboardFileRequest.h|src/core/ClipboardFileRequest.h|${copy_note}
src/ClipboardFileServe.h|src/core/ClipboardFileServe.h|${copy_note}
src/ClipboardFileStaging.h|src/core/ClipboardFileStaging.h|${copy_note}
src/ClipboardHtml.h|src/core/ClipboardHtml.h|${copy_note}
src/ClipboardImage.h|src/core/ClipboardImage.h|${copy_note}|^ \* OPT-072: image conversion for the clipboard bridge, as pure functions|^ \* server's src/ClipboardImage\.h \(krdp repo\)
server/PngMimeData.h|src/core/PngMimeData.h|${copy_note}|OPT-072: a clipboard offer that is one PNG
server/SelectionReaderFd.h|src/core/SelectionReaderFd.h|${copy_note}
server/SelectionReader.h|src/wayland/SelectionReader.h|${copy_note}
server/SelectionReader.cpp|src/wayland/SelectionReader.cpp|${copy_note}
protocols/ext-data-control-v1.xml|protocols/ext-data-control-v1.xml|
server/kwin/protocols/pointer-constraints-unstable-v1.xml|protocols/pointer-constraints-unstable-v1.xml|
EOF
)

normalise() { # $1 file, $2 allow-list regex (may be empty)
    local re="$2"
    sed -E \
        -e 's/\bKrdpClient\b/@NS@/g; s/\bKRdp\b/@NS@/g' \
        -e 's|^(#[[:space:]]*include[[:space:]]+")(core/\|wayland/)|\1|' \
        -e 's/[[:space:]]+$//' "$1" |
        { if [[ -n "${re}" ]]; then grep -Ev -- "${re}" || true; else cat; fi; } |
        sed -e :a -e '/^\n*$/{$d;N;ba' -e '}'
}

checked=0
drift=0
missing=0
while IFS='|' read -r mine_rel other_rel allow; do
    [[ -n "${mine_rel}" ]] || continue
    mine="${here}/${mine_rel}"
    other="${client}/${other_rel}"
    if [[ ! -f "${mine}" || ! -f "${other}" ]]; then
        echo "MISSING: ${mine_rel} (${mine}) or ${other_rel} (${other}) does not exist" >&2
        missing=$((missing + 1))
        continue
    fi
    checked=$((checked + 1))
    if out=$(diff -u --label "krdp:${mine_rel}" --label "client:${other_rel}" \
        <(normalise "${mine}" "${allow}") <(normalise "${other}" "${allow}")); then
        echo "OK:    ${mine_rel} == ${other_rel}"
    else
        drift=$((drift + 1))
        echo "DRIFT: ${mine_rel} != ${other_rel} ($(grep -c '^[+-][^+-]' <<<"${out}") changed lines)"
        echo "${out}"
    fi
done <<<"${pairs}"

echo "checked ${checked} pair(s), ${drift} drifting, ${missing} missing"
if ((missing > 0)); then
    # A listed file that vanished means the table is stale: fail loudly rather than pass silently.
    exit 1
fi
((drift == 0)) || exit 1
