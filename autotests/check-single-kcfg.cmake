# SPDX-FileCopyrightText: 2026 Steve Westers
# SPDX-License-Identifier: BSD-2-Clause
#
# AUD-K13: krdpserverrc has exactly one schema, server/krdpserversettings.kcfg,
# and it is a regular file (not a symlink into the KCM). The KCM builds its
# settings class from that file, so a second copy anywhere would drift.
if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "pass -DSOURCE_DIR=<krdp source tree>")
endif()
set(owner "${SOURCE_DIR}/server/krdpserversettings.kcfg")
if(NOT EXISTS "${owner}" OR IS_SYMLINK "${owner}" OR IS_DIRECTORY "${owner}")
    message(FATAL_ERROR "${owner} must be a regular file")
endif()
file(GLOB_RECURSE copies RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/src/*.kcfg" "${SOURCE_DIR}/server/*.kcfg")
list(REMOVE_ITEM copies "server/krdpserversettings.kcfg")
if(copies)
    message(FATAL_ERROR "extra kcfg files found (keep only server/krdpserversettings.kcfg): ${copies}")
endif()
file(READ "${SOURCE_DIR}/src/kcm/CMakeLists.txt" kcm_cmake)
string(FIND "${kcm_cmake}" "\${CMAKE_SOURCE_DIR}/server/krdpserversettings.kcfg" found)
if(found EQUAL -1)
    message(FATAL_ERROR "src/kcm/CMakeLists.txt does not generate its settings class from server/krdpserversettings.kcfg")
endif()
message(STATUS "single kcfg: ${owner}")
