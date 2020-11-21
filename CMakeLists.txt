project(coro-cloudstorage CXX)
cmake_minimum_required(VERSION 3.17)

set(BUILD_SHARED_LIBS OFF)

add_subdirectory(contrib)
add_subdirectory(src)
add_subdirectory(examples)
