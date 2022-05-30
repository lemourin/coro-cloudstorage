#include "coro/cloudstorage/util/settings_manager.h"

namespace coro::cloudstorage::util {

namespace {

uint16_t GetPort(std::string_view redirect_uri) {
  return http::ParseUri(redirect_uri).port.value_or(80);
}

}  // namespace

SettingsManager::SettingsManager(AuthTokenManager auth_token_manager,
                                 std::string path)
    : auth_token_manager_(std::move(auth_token_manager)),
      path_(std::move(path)),
      effective_is_public_network_enabled_(IsPublicNetworkEnabled()),
      port_(GetPort(CORO_CLOUDSTORAGE_REDIRECT_URI)) {}

void SettingsManager::SetEnablePublicNetwork(bool enable) const {
  EditSettings(path_, [&](nlohmann::json settings) {
    if (enable) {
      settings["public_network"] = true;
    } else {
      settings.erase("public_network");
    }
    return settings;
  });
}

bool SettingsManager::IsPublicNetworkEnabled() const {
  nlohmann::json settings = ReadSettings(path_);
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

}  // namespace coro::cloudstorage::util