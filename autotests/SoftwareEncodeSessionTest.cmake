# SPDX-FileCopyrightText: 2026 Steve Westers
# SPDX-License-Identifier: BSD-2-Clause
#
# AUD-FIX2 F1/F5: software H.264 without VA-API and no per-session fd/thread
# leak; WS-E: software HEVC/AV1 as KRdp configures them. End to end against a
# private PipeWire daemon. Skips (77) without one.
add_executable(SoftwareEncodeSessionTest ${CMAKE_CURRENT_LIST_DIR}/SoftwareEncodeSessionTest.cpp)
target_link_libraries(SoftwareEncodeSessionTest PRIVATE
    Qt6::Test Qt6::Gui K::KPipeWire K::KPipeWireRecord PkgConfig::PipeWire PkgConfig::AVCodec)
find_program(PIPEWIRE_EXECUTABLE pipewire)
if(PIPEWIRE_EXECUTABLE)
    set(_krdp_swenc_pipewire "${PIPEWIRE_EXECUTABLE}")
else()
    set(_krdp_swenc_pipewire "pipewire") # not found: the test skips
endif()
target_compile_definitions(SoftwareEncodeSessionTest PRIVATE KRDP_PIPEWIRE_EXECUTABLE="${_krdp_swenc_pipewire}")
# EncoderSelection.h (header-only) configures the streams the way KRdp's session does.
target_include_directories(SoftwareEncodeSessionTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(SoftwareEncodeSessionTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
add_test(NAME SoftwareEncodeSessionTest COMMAND SoftwareEncodeSessionTest)
set_tests_properties(SoftwareEncodeSessionTest PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
