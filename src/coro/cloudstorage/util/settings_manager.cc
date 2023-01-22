#include "coro/cloudstorage/util/settings_manager.h"

#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http_parse.h"

namespace coro::cloudstorage::util {

namespace {

using ::coro::cloudstorage::util::StrCat;

uint16_t GetPort(std::string_view redirect_uri) {
  return http::ParseUri(redirect_uri).port.value_or(80);
}

}  // namespace

SettingsManager::SettingsManager(AuthTokenManager auth_token_manager,
                                 CloudFactoryConfig config)
    : auth_token_manager_(std::move(auth_token_manager)),
      config_(std::move(config)),
      effective_is_public_network_enabled_(IsPublicNetworkEnabled()),
      port_(GetPort(CORO_CLOUDSTORAGE_REDIRECT_URI)) {}

void SettingsManager::SetEnablePublicNetwork(bool enable) const {
  EditSettings(config_.config_path, [&](nlohmann::json settings) {
    if (enable) {
      settings["public_network"] = true;
    } else {
      settings.erase("public_network");
    }
    return settings;
  });
}

bool SettingsManager::IsPublicNetworkEnabled() const {
  nlohmann::json settings = ReadSettings(config_.config_path);
  if (auto it = settings.find("public_network"); it != settings.end()) {
    return *it;
  }
  return false;
}

http::HttpServerConfig SettingsManager::GetHttpServerConfig() const {
  return {
      .address = EffectiveIsPublicNetworkEnabled() ? "0.0.0.0" : "127.0.0.1",
      .port = port_};
}

std::string SettingsManager::GetPostAuthRedirectUri(
    std::string_view account_type, std::string_view username) const {
  return config_.post_auth_redirect_uri(account_type, username);
}

}  // namespace coro::cloudstorage::util