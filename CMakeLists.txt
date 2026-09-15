cmake_minimum_required(VERSION 3.20)
project(dfs_project CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

add_library(storage
    src/storage/block_storage.cpp
    src/storage/block_manager.cpp
    src/storage/storage_node.cpp
)
target_include_directories(storage PUBLIC include)
target_link_libraries(storage PUBLIC common network Threads::Threads)

add_library(master
    src/master/namespace.cpp
    src/master/storage_registry.cpp
    src/master/replication_manager.cpp
    src/master/master.cpp
)
target_include_directories(master PUBLIC include)
target_link_libraries(master PUBLIC common network Threads::Threads)

add_library(client
    src/client/client.cpp
)
target_include_directories(client PUBLIC include)
target_link_libraries(client PUBLIC common network Threads::Threads)

add_executable(dfs_storage src/storage/main_storage.cpp)
target_link_libraries(dfs_storage PRIVATE storage)

add_executable(dfs_master src/master/main_master.cpp)
target_link_libraries(dfs_master PRIVATE master)

add_executable(dfs_client src/client/main_client.cpp)
target_link_libraries(dfs_client PRIVATE client)

enable_testing()
add_subdirectory(tests)
