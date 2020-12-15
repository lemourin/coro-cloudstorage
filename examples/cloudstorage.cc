#include <coro/cloudstorage/cloud_factory.h>
#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/providers/google_drive.h>
#include <coro/cloudstorage/providers/mega.h>
#include <coro/cloudstorage/util/auth_handler.h>
#include <coro/cloudstorage/util/proxy_handler.h>
#include <coro/cloudstorage/util/serialize_utils.h>
#include <coro/generator.h>
#include <coro/http/curl_http.h>
#include <coro/http/http_parse.h>
#include <coro/http/http_server.h>
#include <coro/promise.h>
#include <coro/stdx/any_invocable.h>
#include <coro/stdx/coroutine.h>
#include <coro/util/for_each.h>
#include <coro/util/function_traits.h>
#include <coro/util/make_pointer.h>
#include <coro/wait_task.h>

#include <csignal>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <regex>

using ::coro::Generator;
using ::coro::InterruptedException;
using ::coro::Promise;
using ::coro::Semaphore;
using ::coro::Task;
using ::coro::Wait;
using ::coro::cloudstorage::CloudProvider;
using ::coro::cloudstorage::GetCloudProviderId;
using ::coro::cloudstorage::GoogleDrive;
using ::coro::cloudstorage::MakeCloudFactory;
using ::coro::cloudstorage::Mega;
using ::coro::cloudstorage::util::MakeAuthHandler;
using ::coro::cloudstorage::util::MakeProxyHandler;
using ::coro::cloudstorage::util::ToAuthToken;
using ::coro::cloudstorage::util::ToJson;
using ::coro::http::CurlHttp;
using ::coro::http::DecodeUri;
using ::coro::http::FromBase64;
using ::coro::http::GetExtension;
using ::coro::http::GetMimeType;
using ::coro::http::HttpServer;
using ::coro::http::ParseQuery;
using ::coro::http::ParseRange;
using ::coro::http::ParseUri;
using ::coro::http::Request;
using ::coro::http::Response;
using ::coro::http::ToBase64;
using ::coro::util::ForEach;
using ::coro::util::MakePointer;

constexpr std::string_view kRedirectUri = "http://localhost:12345";
constexpr std::string_view kTokenFile = "access-token.json";
constexpr std::string_view kGoogleDriveClientId =
    R"(646432077068-hmvk44qgo6d0a64a5h9ieue34p3j2dcv.apps.googleusercontent.com)";
constexpr std::string_view kGoogleDriveClientSecret =
    R"(1f0FG5ch-kKOanTAv1Bqdp9U)";

template <typename>
struct AuthData;

template <>
struct AuthData<GoogleDrive> {
  auto operator()() const {
    return GoogleDrive::Auth::AuthData{
        .client_id = std::string(kGoogleDriveClientId),
        .client_secret = std::string(kGoogleDriveClientSecret),
        .redirect_uri = std::string(kRedirectUri) + "/auth/google"};
  }
};

template <>
struct AuthData<Mega> {
  auto operator()() const {
    return Mega::Auth::AuthData{.api_key = "ZVhB0Czb",
                                .app_name = "coro-cloudstorage"};
  }
};

template <typename CloudProvider,
          typename AuthToken = typename CloudProvider::Auth::AuthToken>
std::optional<AuthToken> LoadToken() {
  std::ifstream token_file{std::string(kTokenFile)};
  if (token_file) {
    try {
      nlohmann::json json;
      token_file >> json;
      return ToAuthToken<AuthToken>(
          json.at(std::string(GetCloudProviderId<CloudProvider>())));
    } catch (const nlohmann::json::exception&) {
    }
  }
  return std::nullopt;
}

template <typename CloudProvider,
          typename AuthToken = typename CloudProvider::Auth::AuthToken>
void SaveToken(AuthToken token) {
  nlohmann::json json;
  {
    std::ifstream input_token_file{std::string(kTokenFile)};
    if (input_token_file) {
      input_token_file >> json;
    }
  }
  json[std::string(GetCloudProviderId<CloudProvider>())] =
      ToJson(std::move(token));
  std::ofstream{std::string(kTokenFile)} << json;
}

template <typename CloudProvider>
void RemoveToken() {
  nlohmann::json json;
  {
    std::ifstream input_token_file{std::string(kTokenFile)};
    if (input_token_file) {
      input_token_file >> json;
    }
  }
  json.erase(std::string(GetCloudProviderId<CloudProvider>()));
  std::ofstream{std::string(kTokenFile)} << json;
}

using HandlerType = coro::stdx::any_invocable<Task<coro::http::Response<>>(
    coro::http::Request<>, coro::stdx::stop_token)>;

template <typename CloudFactory>
class HttpHandler {
 public:
  struct AddAuthHandlerFunctor {
    template <typename CloudProvider>
    void operator()() const {
      handler->AddAuthHandler<CloudProvider>();
    }
    HttpHandler* handler;
  };

  struct GenerateAuthUrlTable {
    template <typename CloudProvider>
    void operator()() const {
      std::string url =
          LoadToken<CloudProvider>()
              ? "/" + std::string(GetCloudProviderId<CloudProvider>()) + "/"
              : factory.template GetAuthorizationUrl<CloudProvider>().value_or(
                    "/auth/" +
                    std::string(GetCloudProviderId<CloudProvider>()));
      stream << "<tr><td><a href='" << url << "'>"
             << GetCloudProviderId<CloudProvider>() << "</a></td></tr>";
    }
    const CloudFactory& factory;
    std::stringstream& stream;
  };

  explicit HttpHandler(const CloudFactory& factory) : factory_(factory) {
    ForEach<coro::cloudstorage::CloudProviders>{}(AddAuthHandlerFunctor{this});
  }

  HttpHandler(const HttpHandler&) = delete;

  Task<Response<>> operator()(Request<> request,
                              coro::stdx::stop_token stop_token) {
    for (auto& handler : handlers_) {
      if (std::regex_match(request.url, handler.regex)) {
        auto url = request.url;
        auto response =
            co_await handler.handler(std::move(request), stop_token);
        std::smatch match;
        if (response.status == 200 &&
            std::regex_match(url, match, std::regex("/auth(/[^?]*)?.*$"))) {
          co_return Response<>{.status = 301,
                               .headers = {{"Location", match[1].str() + "/"}}};
        } else {
          co_return response;
        }
      }
    }
    if (request.url.empty() || request.url == "/") {
      co_return coro::http::Response<>{.status = 200, .body = GetHomePage()};
    } else {
      co_return coro::http::Response<>{.status = 404};
    }
  }

  Generator<std::string> GetHomePage() const {
    std::stringstream result;
    result << "<html><body><table>";
    ForEach<coro::cloudstorage::CloudProviders>{}(
        GenerateAuthUrlTable{factory_, result});
    result << "</table></body></html>";
    co_yield result.str();
  }

  template <typename CloudProvider,
            typename AuthToken = typename CloudProvider::Auth::AuthToken>
  void OnAuthTokenCreated(AuthToken token) {
    SaveToken<CloudProvider>(token);
    AddProxyHandler<CloudProvider>(token);
  }

 private:
  template <typename CloudProvider, typename ProxyHandlerT>
  struct ProxyHandler {
    Task<Response<>> operator()(Request<> request,
                                coro::stdx::stop_token stop_token) {
      try {
        pending_requests++;
        auto guard = MakePointer(
            this, [](ProxyHandler* handler) { handler->pending_requests--; });
        co_return co_await proxy_handler(std::move(request),
                                         std::move(stop_token));
      } catch (const coro::http::HttpException& e) {
        if (e.status() == 401 && pending_requests == 0) {
          RemoveToken<CloudProvider>();
          d->handlers_.erase(std::find_if(
              std::begin(d->handlers_), std::end(d->handlers_),
              [](const Handler& handler) {
                return handler.id == GetCloudProviderId<CloudProvider>();
              }));
          co_return Response<>{.status = 301, .headers = {{"Location", "/"}}};
        }
        throw;
      }
    }

    int pending_requests = 0;
    HttpHandler* d;
    ProxyHandlerT proxy_handler;
  };

  template <typename CloudProvider, typename ProxyHandlerT>
  auto MakeCustomProxyHandler(ProxyHandlerT proxy_handler) {
    return ProxyHandler<CloudProvider, ProxyHandlerT>{
        .d = this, .proxy_handler = std::move(proxy_handler)};
  }

  template <typename CloudProvider>
  void AddAuthHandler() {
    auto auth_token = LoadToken<CloudProvider>();
    if (auth_token) {
      OnAuthTokenCreated<CloudProvider>(*auth_token);
    }
    handlers_.emplace_back(Handler{
        .regex = std::regex("/auth(/" +
                            std::string(GetCloudProviderId<CloudProvider>()) +
                            ".*$)"),
        .handler = factory_.template CreateAuthHandler<CloudProvider>(
            [this](auto d) { this->OnAuthTokenCreated<CloudProvider>(d); })});
  }

  template <typename CloudProvider,
            typename AuthToken = typename CloudProvider::Auth::AuthToken>
  void AddProxyHandler(AuthToken token) {
    auto prefix = "/" + std::string(GetCloudProviderId<CloudProvider>());
    handlers_.emplace_back(Handler{
        .id = std::string(GetCloudProviderId<CloudProvider>()),
        .regex = std::regex(prefix + "(/.*$)"),
        .handler = HandlerType(MakeCustomProxyHandler<CloudProvider>(
            MakeProxyHandler(factory_.template Create<CloudProvider>(
                                 std::move(token), SaveToken<CloudProvider>),
                             prefix)))});
  }

  struct Handler {
    std::string id;
    std::regex regex;
    HandlerType handler;
  };
  const CloudFactory& factory_;
  std::vector<Handler> handlers_;
};

template <typename CloudFactory>
auto MakeHttpHandler(const CloudFactory& factory) {
  return HandlerType(std::make_unique<HttpHandler<CloudFactory>>(factory));
}

Task<> CoMain(event_base* event_loop) noexcept {
  try {
    CurlHttp http(event_loop);
    Semaphore quit;
    auto cloud_factory = MakeCloudFactory<AuthData>(event_loop, http);

    Semaphore semaphore;
    HttpServer http_server(event_loop, {.address = "0.0.0.0", .port = 12345},
                           MakeHttpHandler(cloud_factory),
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