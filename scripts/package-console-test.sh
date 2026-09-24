#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Steve Westers
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

# Build one non-activating deb with the paired Console host/worker and private
# KRdp/KPipeWire libraries. This never installs or restarts a service.
set -euo pipefail
task_root=$(cd "$(dirname "$0")/.." && pwd)
task_build="$task_root/build-console-package"
task_arch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
task_private="$task_root/.deps/kpipewire/lib/$task_arch"
task_revision=$(git -C "$task_root" rev-parse --short=7 HEAD)
task_prefix=/opt/krdp-console
task_libdir="$task_prefix/lib/$task_arch"

[[ -d "$task_private" && -f "$task_private/libKPipeWire.so.6" ]]
cmake -S "$task_root" -B "$task_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$task_prefix" \
    -DCMAKE_INSTALL_RPATH="$task_libdir" \
    -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
    -DKRDP_BUILD_CONSOLE_TEST_PACKAGE=ON \
    -DKRDP_CONSOLE_PACKAGE_REVISION="$task_revision" \
    -DKPipeWire_DIR="$task_private/cmake/KPipeWire" \
    -DKRDP_PRIVATE_KPIPEWIRE_LIBDIR="$task_private"
cmake --build "$task_build" --target krdp-console-host krdp-console-worker KRdp -j"$(nproc)"
cpack -G DEB --config "$task_build/CPackConfig.cmake" -B "$task_build"
task_basename=$(sed -n 's/^set(CPACK_PACKAGE_FILE_NAME "\([^"]*\)")/\1/p' "$task_build/CPackConfig.cmake")
[[ "$task_basename" == krdp-console-test-*-Linux ]]
task_package="$task_build/$task_basename-KRdpConsoleTest.deb"
[[ -f "$task_package" ]]
[[ $(dpkg-deb -f "$task_package" Package) == krdp-console-test ]]
[[ $(dpkg-deb -f "$task_package" Version) == *"-git.$task_revision" ]]
task_contents=$(dpkg-deb -c "$task_package")
for name in krdp-console-host krdp-console-worker; do
    [[ "$task_contents" == *"./opt/krdp-console/bin/$name"* ]]
done
for name in KRdp KPipeWire KPipeWireDmaBuf KPipeWireRecord; do
    [[ "$task_contents" == *"./opt/krdp-console/lib/$task_arch/lib$name.so.6"* ]]
done
[[ "$task_contents" != *'/krdp-virtual-host'* ]]
[[ "$task_contents" != *'/etc/systemd/'* ]]
echo "Paired non-activating Console test deb: $task_package"
