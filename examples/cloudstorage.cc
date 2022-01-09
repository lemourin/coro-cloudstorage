#include <event2/thread.h>

#include <csignal>
#include <iostream>

#include "coro/cloudstorage/providers/local_filesystem.h"
#include "coro/cloudstorage/util/account_manager_handler.h"
#include "coro/cloudstorage/util/cloud_factory_context.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"
#include "coro/http/cache_http.h"
#include "coro/http/curl_http.h"
#include "coro/http/http_server.h"
#include "coro/util/event_loop.h"

using ::coro::Promise;
using ::coro::Task;
using ::coro::cloudstorage::util::AccountManagerHandler;
using ::coro::cloudstorage::util::CloudFactoryContext;
using ::coro::cloudstorage::util::SettingsManager;
using ::coro::http::CurlHttp;
using ::coro::http::HttpServer;
using ::coro::util::TypeList;

using CloudProviders = TypeList<coro::cloudstorage::LocalFileSystem>;

template <typename CloudFactory, typename ThumbnailGenerator>
class HttpHandler {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;

  HttpHandler(const CloudFactory* factory,
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
    template <typename CloudAccount>
    void OnCreate(CloudAccount* d) {
      std::cerr << "CREATED " << d->GetId() << "\n";
    }
    template <typename CloudAccount>
    Task<> OnDestroy(CloudAccount* d) {
      std::cerr << "REMOVED " << d->GetId() << "\n";
      co_return;
    }
  };

  AccountManagerHandler<CloudProviders, CloudFactory, ThumbnailGenerator,
                        AccountListener>
      account_manager_handler_;
  Promise<void>* quit_;
};

Task<> CoMain(event_base* event_base) noexcept {
  try {
    CloudFactoryContext factory_context(event_base);
    SettingsManager settings_manager;
    Promise<void> quit;
    HttpServer<HttpHandler<CloudFactoryContext<>::CloudFactoryT,
                           CloudFactoryContext<>::ThumbnailGeneratorT>>
        http_server(event_base, settings_manager.GetHttpServerConfig(),
                    factory_context.factory(),
                    factory_context.thumbnail_generator(),
                    std::move(settings_manager), &quit);
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
  evthread_use_windows_threads();
#else
  evthread_use_pthreads();
#endif

#ifdef SIGPIPE
  signal(SIGPIPE, SIG_IGN);  // NOLINT
#endif

  std::unique_ptr<event_base, coro::util::EventBaseDeleter> base(
      event_base_new());
  coro::RunTask(CoMain(base.get()));
  return event_base_dispatch(base.get());
}
