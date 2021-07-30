add_subdirectory(coro-http EXCLUDE_FROM_ALL)

include(FindPkgConfig)

if(BUILD_SHARED_LIBS)
    find_package(nlohmann_json 3.9.1 REQUIRED)
    set_target_properties(nlohmann_json::nlohmann_json PROPERTIES IMPORTED_GLOBAL TRUE)

    find_package(fmt 8.0.0 REQUIRED)
    set_target_properties(fmt::fmt PROPERTIES IMPORTED_GLOBAL TRUE)
    add_library(fmt ALIAS fmt::fmt)

    find_package(pugixml 1.11 REQUIRED)
    set_target_properties(pugixml::pugixml PROPERTIES IMPORTED_GLOBAL TRUE)

    pkg_check_modules(avcodec REQUIRED libavcodec>=58.134.100 IMPORTED_TARGET)
    pkg_check_modules(avformat REQUIRED libavformat>=58.76.100 IMPORTED_TARGET)
    pkg_check_modules(avutil REQUIRED libavutil>=56.70.100 IMPORTED_TARGET)
    pkg_check_modules(avfilter REQUIRED libavfilter>=7.110.100 IMPORTED_TARGET)
    pkg_check_modules(swscale REQUIRED libswscale>=5.9.100 IMPORTED_TARGET)
    pkg_check_modules(swresample REQUIRED libswresample>=3.9.100 IMPORTED_TARGET)
    add_library(avcodec ALIAS PkgConfig::avcodec)
    add_library(avformat ALIAS PkgConfig::avformat)
    add_library(avutil ALIAS PkgConfig::avutil)
    add_library(avfilter ALIAS PkgConfig::avfilter)
    add_library(swscale ALIAS PkgConfig::swscale)
    add_library(swresample ALIAS PkgConfig::swresample)

    find_path(CRYPTOPP_INCLUDE cryptopp/config.h REQUIRED)
    find_library(CRYPTOPP_LIBRARY cryptopp REQUIRED)
    add_library(cryptopp UNKNOWN IMPORTED GLOBAL)
    set_target_properties(cryptopp
        PROPERTIES
            IMPORTED_LOCATION "${CRYPTOPP_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${CRYPTOPP_INCLUDE}"
    )
else()
    add_subdirectory(json EXCLUDE_FROM_ALL)
    add_subdirectory(fmt EXCLUDE_FROM_ALL)
    add_subdirectory(pugixml EXCLUDE_FROM_ALL)

    set(ZLIB_COMPAT 1 CACHE BOOL "")
    add_subdirectory(zlib-ng EXCLUDE_FROM_ALL)

    set(FFMPEG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg")
    add_subdirectory(ffmpeg-config EXCLUDE_FROM_ALL)

    set(BUILD_SHARED 0 CACHE BOOL "")
    set(BUILD_STATIC 1 CACHE BOOL "")
    set(BUILD_TESTING 0 CACHE BOOL "" FORCE)
    set(USE_INTERMEDIATE_OBJECTS_TARGET OFF CACHE BOOL "")
    if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(DISABLE_ASM ON CACHE BOOL "")
    endif()
    add_subdirectory(cryptopp EXCLUDE_FROM_ALL)
    if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_definitions(cryptopp-static PUBLIC __EXCEPTIONS CRYPTOPP_SUPPORT_CLANG_CL)
    endif()
    add_library(cryptopp ALIAS cryptopp-static)
    if(ANDROID)
        target_include_directories(cryptopp-static PRIVATE ${CMAKE_ANDROID_NDK}/sources/android/cpufeatures)
    endif()
endif()