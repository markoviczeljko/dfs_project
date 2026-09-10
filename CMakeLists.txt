cmake_minimum_required(VERSION 3.20)
project(dfs_project CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(common INTERFACE)
target_include_directories(common INTERFACE include)

enable_testing()
add_subdirectory(tests)
