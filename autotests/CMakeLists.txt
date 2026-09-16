add_executable(AdaptiveQualityTest AdaptiveQualityTest.cpp)
target_include_directories(AdaptiveQualityTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(AdaptiveQualityTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME AdaptiveQualityTest COMMAND AdaptiveQualityTest)

add_executable(SurfaceLayoutTest SurfaceLayoutTest.cpp)
target_include_directories(SurfaceLayoutTest PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(SurfaceLayoutTest PRIVATE Qt6::Test Qt6::Core)
add_test(NAME SurfaceLayoutTest COMMAND SurfaceLayoutTest)
