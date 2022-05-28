#ifndef CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H
#define CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "coro/cloudstorage/cloud_factory.h"
#include "coro/cloudstorage/util/serialize_utils.h"
#include "coro/cloudstorage/util/settings_utils.h"
#include "coro/util/type_list.h"

namespace coro::cloudstorage::util {

template <typename CloudProviderT>
struct AuthToken : CloudProviderT::Auth::AuthToken {
  using CloudProvider = CloudProviderT;
  std::string id;
};

struct AuthToken2 : AbstractCloudProvider::Auth::AuthToken {
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
      nlohmann::json json = ReadSettings(token_file);
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

}  // namespace internal

class AuthTokenManager {
 public:
  AuthTokenManager(AbstractCloudFactory* factory,
                   std::string path = GetConfigFilePath())
      : factory_(factory), path_(std::move(path)) {}

  std::vector<AuthToken2> LoadTokenData2() const;

  template <typename CloudProviderList>
  auto LoadTokenData() const {
    return internal::LoadToken<CloudProviderList>{}(path_);
  }

  template <typename CloudProvider>
  void SaveToken(typename CloudProvider::Auth::AuthToken token,
                 std::string_view id) const {
    SaveToken(ToJson(std::move(token)), id,
              GetCloudProviderId<CloudProvider>());
  }

  void SaveToken2(AbstractCloudProvider::Auth::AuthToken token,
                  std::string_view id) const;

  template <typename CloudProvider>
  void RemoveToken(std::string_view id) const {
    RemoveToken(id, GetCloudProviderId<CloudProvider>());
  }

  void SaveToken(nlohmann::json token, std::string_view id,
                 std::string_view provider_id) const;
  void RemoveToken(std::string_view id, std::string_view provider_id) const;

 private:
  AbstractCloudFactory* factory_;
  std::string path_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H
