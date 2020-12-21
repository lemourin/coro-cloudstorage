#ifndef CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H
#define CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H

#include <coro/cloudstorage/util/serialize_utils.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>

namespace coro::cloudstorage::util {

namespace internal {

template <typename>
struct LoadToken;

template <typename... CloudProviders>
struct LoadToken<coro::util::TypeList<CloudProviders...>> {
  template <typename CloudProviderT>
  struct AuthToken : CloudProviderT::Auth::AuthToken {
    using CloudProvider = CloudProviderT;
    std::string id;
  };

  using AnyToken = std::variant<AuthToken<CloudProviders>...>;

  std::vector<AnyToken> operator()(std::string_view token_file) const {
    try {
      std::ifstream file{std::string(token_file)};
      if (!file) {
        return {};
      }
      nlohmann::json json;
      file >> json;
      std::vector<AnyToken> result;
      for (const auto& entry : json["auth_token"]) {
        (PutToken<CloudProviders>(entry, result) || ...);
      }
      return result;
    } catch (const nlohmann::json::exception&) {
      return {};
    }
  }

  template <typename CloudProvider>
  bool PutToken(const nlohmann::json& json,
                std::vector<AnyToken>& result) const {
    if (json["type"] == std::string(GetCloudProviderId<CloudProvider>())) {
      result.emplace_back(AuthToken<CloudProvider>{
          {ToAuthToken<typename CloudProvider::Auth::AuthToken>(json)},
          {std::string(json["id"])}});
      return true;
    } else {
      return false;
    }
  }
};

}  // namespace internal

struct AuthTokenManager {
  template <typename CloudProviderList>
  auto LoadTokenData() const {
    return internal::LoadToken<CloudProviderList>{}(token_file);
  }

  template <typename CloudProvider>
  void SaveToken(typename CloudProvider::Auth::AuthToken token,
                 std::string_view id) const {
    nlohmann::json json;
    {
      std::ifstream input_token_file{token_file};
      if (input_token_file) {
        input_token_file >> json;
      }
    }
    bool found = false;
    for (auto& entry : json["auth_token"]) {
      if (entry["type"] == std::string(GetCloudProviderId<CloudProvider>()) &&
          entry["id"] == std::string(id)) {
        entry = ToJson(std::move(token));
        entry["id"] = id;
        entry["type"] = GetCloudProviderId<CloudProvider>();
        found = true;
        break;
      }
    }
    if (!found) {
      auto token_json = ToJson(std::move(token));
      token_json["id"] = id;
      token_json["type"] = GetCloudProviderId<CloudProvider>();
      json["auth_token"].emplace_back(std::move(token_json));
    }
    std::ofstream{token_file} << json;
  }

  template <typename CloudProvider>
  void RemoveToken(std::string_view id) const {
    nlohmann::json json;
    {
      std::ifstream input_token_file{token_file};
      if (input_token_file) {
        input_token_file >> json;
      }
    }
    nlohmann::json result;
    for (auto token : json["auth_token"]) {
      if (token["type"] != std::string(GetCloudProviderId<CloudProvider>()) ||
          token["id"] != std::string(id)) {
        result["auth_token"].emplace_back(std::move(token));
      }
    }
    std::ofstream{token_file} << result;
  }

  std::string token_file;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H
