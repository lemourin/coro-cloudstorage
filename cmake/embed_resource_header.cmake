function(to_camel_case OUTPUT INPUT)
    string(REGEX REPLACE "[-\\.\\/]" "_" INPUT ${INPUT})
    string(REGEX REPLACE "(_+)(.*)$" "\\2" INPUT ${INPUT})
    string(LENGTH "${INPUT}" LENGTH)
    set(CAPITALIZE TRUE)
    foreach(I RANGE ${LENGTH})
        string(SUBSTRING ${INPUT} ${I} 1 CHR)
        string(COMPARE EQUAL "_" "${CHR}" EQ)
        if(EQ)
            set(CAPITALIZE TRUE)
        else()
            if(CAPITALIZE)
                set(CAPITALIZE FALSE)
                string(TOUPPER ${CHR} CHR)
            endif()
            string(CONCAT CONVERTED "${CONVERTED}${CHR}")
        endif()
    endforeach()
    set("${OUTPUT}" "k${CONVERTED}" PARENT_SCOPE)
endfunction()

function(embed_resource_header)
    cmake_parse_arguments(EMBED "" "NAMESPACE;OUTPUT" "INPUT" ${ARGN})

    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" HEADER_GUARD ${EMBED_OUTPUT})

    string(APPEND HEADER_CONTENT
            "#ifndef ${HEADER_GUARD}\n"
            "#define ${HEADER_GUARD}\n"
            "#include <string_view>\n"
            "namespace ${EMBED_NAMESPACE} {\n")
    foreach(ENTRY ${EMBED_INPUT})
        to_camel_case(ENTRY ${ENTRY})
        string(APPEND HEADER_CONTENT
                "extern const std::string_view ${ENTRY};\n"
        )
    endforeach()
    string(APPEND HEADER_CONTENT
            "} // namespace ${EMBED_NAMESPACE}\n"
            "#endif // ${HEADER_GUARD}\n")
    file(GENERATE OUTPUT "${EMBED_OUTPUT}" CONTENT "${HEADER_CONTENT}")
endfunction()
