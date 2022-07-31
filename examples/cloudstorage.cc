#include <csignal>
#include <iostream>

#include "coro/cloudstorage/util/account_manager_handler.h"
#include "coro/cloudstorage/util/cloud_factory_context.h"
#include "coro/util/event_loop.h"

using ::coro::Generator;
using ::coro::Promise;
using ::coro::Task;
using ::coro::cloudstorage::util::AccountManagerHandler;
using ::coro::cloudstorage::util::CloudFactoryContext;
using ::coro::cloudstorage::util::CloudProviderAccount;
using ::coro::http::CurlHttp;
using ::coro::util::AtScopeExit;
using ::coro::util::EventLoop;

EventLoop gEventLoop;
Promise<void> gQuit;

struct AccountListener {
  void OnCreate(std::shared_ptr<CloudProviderAccount> d) {
    std::cerr << "CREATE [" << d->type() << "] " << d->username() << '\n';
  }
  void OnDestroy(std::shared_ptr<CloudProviderAccount> d) {
    std::cerr << "REMOVED [" << d->type() << "] " << d->username() << '\n';
  }
};

class HttpHandler {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;

  HttpHandler(AccountManagerHandler account_manager_handler,
              Promise<void>* quit)
      : account_manager_handler_(std::move(account_manager_handler)),
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
      co_return Response{
          .status = 200,
          .body = GetQuitResponse(
              std::unique_ptr<Promise<void>, QuitDeleter>(quit_))};
    }
    co_return co_await account_manager_handler_(std::move(request),
                                                std::move(stop_token));
  }

  auto Quit() { return account_manager_handler_.Quit(); }

 private:
  struct QuitDeleter {
    void operator()(Promise<void>* quit) { quit->SetValue(); }
  };

  Generator<std::string> GetQuitResponse(
      std::unique_ptr<Promise<void>, QuitDeleter>) const {
    co_yield "QUITTING...\n";
  }

  AccountManagerHandler account_manager_handler_;
  Promise<void>* quit_;
};

Task<> CoMain(CloudFactoryContext* factory_context, Promise<void>* quit) {
  try {
    auto http_server = factory_context->CreateHttpServer(HttpHandler(
        factory_context->CreateAccountManagerHandler(AccountListener{}), quit));
    co_await *quit;
    co_await http_server.Quit();
  } catch (const std::exception& exception) {
    std::cerr << "EXCEPTION: " << exception.what() << "\n";
  }
}

void SignalHandler(int signal) {
  gEventLoop.RunOnEventLoop([] { gQuit.SetValue(); });
}

int main() {
#ifdef SIGPIPE
  signal(SIGPIPE, SIG_IGN);  // NOLINT
#endif

  signal(SIGTERM, SignalHandler);

  CloudFactoryContext factory_context(&gEventLoop);
  coro::RunTask(CoMain(&factory_context, &gQuit));
  gEventLoop.EnterLoop();
  return 0;
}
