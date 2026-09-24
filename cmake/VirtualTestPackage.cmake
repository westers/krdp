# SPDX-FileCopyrightText: 2026 Steve Westers
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

# Paired, non-activating retained-desktop test package. Draft units remain
# under /opt; no post-install hook, system unit, or PAM policy is installed.
set(CPACK_PACKAGE_NAME "krdp-virtual-test")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "Steve Westers <amiga1.2k@gmail.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Isolated KRDP retained-desktop host and worker")
set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/krdp-virtual-service-test")
set(CPACK_GENERATOR "DEB")
set(CPACK_COMPONENTS_ALL KRdpVirtualSessionTest)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_KRDPVIRTUALSESSIONTEST_PACKAGE_NAME "krdp-virtual-test")
set(CPACK_DEBIAN_PACKAGE_RELEASE "git.${KRDP_VIRTUAL_PACKAGE_REVISION}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS
    "${CMAKE_BINARY_DIR}/bin;${KRDP_PRIVATE_KPIPEWIRE_LIBDIR}")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libkscreen-bin, kwin-wayland, pipewire-bin")
include(CPack)
