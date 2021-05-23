#ifndef CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H
#define CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H

#include <coro/cloudstorage/cloud_factory.h>
#include <coro/cloudstorage/util/serialize_utils.h>
#include <coro/util/type_list.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace coro::cloudstorage::util {

template <typename CloudProviderT>
struct AuthToken : CloudProviderT::Auth::AuthToken {
  using CloudProvider = CloudProviderT;
  std::string id;
};

namespace internal {

template <typename>
struct LoadToken;

template <typename... CloudProviders>
struct LoadToken<coro::util::TypeList<CloudProviders...>> {
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
        try {
          (PutToken<CloudProviders>(entry, result) || ...);
        } catch (const nlohmann::json::exception&) {
        }
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

std::string GetDirectoryPath(std::string_view path);

void CreateDirectory(std::string_view path);

}  // namespace internal

std::string GetConfigFilePath(std::string_view app_name = "coro-cloudstorage",
                              std::string_view file_name = "config.json");

class AuthTokenManager {
 public:
  AuthTokenManager(std::string path = GetConfigFilePath())
      : path_(std::move(path)) {}

  template <typename CloudProviderList>
  auto LoadTokenData() const {
    return internal::LoadToken<CloudProviderList>{}(path_);
  }

  template <typename CloudProvider>
  void SaveToken(typename CloudProvider::Auth::AuthToken token,
                 std::string_view id) const {
    auto token_file = path_;
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
    internal::CreateDirectory(internal::GetDirectoryPath(token_file));
    std::ofstream{token_file} << json;
  }

  template <typename CloudProvider>
  void RemoveToken(std::string_view id) const {
    RemoveToken(id, GetCloudProviderId<CloudProvider>());
  }

 private:
  void RemoveToken(std::string_view id, std::string_view provider_id) const;

  std::string path_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H
