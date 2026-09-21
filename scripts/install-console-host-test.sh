#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Steve Westers
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    echo "Run as root: sudo $0"
    exit 1
fi

task_root=$(cd "$(dirname "$0")/.." && pwd)
task_build="$task_root/build"
task_prefix=/opt/krdp-console
task_kpipewire_libdir=$(find "$task_root/.deps/kpipewire/lib" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | head -n 1)

test -x "$task_build/bin/krdp-console-host"
test -x "$task_build/bin/krdp-console-worker"
test -f "$task_prefix/cert/krdp.crt"
test -f "$task_prefix/cert/krdp.key"
test -n "$task_kpipewire_libdir"

# Stop before replacing mapped libraries; in-place replacement can crash a
# running host during teardown.
if systemctl is-active --quiet krdp-console-host-test.service; then
    systemctl stop krdp-console-host-test.service
fi
cmake --install "$task_build" --prefix "$task_prefix"
# KRDP is linked against the staged KPipeWire build rather than the distro ABI.
install -d "$task_prefix/lib/$task_kpipewire_libdir"
cp -a "$task_root/.deps/kpipewire/lib/$task_kpipewire_libdir"/libKPipeWire*.so* "$task_prefix/lib/$task_kpipewire_libdir/"
install -Dm644 "$task_root/server/org.kde.krdpconsoleworker-test.desktop" /usr/share/applications/org.kde.krdpconsoleworker.desktop
install -Dm644 "$task_root/server/krdp-console-host-test.service" /etc/systemd/system/krdp-console-host-test.service
systemctl daemon-reload
systemctl restart krdp-console-host-test.service
systemctl --no-pager --full status krdp-console-host-test.service
