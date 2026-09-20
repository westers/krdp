# SPDX-FileCopyrightText: 2026 Steve Westers
# SPDX-License-Identifier: BSD-2-Clause
# Decodes a krdpplasmastreamer --codec run the way a FreeRDP client does (one H.264 decoder for
# the main and aux pictures, libfreerdp's YUV420CombineToYUV444 + YUV444ToRGB primitives) and
# scores it against the KPIPEWIRE_DUMP_RGBA reference. libavcodec stands in for the H.264 decoder
# hal9000's libfreerdp does not ship (WITH_GFX_H264=OFF); the primitives are always built.
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCodec IMPORTED_TARGET libavcodec libavutil)
if(NOT AVCodec_FOUND)
    message(STATUS "libavcodec not found; not building krdpavc444probe")
    return()
endif()
add_executable(krdpavc444probe main.cpp)
# SYSTEM: matches autotests/CMakeLists.txt's treatment of the same FreeRDP/WinPR headers, which
# avoids -Wpedantic/-Wdeprecated-declarations warnings from wtypes.h/sspi.h that a plain PRIVATE
# include dir surfaces but the main library targets (which use -isystem for these) do not see.
target_include_directories(krdpavc444probe SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(krdpavc444probe PRIVATE Qt${QT_MAJOR_VERSION}::Gui PkgConfig::AVCodec freerdp winpr)
