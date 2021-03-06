add_library(coro-cloudstorage)

set(coro-cloudstorage-HEADERS
    coro/cloudstorage/util/fetch_json.h
    coro/cloudstorage/util/account_manager_handler.h
    coro/cloudstorage/util/auth_token_manager.h
    coro/cloudstorage/util/auth_manager.h
    coro/cloudstorage/util/auth_handler.h
    coro/cloudstorage/util/proxy_handler.h
    coro/cloudstorage/util/serialize_utils.h
    coro/cloudstorage/util/webdav_utils.h
    coro/cloudstorage/util/auth_data.h
    coro/cloudstorage/util/generator_utils.h
    coro/cloudstorage/cloud_exception.h
    coro/cloudstorage/cloud_factory.h
    coro/cloudstorage/cloud_provider.h
    coro/cloudstorage/abstract_cloud_provider.h
    coro/cloudstorage/providers/google_drive.h
    coro/cloudstorage/providers/one_drive.h
    coro/cloudstorage/providers/dropbox.h
    coro/cloudstorage/providers/mega.h
    coro/cloudstorage/providers/mega/file_system_access.h
    coro/cloudstorage/providers/mega/http_io.h
    coro/cloudstorage/providers/box.h
    coro/cloudstorage/providers/yandex_disk.h
)
set(coro-cloudstorage-SOURCES
    coro/cloudstorage/util/webdav_utils.cc
    coro/cloudstorage/util/auth_token_manager.cc
    coro/cloudstorage/util/generator_utils.cc
    coro/cloudstorage/providers/mega.cc
)

target_sources(coro-cloudstorage PRIVATE ${coro-cloudstorage-HEADERS} ${coro-cloudstorage-SOURCES})
target_include_directories(coro-cloudstorage PUBLIC .)
target_link_libraries(coro-cloudstorage PUBLIC coro-http nlohmann_json::nlohmann_json Mega)