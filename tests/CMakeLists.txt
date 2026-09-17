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

add_executable(test_operation_log unit/test_operation_log.cpp)
target_link_libraries(test_operation_log PRIVATE storage gtest_main)

add_executable(test_namespace unit/test_namespace.cpp)
target_link_libraries(test_namespace PRIVATE master gtest_main)

add_executable(test_network_server unit/test_network_server.cpp)
target_link_libraries(test_network_server PRIVATE network gtest_main)

add_executable(test_storage_node unit/test_storage_node.cpp)
target_link_libraries(test_storage_node PRIVATE storage master gtest_main)

add_executable(test_master unit/test_master.cpp)
target_link_libraries(test_master PRIVATE master gtest_main)

add_executable(test_storage_registry unit/test_storage_registry.cpp)
target_link_libraries(test_storage_registry PRIVATE master gtest_main)

add_executable(test_replication_manager unit/test_replication_manager.cpp)
target_link_libraries(test_replication_manager PRIVATE master gtest_main)

add_executable(test_failure_detector unit/test_failure_detector.cpp)
target_link_libraries(test_failure_detector PRIVATE master gtest_main)

add_executable(test_metadata_store unit/test_metadata_store.cpp)
target_link_libraries(test_metadata_store PRIVATE master gtest_main)

add_executable(test_cache unit/test_cache.cpp)
target_link_libraries(test_cache PRIVATE client gtest_main)

add_executable(test_statistics unit/test_statistics.cpp)
target_link_libraries(test_statistics PRIVATE statistics gtest_main)

add_executable(test_create integration/test_create.cpp)
target_link_libraries(test_create PRIVATE client master gtest_main)

add_executable(test_write integration/test_write.cpp)
target_link_libraries(test_write PRIVATE client master storage gtest_main)

add_executable(test_read integration/test_read.cpp)
target_link_libraries(test_read PRIVATE client master storage gtest_main)

add_executable(test_append integration/test_append.cpp)
target_link_libraries(test_append PRIVATE client master storage gtest_main)

add_executable(test_delete integration/test_delete.cpp)
target_link_libraries(test_delete PRIVATE client master storage gtest_main)

add_executable(test_list_stat integration/test_list_stat.cpp)
target_link_libraries(test_list_stat PRIVATE client master storage gtest_main)

add_executable(test_recovery integration/test_recovery.cpp)
target_link_libraries(test_recovery PRIVATE client master storage gtest_main)

# Faza 15: concurrency stress tests (design notes §11.41-43), exercised
# directly against BlockStorage rather than through the network stack.
add_executable(test_concurrent_reads concurrency/test_concurrent_reads.cpp)
target_link_libraries(test_concurrent_reads PRIVATE storage gtest_main)

add_executable(test_concurrent_writes concurrency/test_concurrent_writes.cpp)
target_link_libraries(test_concurrent_writes PRIVATE storage gtest_main)

add_executable(test_concurrent_append concurrency/test_concurrent_append.cpp)
target_link_libraries(test_concurrent_append PRIVATE storage gtest_main)

# Faza 16: real-OS-process failure/recovery/restart tests (design notes
# §16.3-16.5) — spawn the actual dfs_master/dfs_storage/dfs_client/
# dfs_statsctl binaries rather than in-process C++ objects. binary_paths.h
# is generated at CMake generate-time (needs $<TARGET_FILE:...> generator
# expressions, which plain configure_file() cannot resolve) so the harness
# can find its sibling binaries regardless of the test binary's CWD.
set(dfs_test_binary_paths_header "${CMAKE_CURRENT_BINARY_DIR}/binary_paths.h")
file(GENERATE OUTPUT "${dfs_test_binary_paths_header}" CONTENT
"#pragma once
inline constexpr const char* kDfsMasterPath = \"$<TARGET_FILE:dfs_master>\";
inline constexpr const char* kDfsStoragePath = \"$<TARGET_FILE:dfs_storage>\";
inline constexpr const char* kDfsClientPath = \"$<TARGET_FILE:dfs_client>\";
inline constexpr const char* kDfsStatsctlPath = \"$<TARGET_FILE:dfs_statsctl>\";
")

add_library(failure_test_support failure/process_cluster.cpp)
target_link_libraries(failure_test_support PUBLIC network)
target_include_directories(failure_test_support PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
# PUBLIC (not just usable from this same directory via quote-include):
# Faza 17.3's bench/ targets link this library from a sibling directory
# and need to #include "process_cluster.h" too (see docs/design.md §21 —
# this harness was extended for benchmark reuse in Faza 17.1, not
# relocated to a separate module, since a CMake target is reusable from
# anywhere regardless of which directory its sources physically live in).
target_include_directories(failure_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/failure)
add_dependencies(failure_test_support dfs_master dfs_storage dfs_client dfs_statsctl)

add_executable(test_restart failure/test_restart.cpp)
target_link_libraries(test_restart PRIVATE failure_test_support gtest_main)

add_executable(test_storage_failure failure/test_storage_failure.cpp)
target_link_libraries(test_storage_failure PRIVATE failure_test_support gtest_main)

add_executable(test_failure_recovery failure/test_recovery.cpp)
target_link_libraries(test_failure_recovery PRIVATE failure_test_support gtest_main)

# Faza 17.1: replication_factor runtime config knob, verified end to end
# via the same real-process harness (see docs/design.md §21).
add_executable(test_replication_factor_config failure/test_replication_factor_config.cpp)
target_link_libraries(test_replication_factor_config PRIVATE failure_test_support gtest_main)

include(GoogleTest)
gtest_discover_tests(test_common_types)
gtest_discover_tests(test_protocol)
gtest_discover_tests(test_serialization)
gtest_discover_tests(test_block_storage)
gtest_discover_tests(test_block_manager)
gtest_discover_tests(test_operation_log)
gtest_discover_tests(test_namespace)
gtest_discover_tests(test_network_server)
gtest_discover_tests(test_storage_node)
gtest_discover_tests(test_master)
gtest_discover_tests(test_storage_registry)
gtest_discover_tests(test_replication_manager)
gtest_discover_tests(test_failure_detector)
gtest_discover_tests(test_metadata_store)
gtest_discover_tests(test_cache)
gtest_discover_tests(test_statistics)
gtest_discover_tests(test_create)
gtest_discover_tests(test_write)
gtest_discover_tests(test_read)
gtest_discover_tests(test_append)
gtest_discover_tests(test_delete)
gtest_discover_tests(test_list_stat)
gtest_discover_tests(test_recovery)
gtest_discover_tests(test_concurrent_reads)
gtest_discover_tests(test_concurrent_writes)
gtest_discover_tests(test_concurrent_append)
gtest_discover_tests(test_restart)
gtest_discover_tests(test_storage_failure)
gtest_discover_tests(test_failure_recovery)
gtest_discover_tests(test_replication_factor_config)
