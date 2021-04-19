function(embed_resource_header)
    cmake_parse_arguments(EMBED "" "NAMESPACE;OUTPUT" "INPUT" ${ARGN})

    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" HEADER_GUARD ${EMBED_OUTPUT})

    string(APPEND HEADER_CONTENT
            "#ifndef ${HEADER_GUARD}\n"
            "#define ${HEADER_GUARD}\n"
            "#include <string_view>\n"
            "namespace ${EMBED_NAMESPACE} {\n")
    foreach(ENTRY ${EMBED_INPUT})
        string(REGEX REPLACE "[-\\.\\/]" "_" ENTRY ${ENTRY})
        string(REGEX REPLACE "(_+)(.*)$" "\\2" ENTRY ${ENTRY})
        string(APPEND HEADER_CONTENT
                "extern const std::string_view ${ENTRY};\n"
        )
    endforeach()
    string(APPEND HEADER_CONTENT
            "} // namespace ${EMBED_NAMESPACE}\n"
            "#endif // ${HEADER_GUARD}\n")
    file(GENERATE OUTPUT "${EMBED_OUTPUT}" CONTENT "${HEADER_CONTENT}")
endfunction()
