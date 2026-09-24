# SPDX-FileCopyrightText: 2026 Steve Westers
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

# Isolated physical-Console acceptance package, deliberately not a system
# service package. Host and worker use the same private wire protocol and must
# be shipped together. No postinst or automatic activation is provided.
set(CPACK_PACKAGE_NAME "krdp-console-test")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Steve Westers <amiga1.2k@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Isolated KRDP physical-console test host and worker")
set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/krdp-console")
set(CPACK_GENERATOR "DEB")
set(CPACK_COMPONENTS_ALL KRdpConsoleTest)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_KRDPCONSOLETEST_PACKAGE_NAME "krdp-console-test")
set(CPACK_DEBIAN_PACKAGE_RELEASE "git.${KRDP_CONSOLE_PACKAGE_REVISION}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS
    "${CMAKE_BINARY_DIR}/bin;${KRDP_PRIVATE_KPIPEWIRE_LIBDIR}")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libkscreen-bin, kwin-wayland, pipewire-bin")
include(CPack)
