add_library(coro-cloudstorage)

set(coro-cloudstorage-HEADERS
    coro/cloudstorage/version.h
    coro/cloudstorage/util/fetch_json.h
    coro/cloudstorage/util/auth_manager.h
    coro/cloudstorage/cloud_factory.h
    coro/cloudstorage/cloud_provider.h
    coro/cloudstorage/providers/google_drive.h
    coro/cloudstorage/providers/mega.h
    coro/cloudstorage/providers/mega/file_system_access.h
    coro/cloudstorage/providers/mega/http_io.h
)
set(coro-cloudstorage-SOURCES
    coro/cloudstorage/version.cc
    coro/cloudstorage/providers/mega.cc
)

target_sources(coro-cloudstorage PRIVATE ${coro-cloudstorage-HEADERS} ${coro-cloudstorage-SOURCES})
target_include_directories(coro-cloudstorage PUBLIC .)
target_link_libraries(coro-cloudstorage PUBLIC coro-http nlohmann_json::nlohmann_json Mega)