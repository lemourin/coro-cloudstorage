#ifndef CORO_CLOUDSTORAGE_UTIL_SETTINGS_MANAGER_H
#define CORO_CLOUDSTORAGE_UTIL_SETTINGS_MANAGER_H

#include "coro/cloudstorage/util/auth_token_manager.h"
#include "coro/cloudstorage/util/settings_utils.h"

namespace coro::cloudstorage::util {

class SettingsManager {
 public:
  explicit SettingsManager(std::string path = GetConfigFilePath())
      : auth_token_manager_(path), path_(std::move(path)) {}

  template <typename CloudProviderList>
  auto LoadTokenData() const {
    return auth_token_manager_.LoadTokenData<CloudProviderList>();
  }

  template <typename CloudProvider>
  void SaveToken(typename CloudProvider::Auth::AuthToken token,
                 std::string_view id) const {
    auth_token_manager_.SaveToken<CloudProvider>(std::move(token), id);
  }

  template <typename CloudProvider>
  void RemoveToken(std::string_view id) const {
    auth_token_manager_.RemoveToken<CloudProvider>(id);
  }

 private:
  AuthTokenManager auth_token_manager_;
  std::string path_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_SETTINGS_MANAGER_H