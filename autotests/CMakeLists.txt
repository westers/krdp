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
target_include_directories(OutputSnapshotTest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(OutputSnapshotTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME OutputSnapshotTest COMMAND OutputSnapshotTest)

add_executable(TakeoverDetectorTest TakeoverDetectorTest.cpp)
target_include_directories(TakeoverDetectorTest PRIVATE ${CMAKE_SOURCE_DIR}/server)
target_link_libraries(TakeoverDetectorTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME TakeoverDetectorTest COMMAND TakeoverDetectorTest)

add_executable(ClipboardTextTest ClipboardTextTest.cpp)
target_include_directories(ClipboardTextTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ClipboardTextTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME ClipboardTextTest COMMAND ClipboardTextTest)
