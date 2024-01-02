#ifndef CORO_CLOUDSTORAGE_FAKE_CLOUD_FACTORY_CONTEXT_H
#define CORO_CLOUDSTORAGE_FAKE_CLOUD_FACTORY_CONTEXT_H

#include <coro/util/event_loop.h>

#include "coro/cloudstorage/util/cloud_factory_context.h"
#include "coro/cloudstorage/util/cloud_provider_account.h"
#include "fake_http_client.h"

namespace coro::cloudstorage::test {

class TestCloudProviderAccount {
 public:
  template <typename F>
  auto WithAccount(F func) const {
    return event_loop_->Do([this, func = std::move(func)]() mutable {
      return std::move(func)(GetAccount());
    });
  }

  auto GetRoot() const {
    return WithAccount(
        [](coro::cloudstorage::util::CloudProviderAccount account) {
          return account.provider()->GetRoot(stdx::stop_token());
        });
  }

  template <typename... Ts>
  auto ListDirectoryPage(Ts... args) const {
    return WithAccount([... args = std::move(args)](auto account) {
      return account.provider()->ListDirectoryPage(std::move(args)...,
                                                   stdx::stop_token());
    });
  }

 private:
  friend class FakeCloudFactoryContext;

  TestCloudProviderAccount(
      coro::util::EventLoop* event_loop,
      coro::cloudstorage::util::CloudProviderAccount::Id id,
      std::span<const coro::cloudstorage::util::CloudProviderAccount> accounts)
      : event_loop_(event_loop), id_(std::move(id)), accounts_(accounts) {}

  coro::cloudstorage::util::CloudProviderAccount GetAccount() const;

  coro::util::EventLoop* event_loop_;
  coro::cloudstorage::util::CloudProviderAccount::Id id_;
  std::span<const coro::cloudstorage::util::CloudProviderAccount> accounts_;
};

class FakeCloudFactoryContext {
 public:
  explicit FakeCloudFactoryContext(FakeHttpClient http = {});
  FakeCloudFactoryContext(const FakeCloudFactoryContext&) = delete;
  FakeCloudFactoryContext(FakeCloudFactoryContext&&) = delete;
  FakeCloudFactoryContext& operator=(const FakeCloudFactoryContext&) = delete;
  FakeCloudFactoryContext& operator=(FakeCloudFactoryContext&&) = delete;
  ~FakeCloudFactoryContext();

  ResponseContent Fetch(http::Request<std::string> request);

  TestCloudProviderAccount GetAccount(
      coro::cloudstorage::util::CloudProviderAccount::Id id);

 private:
  void RunThread(FakeHttpClient http);

  class ThreadState {
   public:
    explicit ThreadState(FakeHttpClient http);

    coro::util::EventLoop& event_loop() { return event_loop_; }
    coro::http::Http& http() { return http_; }
    coro::cloudstorage::util::CloudFactoryContext& context() {
      return context_;
    }
    Promise<void>& quit() { return quit_; }
    std::vector<coro::cloudstorage::util::CloudProviderAccount>& accounts() {
      return accounts_;
    }

   private:
    coro::util::EventLoop event_loop_;
    coro::http::Http http_{coro::http::CurlHttp{&event_loop_}};
    coro::cloudstorage::util::CloudFactoryContext context_;
    Promise<void> quit_;
    std::vector<coro::cloudstorage::util::CloudProviderAccount> accounts_;
  };
  std::optional<ThreadState> state_;
  std::promise<void> ready_;
  std::optional<std::string> address_;
  std::thread thread_;
};

}  // namespace coro::cloudstorage::test

#endif  // CORO_CLOUDSTORAGE_FAKE_CLOUD_FACTORY_CONTEXT_H
