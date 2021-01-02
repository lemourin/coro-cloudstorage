add_subdirectory(coro-http EXCLUDE_FROM_ALL)
add_subdirectory(json EXCLUDE_FROM_ALL)
set(BUILD_TESTING 0)
add_subdirectory(cryptopp EXCLUDE_FROM_ALL)
add_library(cryptopp ALIAS cryptopp-static)
if(ANDROID)
    target_include_directories(cryptopp-object PRIVATE ${CMAKE_ANDROID_NDK}/sources/android/cpufeatures)
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
    ${CMAKE_BINARY_DIR}/contrib/zlib
)

if(ANDROID)
    target_compile_definitions(Mega PRIVATE _LIBCPP_HAS_NO_OFF_T_FUNCTIONS)
endif()