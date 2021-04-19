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
    ../assets/folder.svg
    ../assets/gtk-file.svg
    ../assets/audio-x-generic.svg
    ../assets/image.svg
    ../assets/video.svg
    ../assets/providers/box.png
    ../assets/providers/yandex.png
    ../assets/providers/dropbox.png
    ../assets/providers/onedrive.png
    ../assets/providers/pcloud.png
    ../assets/providers/webdav.png
    ../assets/providers/google.png
    ../assets/providers/mega.png
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

function(embed_resource)
    cmake_parse_arguments(EMBED "" "OUTPUT" "INPUT" ${ARGN})

    set(OUTPUT_HEADER "${EMBED_OUTPUT}.h")
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" HEADER_GUARD ${OUTPUT_HEADER})

    string(APPEND HEADER_CONTENT
        "#ifndef ${HEADER_GUARD}\n"
        "#define ${HEADER_GUARD}\n"
        "#include <string_view>\n"
        "namespace coro::cloudstorage::util {\n")
    string(APPEND IMPL_CONTENT
        "#include \"${OUTPUT_HEADER}\"\n"
        "namespace coro::cloudstorage::util {\n"
    )
    foreach(ENTRY ${EMBED_INPUT})
        file(READ "${ENTRY}" HEX_CONTENT HEX)
        string(REPEAT "[0-9a-f]" 32 PATTERN)
        string(REGEX REPLACE "(${PATTERN})" "\\1\n" CONTENT "${HEX_CONTENT}")

        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " CONTENT "${CONTENT}")

        string(REGEX REPLACE ", $" "" CONTENT "${CONTENT}")

        string(REGEX REPLACE "[\-\.\/]" "_" ENTRY ${ENTRY})
        string(REGEX REPLACE "(_+)(.*)$" "\\2" ENTRY ${ENTRY})
        string(APPEND IMPL_CONTENT
            "static const unsigned char ${ENTRY}_data[] = { ${CONTENT} };\n"
            "const std::string_view ${ENTRY}(reinterpret_cast<const char*>(${ENTRY}_data), sizeof(${ENTRY}_data));\n"
        )

        string(APPEND HEADER_CONTENT
            "extern const std::string_view ${ENTRY};\n"
        )
    endforeach()
    string(APPEND HEADER_CONTENT
        "} // namespace coro::cloudstorage::util\n"
        "#endif // ${HEADER_GUARD}\n")
    string(APPEND IMPL_CONTENT
        "} // namespace coro::cloudstorage::util\n"
    )
    file(GENERATE OUTPUT "${OUTPUT_HEADER}" CONTENT "${HEADER_CONTENT}")
    file(GENERATE OUTPUT "${EMBED_OUTPUT}.cc" CONTENT "${IMPL_CONTENT}")
endfunction()

embed_resource(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/coro/cloudstorage/util/assets
    INPUT
        ${CORO_CLOUDSTORAGE_ASSETS}
)

target_sources(coro-cloudstorage PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/coro/cloudstorage/util/assets.h
    ${CMAKE_CURRENT_BINARY_DIR}/coro/cloudstorage/util/assets.cc
)