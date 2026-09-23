cmake_minimum_required(VERSION 3.20)
project(dfs_project CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to a Release build (optimized, NDEBUG) when the caller doesn't pick
# one explicitly, e.g. via `-DCMAKE_BUILD_TYPE=...` — matters for bench/, whose
# numbers are meaningless if silently compiled unoptimized.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

add_library(common INTERFACE)
target_include_directories(common INTERFACE include)

add_library(network
    src/network/socket.cpp
    src/network/protocol.cpp
    src/network/connection.cpp
    src/network/network_server.cpp
)
target_include_directories(network PUBLIC include)
target_link_libraries(network PUBLIC common Threads::Threads)

add_library(statistics
    src/statistics/histogram.cpp
    src/statistics/statistics.cpp
)
target_include_directories(statistics PUBLIC include)
target_link_libraries(statistics PUBLIC common Threads::Threads)

add_library(storage
    src/storage/block_storage.cpp
    src/storage/block_manager.cpp
    src/storage/operation_log.cpp
    src/storage/recovery.cpp
    src/storage/storage_node.cpp
)
target_include_directories(storage PUBLIC include)
target_link_libraries(storage PUBLIC common network statistics Threads::Threads)

add_library(master
    src/master/namespace.cpp
    src/master/storage_registry.cpp
    src/master/replication_manager.cpp
    src/master/failure_detector.cpp
    src/master/recovery_manager.cpp
    src/master/metadata_store.cpp
    src/master/master.cpp
)
target_include_directories(master PUBLIC include)
target_link_libraries(master PUBLIC common network statistics Threads::Threads)

add_library(client
    src/client/client.cpp
    src/client/cache.cpp
)
target_include_directories(client PUBLIC include)
target_link_libraries(client PUBLIC common network statistics Threads::Threads)

add_executable(dfs_storage src/storage/main_storage.cpp)
target_link_libraries(dfs_storage PRIVATE storage)

add_executable(dfs_master src/master/main_master.cpp)
target_link_libraries(dfs_master PRIVATE master)

add_executable(dfs_client src/client/main_client.cpp)
target_link_libraries(dfs_client PRIVATE client)

add_executable(dfs_statsctl src/statistics/main_statsctl.cpp)
target_link_libraries(dfs_statsctl PRIVATE network)

enable_testing()
add_subdirectory(tests)

# Faza 17: benchmark executables (see docs/design.md §23) — added after
# tests/ so bench/'s targets can link tests/failure/'s already-defined
# failure_test_support (Faza 16 real-process harness, reused rather than
# relocated per Faza 17.1's docs/design.md §21).
add_subdirectory(bench)
