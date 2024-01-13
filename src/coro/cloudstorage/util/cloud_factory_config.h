#ifndef CORO_CLOUDBROWSER_ANDROID_CLOUD_FACTORY_CONFIG_H
#define CORO_CLOUDBROWSER_ANDROID_CLOUD_FACTORY_CONFIG_H

#include <functional>
#include <random>
#include <string>

#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/file_utils.h"
#include "coro/cloudstorage/util/random_number_generator.h"
#include "coro/cloudstorage/util/settings_utils.h"
#include "coro/http/cache_http.h"
#include "coro/http/curl_http.h"

namespace coro::cloudstorage::util {

std::string GetDefaultPostAuthRedirectUri(std::string_view account_type,
                                          std::string_view username);

AuthData GetDefaultAuthData();

struct CloudFactoryConfig {
  const coro::util::EventLoop* event_loop = nullptr;
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
  http::Http http{http::CurlHttp(
      event_loop,
      http::CurlHttpConfig{.alt_svc_path = StrCat(GetDirectoryPath(cache_path),
                                                  "/alt-svc.txt")})};
  util::RandomNumberGenerator random_number_generator{
      std::mt19937(std::random_device()())};
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDBROWSER_ANDROID_CLOUD_FACTORY_CONFIG_H
