add_executable(cloudstorage cloudstorage.cc)

target_link_libraries(cloudstorage PRIVATE coro-cloudstorage coro::coro-http)

add_executable(js js.cc)
target_link_libraries(js PRIVATE coro-cloudstorage)
