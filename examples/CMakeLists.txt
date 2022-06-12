add_executable(cloudstorage cloudstorage.cc)

target_link_libraries(cloudstorage PRIVATE coro-cloudstorage coro-http)
