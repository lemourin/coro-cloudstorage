#ifndef CORO_CLOUDSTORAGE_SERIALIZE_UTILS_H
#define CORO_CLOUDSTORAGE_SERIALIZE_UTILS_H

#include <coro/http/http_parse.h>

namespace coro::cloudstorage::util {

template <typename AuthToken>
auto ToJson(AuthToken token) {
  nlohmann::json json;
  json["access_token"] = std::move(token.access_token);
  json["refresh_token"] = std::move(token.refresh_token);
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
  return AuthToken{.access_token = json.at("access_token"),
                   .refresh_token = json.at("refresh_token")};
}

template <>
auto ToAuthToken<Mega::AuthToken>(const nlohmann::json& json) {
  return Mega::AuthToken{.session =
                             http::FromBase64(std::string(json.at("session")))};
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_SERIALIZE_UTILS_H
