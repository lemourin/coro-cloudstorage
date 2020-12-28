#include <coro/cloudstorage/cloud_factory.h>
#include <coro/cloudstorage/providers/dropbox.h>
#include <coro/cloudstorage/providers/google_drive.h>
#include <coro/cloudstorage/providers/mega.h>
#include <coro/cloudstorage/providers/one_drive.h>
#include <coro/cloudstorage/util/account_manager_handler.h>
#include <coro/http/cache_http.h>
#include <coro/http/curl_http.h>
#include <coro/http/http_server.h>
#include <coro/stdx/coroutine.h>
#include <coro/util/event_loop.h>

#include <csignal>
#include <iostream>

using ::coro::Semaphore;
using ::coro::Task;
using ::coro::cloudstorage::util::AccountManagerHandler;
using ::coro::http::CacheHttp;
using ::coro::http::CurlHttp;
using ::coro::http::HttpServer;
using ::coro::util::MakePointer;

using CloudProviders = ::coro::util::TypeList<
    coro::cloudstorage::GoogleDrive, coro::cloudstorage::Mega,
    coro::cloudstorage::OneDrive, coro::cloudstorage::Dropbox>;

constexpr std::string_view kTokenFile = "access-token.json";

template <typename CloudFactory>
class HttpHandler {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;

  HttpHandler(const CloudFactory& factory, Semaphore* quit)
      : auth_handler_(factory, AccountListener{},
                      coro::cloudstorage::util::AuthTokenManager{
                          .token_file = std::string(kTokenFile)}),
        quit_(quit) {}

  Task<Response> operator()(Request request,
                            coro::stdx::stop_token stop_token) {
    auto range_str = coro::http::GetHeader(request.headers, "Range");
    std::cerr << coro::http::MethodToString(request.method) << " "
              << request.url;
    if (range_str) {
      std::cerr << " " << *range_str;
    }
    std::cerr << "\n";
    if (request.url == "/quit") {
      quit_->resume();
      co_return Response{.status = 200};
    }
    co_return co_await auth_handler_(std::move(request), stop_token);
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
  Semaphore* quit_;
};

Task<> CoMain(event_base* event_base) noexcept {
  try {
    CacheHttp<CurlHttp> http{CurlHttp(event_base)};
    coro::util::EventLoop event_loop(event_base);
    coro::cloudstorage::CloudFactory cloud_factory(event_loop, http);

    Semaphore quit;
    HttpServer http_server(event_base, {.address = "0.0.0.0", .port = 12345},
                           HttpHandler(cloud_factory, &quit));
    co_await quit;
    co_await http_server.Quit();
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
  Invoke(CoMain(base.get()));
  return event_base_dispatch(base.get());
}