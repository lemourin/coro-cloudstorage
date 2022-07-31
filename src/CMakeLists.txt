include(CheckSymbolExists)
include(CheckCXXSourceCompiles)
include(embed_resource)

add_library(coro-cloudstorage)

check_cxx_source_compiles(
    "
    #include <string.h>
    int main() {
      const char* ret = strerror_r(0, NULL, 0);
      return 0;
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
      int err = strerror_r(0, NULL, 0);
      return 0;
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
    coro/cloudstorage/providers/yandex_disk.h
    coro/cloudstorage/providers/yandex_disk.cc
    coro/cloudstorage/providers/pcloud.h
    coro/cloudstorage/providers/pcloud.cc
    coro/cloudstorage/providers/webdav.h
    coro/cloudstorage/providers/webdav.cc
    coro/cloudstorage/providers/amazon_s3.h
    coro/cloudstorage/providers/amazon_s3.cc
    coro/cloudstorage/providers/open_stack.h
    coro/cloudstorage/providers/open_stack.cc
    coro/cloudstorage/providers/hubic.h
    coro/cloudstorage/providers/hubic.cc
    coro/cloudstorage/providers/local_filesystem.h
    coro/cloudstorage/providers/local_filesystem.cc
    coro/cloudstorage/util/webdav_utils.cc
    coro/cloudstorage/util/auth_token_manager.cc
    coro/cloudstorage/util/generator_utils.cc
    coro/cloudstorage/util/serialize_utils.cc
    coro/cloudstorage/util/thumbnail_generator.cc
    coro/cloudstorage/util/muxer.cc
    coro/cloudstorage/util/ffmpeg_utils.cc
    coro/cloudstorage/util/file_utils.cc
    coro/cloudstorage/util/crypto_utils.h
    coro/cloudstorage/util/crypto_utils.cc
    coro/cloudstorage/util/recursive_visit.h 
    coro/cloudstorage/util/on_auth_token_updated.h 
    coro/cloudstorage/util/thumbnail_quality.h)

target_include_directories(coro-cloudstorage PUBLIC .)
target_include_directories(coro-cloudstorage PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ../contrib/)

target_link_libraries(coro-cloudstorage PUBLIC
    coro-http
    nlohmann_json::nlohmann_json
)

target_link_libraries(coro-cloudstorage PRIVATE
    FFMPEG::avcodec
    FFMPEG::avutil
    FFMPEG::avformat
    FFMPEG::swscale
    FFMPEG::avfilter
    cryptopp::cryptopp
    fmt::fmt
    pugixml::pugixml
    libevent::core
    Boost::regex
)

embed_resource(
    TARGET coro-cloudstorage
    NAMESPACE coro::cloudstorage::util
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/coro/cloudstorage/util/assets
    INPUT
        ../assets/icons/mimetypes/64/audio-x-generic.svg
        ../assets/icons/mimetypes/64/image-x-generic.svg
        ../assets/icons/mimetypes/64/unknown.svg
        ../assets/icons/mimetypes/64/video-x-generic.svg
        ../assets/icons/places/64/folder.svg
        ../assets/icons/places/64/user-trash.svg
        ../assets/icons/actions/32/settings-configure.svg
        ../assets/icons/actions/32/go-previous.svg
        ../assets/icons-dark/mimetypes/64/audio-x-generic.svg
        ../assets/icons-dark/mimetypes/64/image-x-generic.svg
        ../assets/icons-dark/mimetypes/64/unknown.svg
        ../assets/icons-dark/mimetypes/64/video-x-generic.svg
        ../assets/icons-dark/places/64/folder.svg
        ../assets/icons-dark/places/64/user-trash.svg
        ../assets/icons-dark/actions/32/settings-configure.svg
        ../assets/icons-dark/actions/32/go-previous.svg
        ../assets/html/mega_login.html
        ../assets/html/local_login.html
        ../assets/html/webdav_login.html
        ../assets/html/amazons3_login.html
        ../assets/html/dash_player.html
        ../assets/html/item_entry.html
        ../assets/html/provider_entry.html
        ../assets/html/account_entry.html
        ../assets/html/home_page.html
        ../assets/html/settings_page.html
        ../assets/html/error_page.html
        ../assets/providers/box.png
        ../assets/providers/yandex.png
        ../assets/providers/dropbox.png
        ../assets/providers/onedrive.png
        ../assets/providers/pcloud.png
        ../assets/providers/webdav.png
        ../assets/providers/google.png
        ../assets/providers/mega.png
        ../assets/providers/amazons3.png
        ../assets/providers/local.png
        ../assets/providers/hubic.png
        ../assets/styles/layout.css
        ../assets/styles/colors_light.css
        ../assets/styles/colors_dark.css
        ../assets/js/account_list_main.js
        ../assets/js/settings_main.js
)

set(CORO_CLOUDSTORAGE_REDIRECT_URI "http://localhost:12345" CACHE STRING "redirect uri")

target_compile_definitions(
    coro-cloudstorage
        PUBLIC CORO_CLOUDSTORAGE_REDIRECT_URI="${CORO_CLOUDSTORAGE_REDIRECT_URI}"
)

function(define_auth_data)
    math(EXPR CNT "${ARGC} - 1")
    foreach(INDEX RANGE 0 ${CNT} 2)
        list(GET ARGN ${INDEX} NAME)
        math(EXPR I "${INDEX} + 1")
        list(GET ARGN ${I} VALUE)
        set(${NAME} "${VALUE}" CACHE STRING "${NAME}")
        target_compile_definitions(coro-cloudstorage PRIVATE ${NAME}="${VALUE}")
    endforeach()
endfunction()

define_auth_data(
    GOOGLE_DRIVE_CLIENT_ID "646432077068-hmvk44qgo6d0a64a5h9ieue34p3j2dcv.apps.googleusercontent.com"
    GOOGLE_DRIVE_CLIENT_SECRET "1f0FG5ch-kKOanTAv1Bqdp9U"
    BOX_CLIENT_ID "zmiv9tv13hunxhyjk16zqv8dmdw0d773"
    BOX_CLIENT_SECRET "IZ0T8WsUpJin7Qt3rHMf7qDAIFAkYZ0R"
    DROPBOX_CLIENT_ID "ktryxp68ae5cicj"
    DROPBOX_CLIENT_SECRET "6evu94gcxnmyr59"
    HUBIC_CLIENT_ID "api_hubic_kHQ5NUmE2yAAeiWfXPwQy2T53M6RP2fe"
    HUBIC_CLIENT_SECRET "Xk1ezMMSGNeU4r0wv3jutleYX9XvXmgg8XVElJjqoDvlDw0KsC9U2tkSxJxYof8V"
    MEGA_API_KEY "ZVhB0Czb"
    MEGA_APP_NAME "coro-cloudstorage"
    ONE_DRIVE_CLIENT_ID "56a1d60f-ea71-40e9-a489-b87fba12a23e"
    ONE_DRIVE_CLIENT_SECRET "zJRAsd0o4E9c33q4OLc7OhY"
    PCLOUD_CLIENT_ID "rjR7bUpwgdz"
    PCLOUD_CLIENT_SECRET "zNtirCfoYfmX5aFzoavWikKWyMlV"
    YANDEX_DISK_CLIENT_ID "04d700d432884c4381c07e760213ed8a"
    YANDEX_DISK_CLIENT_SECRET "197f9693caa64f0ebb51d201110074f9"
)
