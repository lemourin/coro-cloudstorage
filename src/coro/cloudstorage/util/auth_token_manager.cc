#include "coro/cloudstorage/util/auth_token_manager.h"

#include <utility>

namespace coro::cloudstorage::util {

void AuthTokenManager::SaveToken(nlohmann::json token, std::string_view id,
                                 std::string_view provider_id) const {
  EditSettings(path_, [&](nlohmann::json json) {
    bool found = false;
    for (auto& entry : json["auth_token"]) {
      if (entry["type"] == std::string(provider_id) &&
          entry["id"] == std::string(id)) {
        entry = std::move(token);
        entry["id"] = id;
        entry["type"] = provider_id;
        found = true;
        break;
      }
    }
    if (!found) {
      auto token_json = std::move(token);
      token_json["id"] = id;
      token_json["type"] = provider_id;
      json["auth_token"].emplace_back(std::move(token_json));
    }
    return json;
  });
}

void AuthTokenManager::RemoveToken(std::string_view id,
                                   std::string_view provider_id) const {
  EditSettings(path_, [&](nlohmann::json json) {
    nlohmann::json result;
    for (auto token : json["auth_token"]) {
      if (token["type"] != std::string(provider_id) ||
          token["id"] != std::string(id)) {
        result.emplace_back(std::move(token));
      }
    }
    if (!result.is_null()) {
      json["auth_token"] = std::move(result);
    } else {
      json.erase("auth_token");
    }
    return json;
  });
}

std::vector<AuthToken> AuthTokenManager::LoadTokenData() const {
  try {
    nlohmann::json json = ReadSettings(path_);
    std::vector<AuthToken> result;

    for (const auto& entry : json["auth_token"]) {
      try {
        for (auto type : factory_->GetSupportedCloudProviders()) {
          const auto& auth = factory_->GetAuth(type);
          if (std::string(entry["type"]) == auth.GetId()) {
            AuthToken auth_token{{auth.ToAuthToken(entry)}, entry["id"]};
            result.emplace_back(std::move(auth_token));
          }
        }
      } catch (const nlohmann::json::exception&) {
      }
    }
    return result;
  } catch (const nlohmann::json::exception&) {
    return {};
  }
}

void AuthTokenManager::SaveToken(AbstractCloudProvider::Auth::AuthToken token,
                                 std::string_view id) const {
  SaveToken(factory_->GetAuth(token.type).ToJson(token), id,
            factory_->GetAuth(token.type).GetId());
}

}  // namespace coro::cloudstorage::util
