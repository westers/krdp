# SPDX-FileCopyrightText: 2023 Arjen Hiemstra <ahiemstra@heimr.nl>
# SPDX-License-Identifier: BSD-2-Clause

add_executable(krdpplasmastreamer)

target_sources(krdpplasmastreamer PRIVATE main.cpp)

target_link_libraries(krdpplasmastreamer Qt${QT_MAJOR_VERSION}::Gui KRdp)
