#ifndef CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
#define CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H

#include "coro/cloudstorage/util/cache_manager.h"
#include "coro/cloudstorage/util/clock.h"
#include "coro/cloudstorage/util/cloud_provider_account.h"
#include "coro/cloudstorage/util/muxer.h"
#include "coro/cloudstorage/util/settings_manager.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"
#include "coro/stdx/any_invocable.h"
#include "coro/when_all.h"

namespace coro::cloudstorage::util {

class AccountListener {
 public:
  template <typename T>
    requires requires(T impl, CloudProviderAccount account) {
      impl.OnCreate(std::move(account));
      impl.OnDestroy(std::move(account));
    }
  AccountListener(T impl)
      : d_(std::make_unique<AccountListenerImpl<T>>(std::move(impl))) {}

  void OnCreate(CloudProviderAccount account) {
    d_->OnCreate(std::move(account));
  }

  void OnDestroy(CloudProviderAccount account) {
    d_->OnDestroy(std::move(account));
  }

 private:
  class Interface {
   public:
    virtual ~Interface() = default;
    virtual void OnCreate(CloudProviderAccount) = 0;
    virtual void OnDestroy(CloudProviderAccount) = 0;
  };

  template <typename T>
  class AccountListenerImpl : public Interface {
   public:
    explicit AccountListenerImpl(T impl) : impl_(std::move(impl)) {}

    void OnCreate(CloudProviderAccount account) override {
      impl_.OnCreate(std::move(account));
    }

    void OnDestroy(CloudProviderAccount account) override {
      impl_.OnDestroy(std::move(account));
    }

   private:
    T impl_;
  };

  std::unique_ptr<Interface> d_;
};

class AccountManagerHandler {
 public:
  AccountManagerHandler(const AbstractCloudFactory* factory,
                        const ThumbnailGenerator* thumbnail_generator,
                        const Muxer* muxer, const Clock* clock,
                        AccountListener account_listener,
                        SettingsManager* settings_manager,
                        CacheManager* cache_manager);
  AccountManagerHandler(AccountManagerHandler&&) noexcept = default;
  AccountManagerHandler(const AccountManagerHandler&) = delete;
  ~AccountManagerHandler();

  AccountManagerHandler& operator=(const AccountManagerHandler&) = delete;
  AccountManagerHandler& operator=(AccountManagerHandler&&) noexcept = default;

  Task<http::Response<>> operator()(http::Request<> request,
                                    stdx::stop_token stop_token);

  void Quit();

 private:
  struct AuthHandler {
    Task<http::Response<>> operator()(http::Request<> request,
                                      stdx::stop_token stop_token) const;

    AbstractCloudProvider::Type type;
    AccountManagerHandler* d;
  };

  struct OnRemoveHandler {
    Task<http::Response<>> operator()(http::Request<> request,
                                      stdx::stop_token stop_token) const;
    AccountManagerHandler* d;
    CloudProviderAccount account;
  };

  struct Handler {
    std::optional<CloudProviderAccount> account;
    stdx::any_invocable<Task<http::Response<>>(http::Request<>,
                                               stdx::stop_token)>
        handler;
  };

  Task<http::Response<>> HandleRequest(http::Request<> request,
                                       coro::stdx::stop_token stop_token);

  http::Response<> GetWebDAVResponse(
      std::string_view path,
      std::span<const std::pair<std::string, std::string>> headers) const;

  template <typename F>
  void RemoveCloudProvider(const F& predicate);

  void OnCloudProviderCreated(CloudProviderAccount account);

  CloudProviderAccount CreateAccount(
      std::unique_ptr<AbstractCloudProvider> provider, std::string username,
      int64_t version);

  Task<CloudProviderAccount> Create(
      AbstractCloudProvider::Auth::AuthToken auth_token,
      stdx::stop_token stop_token);

  std::optional<Handler> ChooseHandler(std::string_view path);

  Generator<std::string> GetHomePage() const;

  const AbstractCloudFactory* factory_;
  const ThumbnailGenerator* thumbnail_generator_;
  const Muxer* muxer_;
  const Clock* clock_;
  AccountListener account_listener_;
  SettingsManager* settings_manager_;
  CacheManager* cache_manager_;
  std::vector<CloudProviderAccount> accounts_;
  int64_t version_ = 0;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
