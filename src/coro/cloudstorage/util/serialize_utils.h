#ifndef CORO_CLOUDSTORAGE_SERIALIZE_UTILS_H
#define CORO_CLOUDSTORAGE_SERIALIZE_UTILS_H

#include <coro/http/http_parse.h>
#include <coro/stdx/concepts.h>

#include <nlohmann/json.hpp>

namespace coro::cloudstorage::util {

template <typename T>
concept HasEndpoint = requires(T v) {
  { v.endpoint }
  ->stdx::convertible_to<std::string>;
};

template <typename T>
concept HasRefreshToken = requires(T v) {
  { v.refresh_token }
  ->stdx::convertible_to<std::string>;
};

template <typename AuthToken>
auto ToJson(AuthToken token) {
  nlohmann::json json;
  json["access_token"] = std::move(token.access_token);
  if constexpr (HasRefreshToken<AuthToken>) {
    json["refresh_token"] = std::move(token.refresh_token);
  }
  if constexpr (HasEndpoint<AuthToken>) {
    json["endpoint"] = std::move(token.endpoint);
  }
  return json;
}

template <typename AuthToken>
auto ToAuthToken(const nlohmann::json& json) {
  AuthToken auth_token{.access_token = json.at("access_token")};
  if constexpr (HasRefreshToken<AuthToken>) {
    auth_token.refresh_token = json.at("refresh_token");
  }
  if constexpr (HasEndpoint<AuthToken>) {
    auth_token.endpoint = json.at("endpoint");
  }
  return auth_token;
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_SERIALIZE_UTILS_H
