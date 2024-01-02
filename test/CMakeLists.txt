include(GoogleTest)

find_package(GTest REQUIRED)

add_executable(
    coro-cloudstorage-test
    account_manager_test.cc
    muxer_test.cc
    thumbnail_generator_test.cc
    google_drive_test.cc
    coro/cloudstorage/test/test_utils.h
    coro/cloudstorage/test/test_utils.cc
    coro/cloudstorage/test/fake_http_client.h
    coro/cloudstorage/test/fake_http_client.cc
    coro/cloudstorage/test/fake_cloud_factory_context.h
    coro/cloudstorage/test/fake_cloud_factory_context.cc
)

target_include_directories(coro-cloudstorage-test PRIVATE .)

target_link_libraries(
    coro-cloudstorage-test
        GTest::gtest_main
        GTest::gtest
        coro-cloudstorage
        coro::coro-http
        FFMPEG::avfilter
        fmt::fmt
)
target_compile_definitions(
    coro-cloudstorage-test
        PRIVATE
            TEST_DATA_DIRECTORY="${CMAKE_CURRENT_LIST_DIR}/data"
            BUILD_DIRECTORY="${CMAKE_CURRENT_BINARY_DIR}"
)

gtest_discover_tests(coro-cloudstorage-test)
