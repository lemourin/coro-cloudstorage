#ifndef CORO_CLOUDSTORAGE_HUBIC_H
#define CORO_CLOUDSTORAGE_HUBIC_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/providers/open_stack.h>
#include <coro/cloudstorage/util/assets.h>
#include <coro/cloudstorage/util/auth_manager.h>
#include <coro/cloudstorage/util/fetch_json.h>
#include <coro/cloudstorage/util/string_utils.h>
#include <coro/http/http.h>
#include <coro/http/http_parse.h>

#include <iostream>
#include <string_view>

namespace coro::cloudstorage {

class HubiC : public OpenStack {
 public:
  struct GeneralData {
    std::string username;
    int64_t space_used;
    int64_t space_total;
  };

  struct Auth {
    struct AuthData {
      std::string client_id;
      std::string client_secret;
      std::string redirect_uri;
      std::string state;
    };

    struct AuthToken {
      std::string access_token;
      std::string refresh_token;
      OpenStack::Auth::AuthToken openstack_auth_token;
    };

    static std::string GetAuthorizationUrl(const AuthData& data) {
      return util::StrCat("https://api.hubic.com/oauth/auth", "?",
                          http::FormDataToString(
                              {{"client_id", data.client_id},
                               {"response_type", "code"},
                               {"redirect_uri", data.redirect_uri},
                               {"state", data.state},
                               {"scope", "credentials.r,account.r,usage.r"}}));
    }

    template <http::HttpClient Http>
    static Task<AuthToken> ExchangeAuthorizationCode(
        const Http& http, AuthData auth_data, std::string code,
        stdx::stop_token stop_token) {
      http::Request<std::string> request{
          .url = "https://api.hubic.com/oauth/token",
          .method = http::Method::kPost,
          .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
          .body = http::FormDataToString(
              {{"grant_type", "authorization_code"},
               {"client_secret", auth_data.client_secret},
               {"client_id", auth_data.client_id},
               {"redirect_uri", auth_data.redirect_uri},
               {"code", std::move(code)}})};
      auto json =
          co_await util::FetchJson(http, std::move(request), stop_token);
      std::string access_token = json.at("access_token");
      auto credentials = co_await util::FetchJson(
          http,
          http::Request<std::string>{
              .url = GetEndpoint("/account/credentials"),
              .headers = {{"Authorization",
                           util::StrCat("Bearer ", access_token)}}},
          std::move(stop_token));
      co_return AuthToken{
          .access_token = std::move(access_token),
          .refresh_token = json.at("refresh_token"),
          .openstack_auth_token = {.endpoint = credentials.at("endpoint"),
                                   .token = credentials.at("token")}};
    }

    template <http::HttpClient Http>
    static Task<AuthToken> RefreshAccessToken(const Http& http,
                                              AuthData auth_data,
                                              AuthToken auth_token,
                                              stdx::stop_token stop_token) {
      auto request = http::Request<std::string>{
          .url = "https://api.hubic.com/oauth/token",
          .method = http::Method::kPost,
          .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
          .body = http::FormDataToString(
              {{"refresh_token", auth_token.refresh_token},
               {"client_id", auth_data.client_id},
               {"client_secret", auth_data.client_secret},
               {"grant_type", "refresh_token"}})};
      auto json = co_await util::FetchJson(http, std::move(request),
                                           std::move(stop_token));
      auth_token.access_token = json["access_token"];
      co_return auth_token;
    }
  };

  static std::string GetEndpoint(std::string_view endpoint) {
    return util::StrCat("https://api.hubic.com/1.0", endpoint);
  }

  template <typename AuthManager, typename>
  class CloudProvider;

  static constexpr std::string_view kId = "hubic";
  static inline constexpr auto& kIcon = util::kAssetsProvidersHubicPng;
};

template <typename AuthManager,
          typename BaseCloudProvider =
              OpenStack::CloudProvider<typename AuthManager::Http, HubiC>>
class HubiC::CloudProvider : public BaseCloudProvider {
 public:
  explicit CloudProvider(AuthManager auth_manager)
      : BaseCloudProvider(&auth_manager.GetHttp(),
                          auth_manager.GetAuthToken().openstack_auth_token),
        auth_manager_(std::move(auth_manager)) {}

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) {
    auto [json1, json2] = co_await WhenAll(
        auth_manager_.FetchJson(
            http::Request<std::string>{.url = GetEndpoint("/account")},
            stop_token),
        auth_manager_.FetchJson(
            http::Request<std::string>{.url = GetEndpoint("/account/usage")},
            stop_token));
    co_return GeneralData{.username = json1["email"],
                          .space_used = json2["used"],
                          .space_total = json2["quota"]};
  }

 private:
  AuthManager auth_manager_;
};

namespace util {

template <>
inline nlohmann::json ToJson<HubiC::Auth::AuthToken>(
    HubiC::Auth::AuthToken token) {
  nlohmann::json json;
  json["access_token"] = std::move(token.access_token);
  json["refresh_token"] = std::move(token.refresh_token);
  json["openstack_auth_token"] = ToJson(token.openstack_auth_token);
  return json;
}

template <>
inline HubiC::Auth::AuthToken ToAuthToken<HubiC::Auth::AuthToken>(
    const nlohmann::json& json) {
  HubiC::Auth::AuthToken token;
  token.access_token = json.at("access_token");
  token.refresh_token = json.at("refresh_token");
  token.openstack_auth_token =
      ToAuthToken<OpenStack::Auth::AuthToken>(json.at("openstack_auth_token"));
  return token;
}

template <>
inline HubiC::Auth::AuthData GetAuthData<HubiC>() {
  return {
      .client_id = "api_hubic_kHQ5NUmE2yAAeiWfXPwQy2T53M6RP2fe",
      .client_secret =
          "Xk1ezMMSGNeU4r0wv3jutleYX9XvXmgg8XVElJjqoDvlDw0KsC9U2tkSxJxYof8V"};
}
}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_HUBIC_H