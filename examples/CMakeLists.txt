# SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
# SPDX-License-Identifier: BSD-2-Clause

add_subdirectory(streamer)
add_subdirectory(krdpctl-probe)

if(BUILD_PLASMA_SESSION)
    add_subdirectory(plasmastreamer)
endif()
