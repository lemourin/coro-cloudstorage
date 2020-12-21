#include <coro/cloudstorage/cloud_factory.h>
#include <coro/cloudstorage/providers/dropbox.h>
#include <coro/cloudstorage/providers/google_drive.h>
#include <coro/cloudstorage/providers/mega.h>
#include <coro/cloudstorage/providers/one_drive.h>
#include <coro/cloudstorage/util/account_manager_handler.h>
#include <coro/http/curl_http.h>
#include <coro/http/http_server.h>
#include <coro/stdx/coroutine.h>

#include <csignal>
#include <iostream>

using ::coro::Semaphore;
using ::coro::Task;
using ::coro::cloudstorage::util::AccountManagerHandler;
using ::coro::http::CurlHttp;
using ::coro::http::HttpServer;
using ::coro::http::Request;
using ::coro::http::Response;
using ::coro::util::MakePointer;

using CloudProviders = ::coro::util::TypeList<
    coro::cloudstorage::GoogleDrive, coro::cloudstorage::Mega,
    coro::cloudstorage::OneDrive, coro::cloudstorage::Dropbox>;

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

template <typename CloudFactory>
class HttpHandler {
 public:
  explicit HttpHandler(const CloudFactory& factory)
      : auth_handler_(factory, AccountListener{},
                      coro::cloudstorage::util::AuthTokenManager{
                          .token_file = std::string(kTokenFile)}) {}

  Task<Response<>> operator()(Request<> request,
                              coro::stdx::stop_token stop_token) {
    std::cerr << coro::http::MethodToString(request.method) << " "
              << request.url << "\n";
    if (auth_handler_.CanHandleUrl(request.url)) {
      co_return co_await auth_handler_(std::move(request), stop_token);
    }
    co_return coro::http::Response<>{.status = 302,
                                     .headers = {{"Location", "/"}}};
  }

 private:
  struct AccountListener {
    template <typename CloudAccount>
    void OnCreate(CloudAccount* d) {
      std::cerr << "CREATED " << d->id << "\n";
    }
    template <typename CloudAccount>
    void OnDestroy(CloudAccount* d) {
      std::cerr << "REMOVED " << d->id << "\n";
    }
  };

  AccountManagerHandler<CloudProviders, CloudFactory, AccountListener>
      auth_handler_;
};

Task<> CoMain(event_base* event_loop) noexcept {
  try {
    CurlHttp http(event_loop);
    Semaphore quit;
    coro::cloudstorage::CloudFactory cloud_factory(event_loop, http,
                                                   AuthData{});

    Semaphore semaphore;
    HttpServer http_server(event_loop, {.address = "0.0.0.0", .port = 12345},
                           HttpHandler(cloud_factory),
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