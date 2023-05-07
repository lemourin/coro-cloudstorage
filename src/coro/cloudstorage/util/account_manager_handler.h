#ifndef CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
#define CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H

#include "coro/cloudstorage/util/cache_manager.h"
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
  AccountListener(T impl)
      : d_(std::make_unique<AccountListenerImpl<T>>(std::move(impl))) {}

  void OnCreate(std::shared_ptr<CloudProviderAccount> account) {
    d_->OnCreate(std::move(account));
  }

  void OnDestroy(std::shared_ptr<CloudProviderAccount> account) {
    d_->OnDestroy(std::move(account));
  }

 private:
  class Interface {
   public:
    virtual ~Interface() = default;
    virtual void OnCreate(std::shared_ptr<CloudProviderAccount>) = 0;
    virtual void OnDestroy(std::shared_ptr<CloudProviderAccount>) = 0;
  };

  template <typename T>
  class AccountListenerImpl : public Interface {
   public:
    explicit AccountListenerImpl(T impl) : impl_(std::move(impl)) {}

    void OnCreate(std::shared_ptr<CloudProviderAccount> account) override {
      impl_.OnCreate(std::move(account));
    }

    void OnDestroy(std::shared_ptr<CloudProviderAccount> account) override {
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
                        const Muxer* muxer, AccountListener account_listener,
                        SettingsManager* settings_manager, CacheManager* cache_manager);
  AccountManagerHandler(AccountManagerHandler&&) noexcept;

  ~AccountManagerHandler();

  AccountManagerHandler& operator=(AccountManagerHandler&&) noexcept;

  Task<http::Response<>> operator()(http::Request<> request,
                                    stdx::stop_token stop_token);

  void Quit();

 private:
  class Impl;

  std::unique_ptr<Impl> impl_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
