add_subdirectory(coro-http EXCLUDE_FROM_ALL)
add_subdirectory(json EXCLUDE_FROM_ALL)
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

set(USE_CRYPTOPP 1 CACHE BOOL "" FORCE)
set(USE_OPENSSL 0 CACHE BOOL "" FORCE)
set(USE_THIRDPARTY_FROM_VCPKG 0 CACHE BOOL "" FORCE)
set(USE_ASIO 0 CACHE BOOL "" FORCE)
set(USE_CURL 0 CACHE BOOL "" FORCE)
set(USE_SQLITE 0 CACHE BOOL "" FORCE)
set(USE_MEDIAINFO 0 CACHE BOOL "" FORCE)
set(USE_FREEIMAGE 0 CACHE BOOL "" FORCE)
set(USE_SODIUM 0 CACHE BOOL "" FORCE)
set(ENABLE_SYNC 0 CACHE BOOL "" FORCE)
set(ENABLE_CHAT 0 CACHE BOOL "" FORCE)
set(ENABLE_LOG_PERFORMANCE 0 CACHE BOOL "" FORCE)
set(HAVE_FFMPEG 0 CACHE BOOL "" FORCE)
set(USE_WEBRTC 0 CACHE BOOL "" FORCE)
set(USE_LIBUV 0 CACHE BOOL "" FORCE)
set(USE_QT 0 CACHE BOOL "" FORCE)
set(USE_PDFIUM 0 CACHE BOOL "" FORCE)
set(USE_LIBRAW 0 CACHE BOOL "" FORCE)
add_subdirectory(megasdk/contrib/cmake EXCLUDE_FROM_ALL)

target_include_directories(Mega PUBLIC
    .
    ${CMAKE_BINARY_DIR}
    ${CMAKE_BINARY_DIR}/include
)

if(ANDROID)
    target_compile_definitions(Mega PRIVATE _LIBCPP_HAS_NO_OFF_T_FUNCTIONS)
endif()

add_subdirectory(pugixml EXCLUDE_FROM_ALL)
set(ZLIB_COMPAT 1 CACHE BOOL "")
add_subdirectory(zlib-ng EXCLUDE_FROM_ALL)

add_subdirectory(fmt EXCLUDE_FROM_ALL)

set(FFMPEG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg")
add_subdirectory(ffmpeg-config EXCLUDE_FROM_ALL)