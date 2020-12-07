add_library(coro-cloudstorage)

set(coro-cloudstorage-HEADERS
    coro/cloudstorage/version.h
    coro/cloudstorage/util/fetch_json.h
    coro/cloudstorage/cloud_provider.h
    coro/cloudstorage/providers/google_drive.h
    coro/cloudstorage/providers/mega.h
)
set(coro-cloudstorage-SOURCES
    coro/cloudstorage/version.cc
)

target_sources(coro-cloudstorage PRIVATE ${coro-cloudstorage-HEADERS} ${coro-cloudstorage-SOURCES})
target_include_directories(coro-cloudstorage PUBLIC .)
target_link_libraries(coro-cloudstorage PUBLIC coro-http nlohmann_json::nlohmann_json Mega)