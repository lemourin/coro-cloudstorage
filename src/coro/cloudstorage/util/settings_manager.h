#ifndef CORO_CLOUDSTORAGE_UTIL_SETTINGS_MANAGER_H
#define CORO_CLOUDSTORAGE_UTIL_SETTINGS_MANAGER_H

#include "coro/cloudstorage/util/auth_token_manager.h"
#include "coro/cloudstorage/util/settings_utils.h"
#include "coro/http/http_server.h"

namespace coro::cloudstorage::util {

class SettingsManager {
 public:
  SettingsManager(AuthTokenManager auth_token_manager,
                  std::string path = GetConfigFilePath())
      : auth_token_manager_(std::move(auth_token_manager)),
        path_(std::move(path)),
        effective_is_public_network_enabled_(IsPublicNetworkEnabled()) {}

  auto LoadTokenData() const { return auth_token_manager_.LoadTokenData(); }

  void SaveToken(AbstractCloudProvider::Auth::AuthToken token,
                 std::string_view id) const {
    auth_token_manager_.SaveToken(std::move(token), id);
  }

  void RemoveToken(std::string_view type, std::string_view id) const {
    auth_token_manager_.RemoveToken(type, id);
  }

  void SetEnablePublicNetwork(bool enable) const;
  bool IsPublicNetworkEnabled() const;

  bool EffectiveIsPublicNetworkEnabled() const {
    return effective_is_public_network_enabled_;
  }

  http::HttpServerConfig GetHttpServerConfig() const;

 private:
  AuthTokenManager auth_token_manager_;
  std::string path_;
  bool effective_is_public_network_enabled_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_SETTINGS_MANAGER_H