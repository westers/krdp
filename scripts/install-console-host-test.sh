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

test -x "$task_build/bin/krdp-console-host"
test -x "$task_build/bin/krdp-console-worker"
test -f "$task_prefix/cert/krdp.crt"
test -f "$task_prefix/cert/krdp.key"

cmake --install "$task_build" --prefix "$task_prefix"
install -Dm644 "$task_root/server/org.kde.krdpconsoleworker-test.desktop" /usr/share/applications/org.kde.krdpconsoleworker.desktop
install -Dm644 "$task_root/server/krdp-console-host-test.service" /etc/systemd/system/krdp-console-host-test.service
systemctl daemon-reload
systemctl restart krdp-console-host-test.service
systemctl --no-pager --full status krdp-console-host-test.service
