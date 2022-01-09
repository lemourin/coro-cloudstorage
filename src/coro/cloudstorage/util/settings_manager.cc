#include "coro/cloudstorage/util/settings_manager.h"

namespace coro::cloudstorage::util {

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
  return {.address = IsPublicNetworkEnabled() ? "0.0.0.0" : "127.0.0.1",
          .port = 12345};
}

}  // namespace coro::cloudstorage::util