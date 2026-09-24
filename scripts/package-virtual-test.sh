#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Steve Westers
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

# Build a non-activating, prefix-isolated paired virtual host/worker deb.
# Never install this over a running broker: the next worker would use a new
# protocol while the old broker stays mapped. Stop/install/start is user-run.
set -euo pipefail
task_root=$(cd "$(dirname "$0")/.." && pwd)
task_build="$task_root/build-virtual-package"
task_arch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
task_private="$task_root/.deps/kpipewire/lib/$task_arch"
task_revision=$(git -C "$task_root" rev-parse --short=7 HEAD)
task_prefix=/opt/krdp-virtual-service-test
task_libdir="$task_prefix/lib/$task_arch"
task_runtime=${KRDP_VIRTUAL_RUNTIME_LIBDIR:-$task_private}

[[ -d "$task_private" && -f "$task_private/libKPipeWire.so.6" ]]
# Sol's isolated virtual service can carry an independently accepted private
# KPipeWireRecord (live x264 CRF). Preserve that exact installed library in a
# paired update instead of silently replacing it with the older build prefix.
[[ "$task_runtime" == "$task_private" || "$task_runtime" == "$task_libdir" ]]
for name in KPipeWire KPipeWireDmaBuf KPipeWireRecord; do
    [[ -f "$task_runtime/lib$name.so.6" ]]
done
cmake -S "$task_root" -B "$task_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$task_prefix" \
    -DCMAKE_INSTALL_RPATH="$task_libdir" \
    -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
    -DKRDP_BUILD_CONSOLE_TEST_PACKAGE=OFF \
    -DKRDP_BUILD_VIRTUAL_TEST_PACKAGE=ON \
    -DKRDP_VIRTUAL_PACKAGE_REVISION="$task_revision" \
    -DKPipeWire_DIR="$task_private/cmake/KPipeWire" \
    -DKRDP_PRIVATE_KPIPEWIRE_LIBDIR="$task_runtime"
cmake --build "$task_build" --target krdp-virtual-host krdp-virtual-session-entry \
    krdp-virtual-pam-keeper krdp-virtual-session-cleanup krdp-virtual-device-entry \
    krdp-virtual-guardian krdp-virtual-guardianctl krdp-console-worker KRdp -j8
cpack -G DEB --config "$task_build/CPackConfig.cmake" -B "$task_build"
task_basename=$(sed -n 's/^set(CPACK_PACKAGE_FILE_NAME "\([^"]*\)")/\1/p' "$task_build/CPackConfig.cmake")
[[ "$task_basename" == krdp-virtual-test-*-Linux ]]
task_package="$task_build/$task_basename-KRdpVirtualSessionTest.deb"
[[ -f "$task_package" ]]
[[ $(dpkg-deb -f "$task_package" Package) == krdp-virtual-test ]]
[[ $(dpkg-deb -f "$task_package" Version) == *"-git.$task_revision" ]]
task_contents=$(dpkg-deb -c "$task_package")
for name in krdp-virtual-host krdp-virtual-session-entry krdp-virtual-pam-keeper \
    krdp-virtual-session-cleanup krdp-virtual-device-entry krdp-virtual-guardian \
    krdp-virtual-guardianctl krdp-console-worker; do
    [[ "$task_contents" == *"./opt/krdp-virtual-service-test/bin/$name"* ]]
done
for name in KRdp KPipeWire KPipeWireDmaBuf KPipeWireRecord; do
    [[ "$task_contents" == *"./opt/krdp-virtual-service-test/lib/$task_arch/lib$name.so.6"* ]]
done
[[ "$task_contents" == *"./opt/krdp-virtual-service-test/share/krdp/virtual-session/drafts/krdp-virtual-host.service"* ]]
[[ "$task_contents" == *"./opt/krdp-virtual-service-test/share/krdp/virtual-session/drafts/krdp-virtual-session@.service"* ]]
[[ "$task_contents" != *'/etc/systemd/'* && "$task_contents" != *'/usr/share/applications/'* ]]
echo "Paired non-activating virtual test deb: $task_package"
