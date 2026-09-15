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
)
target_include_directories(master PUBLIC include)
target_link_libraries(master PUBLIC common Threads::Threads)

add_executable(dfs_storage src/storage/main_storage.cpp)
target_link_libraries(dfs_storage PRIVATE storage)

enable_testing()
add_subdirectory(tests)
