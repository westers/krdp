add_executable(AdaptiveQualityTest AdaptiveQualityTest.cpp)
target_include_directories(AdaptiveQualityTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(AdaptiveQualityTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME AdaptiveQualityTest COMMAND AdaptiveQualityTest)

add_executable(SurfaceLayoutTest SurfaceLayoutTest.cpp)
target_include_directories(SurfaceLayoutTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(SurfaceLayoutTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME SurfaceLayoutTest COMMAND SurfaceLayoutTest)

add_executable(MultiLayoutTest MultiLayoutTest.cpp)
target_include_directories(MultiLayoutTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(MultiLayoutTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME MultiLayoutTest COMMAND MultiLayoutTest)

add_executable(FrameQueuePolicyTest FrameQueuePolicyTest.cpp)
target_include_directories(FrameQueuePolicyTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(FrameQueuePolicyTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME FrameQueuePolicyTest COMMAND FrameQueuePolicyTest)

add_executable(ClientDisplayInfoTest ClientDisplayInfoTest.cpp)
target_include_directories(ClientDisplayInfoTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ClientDisplayInfoTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME ClientDisplayInfoTest COMMAND ClientDisplayInfoTest)

add_executable(OutputSnapshotTest OutputSnapshotTest.cpp)
# ${CMAKE_BINARY_DIR}/src for krdp_export.h: OutputSnapshot.h now includes LayoutControl.h
# (hostMonitorsFrom); header-only use, nothing from libKRdp is linked.
target_include_directories(OutputSnapshotTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server ${CMAKE_BINARY_DIR}/src)
target_link_libraries(OutputSnapshotTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME OutputSnapshotTest COMMAND OutputSnapshotTest)

add_executable(TakeoverDetectorTest TakeoverDetectorTest.cpp)
target_include_directories(TakeoverDetectorTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(TakeoverDetectorTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME TakeoverDetectorTest COMMAND TakeoverDetectorTest)

# The physical-console host reads logind asynchronously, but its adapter
# selection is deliberately pure: test the greeter -> seat0-user handoff
# without requiring a display manager in the test environment.
add_executable(ConsoleSeatTest ConsoleSeatTest.cpp)
target_include_directories(ConsoleSeatTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleSeatTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME ConsoleSeatTest COMMAND ConsoleSeatTest)

# The future persistent console broker uses this state machine to revoke the
# outgoing adapter before an SDDM/Plasma worker can be replaced.
add_executable(ConsoleHandoffTest ConsoleHandoffTest.cpp)
target_include_directories(ConsoleHandoffTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(ConsoleHandoffTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME ConsoleHandoffTest COMMAND ConsoleHandoffTest)

add_executable(PipeWireAudioRoutingTest PipeWireAudioRoutingTest.cpp)
target_include_directories(PipeWireAudioRoutingTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(PipeWireAudioRoutingTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME PipeWireAudioRoutingTest COMMAND PipeWireAudioRoutingTest)

add_executable(ClipboardTextTest ClipboardTextTest.cpp)
target_include_directories(ClipboardTextTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ClipboardTextTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME ClipboardTextTest COMMAND ClipboardTextTest)

# LayoutControl has an out-of-line .cpp (codec + planner), unlike its
# header-only neighbours above, so it is compiled straight into the test
# binary rather than linked through the KRdp library (which would drag in
# FreeRDP/WinPR/PAM for a pure-logic test).
add_executable(LayoutControlTest LayoutControlTest.cpp ${CMAKE_SOURCE_DIR}/src/LayoutControl.cpp)
target_include_directories(LayoutControlTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/src)
target_link_libraries(LayoutControlTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME LayoutControlTest COMMAND LayoutControlTest)

# LayoutOwner is header-only (server/); ${CMAKE_BINARY_DIR}/src for krdp_export.h via LayoutControl.h.
add_executable(LayoutOwnerTest LayoutOwnerTest.cpp)
target_include_directories(LayoutOwnerTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server ${CMAKE_BINARY_DIR}/src)
target_link_libraries(LayoutOwnerTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME LayoutOwnerTest COMMAND LayoutOwnerTest)

# LayoutSessionDiff is header-only (server/): the per-connection session diff a live KRDPCTL apply runs.
add_executable(LayoutSessionDiffTest LayoutSessionDiffTest.cpp)
target_include_directories(LayoutSessionDiffTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(LayoutSessionDiffTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME LayoutSessionDiffTest COMMAND LayoutSessionDiffTest)

# VideoCodecSupport.h uses the RDPGFX enums: FreeRDP headers only, nothing linked.
# FreeRDP/WinPR dirs are SYSTEM (matching src/CMakeLists.txt's -isystem for the
# same headers): plain PRIVATE include dirs surfaced -Wpedantic/-Wdeprecated-declarations
# warnings from wtypes.h/sspi.h that the main library targets do not see.
add_executable(VideoCodecSupportTest VideoCodecSupportTest.cpp)
target_include_directories(VideoCodecSupportTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(VideoCodecSupportTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(VideoCodecSupportTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME VideoCodecSupportTest COMMAND VideoCodecSupportTest)

# GfxSurfaceCommand.h uses the RDPGFX bitmap stream structs (FreeRDP headers only, nothing
# linked) plus QSize, so it needs Qt6::Gui in addition to VideoCodecSupportTest's set above.
add_executable(GfxSurfaceCommandTest GfxSurfaceCommandTest.cpp)
target_include_directories(GfxSurfaceCommandTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(GfxSurfaceCommandTest SYSTEM PRIVATE ${FreeRDP_INCLUDE_DIR} ${WinPR_INCLUDE_DIR})
target_link_libraries(GfxSurfaceCommandTest PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
add_test(NAME GfxSurfaceCommandTest COMMAND GfxSurfaceCommandTest)
