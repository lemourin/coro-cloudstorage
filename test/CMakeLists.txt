include(GoogleTest)

find_package(GTest REQUIRED)

add_executable(
    coro-cloudstorage-test
    functional_test.cc
)

target_link_libraries(coro-cloudstorage-test GTest::gtest_main GTest::gtest coro-cloudstorage coro::coro-http)
target_compile_definitions(
    coro-cloudstorage-test
        PRIVATE
            TEST_DATA_DIRECTORY="${CMAKE_CURRENT_LIST_DIR}/data"
            BUILD_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}"
)

gtest_discover_tests(coro-cloudstorage-test)
