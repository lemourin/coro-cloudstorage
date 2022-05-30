project(coro-cloudstorage CXX C)
cmake_minimum_required(VERSION 3.17)

option(BUILD_SHARED_LIBS "build shared libs" OFF)
option(USE_BUNDLED_DEPENDENCIES "use bundled dependencies" ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/contrib/coro-http/cmake)

add_subdirectory(contrib)
add_subdirectory(src)
add_subdirectory(examples)
