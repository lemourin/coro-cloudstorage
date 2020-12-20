#ifndef CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H
#define CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H

#include <coro/cloudstorage/util/serialize_utils.h>

#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>

namespace coro::cloudstorage::util {

struct AuthTokenManager {
  template <typename CloudProvider,
            typename AuthToken = typename CloudProvider::Auth::AuthToken>
  std::optional<AuthToken> LoadToken() const {
    std::ifstream file{token_file};
    if (file) {
      try {
        nlohmann::json json;
        file >> json;
        return ToAuthToken<AuthToken>(
            json.at(std::string(GetCloudProviderId<CloudProvider>())));
      } catch (const nlohmann::json::exception&) {
      }
    }
    return std::nullopt;
  }

  template <typename CloudProvider,
            typename AuthToken = typename CloudProvider::Auth::AuthToken>
  void SaveToken(AuthToken token) const {
    nlohmann::json json;
    {
      std::ifstream input_token_file{token_file};
      if (input_token_file) {
        input_token_file >> json;
      }
    }
    json[std::string(GetCloudProviderId<CloudProvider>())] =
        ToJson(std::move(token));
    std::ofstream{token_file} << json;
  }

  template <typename CloudProvider>
  void RemoveToken() const {
    nlohmann::json json;
    {
      std::ifstream input_token_file{token_file};
      if (input_token_file) {
        input_token_file >> json;
      }
    }
    json.erase(std::string(GetCloudProviderId<CloudProvider>()));
    std::ofstream{token_file} << json;
  }

  std::string token_file;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H
