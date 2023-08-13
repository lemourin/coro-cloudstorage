define_property(TARGET
                PROPERTY ANTLR_TOKEN_DIRECTORY
                BRIEF_DOCS "antlr token directory"
                FULL_DOCS "antlr token directory")

function(antlr_target name)
    find_package(antlr4-generator 4.9.1 CONFIG REQUIRED)
    cmake_parse_arguments(PARSE_ARGV 1 ANTLR "LISTENER;VISITOR" "GRAMMAR;TYPE;NAMESPACE" "DEPENDS")

    get_filename_component(DIRECTORY ${ANTLR_GRAMMAR} DIRECTORY)
    set(ANTLR4_GENERATED_SRC_DIR ${CMAKE_CURRENT_BINARY_DIR}/${DIRECTORY})

    set(ANTLR_LIBRARY_PATH)
    foreach(DEPENDENCY ${ANTLR_DEPENDS})
        get_target_property(TOKEN_DIRECTORY ${DEPENDENCY} ANTLR_TOKEN_DIRECTORY)
        if(TOKEN_DIRECTORY)
            list(APPEND ANTLR_LIBRARY_PATH ${TOKEN_DIRECTORY})
        endif()
    endforeach()

    antlr4_generate(${name}
                    "${CMAKE_CURRENT_SOURCE_DIR}/${ANTLR_GRAMMAR}"
                    ${ANTLR_TYPE}
                    ${ANTLR_LISTENER}
                    ${ANTLR_VISITOR}
                    ${ANTLR_NAMESPACE}
                    "${ANTLR_DEPENDS}"
                    "${ANTLR_LIBRARY_PATH}")

    add_library(${name} ${ANTLR4_SRC_FILES_${name}} ${ANTLR4_TOKEN_FILES_${name}})
    target_include_directories(${name} PRIVATE
                               ${ANTLR4_INCLUDE_DIR_${name}}
                               ${ANTLR4_INCLUDE_DIR}
                               ${CMAKE_CURRENT_BINARY_DIR})
    set_target_properties(${name}
                          PROPERTIES ANTLR_TOKEN_DIRECTORY ${ANTLR4_TOKEN_DIRECTORY_${name}})
    target_compile_features(${name} PUBLIC cxx_std_20)

    target_link_libraries(${name} PUBLIC antlr4::antlr4)
    foreach(DEPENDENCY ${ANTLR_DEPENDS})
        target_link_libraries(${name} PUBLIC ${DEPENDENCY})
    endforeach()
endfunction()