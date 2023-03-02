#ifndef CORO_CLOUDBROWSER_ANDROID_CLOUD_FACTORY_CONFIG_H
#define CORO_CLOUDBROWSER_ANDROID_CLOUD_FACTORY_CONFIG_H

#include <functional>
#include <string>

#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/settings_utils.h"
#include "coro/http/cache_http.h"

namespace coro::cloudstorage::util {

struct CloudFactoryConfig {
  coro::http::CacheHttpConfig http_cache_config = {};
  std::string config_path = GetConfigFilePath();
  std::function<std::string(std::string_view account_type,
                            std::string_view username)>
      post_auth_redirect_uri = GetDefaultPostAuthRedirectUri;
  AuthData auth_data = GetDefaultAuthData();

  static std::string GetDefaultPostAuthRedirectUri(
      std::string_view account_type, std::string_view username);

  static AuthData GetDefaultAuthData();
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDBROWSER_ANDROID_CLOUD_FACTORY_CONFIG_H
