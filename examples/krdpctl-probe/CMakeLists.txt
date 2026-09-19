# SPDX-FileCopyrightText: 2026 Steve Westers
# SPDX-License-Identifier: BSD-2-Clause

# A libfreerdp *client* that speaks the KRDPCTL channel (OPT-044); needs the
# client half of FreeRDP, which the server build otherwise never looks for.
find_package(FreeRDP-Client 3.1 QUIET)
if(NOT FreeRDP-Client_FOUND)
    message(STATUS "FreeRDP-Client not found; not building krdpctl-probe")
    return()
endif()

add_executable(krdpctl-probe)

# LayoutControl.cpp compiled in rather than libKRdp linked (the same way
# autotests/LayoutControlTest does it): the probe then needs nothing but Qt
# Core and the FreeRDP client libraries, so the binary built here also runs
# on a machine whose libfreerdp can decode H.264 (buzz), which hal9000's
# cannot - and that is where --gfx is of any use.
target_sources(krdpctl-probe PRIVATE main.cpp ${CMAKE_SOURCE_DIR}/src/LayoutControl.cpp)

target_include_directories(krdpctl-probe PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/src
    ${FreeRDP_INCLUDE_DIR}
    ${WinPR_INCLUDE_DIR}
    ${FreeRDP_Client_INCLUDE_DIR}
)

target_link_libraries(krdpctl-probe PRIVATE Qt${QT_MAJOR_VERSION}::Core freerdp-client freerdp winpr)
