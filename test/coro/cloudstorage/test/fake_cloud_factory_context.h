#ifndef CORO_CLOUDSTORAGE_TEST_FAKE_CLOUD_FACTORY_CONTEXT_H
#define CORO_CLOUDSTORAGE_TEST_FAKE_CLOUD_FACTORY_CONTEXT_H

#include <coro/util/event_loop.h>

#include "coro/cloudstorage/test/fake_http_client.h"
#include "coro/cloudstorage/test/test_utils.h"
#include "coro/cloudstorage/util/cloud_factory_context.h"
#include "coro/cloudstorage/util/cloud_provider_account.h"

namespace coro::cloudstorage::test {

class TestCloudProviderAccount {
 private:
  template <typename F, typename... Args>
  auto WithAccount(F method, Args... args) const {
    return event_loop_->Do(
        [this, ... args = std::move(args), method]() mutable {
          return std::invoke(method, GetAccount().provider(),
                             std::move(args)..., stdx::stop_token());
        });
  }

 public:
  auto GetRoot() const {
    return WithAccount(
        &coro::cloudstorage::util::AbstractCloudProvider::GetRoot);
  }

  template <typename... Ts>
  auto ListDirectoryPage(Ts... args) const {
    return WithAccount(
        &coro::cloudstorage::util::AbstractCloudProvider::ListDirectoryPage,
        std::move(args)...);
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

struct FakeCloudFactoryContextConfig {
  std::optional<TemporaryFile> config_file = TemporaryFile();
  std::optional<TemporaryFile> cache_file = TemporaryFile();
  std::string config_file_path{config_file->path()};
  std::string cache_file_path{cache_file->path()};
  FakeHttpClient http;
};

class FakeCloudFactoryContext {
 public:
  explicit FakeCloudFactoryContext(FakeCloudFactoryContextConfig);
  explicit FakeCloudFactoryContext(FakeHttpClient http = {})
      : FakeCloudFactoryContext(
            FakeCloudFactoryContextConfig{.http = std::move(http)}) {}
  FakeCloudFactoryContext(const FakeCloudFactoryContext&) = delete;
  FakeCloudFactoryContext(FakeCloudFactoryContext&&) = delete;
  FakeCloudFactoryContext& operator=(const FakeCloudFactoryContext&) = delete;
  FakeCloudFactoryContext& operator=(FakeCloudFactoryContext&&) = delete;
  ~FakeCloudFactoryContext();

  ResponseContent Fetch(http::Request<std::string> request);

  TestCloudProviderAccount GetAccount(
      coro::cloudstorage::util::CloudProviderAccount::Id id);

 private:
  void RunThread(FakeCloudFactoryContextConfig);

  class ThreadState {
   public:
    explicit ThreadState(FakeCloudFactoryContextConfig);

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
    FakeCloudFactoryContextConfig config_;
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

#endif  // CORO_CLOUDSTORAGE_TEST_FAKE_CLOUD_FACTORY_CONTEXT_H
