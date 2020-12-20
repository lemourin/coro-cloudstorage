#include <coro/cloudstorage/cloud_factory.h>
#include <coro/cloudstorage/providers/dropbox.h>
#include <coro/cloudstorage/providers/google_drive.h>
#include <coro/cloudstorage/providers/mega.h>
#include <coro/cloudstorage/providers/one_drive.h>
#include <coro/cloudstorage/providers/youtube.h>
#include <coro/cloudstorage/util/account_manager_handler.h>
#include <coro/cloudstorage/util/auth_handler.h>
#include <coro/cloudstorage/util/auth_token_manager.h>
#include <coro/cloudstorage/util/proxy_handler.h>
#include <coro/cloudstorage/util/serialize_utils.h>
#include <coro/cloudstorage/util/webdav_utils.h>
#include <coro/http/curl_http.h>
#include <coro/http/http_parse.h>
#include <coro/http/http_server.h>
#include <coro/stdx/any_invocable.h>
#include <coro/stdx/coroutine.h>
#include <coro/util/for_each.h>

#include <csignal>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <regex>

using ::coro::Generator;
using ::coro::Semaphore;
using ::coro::Task;
using ::coro::cloudstorage::CloudException;
using ::coro::cloudstorage::CloudProvider;
using ::coro::cloudstorage::GetCloudProviderId;
using ::coro::cloudstorage::util::AccountManagerHandler;
using ::coro::cloudstorage::util::ElementData;
using ::coro::cloudstorage::util::GetElement;
using ::coro::cloudstorage::util::GetMultiStatusResponse;
using ::coro::cloudstorage::util::ProxyHandler;
using ::coro::cloudstorage::util::ToAuthToken;
using ::coro::cloudstorage::util::ToJson;
using ::coro::http::CurlHttp;
using ::coro::http::HttpServer;
using ::coro::http::Request;
using ::coro::http::Response;
using ::coro::util::ForEach;
using ::coro::util::MakePointer;

using CloudProviders = ::coro::util::TypeList<
    coro::cloudstorage::GoogleDrive, coro::cloudstorage::Mega,
    coro::cloudstorage::OneDrive, coro::cloudstorage::Dropbox,
    coro::cloudstorage::YouTube>;

constexpr std::string_view kRedirectUri = "http://localhost:12345";
constexpr std::string_view kTokenFile = "access-token.json";

struct AuthData {
  template <typename CloudProvider>
  auto operator()() const {
    using AuthData = typename CloudProvider::Auth::AuthData;

    if constexpr (std::is_same_v<CloudProvider,
                                 coro::cloudstorage::GoogleDrive>) {
      return AuthData{
          .client_id =
              R"(646432077068-hmvk44qgo6d0a64a5h9ieue34p3j2dcv.apps.googleusercontent.com)",
          .client_secret = "1f0FG5ch-kKOanTAv1Bqdp9U",
          .redirect_uri = std::string(kRedirectUri) + "/auth/google"};
    } else if constexpr (std::is_same_v<CloudProvider,
                                        coro::cloudstorage::YouTube>) {
      return AuthData{
          .client_id =
              R"(379556609343-0v8r2fpijkjpj707a76no2rve6nto2co.apps.googleusercontent.com)",
          .client_secret = "_VUpM5Pf9_54RIZq2GGUbUMZ",
          .redirect_uri = std::string(kRedirectUri) + "/auth/youtube"};
    } else if constexpr (std::is_same_v<CloudProvider,
                                        coro::cloudstorage::Mega>) {
      return AuthData{.api_key = "ZVhB0Czb", .app_name = "coro-cloudstorage"};
    } else if constexpr (std::is_same_v<CloudProvider,
                                        coro::cloudstorage::OneDrive>) {
      return AuthData{
          .client_id = "56a1d60f-ea71-40e9-a489-b87fba12a23e",
          .client_secret = "zJRAsd0o4E9c33q4OLc7OhY",
          .redirect_uri = std::string(kRedirectUri) + "/auth/onedrive"};
    } else {
      static_assert(std::is_same_v<CloudProvider, coro::cloudstorage::Dropbox>);
      return AuthData{
          .client_id = "ktryxp68ae5cicj",
          .client_secret = "6evu94gcxnmyr59",
          .redirect_uri = std::string(kRedirectUri) + "/auth/dropbox"};
    }
  }
};

using HandlerType = coro::stdx::any_invocable<Task<Response<>>(
    Request<>, coro::stdx::stop_token)>;

template <typename CloudFactory>
class HttpHandler {
 public:
  struct AuthTokenManager : coro::cloudstorage::util::AuthTokenManager {
    template <typename CloudProvider>
    void OnAuthTokenCreated(typename CloudProvider::Auth::AuthToken token) {
      SaveToken<CloudProvider>(token);
      handler->AddProxyHandler<CloudProvider>(token);
    }
    template <typename CloudProvider>
    void OnCloudProviderRemoved() {
      RemoveToken<CloudProvider>();
      handler->RemoveCloudProvider<CloudProvider>();
    }
    HttpHandler* handler;
  };

  explicit HttpHandler(const CloudFactory& factory)
      : factory_(factory),
        auth_handler_(
            factory,
            AuthTokenManager{{.token_file = std::string(kTokenFile)}, this}) {}

  HttpHandler(const HttpHandler&) = delete;

  Task<Response<>> operator()(Request<> request,
                              coro::stdx::stop_token stop_token) {
    std::cerr << coro::http::MethodToString(request.method) << " "
              << request.url << "\n";
    if (auth_handler_.CanHandleUrl(request.url)) {
      co_return co_await auth_handler_(std::move(request), stop_token);
    }
    for (auto& handler : handlers_) {
      if (std::regex_match(request.url, handler.regex)) {
        co_return co_await handler.handler(std::move(request), stop_token);
      }
    }
    co_return coro::http::Response<>{.status = 302,
                                     .headers = {{"Location", "/"}}};
  }

 private:
  template <typename CloudProvider>
  void RemoveCloudProvider() {
    handlers_.erase(std::find_if(
        std::begin(handlers_), std::end(handlers_), [](const Handler& handler) {
          return handler.id == GetCloudProviderId<CloudProvider>();
        }));
  }

  template <typename CloudProvider,
            typename AuthToken = typename CloudProvider::Auth::AuthToken>
  void AddProxyHandler(AuthToken token) {
    auto prefix = "/" + std::string(GetCloudProviderId<CloudProvider>());
    handlers_.emplace_back(
        Handler{.id = std::string(GetCloudProviderId<CloudProvider>()),
                .regex = std::regex(prefix + "(.*$)"),
                .handler = HandlerType(ProxyHandler(
                    factory_.template Create<CloudProvider>(
                        std::move(token), SaveToken<CloudProvider>{}),
                    prefix))});
  }

  template <typename CloudProvider>
  struct SaveToken {
    void operator()(typename CloudProvider::Auth::AuthToken token) const {
      coro::cloudstorage::util::AuthTokenManager{.token_file =
                                                     std::string(kTokenFile)}
          .SaveToken<CloudProvider>(token);
    }
  };

  struct Handler {
    std::string id;
    std::regex regex;
    HandlerType handler;
  };
  const CloudFactory& factory_;
  std::vector<Handler> handlers_;
  AccountManagerHandler<CloudProviders, CloudFactory, AuthTokenManager>
      auth_handler_;
};

Task<> CoMain(event_base* event_loop) noexcept {
  try {
    CurlHttp http(event_loop);
    Semaphore quit;
    coro::cloudstorage::CloudFactory cloud_factory(event_loop, http,
                                                   AuthData{});

    Semaphore semaphore;
    HttpServer http_server(
        event_loop, {.address = "0.0.0.0", .port = 12345},
        HandlerType(std::make_unique<HttpHandler<decltype(cloud_factory)>>(
            cloud_factory)),
        [&] { semaphore.resume(); });
    co_await semaphore;
  } catch (const std::exception& exception) {
    std::cerr << "EXCEPTION: " << exception.what() << "\n";
  }
}

int main() {
#ifdef _WIN32
  WORD version_requested = MAKEWORD(2, 2);
  WSADATA wsa_data;

  (void)WSAStartup(version_requested, &wsa_data);
#endif

#ifdef SIGPIPE
  signal(SIGPIPE, SIG_IGN);
#endif

  auto base = MakePointer(event_base_new(), event_base_free);
  CoMain(base.get());
  return event_base_dispatch(base.get());
}