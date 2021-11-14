add_executable(cloudstorage cloudstorage.cc)

if(MSVC)
    target_compile_options(cloudstorage PRIVATE -bigobj)
endif()

target_link_libraries(cloudstorage PRIVATE coro-cloudstorage coro-http)

if(UNIX)
    target_link_libraries(cloudstorage PRIVATE libevent::pthreads)
endif()
