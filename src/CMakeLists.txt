add_library(coro-cloudstorage)

set(CORO_CLOUDSTORAGE_HEADERS
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
    coro/cloudstorage/util/avio_context.h
    coro/cloudstorage/util/thumbnail_generator.h
    coro/cloudstorage/util/thumbnail_options.h
    coro/cloudstorage/util/muxer.h
    coro/cloudstorage/util/file_utils.h
    coro/cloudstorage/util/string_utils.h
    coro/cloudstorage/util/pch.h
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
    coro/cloudstorage/providers/pcloud.h
    coro/cloudstorage/providers/webdav.h
)
set(CORO_CLOUDSTORAGE_SOURCES
    coro/cloudstorage/util/webdav_utils.cc
    coro/cloudstorage/util/auth_token_manager.cc
    coro/cloudstorage/util/generator_utils.cc
    coro/cloudstorage/util/thumbnail_generator.cc
    coro/cloudstorage/util/muxer.cc
    coro/cloudstorage/util/ffmpeg_utils.cc
    coro/cloudstorage/util/file_utils.cc
    coro/cloudstorage/providers/mega.cc
    coro/cloudstorage/providers/webdav.cc
)
set(CORO_CLOUDSTORAGE_ASSETS
    ../assets/icons/folder.svg
    ../assets/icons/gtk-file.svg
    ../assets/icons/audio-x-generic.svg
    ../assets/icons/image.svg
    ../assets/icons/video.svg
    ../assets/icons/user-trash.svg
    ../assets/html/mega_login.html
    ../assets/html/webdav_login.html
    ../assets/providers/box.png
    ../assets/providers/yandex.png
    ../assets/providers/dropbox.png
    ../assets/providers/onedrive.png
    ../assets/providers/pcloud.png
    ../assets/providers/webdav.png
    ../assets/providers/google.png
    ../assets/providers/mega.png
    ../assets/styles/default.css
)

target_sources(coro-cloudstorage PRIVATE
    ${CORO_CLOUDSTORAGE_HEADERS}
    ${CORO_CLOUDSTORAGE_SOURCES}
    ${CORO_CLOUDSTORAGE_ASSETS}
)
target_include_directories(coro-cloudstorage PUBLIC . ${CMAKE_CURRENT_BINARY_DIR})
target_precompile_headers(coro-cloudstorage PUBLIC coro/cloudstorage/util/pch.h)
target_link_libraries(coro-cloudstorage PUBLIC
    coro-http
    nlohmann_json::nlohmann_json
    Mega
    pugixml::pugixml
    avcodec
    avutil
    avformat
    swscale
    avfilter
)

include(../cmake/embed_resource.cmake)

embed_resource(
    TARGET coro-cloudstorage
    NAMESPACE coro::cloudstorage::util
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/coro/cloudstorage/util/assets
    INPUT ${CORO_CLOUDSTORAGE_ASSETS}
)
