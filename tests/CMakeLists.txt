include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(test_common_types unit/test_common_types.cpp)
target_link_libraries(test_common_types PRIVATE common gtest_main)

add_executable(test_protocol unit/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE network gtest_main)

add_executable(test_serialization unit/test_serialization.cpp)
target_link_libraries(test_serialization PRIVATE network gtest_main)

include(GoogleTest)
gtest_discover_tests(test_common_types)
gtest_discover_tests(test_protocol)
gtest_discover_tests(test_serialization)
