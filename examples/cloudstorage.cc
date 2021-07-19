#include <coro/cloudstorage/cloud_factory.h>
#include <coro/cloudstorage/providers/mega.h>
#include <coro/cloudstorage/util/account_manager_handler.h>
#include <coro/cloudstorage/util/muxer.h>
#include <coro/cloudstorage/util/random_number_generator.h>
#include <coro/cloudstorage/util/thumbnail_generator.h>
#include <coro/http/cache_http.h>
#include <coro/http/curl_http.h>
#include <coro/http/http_server.h>
#include <coro/stdx/coroutine.h>
#include <coro/util/event_loop.h>
#include <event2/thread.h>

#include <csignal>
#include <iostream>

using ::coro::Promise;
using ::coro::Task;
using ::coro::cloudstorage::CloudFactory;
using ::coro::cloudstorage::util::AccountManagerHandler;
using ::coro::cloudstorage::util::Muxer;
using ::coro::cloudstorage::util::RandomNumberGenerator;
using ::coro::cloudstorage::util::ThumbnailGenerator;
using ::coro::http::CacheHttp;
using ::coro::http::CacheHttpConfig;
using ::coro::http::CurlHttp;
using ::coro::http::HttpServer;
using ::coro::util::EventLoop;
using ::coro::util::ThreadPool;
using ::coro::util::TypeList;

using CloudProviders = TypeList<coro::cloudstorage::Mega>;

template <typename CloudFactory, typename ThumbnailGenerator>
class HttpHandler {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;

  HttpHandler(const CloudFactory* factory,
              const ThumbnailGenerator* thumbnail_generator,
              Promise<void>* quit)
      : account_manager_handler_(factory, thumbnail_generator,
                                 AccountListener{}),
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
    co_return co_await account_manager_handler_(std::move(request), stop_token);
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
    CacheHttp<CurlHttp> http{CacheHttpConfig{}, event_base};
    EventLoop event_loop(event_base);
    ThreadPool thread_pool(event_loop);
    ThumbnailGenerator thumbnail_generator(&thread_pool, &event_loop);
    Muxer muxer(&event_loop, &thread_pool);
    std::default_random_engine random_engine{std::random_device()()};
    RandomNumberGenerator random_number_generator(&random_engine);
    CloudFactory cloud_factory(&event_loop, &http, &thumbnail_generator, &muxer,
                               &random_number_generator);

    Promise<void> quit;
    HttpServer<
        HttpHandler<decltype(cloud_factory), decltype(thumbnail_generator)>>
        http_server(event_base, {.address = "127.0.0.1", .port = 12345},
                    &cloud_factory, &thumbnail_generator, &quit);
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
  signal(SIGPIPE, SIG_IGN);
#endif

  std::unique_ptr<event_base, coro::util::EventBaseDeleter> base(
      event_base_new());
  coro::RunTask(CoMain(base.get()));
  return event_base_dispatch(base.get());
}