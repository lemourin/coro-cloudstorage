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
    coro/cloudstorage/providers/pcloud.h
    coro/cloudstorage/providers/webdav.h
)
set(coro-cloudstorage-SOURCES
    coro/cloudstorage/util/webdav_utils.cc
    coro/cloudstorage/util/auth_token_manager.cc
    coro/cloudstorage/util/generator_utils.cc
    coro/cloudstorage/providers/mega.cc
    coro/cloudstorage/providers/webdav.cc
)

target_sources(coro-cloudstorage PRIVATE ${coro-cloudstorage-HEADERS} ${coro-cloudstorage-SOURCES})
target_include_directories(coro-cloudstorage PUBLIC . ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(coro-cloudstorage PUBLIC coro-http nlohmann_json::nlohmann_json Mega pugixml::pugixml)

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
        file(READ "../${ENTRY}" HEX_CONTENT HEX)
        string(REPEAT "[0-9a-f]" 32 PATTERN)
        string(REGEX REPLACE "(${PATTERN})" "\\1\n" CONTENT "${HEX_CONTENT}")

        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " CONTENT "${CONTENT}")

        string(REGEX REPLACE ", $" "" CONTENT "${CONTENT}")
        
        string(REGEX REPLACE "[\-\.\/]" "_" ENTRY ${ENTRY})
        string(APPEND IMPL_CONTENT
            "static const char ${ENTRY}_data[] = { ${CONTENT} };\n"
            "std::string_view ${ENTRY}(${ENTRY}_data, sizeof(${ENTRY}_data));\n"
        )

        string(APPEND HEADER_CONTENT
            "extern std::string_view ${ENTRY};"
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
        assets/folder.svg 
        assets/gtk-file.svg
)

target_sources(coro-cloudstorage PRIVATE 
    ${CMAKE_CURRENT_BINARY_DIR}/coro/cloudstorage/util/assets.h
    ${CMAKE_CURRENT_BINARY_DIR}/coro/cloudstorage/util/assets.cc
)