function(embed_resource_impl)
    cmake_parse_arguments(EMBED "" "NAMESPACE;OUTPUT" "INPUT" ${ARGN})

    string(APPEND IMPL_CONTENT
            "#include \"${EMBED_OUTPUT}.h\"\n"
            "namespace ${EMBED_NAMESPACE} {\n"
    )

    foreach(ENTRY ${EMBED_INPUT})
        file(READ "${ENTRY}" HEX_CONTENT HEX)
        string(REPEAT "[0-9a-f]" 32 PATTERN)
        string(REGEX REPLACE "(${PATTERN})" "\\1\n" CONTENT "${HEX_CONTENT}")

        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " CONTENT "${CONTENT}")

        string(REGEX REPLACE ", $" "" CONTENT "${CONTENT}")

        string(REGEX REPLACE "[-\\.\\/]" "_" ENTRY ${ENTRY})
        string(REGEX REPLACE "(_+)(.*)$" "\\2" ENTRY ${ENTRY})
        string(APPEND IMPL_CONTENT
                "static const unsigned char ${ENTRY}_data[] = { ${CONTENT} };\n"
                "const std::string_view ${ENTRY}(reinterpret_cast<const char*>(${ENTRY}_data), sizeof(${ENTRY}_data));\n"
        )
    endforeach()
    string(APPEND IMPL_CONTENT
            "} // namespace ${EMBED_NAMESPACE}\n"
    )
    file(WRITE "${EMBED_OUTPUT}.cc" "${IMPL_CONTENT}")
endfunction()

string(REPLACE " " ";" ASSETS "${ASSETS}")

embed_resource_impl(
        NAMESPACE ${NAMESPACE}
        OUTPUT ${OUTPUT}
        INPUT ${ASSETS}
)
