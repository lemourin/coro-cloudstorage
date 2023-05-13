#ifndef CORO_CLOUDSTORAGE_UTIL_SETTINGS_MANAGER_H
#define CORO_CLOUDSTORAGE_UTIL_SETTINGS_MANAGER_H

#include <any>
#include <functional>

#include "coro/cloudstorage/util/abstract_cloud_factory.h"
#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/cloudstorage/util/cloud_factory_config.h"
#include "coro/cloudstorage/util/settings_utils.h"
#include "coro/http/http_server.h"

namespace coro::cloudstorage::util {

class SettingsManager {
 public:
  struct AuthToken : AbstractCloudProvider::Auth::AuthToken {
    std::string id;
  };

  SettingsManager(AbstractCloudFactory* factory, CloudFactoryConfig config);

  std::vector<AuthToken> LoadTokenData() const;

  void SaveToken(AbstractCloudProvider::Auth::AuthToken token,
                 std::string_view id) const;

  void RemoveToken(std::string_view id, std::string_view type) const;

  void SetEnablePublicNetwork(bool enable) const;
  bool IsPublicNetworkEnabled() const;

  bool EffectiveIsPublicNetworkEnabled() const {
    return effective_is_public_network_enabled_;
  }

  http::HttpServerConfig GetHttpServerConfig() const;

  std::string GetPostAuthRedirectUri(std::string_view account_type,
                                     std::string_view username) const;

 private:
  AbstractCloudFactory* factory_;
  CloudFactoryConfig config_;
  mutable std::any db_;
  bool effective_is_public_network_enabled_;
  uint16_t port_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_SETTINGS_MANAGER_H