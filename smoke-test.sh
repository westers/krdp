#!/usr/bin/env bash
set -euo pipefail

run_build=1
run_restart=1
run_watch=1
watch_seconds="${WATCH_SECONDS:-120}"

usage() {
    cat <<'EOF'
Usage: ./smoke-test.sh [options]

Options:
  --no-build      Skip cmake build
  --no-restart    Skip service restart
  --no-watch      Skip log follow
  --watch-seconds N
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

if command -v rg >/dev/null 2>&1; then
    filter_cmd=(rg -i)
else
    filter_cmd=(grep -Ei)
fi

log_pattern='Using PipeWire H264 encoder profile|Started Plasma session|Closing session|Listening for connections|Cannot create children|sequence gap|desynchronized|No matching damage metadata|queue overflow|Failed receiving filtered frame|Filter queue is full|ERRINFO|Broken pipe|fake input'

if (( run_build )); then
    cmake --build build -j"$(nproc)"
fi

if (( run_restart )); then
    systemctl --user restart plasma-xdg-desktop-portal-kde app-org.kde.krdpserver
fi

echo
echo "Recent KRDP log summary:"
journalctl --user -n 60 -o cat -u app-org.kde.krdpserver -u plasma-xdg-desktop-portal-kde | "${filter_cmd[@]}" "${log_pattern}" || true

if (( run_watch )); then
    echo
    echo "Watching logs for ${watch_seconds}s..."
    timeout "${watch_seconds}"s \
        journalctl --user -f -o cat -u app-org.kde.krdpserver -u plasma-xdg-desktop-portal-kde \
        | "${filter_cmd[@]}" "${log_pattern}" || true
fi
