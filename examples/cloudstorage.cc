#include <csignal>
#include <iostream>

#include "coro/cloudstorage/util/account_manager_handler.h"
#include "coro/cloudstorage/util/cloud_factory_context.h"
#include "coro/util/event_loop.h"

using ::coro::Promise;
using ::coro::Task;
using ::coro::cloudstorage::util::AccountManagerHandler;
using ::coro::cloudstorage::util::CloudFactoryContext;
using ::coro::cloudstorage::util::CloudProviderAccount;
using ::coro::http::CurlHttp;

struct AccountListener {
  void OnCreate(std::shared_ptr<CloudProviderAccount> d) {
    std::cerr << "CREATE [" << d->type() << "] " << d->username() << '\n';
  }
  Task<> OnDestroy(std::shared_ptr<CloudProviderAccount> d) {
    std::cerr << "REMOVED [" << d->type() << "] " << d->username() << '\n';
    co_return;
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
      quit_->SetValue();
      co_return Response{.status = 200};
    }
    co_return co_await account_manager_handler_(std::move(request),
                                                std::move(stop_token));
  }

  auto Quit() { return account_manager_handler_.Quit(); }

 private:
  AccountManagerHandler account_manager_handler_;
  Promise<void>* quit_;
};

Task<> CoMain(const coro::util::EventLoop* event_loop) {
  try {
    CloudFactoryContext factory_context(event_loop);
    Promise<void> quit;
    auto http_server = factory_context.CreateHttpServer(HttpHandler(
        factory_context.CreateAccountManagerHandler(AccountListener{}), &quit));
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
