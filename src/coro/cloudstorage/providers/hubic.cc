#include "coro/cloudstorage/providers/hubic.h"

namespace coro::cloudstorage {

namespace {

std::string GetEndpoint(std::string_view endpoint) {
  return util::StrCat("https://api.hubic.com/1.0", endpoint);
}

template <typename FetchJson>
Task<OpenStack::Auth::AuthToken> GetOpenStackAuthToken(
    const coro::http::Http& http, const FetchJson& fetch,
    stdx::stop_token stop_token) {
  using Request = http::Request<std::string>;
  auto credentials = co_await fetch(
      Request{.url = GetEndpoint("/account/credentials")}, stop_token);
  OpenStack::Auth::AuthToken openstack_auth_token = {
      .endpoint = credentials.at("endpoint"), .token = credentials.at("token")};
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

struct RefreshAccessToken {
  Task<HubiC::Auth::AuthToken> operator()(HubiC::Auth::AuthToken auth_token,
                                          stdx::stop_token stop_token) const {
    auto request = http::Request<std::string>{
        .url = "https://api.hubic.com/oauth/token",
        .method = http::Method::kPost,
        .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
        .body =
            http::FormDataToString({{"refresh_token", auth_token.refresh_token},
                                    {"client_id", auth_data.client_id},
                                    {"client_secret", auth_data.client_secret},
                                    {"grant_type", "refresh_token"}})};
    auto json = co_await util::FetchJson(*http, std::move(request),
                                         std::move(stop_token));
    auth_token.access_token = json["access_token"];
    auth_token.openstack_auth_token = **current_openstack_token;
    co_return auth_token;
  }

  const coro::http::Http* http;
  const OpenStack::Auth::AuthToken** current_openstack_token;
  HubiC::Auth::AuthData auth_data;
};

struct RefreshOpenStackToken {
  using AuthToken = OpenStack::Auth::AuthToken;

  Task<AuthToken> operator()(const AuthToken&,
                             stdx::stop_token stop_token) const {
    co_return co_await GetOpenStackAuthToken(
        *http,
        [&](auto request, stdx::stop_token stop_token) {
          return auth_manager->FetchJson(std::move(request),
                                         std::move(stop_token));
        },
        std::move(stop_token));
  }

  util::AuthManager3<HubiC::Auth>* auth_manager;
  const coro::http::Http* http;
};

using OnAuthTokenUpdatedT = OnAuthTokenUpdated<HubiC::Auth::AuthToken>;

struct OnOpenStackTokenUpdated {
  void operator()(OpenStack::Auth::AuthToken auth_token) const {
    auto new_auth_token = auth_manager->GetAuthToken();
    new_auth_token.openstack_auth_token = std::move(auth_token);
    auth_manager->OnAuthTokenUpdated(std::move(new_auth_token));
  }
  util::AuthManager3<HubiC::Auth>* auth_manager;
};

using OpenStackAuthManager =
    util::AuthManager<OpenStack::Auth, OnOpenStackTokenUpdated,
                      RefreshOpenStackToken, HubiC::AuthorizeRequest>;

using HubiCAuthManager =
    util::AuthManager<HubiC::Auth, OnAuthTokenUpdatedT, RefreshAccessToken>;

}  // namespace

std::string HubiC::Auth::GetAuthorizationUrl(const AuthData& data) {
  return util::StrCat(
      "https://api.hubic.com/oauth/auth", "?",
      http::FormDataToString({{"client_id", data.client_id},
                              {"response_type", "code"},
                              {"redirect_uri", data.redirect_uri},
                              {"state", data.state},
                              {"scope", "credentials.r,account.r,usage.r"}}));
}

auto HubiC::Auth::ExchangeAuthorizationCode(const coro::http::Http& http,
                                            AuthData auth_data,
                                            std::string code,
                                            stdx::stop_token stop_token)
    -> Task<AuthToken> {
  using Request = http::Request<std::string>;
  Request request{
      .url = "https://api.hubic.com/oauth/token",
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
      .body =
          http::FormDataToString({{"grant_type", "authorization_code"},
                                  {"client_secret", auth_data.client_secret},
                                  {"client_id", auth_data.client_id},
                                  {"redirect_uri", auth_data.redirect_uri},
                                  {"code", std::move(code)}})};
  auto json = co_await util::FetchJson(http, std::move(request), stop_token);
  std::string access_token = json.at("access_token");
  auto openstack_auth_token = co_await GetOpenStackAuthToken(
      http,
      [&](auto request, stdx::stop_token stop_token) {
        request.headers.emplace_back("Authorization",
                                     util::StrCat("Bearer ", access_token));
        return util::FetchJson(http, std::move(request), std::move(stop_token));
      },
      std::move(stop_token));
  co_return AuthToken{.access_token = std::move(access_token),
                      .refresh_token = json.at("refresh_token"),
                      .openstack_auth_token = std::move(openstack_auth_token)};
}

HubiC::CloudProvider::CloudProvider(
    const coro::http::Http* http, Auth::AuthToken auth_token,
    Auth::AuthData auth_data,
    OnAuthTokenUpdated<Auth::AuthToken> on_auth_token_updated)
    : http_(http),
      auth_manager_(HubiCAuthManager(
          http, std::move(auth_token), std::move(on_auth_token_updated),
          RefreshAccessToken{
              .http = http,
              .current_openstack_token = current_openstack_token_.get(),
              .auth_data = std::move(auth_data)},
          util::AuthorizeRequest{})),
      provider_(CreateOpenStackProvider()) {
  *current_openstack_token_ = &provider_.auth_token();
}

HubiC::CloudProvider::CloudProvider(CloudProvider&& other) noexcept
    : http_(other.http_),
      current_openstack_token_(std::move(other.current_openstack_token_)),
      auth_manager_(std::move(other.auth_manager_)),
      provider_(CreateOpenStackProvider()) {
  *current_openstack_token_ = &provider_.auth_token();
}

HubiC::CloudProvider& HubiC::CloudProvider::operator=(
    CloudProvider&& other) noexcept {
  http_ = other.http_;
  current_openstack_token_ = std::move(other.current_openstack_token_);
  auth_manager_ = std::move(other.auth_manager_);
  provider_ = CreateOpenStackProvider();
  *current_openstack_token_ = &provider_.auth_token();
  return *this;
}

auto HubiC::CloudProvider::GetRoot(stdx::stop_token stop_token) const
    -> Task<Directory> {
  return provider_.GetRoot(std::move(stop_token));
}

auto HubiC::CloudProvider::ListDirectoryPage(
    Directory directory, std::optional<std::string> page_token,
    stdx::stop_token stop_token) -> Task<PageData> {
  return provider_.ListDirectoryPage(
      std::move(directory), std::move(page_token), std::move(stop_token));
}

auto HubiC::CloudProvider::GetFileContent(File file, http::Range range,
                                          stdx::stop_token stop_token)
    -> Generator<std::string> {
  return provider_.GetFileContent(std::move(file), range,
                                  std::move(stop_token));
}

auto HubiC::CloudProvider::CreateDirectory(Directory parent,
                                           std::string_view name,
                                           stdx::stop_token stop_token)
    -> Task<Directory> {
  return provider_.CreateDirectory(std::move(parent), name,
                                   std::move(stop_token));
}

template <typename Item>
Task<> HubiC::CloudProvider::RemoveItem(Item item,
                                        stdx::stop_token stop_token) {
  return provider_.RemoveItem(std::move(item), std::move(stop_token));
}

template <typename Item>
Task<Item> HubiC::CloudProvider::MoveItem(Item source, Directory destination,
                                          stdx::stop_token stop_token) {
  return provider_.MoveItem(std::move(source), std::move(destination),
                            std::move(stop_token));
}

template <typename Item>
Task<Item> HubiC::CloudProvider::RenameItem(Item item, std::string new_name,
                                            stdx::stop_token stop_token) {
  return provider_.RenameItem(std::move(item), std::move(new_name),
                              std::move(stop_token));
}

auto HubiC::CloudProvider::CreateFile(Directory parent, std::string_view name,
                                      FileContent content,
                                      stdx::stop_token stop_token)
    -> Task<File> {
  return provider_.CreateFile(std::move(parent), name, std::move(content),
                              std::move(stop_token));
}

auto HubiC::CloudProvider::GetGeneralData(stdx::stop_token stop_token)
    -> Task<GeneralData> {
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

OpenStack::CloudProvider HubiC::CloudProvider::CreateOpenStackProvider() {
  return OpenStack::CloudProvider(
      util::AuthManager3<OpenStack::Auth>(OpenStackAuthManager(
          http_, auth_manager_.GetAuthToken().openstack_auth_token,
          OnOpenStackTokenUpdated{&auth_manager_},
          RefreshOpenStackToken{&auth_manager_, http_},
          OpenStack::AuthorizeRequest{})),
      http_);
}

namespace util {

template <>
nlohmann::json ToJson<HubiC::Auth::AuthToken>(HubiC::Auth::AuthToken token) {
  nlohmann::json json;
  json["access_token"] = std::move(token.access_token);
  json["refresh_token"] = std::move(token.refresh_token);
  json["openstack_auth_token"] = ToJson(token.openstack_auth_token);
  return json;
}

template <>
HubiC::Auth::AuthToken ToAuthToken<HubiC::Auth::AuthToken>(
    const nlohmann::json& json) {
  HubiC::Auth::AuthToken token;
  token.access_token = json.at("access_token");
  token.refresh_token = json.at("refresh_token");
  token.openstack_auth_token =
      ToAuthToken<OpenStack::Auth::AuthToken>(json.at("openstack_auth_token"));
  return token;
}

template <>
HubiC::Auth::AuthData GetAuthData<HubiC>() {
  return {
      .client_id = "api_hubic_kHQ5NUmE2yAAeiWfXPwQy2T53M6RP2fe",
      .client_secret =
          "Xk1ezMMSGNeU4r0wv3jutleYX9XvXmgg8XVElJjqoDvlDw0KsC9U2tkSxJxYof8V"};
}

}  // namespace util

template auto HubiC::CloudProvider::RenameItem<HubiC::File>(
    File item, std::string new_name, stdx::stop_token stop_token) -> Task<File>;

template auto HubiC::CloudProvider::RenameItem<HubiC::Directory>(
    Directory item, std::string new_name, stdx::stop_token stop_token)
    -> Task<Directory>;

template auto HubiC::CloudProvider::MoveItem<HubiC::File>(File, Directory,
                                                          stdx::stop_token)
    -> Task<File>;

template auto HubiC::CloudProvider::MoveItem<HubiC::Directory>(Directory,
                                                               Directory,
                                                               stdx::stop_token)
    -> Task<Directory>;

template auto HubiC::CloudProvider::RemoveItem<HubiC::File>(File item,
                                                            stdx::stop_token)
    -> Task<>;

template auto HubiC::CloudProvider::RemoveItem<HubiC::Directory>(
    Directory item, stdx::stop_token) -> Task<>;

}  // namespace coro::cloudstorage