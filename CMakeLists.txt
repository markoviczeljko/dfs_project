cmake_minimum_required(VERSION 3.20)
project(dfs_project CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_library(common INTERFACE)
target_include_directories(common INTERFACE include)

add_library(network
    src/network/socket.cpp
    src/network/protocol.cpp
    src/network/connection.cpp
)
target_include_directories(network PUBLIC include)
target_link_libraries(network PUBLIC common Threads::Threads)

enable_testing()
add_subdirectory(tests)
