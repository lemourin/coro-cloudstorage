include(CheckSymbolExists)
include(CheckCXXSourceCompiles)
include(EmbedResource)
include(antlr)

add_library(coro-cloudstorage)

check_cxx_source_compiles(
    "
    #include <string.h>
    int main() {
      char buf;
      const char* ret = strerror_r(0, &buf, 1);
      return ret ? 0 : 1;
    }
    "
    CORO_CLOUDSTORAGE_HAVE_GLIBC_STRERROR_R
)
if(CORO_CLOUDSTORAGE_HAVE_GLIBC_STRERROR_R)
    target_compile_definitions(coro-cloudstorage PRIVATE HAVE_GLIBC_STRERROR_R)
endif()

check_cxx_source_compiles(
    "
    #include <string.h>
    int main() {
      char buf;
      int err = strerror_r(0, &buf, 1);
      return err;
    }
    "
    CORO_CLOUDSTORAGE_HAVE_STRERROR_R
)
if(CORO_CLOUDSTORAGE_HAVE_STRERROR_R)
    target_compile_definitions(coro-cloudstorage PRIVATE HAVE_STRERROR_R)
endif()

check_symbol_exists(strerror_s "string.h" CORO_CLOUDSTORAGE_HAVE_STRERROR_S)
if(CORO_CLOUDSTORAGE_HAVE_STRERROR_S)
    target_compile_definitions(coro-cloudstorage PRIVATE HAVE_STRERROR_S)
endif()

check_symbol_exists(getifaddrs "ifaddrs.h" HAVE_GETIFADDRS_H)
check_symbol_exists(freeifaddrs "ifaddrs.h" HAVE_FREEIFADDRS_H)
if(HAVE_GETIFADDRS_H AND HAVE_FREEIFADDRS_H)
    target_compile_definitions(coro-cloudstorage PRIVATE HAVE_IFADDRS_H)
endif()

target_sources(coro-cloudstorage PRIVATE
    coro/cloudstorage/util/fetch_json.h
    coro/cloudstorage/util/account_manager_handler.h
    coro/cloudstorage/util/account_manager_handler.cc
    coro/cloudstorage/util/static_file_handler.h
    coro/cloudstorage/util/static_file_handler.cc
    coro/cloudstorage/util/cloud_provider_account.h
    coro/cloudstorage/util/get_size_handler.h
    coro/cloudstorage/util/get_size_handler.cc
    coro/cloudstorage/util/theme_handler.h
    coro/cloudstorage/util/theme_handler.cc
    coro/cloudstorage/util/settings_handler.h
    coro/cloudstorage/util/settings_handler.cc
    coro/cloudstorage/util/auth_token_manager.h
    coro/cloudstorage/util/auth_manager.h
    coro/cloudstorage/util/auth_handler.h
    coro/cloudstorage/util/cloud_provider_handler.h
    coro/cloudstorage/util/cloud_provider_handler.cc
    coro/cloudstorage/util/webdav_handler.h
    coro/cloudstorage/util/webdav_handler.cc
    coro/cloudstorage/util/handler_utils.h
    coro/cloudstorage/util/handler_utils.cc
    coro/cloudstorage/util/serialize_utils.h
    coro/cloudstorage/util/webdav_utils.h
    coro/cloudstorage/util/auth_data.h
    coro/cloudstorage/util/generator_utils.h
    coro/cloudstorage/util/avio_context.h
    coro/cloudstorage/util/avio_context.cc
    coro/cloudstorage/util/thumbnail_generator.h
    coro/cloudstorage/util/thumbnail_options.h
    coro/cloudstorage/util/muxer.h
    coro/cloudstorage/util/file_utils.h
    coro/cloudstorage/util/string_utils.h
    coro/cloudstorage/util/string_utils.cc
    coro/cloudstorage/util/merged_cloud_provider.h
    coro/cloudstorage/util/merged_cloud_provider.cc
    coro/cloudstorage/util/timing_out_cloud_provider.h
    coro/cloudstorage/util/timing_out_cloud_provider.cc
    coro/cloudstorage/util/timing_out_stop_token.h
    coro/cloudstorage/util/timing_out_stop_token.cc
    coro/cloudstorage/util/abstract_cloud_factory.h
    coro/cloudstorage/util/abstract_cloud_provider.h
    coro/cloudstorage/util/abstract_cloud_provider_impl.h
    coro/cloudstorage/util/random_number_generator.h
    coro/cloudstorage/util/cloud_factory_context.h
    coro/cloudstorage/util/cloud_factory_context.cc
    coro/cloudstorage/util/net_utils.h
    coro/cloudstorage/util/net_utils.cc
    coro/cloudstorage/util/settings_utils.h
    coro/cloudstorage/util/settings_utils.cc
    coro/cloudstorage/util/settings_manager.h
    coro/cloudstorage/util/settings_manager.cc
    coro/cloudstorage/util/cloud_provider_utils.h
    coro/cloudstorage/util/cloud_provider_utils.cc
    coro/cloudstorage/cloud_exception.h
    coro/cloudstorage/cloud_factory.h
    coro/cloudstorage/cloud_factory.cc
    coro/cloudstorage/providers/youtube.h
    coro/cloudstorage/providers/youtube.cc
    coro/cloudstorage/providers/google_drive.h
    coro/cloudstorage/providers/google_drive.cc
    coro/cloudstorage/providers/one_drive.h
    coro/cloudstorage/providers/one_drive.cc
    coro/cloudstorage/providers/dropbox.cc
    coro/cloudstorage/providers/dropbox.h
    coro/cloudstorage/providers/mega.h
    coro/cloudstorage/providers/mega.cc
    coro/cloudstorage/providers/box.h
    coro/cloudstorage/providers/box.cc
    coro/cloudstorage/providers/mega.cc
    coro/cloudstorage/providers/yandex_disk.cc
    coro/cloudstorage/providers/pcloud.cc
    coro/cloudstorage/providers/amazon_s3.cc
    coro/cloudstorage/providers/dropbox.cc
    coro/cloudstorage/providers/google_drive.cc
    coro/cloudstorage/providers/local_filesystem.cc
    coro/cloudstorage/providers/webdav.cc
    coro/cloudstorage/providers/one_drive.cc
    coro/cloudstorage/providers/open_stack.cc
    coro/cloudstorage/util/merged_cloud_provider.cc
    coro/cloudstorage/util/account_manager_handler.cc
    coro/cloudstorage/util/crypto_utils.cc
    coro/cloudstorage/util/file_utils.cc
    coro/cloudstorage/util/string_utils.cc
    coro/cloudstorage/util/cloud_provider_utils.cc
    coro/cloudstorage/util/timing_out_cloud_provider.cc
    coro/cloudstorage/util/avio_context.cc
    coro/cloudstorage/util/webdav_utils.cc
    coro/cloudstorage/util/exception_utils.cc
    coro/cloudstorage/util/settings_utils.cc
    coro/cloudstorage/util/webdav_handler.cc
    coro/cloudstorage/util/cloud_factory_context.cc
    coro/cloudstorage/util/static_file_handler.cc
    coro/cloudstorage/util/handler_utils.cc
    coro/cloudstorage/util/serialize_utils.cc
    coro/cloudstorage/util/muxer.cc
    coro/cloudstorage/util/thumbnail_generator.cc
    coro/cloudstorage/util/settings_handler.cc
    coro/cloudstorage/util/get_size_handler.cc
    coro/cloudstorage/util/net_utils.cc
    coro/cloudstorage/util/settings_manager.cc
    coro/cloudstorage/util/theme_handler.cc
    coro/cloudstorage/util/ffmpeg_utils.cc
    coro/cloudstorage/util/timing_out_stop_token.cc
    coro/cloudstorage/util/cloud_factory_config.cc
    coro/cloudstorage/util/generator_utils.cc
    coro/cloudstorage/util/list_directory_handler.cc
    coro/cloudstorage/util/mux_handler.cc
    coro/cloudstorage/util/cache_manager.cc
    coro/cloudstorage/util/item_thumbnail_handler.cc
    coro/cloudstorage/util/item_content_handler.cc
    coro/cloudstorage/util/clock.cc
    coro/cloudstorage/util/cloud_provider_account.cc
    coro/cloudstorage/util/dash_handler.cc
    coro/cloudstorage/cloud_factory.cc
)

target_sources(coro-cloudstorage
    INTERFACE FILE_SET public_headers TYPE HEADERS FILES
        coro/cloudstorage/providers/yandex_disk.h
        coro/cloudstorage/providers/open_stack.h
        coro/cloudstorage/providers/google_drive.h
        coro/cloudstorage/providers/pcloud.h
        coro/cloudstorage/providers/dropbox.h
        coro/cloudstorage/providers/local_filesystem.h
        coro/cloudstorage/providers/one_drive.h
        coro/cloudstorage/providers/amazon_s3.h
        coro/cloudstorage/providers/webdav.h
        coro/cloudstorage/providers/mega.h
        coro/cloudstorage/providers/box.h
        coro/cloudstorage/util/account_manager_handler.h
        coro/cloudstorage/util/avio_context.h
        coro/cloudstorage/util/cloud_factory_context.h
        coro/cloudstorage/util/thumbnail_generator.h
        coro/cloudstorage/util/crypto_utils.h
        coro/cloudstorage/util/auth_handler.h
        coro/cloudstorage/util/thumbnail_quality.h
        coro/cloudstorage/util/exception_utils.h
        coro/cloudstorage/util/generator_utils.h
        coro/cloudstorage/util/auth_manager.h
        coro/cloudstorage/util/fetch_json.h
        coro/cloudstorage/util/cloud_factory_config.h
        coro/cloudstorage/util/thumbnail_options.h
        coro/cloudstorage/util/abstract_cloud_provider_impl.h
        coro/cloudstorage/util/cloud_provider_utils.h
        coro/cloudstorage/util/cloud_provider_account.h
        coro/cloudstorage/util/theme_handler.h
        coro/cloudstorage/util/on_auth_token_updated.h
        coro/cloudstorage/util/net_utils.h
        coro/cloudstorage/util/timing_out_stop_token.h
        coro/cloudstorage/util/muxer.h
        coro/cloudstorage/util/file_utils.h
        coro/cloudstorage/util/static_file_handler.h
        coro/cloudstorage/util/abstract_cloud_factory.h
        coro/cloudstorage/util/recursive_visit.h
        coro/cloudstorage/util/webdav_handler.h
        coro/cloudstorage/util/random_number_generator.h
        coro/cloudstorage/util/list_directory_handler.h
        coro/cloudstorage/util/auth_data.h
        coro/cloudstorage/util/string_utils.h
        coro/cloudstorage/util/webdav_utils.h
        coro/cloudstorage/util/settings_manager.h
        coro/cloudstorage/util/settings_utils.h
        coro/cloudstorage/util/ffmpeg_utils.h
        coro/cloudstorage/util/handler_utils.h
        coro/cloudstorage/util/abstract_cloud_provider.h
        coro/cloudstorage/util/timing_out_cloud_provider.h
        coro/cloudstorage/util/serialize_utils.h
        coro/cloudstorage/util/settings_handler.h
        coro/cloudstorage/util/get_size_handler.h
        coro/cloudstorage/util/merged_cloud_provider.h
        coro/cloudstorage/util/mux_handler.h
        coro/cloudstorage/util/cache_manager.h
        coro/cloudstorage/util/item_thumbnail_handler.h
        coro/cloudstorage/util/item_content_handler.h
        coro/cloudstorage/util/clock.h
        coro/cloudstorage/util/dash_handler.h
        coro/cloudstorage/util/item_url_provider.h
        coro/cloudstorage/cloud_exception.h
        coro/cloudstorage/cloud_factory.h
)

target_include_directories(coro-cloudstorage PRIVATE . ${CMAKE_CURRENT_BINARY_DIR} ../contrib/)

target_link_libraries(coro-cloudstorage PUBLIC
    coro::coro-http
    nlohmann_json::nlohmann_json
)

target_link_libraries(coro-cloudstorage PRIVATE
    cryptopp::cryptopp
    fmt::fmt
    pugixml::pugixml
    libevent::core
    sqlite_orm::sqlite_orm
)

if(FFMPEG_LIBRARIES)
    target_include_directories(coro-cloudstorage PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(coro-cloudstorage PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(coro-cloudstorage PRIVATE ${FFMPEG_LIBRARIES})
else()
    target_link_libraries(coro-cloudstorage PRIVATE
        FFMPEG::avcodec
        FFMPEG::avutil
        FFMPEG::avformat
        FFMPEG::swscale
        FFMPEG::avfilter
    )
endif()

add_resource_library(
    coro-cloudstorage-resource
    NAMESPACE coro::cloudstorage::util
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/coro/cloudstorage/util/assets.h
    INPUT
        kAudioIcon ../assets/icons/mimetypes/64/audio-x-generic.svg
        kImageIcon ../assets/icons/mimetypes/64/image-x-generic.svg
        kUnknownIcon ../assets/icons/mimetypes/64/unknown.svg
        kVideoIcon ../assets/icons/mimetypes/64/video-x-generic.svg
        kFolderIcon ../assets/icons/places/64/folder.svg
        kTrashIcon ../assets/icons/places/64/user-trash.svg
        kSettingsIcon ../assets/icons/actions/32/settings-configure.svg
        kGoBackIcon ../assets/icons/actions/32/go-previous.svg
        kDarkAudioIcon ../assets/icons-dark/mimetypes/64/audio-x-generic.svg
        kDarkImageIcon ../assets/icons-dark/mimetypes/64/image-x-generic.svg
        kDarkUnknownIcon ../assets/icons-dark/mimetypes/64/unknown.svg
        kDarkVideoIcon ../assets/icons-dark/mimetypes/64/video-x-generic.svg
        kDarkFolderIcon ../assets/icons-dark/places/64/folder.svg
        kDarkTrashIcon ../assets/icons-dark/places/64/user-trash.svg
        kDarkSettingsIcon ../assets/icons-dark/actions/32/settings-configure.svg
        kDarkGoBackIcon ../assets/icons-dark/actions/32/go-previous.svg
        kFavIcon ../assets/icons/favicon.ico
        kMegaLoginHtml ../assets/html/mega_login.html
        kLocalDriveLoginHtml ../assets/html/local_login.html
        kWebdavLoginHtml ../assets/html/webdav_login.html
        kAmazonS3LoginHtml ../assets/html/amazons3_login.html
        kOpenStackLoginHtml ../assets/html/open_stack_login.html
        kDashPlayerHtml ../assets/html/dash_player.html
        kItemEntryHtml ../assets/html/item_entry.html
        kProviderEntryHtml ../assets/html/provider_entry.html
        kAccountEntryHtml ../assets/html/account_entry.html
        kHomePageHtml ../assets/html/home_page.html
        kSettingsPageHtml ../assets/html/settings_page.html
        kErrorPageHtml ../assets/html/error_page.html
        kBoxIcon ../assets/providers/box.png
        kYandexIcon ../assets/providers/yandex.png
        kDropboxIcon ../assets/providers/dropbox.png
        kOneDriveIcon ../assets/providers/onedrive.png
        kPCloudIcon ../assets/providers/pcloud.png
        kWebDAVIcon ../assets/providers/webdav.png
        kGoogleDriveIcon ../assets/providers/google.png
        kMegaIcon ../assets/providers/mega.png
        kAmazonS3Icon ../assets/providers/amazons3.png
        kYouTubeIcon ../assets/providers/youtube.png
        kLocalDriveIcon ../assets/providers/local.png
        kOpenStackIcon ../assets/providers/openstack.png
        kLayoutCss ../assets/styles/layout.css
        kColorsLightCss ../assets/styles/colors_light.css
        kColorsDarkCss ../assets/styles/colors_dark.css
        kAccountListMainJs ../assets/js/account_list_main.js
        kSettingsMainJs ../assets/js/settings_main.js
        kAuthDataJson ../assets/config/auth_data.json
)

target_link_libraries(coro-cloudstorage PRIVATE coro-cloudstorage-resource)

antlr_target(javascript-lexer
             GRAMMAR coro/cloudstorage/util/antlr/javascript_lexer.g4
             TYPE LEXER
             NAMESPACE coro_cloudstorage_util_antlr)
target_include_directories(javascript-lexer PRIVATE coro/cloudstorage/util)

antlr_target(javascript-parser
             GRAMMAR coro/cloudstorage/util/antlr/javascript_parser.g4
             TYPE PARSER
             NAMESPACE coro_cloudstorage_util_antlr
             VISITOR
             DEPENDS javascript-lexer)

add_library(evaluate-javascript
            coro/cloudstorage/util/antlr/javascript_lexer_base.h
            coro/cloudstorage/util/antlr/javascript_lexer_base.cc
            coro/cloudstorage/util/evaluate_javascript.h
            coro/cloudstorage/util/evaluate_javascript.cc)
target_include_directories(
    evaluate-javascript 
    PRIVATE 
        .
        ${ANTLR4_INCLUDE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}
        coro/cloudstorage/util
)
target_link_libraries(evaluate-javascript PRIVATE javascript-parser coro::coro-http)
target_link_libraries(coro-cloudstorage PRIVATE evaluate-javascript)

if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
    target_compile_definitions(
        coro-cloudstorage PRIVATE WINRT
    )
    target_link_libraries(coro-cloudstorage PRIVATE Microsoft::CppWinRT)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(
    TARGETS 
        coro-cloudstorage 
        coro-cloudstorage-resource 
        evaluate-javascript
        javascript-parser
        javascript-lexer
    EXPORT 
        coro-cloudstorage 
    FILE_SET public_headers
)
install(EXPORT coro-cloudstorage NAMESPACE coro:: DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/cmake/${PROJECT_NAME})
configure_package_config_file(
        ../cmake/coro-cloudstorage-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/coro-cloudstorage-config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/cmake/${PROJECT_NAME}
)
install(
        FILES
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/coro-cloudstorage-config.cmake
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/cmake/${PROJECT_NAME}
)
