#ifndef CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
#define CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H

#include <fmt/core.h>
#include <fmt/format.h>

#include <list>

#include "coro/cloudstorage/util/cloud_provider_account.h"
#include "coro/cloudstorage/util/settings_manager.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"
#include "coro/stdx/any_invocable.h"
#include "coro/util/type_list.h"
#include "coro/when_all.h"

namespace coro::cloudstorage::util {

class AccountManagerHandler {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;

  class AccountListener {
   public:
    virtual ~AccountListener() = default;
    virtual void OnCreate(CloudProviderAccount*) = 0;
    virtual Task<> OnDestroy(CloudProviderAccount*) = 0;
  };

  template <typename AccountListenerT>
  AccountManagerHandler(const AbstractCloudFactory* factory,
                        const ThumbnailGenerator* thumbnail_generator,
                        AccountListenerT account_listener,
                        SettingsManager settings_manager)
      : AccountManagerHandler(
            factory, thumbnail_generator,
            static_cast<std::unique_ptr<AccountListener>>(
                std::make_unique<AccountListenerImpl<AccountListenerT>>(
                    std::move(account_listener))),
            std::move(settings_manager)) {}

  AccountManagerHandler(AccountManagerHandler&&) = delete;
  AccountManagerHandler& operator=(AccountManagerHandler&&) = delete;

  Task<Response> operator()(Request request, coro::stdx::stop_token stop_token);

  Task<> Quit();

 private:
  struct AuthHandler {
    Task<Response> operator()(Request request,
                              stdx::stop_token stop_token) const;

    AbstractCloudProvider::Type type;
    AccountManagerHandler* d;
  };

  struct OnRemoveHandler {
    Task<Response> operator()(Request request,
                              stdx::stop_token stop_token) const;
    AccountManagerHandler* d;
    std::string account_id;
  };

  struct Handler {
    std::string id;
    std::string prefix;
    stdx::any_invocable<Task<http::Response<>>(http::Request<>,
                                               stdx::stop_token)>
        handler;
  };

  AccountManagerHandler(const AbstractCloudFactory* factory,
                        const ThumbnailGenerator* thumbnail_generator,
                        std::unique_ptr<AccountListener> account_listener,
                        SettingsManager settings_manager);

  Task<Response> HandleRequest(Request request,
                               coro::stdx::stop_token stop_token);

  Response GetWebDAVRootResponse(
      std::span<const std::pair<std::string, std::string>> headers) const;

  void RemoveHandler(std::string_view account_id);

  template <typename F>
  Task<> RemoveCloudProvider(const F& predicate);

  void OnCloudProviderCreated(CloudProviderAccount* account);

  CloudProviderAccount* CreateAccount(
      AbstractCloudProvider::Auth::AuthToken auth_token,
      std::shared_ptr<std::optional<std::string>> username);

  Task<CloudProviderAccount*> Create(
      AbstractCloudProvider::Auth::AuthToken auth_token,
      stdx::stop_token stop_token);

  Handler* ChooseHandler(std::string_view path);

  Generator<std::string> GetHomePage() const;

  template <typename Impl>
  class AccountListenerImpl : public AccountListener {
   public:
    explicit AccountListenerImpl(Impl impl) : impl_(std::move(impl)) {}

    void OnCreate(CloudProviderAccount* d) override { impl_.OnCreate(d); }
    Task<> OnDestroy(CloudProviderAccount* d) override {
      return impl_.OnDestroy(d);
    }

   private:
    Impl impl_;
  };

  const AbstractCloudFactory* factory_;
  const ThumbnailGenerator* thumbnail_generator_;
  std::vector<Handler> handlers_;
  std::unique_ptr<AccountListener> account_listener_;
  SettingsManager settings_manager_;
  std::list<CloudProviderAccount> accounts_;
  int64_t version_ = 0;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
