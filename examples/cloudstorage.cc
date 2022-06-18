#include <csignal>
#include <iostream>

#include "coro/cloudstorage/util/account_manager_handler.h"
#include "coro/cloudstorage/util/cloud_factory_context.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"
#include "coro/http/cache_http.h"
#include "coro/http/curl_http.h"
#include "coro/http/http_server.h"
#include "coro/util/event_loop.h"

using ::coro::Promise;
using ::coro::Task;
using ::coro::cloudstorage::util::AbstractCloudFactory;
using ::coro::cloudstorage::util::AccountManagerHandler;
using ::coro::cloudstorage::util::AuthTokenManager;
using ::coro::cloudstorage::util::CloudFactoryContext;
using ::coro::cloudstorage::util::CloudProviderAccount;
using ::coro::cloudstorage::util::SettingsManager;
using ::coro::cloudstorage::util::ThumbnailGenerator;
using ::coro::http::CurlHttp;
using ::coro::http::HttpServer;

class HttpHandler {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;

  HttpHandler(const AbstractCloudFactory* factory,
              const ThumbnailGenerator* thumbnail_generator,
              SettingsManager settings_manager, Promise<void>* quit)
      : account_manager_handler_(factory, thumbnail_generator,
                                 AccountListener{},
                                 std::move(settings_manager)),
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
      quit_->SetValue();
      co_return Response{.status = 200};
    }
    co_return co_await account_manager_handler_(std::move(request),
                                                std::move(stop_token));
  }

 private:
  struct AccountListener {
    void OnCreate(CloudProviderAccount* d) {
      std::cerr << "CREATED " << d->id() << "\n";
    }
    Task<> OnDestroy(CloudProviderAccount* d) {
      std::cerr << "REMOVED " << d->id() << "\n";
      co_return;
    }
  };

  AccountManagerHandler account_manager_handler_;
  Promise<void>* quit_;
};

Task<> CoMain(const coro::util::EventLoop* event_loop) {
  try {
    CloudFactoryContext factory_context(event_loop);
    SettingsManager settings_manager(
        AuthTokenManager{factory_context.factory()});
    Promise<void> quit;
    HttpServer<HttpHandler> http_server(
        event_loop, settings_manager.GetHttpServerConfig(),
        factory_context.factory(), factory_context.thumbnail_generator(),
        std::move(settings_manager), &quit);
    co_await quit;
    co_await http_server.Quit();
  } catch (const std::exception& exception) {
    std::cerr << "EXCEPTION: " << exception.what() << "\n";
  }
}

int main() {
#ifdef SIGPIPE
  signal(SIGPIPE, SIG_IGN);  // NOLINT
#endif

  coro::util::EventLoop event_loop;
  coro::RunTask(CoMain(&event_loop));
  event_loop.EnterLoop();
  return 0;
}
