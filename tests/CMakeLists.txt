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

add_executable(test_block_storage unit/test_block_storage.cpp)
target_link_libraries(test_block_storage PRIVATE storage gtest_main)

add_executable(test_block_manager unit/test_block_manager.cpp)
target_link_libraries(test_block_manager PRIVATE storage gtest_main)

add_executable(test_namespace unit/test_namespace.cpp)
target_link_libraries(test_namespace PRIVATE master gtest_main)

add_executable(test_network_server unit/test_network_server.cpp)
target_link_libraries(test_network_server PRIVATE network gtest_main)

add_executable(test_storage_node unit/test_storage_node.cpp)
target_link_libraries(test_storage_node PRIVATE storage gtest_main)

add_executable(test_master unit/test_master.cpp)
target_link_libraries(test_master PRIVATE master gtest_main)

add_executable(test_create integration/test_create.cpp)
target_link_libraries(test_create PRIVATE client master gtest_main)

add_executable(test_write integration/test_write.cpp)
target_link_libraries(test_write PRIVATE client master storage gtest_main)

add_executable(test_read integration/test_read.cpp)
target_link_libraries(test_read PRIVATE client master storage gtest_main)

include(GoogleTest)
gtest_discover_tests(test_common_types)
gtest_discover_tests(test_protocol)
gtest_discover_tests(test_serialization)
gtest_discover_tests(test_block_storage)
gtest_discover_tests(test_block_manager)
gtest_discover_tests(test_namespace)
gtest_discover_tests(test_network_server)
gtest_discover_tests(test_storage_node)
gtest_discover_tests(test_master)
gtest_discover_tests(test_create)
gtest_discover_tests(test_write)
gtest_discover_tests(test_read)
