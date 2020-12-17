#ifndef CORO_CLOUDSTORAGE_SERIALIZE_UTILS_H
#define CORO_CLOUDSTORAGE_SERIALIZE_UTILS_H

#include <coro/http/http_parse.h>
#include <coro/stdx/concepts.h>

namespace coro::cloudstorage::util {

template <typename T>
concept HasEndpoint = requires(T v) {
  { v.endpoint }
  ->stdx::convertible_to<std::string>;
};

template <typename AuthToken>
auto ToJson(AuthToken token) {
  nlohmann::json json;
  json["access_token"] = std::move(token.access_token);
  json["refresh_token"] = std::move(token.refresh_token);
  if constexpr (HasEndpoint<AuthToken>) {
    json["endpoint"] = std::move(token.endpoint);
  }
  return json;
}

template <>
auto ToJson<Mega::AuthToken>(Mega::AuthToken token) {
  nlohmann::json json;
  json["session"] = http::ToBase64(token.session);
  return json;
}

template <typename AuthToken>
auto ToAuthToken(const nlohmann::json& json) {
  AuthToken auth_token{.access_token = json.at("access_token"),
                       .refresh_token = json.at("refresh_token")};
  if constexpr (HasEndpoint<AuthToken>) {
    auth_token.endpoint = json.at("endpoint");
  }
  return auth_token;
}

template <>
auto ToAuthToken<Mega::AuthToken>(const nlohmann::json& json) {
  return Mega::AuthToken{.session =
                             http::FromBase64(std::string(json.at("session")))};
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_SERIALIZE_UTILS_H
