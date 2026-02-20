#!/usr/bin/env bash
set -euo pipefail

run_build=1
run_restart=1
run_watch=1
watch_seconds="${WATCH_SECONDS:-120}"
assert_encoder="${ASSERT_ENCODER:-}"

usage() {
    cat <<'EOF'
Usage: ./smoke-test.sh [options]

Options:
  --no-build      Skip cmake build
  --no-restart    Skip service restart
  --no-watch      Skip log follow
  --watch-seconds N
  --assert-encoder MODE
                 Assert encoder path from logs (MODE: vaapi|software)
  -h, --help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build) run_build=0 ;;
        --no-restart) run_restart=0 ;;
        --no-watch) run_watch=0 ;;
        --watch-seconds)
            shift
            watch_seconds="${1:-}"
            ;;
        --assert-encoder)
            shift
            assert_encoder="${1:-}"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 2
            ;;
    esac
    shift
done

assert_encoder="${assert_encoder,,}"
case "${assert_encoder}" in
    ""|vaapi|software) ;;
    *)
        echo "Invalid --assert-encoder value: ${assert_encoder}. Use vaapi or software." >&2
        exit 2
        ;;
esac

if command -v rg >/dev/null 2>&1; then
    filter_cmd=(rg -i)
else
    filter_cmd=(grep -Ei)
fi

log_pattern='KRDP startup summary|Using PipeWire H264 encoder profile|Started Plasma session|Closing session|Listening for connections|Cannot create children|sequence gap|desynchronized|No matching damage metadata|queue overflow|Failed receiving filtered frame|Filter queue is full|ERRINFO|Broken pipe|fake input|kpipewire_vaapi_logging|libx264|KRDP_FORCE_VAAPI_DRIVER|KRDP_AUTO_VAAPI_DRIVER|PipeWire encoder initialization failed|software fallback'
log_file="$(mktemp)"
trap 'rm -f "${log_file}"' EXIT

if (( run_build )); then
    cmake --build build -j"$(nproc)"
fi

if (( run_restart )); then
    systemctl --user restart plasma-xdg-desktop-portal-kde app-org.kde.krdpserver
fi

journalctl --user -n 120 -o cat -u app-org.kde.krdpserver -u plasma-xdg-desktop-portal-kde > "${log_file}"

echo
echo "Recent KRDP log summary:"
"${filter_cmd[@]}" "${log_pattern}" "${log_file}" || true

if (( run_watch )); then
    echo
    echo "Watching logs for ${watch_seconds}s..."
    timeout "${watch_seconds}"s \
        journalctl --user -f -o cat -u app-org.kde.krdpserver -u plasma-xdg-desktop-portal-kde \
        | tee -a "${log_file}" \
        | "${filter_cmd[@]}" "${log_pattern}" || true
fi

if [[ -n "${assert_encoder}" ]]; then
    case "${assert_encoder}" in
        vaapi)
            assert_pattern='kpipewire_vaapi_logging: VAAPI: .* in use for device'
            ;;
        software)
            assert_pattern='Forcing encoder to "libx264"|\[libx264 @'
            ;;
    esac

    if "${filter_cmd[@]}" "${assert_pattern}" "${log_file}" >/dev/null; then
        echo
        echo "Encoder assertion passed: ${assert_encoder}"
    else
        echo
        echo "Encoder assertion failed: expected ${assert_encoder} markers in KRDP logs. Connect an RDP client during --watch to collect encoder activity." >&2
        exit 1
    fi
fi
