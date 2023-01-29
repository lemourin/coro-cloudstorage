cmake_minimum_required(VERSION 3.23)

project(coro-cloudstorage CXX C)

option(BUILD_SHARED_LIBS "build shared libs" OFF)
option(BUILD_EXAMPLES "build examples" ON)

find_package(coro-http REQUIRED)
list(APPEND CMAKE_MODULE_PATH ${coro-http_DIR})

find_package(nlohmann_json 3.9.1 REQUIRED)
find_package(fmt 8.0.0 REQUIRED)
find_package(pugixml 1.11 REQUIRED)
find_package(Libevent 2.1.12 REQUIRED)

include(FindPkgConfig)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(avcodec REQUIRED libavcodec>=58.134.100 IMPORTED_TARGET)
    pkg_check_modules(avformat REQUIRED libavformat>=58.76.100 IMPORTED_TARGET)
    pkg_check_modules(avutil REQUIRED libavutil>=56.70.100 IMPORTED_TARGET)
    pkg_check_modules(avfilter REQUIRED libavfilter>=7.110.100 IMPORTED_TARGET)
    pkg_check_modules(swscale REQUIRED libswscale>=5.9.100 IMPORTED_TARGET)
    pkg_check_modules(swresample REQUIRED libswresample>=3.9.100 IMPORTED_TARGET)
    add_library(FFMPEG::avcodec ALIAS PkgConfig::avcodec)
    add_library(FFMPEG::avformat ALIAS PkgConfig::avformat)
    add_library(FFMPEG::avutil ALIAS PkgConfig::avutil)
    add_library(FFMPEG::avfilter ALIAS PkgConfig::avfilter)
    add_library(FFMPEG::swscale ALIAS PkgConfig::swscale)
    add_library(FFMPEG::swresample ALIAS PkgConfig::swresample)
else()
    find_package(FFMPEG 4.4 REQUIRED)
endif()

find_path(CRYPTOPP_INCLUDE cryptopp/config.h REQUIRED)
find_library(CRYPTOPP_LIBRARY cryptopp cryptopp-static REQUIRED)
add_library(cryptopp::cryptopp UNKNOWN IMPORTED)
set_target_properties(cryptopp::cryptopp
    PROPERTIES
        IMPORTED_LOCATION "${CRYPTOPP_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CRYPTOPP_INCLUDE}"
)

add_subdirectory(src)
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
