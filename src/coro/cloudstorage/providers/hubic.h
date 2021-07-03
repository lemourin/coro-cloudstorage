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

    template <http::HttpClient HttpT, typename FetchJson>
    static Task<OpenStack::Auth::AuthToken> GetOpenStackAuthToken(
        const HttpT& http, const FetchJson& fetch,
        stdx::stop_token stop_token) {
      using Request = http::Request<std::string>;
      auto credentials = co_await fetch(
          Request{.url = GetEndpoint("/account/credentials")}, stop_token);
      OpenStack::Auth::AuthToken openstack_auth_token = {
          .endpoint = credentials.at("endpoint"),
          .token = credentials.at("token")};
      nlohmann::json bucket = co_await util::FetchJson(
          http,
          Request{.url = openstack_auth_token.endpoint,
                  .headers = {{"X-Auth-Token", openstack_auth_token.token}}},
          std::move(stop_token));
      if (bucket.empty()) {
        throw CloudException("no buckets");
      }
      openstack_auth_token.bucket = bucket[0].at("name");
      co_return openstack_auth_token;
    }

    template <http::HttpClient Http>
    static Task<AuthToken> ExchangeAuthorizationCode(
        const Http& http, AuthData auth_data, std::string code,
        stdx::stop_token stop_token) {
      using Request = http::Request<std::string>;
      Request request{
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
      auto openstack_auth_token = co_await GetOpenStackAuthToken(
          http,
          [&](auto request, stdx::stop_token stop_token) {
            request.headers.emplace_back("Authorization",
                                         util::StrCat("Bearer ", access_token));
            return util::FetchJson(http, std::move(request),
                                   std::move(stop_token));
          },
          std::move(stop_token));
      co_return AuthToken{
          .access_token = std::move(access_token),
          .refresh_token = json.at("refresh_token"),
          .openstack_auth_token = std::move(openstack_auth_token)};
    }

    template <http::HttpClient Http>
    struct RefreshAccessToken {
      Task<AuthToken> operator()(AuthToken auth_token,
                                 stdx::stop_token stop_token) const {
        auto request = http::Request<std::string>{
            .url = "https://api.hubic.com/oauth/token",
            .method = http::Method::kPost,
            .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
            .body = http::FormDataToString(
                {{"refresh_token", auth_token.refresh_token},
                 {"client_id", auth_data.client_id},
                 {"client_secret", auth_data.client_secret},
                 {"grant_type", "refresh_token"}})};
        auto json = co_await util::FetchJson(*http, std::move(request),
                                             std::move(stop_token));
        auth_token.access_token = json["access_token"];
        auth_token.openstack_auth_token = *current_openstack_token;
        co_return auth_token;
      }

      const Http* http;
      const OpenStack::Auth::AuthToken* current_openstack_token;
      AuthData auth_data;
    };
  };

  static std::string GetEndpoint(std::string_view endpoint) {
    return util::StrCat("https://api.hubic.com/1.0", endpoint);
  }

  template <http::HttpClient Http, typename OnAuthTokenUpdated>
  class CloudProvider;

  template <typename AuthManager>
  struct RefreshOpenStackToken {
    using AuthToken = OpenStack::Auth::AuthToken;

    Task<AuthToken> operator()(const AuthToken&,
                               stdx::stop_token stop_token) const {
      co_return co_await Auth::GetOpenStackAuthToken(
          auth_manager->GetHttp(),
          [&](auto request, stdx::stop_token stop_token) {
            return auth_manager->FetchJson(std::move(request),
                                           std::move(stop_token));
          },
          std::move(stop_token));
    }

    AuthManager* auth_manager;
  };

  static constexpr std::string_view kId = "hubic";
  static inline constexpr auto& kIcon = util::kAssetsProvidersHubicPng;
};

template <http::HttpClient Http, typename OnAuthTokenUpdated>
class HubiC::CloudProvider
    : public coro::cloudstorage::CloudProvider<
          HubiC, CloudProvider<Http, OnAuthTokenUpdated>> {
 public:
  using AuthManager = util::AuthManager<Http, Auth, OnAuthTokenUpdated,
                                        Auth::RefreshAccessToken<Http>>;

  struct OnOpenStackTokenUpdated {
    void operator()(OpenStack::Auth::AuthToken auth_token) const {
      auto new_auth_token = auth_manager->GetAuthToken();
      new_auth_token.openstack_auth_token = std::move(auth_token);
      auth_manager->OnAuthTokenUpdated(std::move(new_auth_token));
    }
    AuthManager* auth_manager;
  };

  using OpenStackAuthManager =
      util::AuthManager<Http, OpenStack::Auth, OnOpenStackTokenUpdated,
                        RefreshOpenStackToken<AuthManager>, AuthorizeRequest>;

  CloudProvider(const Http& http, Auth::AuthToken auth_token,
                Auth::AuthData auth_data,
                OnAuthTokenUpdated on_auth_token_updated)
      : auth_manager_(http, std::move(auth_token),
                      std::move(on_auth_token_updated),
                      Auth::RefreshAccessToken<Http>{
                          .http = &http, .auth_data = std::move(auth_data)},
                      util::AuthorizeRequest{}),
        provider_(CreateOpenStackProvider()) {
    auth_manager_.refresh_token().current_openstack_token =
        &provider_.auth_token();
  }

  CloudProvider(CloudProvider&& other) noexcept
      : auth_manager_(std::move(other.auth_manager_)),
        provider_(CreateOpenStackProvider()) {
    auth_manager_.refresh_token().current_openstack_token =
        &provider_.auth_token();
  }

  CloudProvider& operator=(CloudProvider&& other) noexcept {
    auth_manager_ = std::move(other.auth_manager_);
    provider_ = CreateOpenStackProvider();
    auth_manager_.refresh_token().current_openstack_token =
        &provider_.auth_token();
  }

  auto GetRoot(stdx::stop_token stop_token) const {
    return provider_.GetRoot(std::move(stop_token));
  }

  auto ListDirectoryPage(Directory directory,
                         std::optional<std::string> page_token,
                         stdx::stop_token stop_token) {
    return provider_.ListDirectoryPage(
        std::move(directory), std::move(page_token), std::move(stop_token));
  }

  auto GetFileContent(File file, http::Range range,
                      stdx::stop_token stop_token) {
    return provider_.GetFileContent(std::move(file), range,
                                    std::move(stop_token));
  }

  Task<Directory> CreateDirectory(Directory parent, std::string_view name,
                                  stdx::stop_token stop_token) {
    return provider_.CreateDirectory(std::move(parent), name,
                                     std::move(stop_token));
  }

  template <typename Item>
  Task<> RemoveItem(Item item, stdx::stop_token stop_token) {
    return provider_.RemoveItem(std::move(item), std::move(stop_token));
  }

  template <typename Item>
  Task<Item> MoveItem(Item source, Directory destination,
                      stdx::stop_token stop_token) {
    return provider_.MoveItem(std::move(source), std::move(destination),
                              std::move(stop_token));
  }

  template <typename Item>
  Task<Item> RenameItem(Item item, std::string new_name,
                        stdx::stop_token stop_token) {
    return provider_.RenameItem(std::move(item), std::move(new_name),
                                std::move(stop_token));
  }

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token) {
    return provider_.CreateFile(std::move(parent), name, std::move(content),
                                std::move(stop_token));
  }

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
  auto CreateOpenStackProvider() {
    return OpenStack::CloudProvider<OpenStackAuthManager>(
        OpenStackAuthManager(auth_manager_.GetHttp(),
                             auth_manager_.GetAuthToken().openstack_auth_token,
                             OnOpenStackTokenUpdated{&auth_manager_},
                             RefreshOpenStackToken<AuthManager>{&auth_manager_},
                             OpenStack::AuthorizeRequest{}));
  }

  AuthManager auth_manager_;
  OpenStack::CloudProvider<OpenStackAuthManager> provider_;
};

template <>
struct CreateCloudProvider<HubiC> {
  template <typename F, typename CloudFactory, typename OnTokenUpdated>
  auto operator()(const F& create, const CloudFactory& factory,
                  HubiC::Auth::AuthToken auth_token,
                  OnTokenUpdated on_token_updated) const {
    using CloudProviderT =
        HubiC::CloudProvider<typename CloudFactory::Http, OnTokenUpdated>;
    return create.template operator()<CloudProviderT>(
        *factory.http_, std::move(auth_token),
        factory.template GetAuthData<HubiC>(), std::move(on_token_updated));
  }
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