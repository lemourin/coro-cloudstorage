#ifndef CORO_CLOUDBROWSER_ANDROID_CLOUD_FACTORY_CONFIG_H
#define CORO_CLOUDBROWSER_ANDROID_CLOUD_FACTORY_CONFIG_H

#include <functional>
#include <string>

#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/file_utils.h"
#include "coro/cloudstorage/util/settings_utils.h"
#include "coro/http/cache_http.h"
#include "coro/http/curl_http.h"

namespace coro::cloudstorage::util {

struct CloudFactoryConfig {
  coro::http::CacheHttpConfig http_cache_config = {};
  std::string config_path = [] {
    std::string path = GetConfigFilePath();
    CreateDirectory(GetDirectoryPath(path));
    return path;
  }();
  std::string cache_path = [] {
    std::string path = GetCacheFilePath();
    CreateDirectory(GetDirectoryPath(path));
    return path;
  }();
  std::function<std::string(std::string_view account_type,
                            std::string_view username)>
      post_auth_redirect_uri = GetDefaultPostAuthRedirectUri;
  AuthData auth_data = GetDefaultAuthData();
  coro::http::CurlHttpConfig http_client_config = {
      .cache_path = GetDirectoryPath(this->cache_path)};

  static std::string GetDefaultPostAuthRedirectUri(
      std::string_view account_type, std::string_view username);

  static AuthData GetDefaultAuthData();
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDBROWSER_ANDROID_CLOUD_FACTORY_CONFIG_H
