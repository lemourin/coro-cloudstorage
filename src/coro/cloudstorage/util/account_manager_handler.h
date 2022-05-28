#ifndef CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
#define CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H

#include <fmt/core.h>
#include <fmt/format.h>

#include <list>

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/auth_handler.h"
#include "coro/cloudstorage/util/cloud_provider_account.h"
#include "coro/cloudstorage/util/cloud_provider_handler.h"
#include "coro/cloudstorage/util/get_size_handler.h"
#include "coro/cloudstorage/util/serialize_utils.h"
#include "coro/cloudstorage/util/settings_handler.h"
#include "coro/cloudstorage/util/settings_manager.h"
#include "coro/cloudstorage/util/static_file_handler.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/cloudstorage/util/theme_handler.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"
#include "coro/cloudstorage/util/webdav_utils.h"
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

  template <typename Impl>
  class AccountListenerImpl;

  template <typename T>
  struct Id {};

  template <typename... CloudProviders, typename AccountListenerT>
  AccountManagerHandler(Id<coro::util::TypeList<CloudProviders...>>,
                        const CloudFactory* factory,
                        const AbstractCloudFactory* factory2,
                        const ThumbnailGenerator* thumbnail_generator,
                        AccountListenerT account_listener,
                        SettingsManager settings_manager)
      : factory_(factory),
        factory2_(factory2),
        thumbnail_generator_(thumbnail_generator),
        account_listener_(
            std::make_unique<AccountListenerImpl<AccountListenerT>>(
                std::move(account_listener))),
        settings_manager_(std::move(settings_manager)) {
    handlers_.emplace_back(
        Handler{.prefix = "/static/", .handler = StaticFileHandler{factory2_}});
    handlers_.emplace_back(
        Handler{.prefix = "/size", .handler = GetSizeHandler{&accounts_}});
    handlers_.emplace_back(Handler{
        .prefix = "/settings", .handler = SettingsHandler(&settings_manager_)});
    handlers_.emplace_back(
        Handler{.prefix = "/settings/theme-toggle", .handler = ThemeHandler{}});

    for (AbstractCloudProvider::Type type :
         factory2->GetSupportedCloudProviders()) {
      handlers_.emplace_back(Handler{
          .prefix = util::StrCat("/auth/", factory2->CreateAuth(type)->GetId()),
          .handler = AuthHandler2{type, this}});
    }

    for (auto auth_token : settings_manager_.LoadTokenData2()) {
      auto id = std::move(auth_token.id);
      auto* account =
          CreateAccount2(std::move(auth_token),
                         std::make_shared<std::optional<std::string>>(id));
      OnCloudProviderCreated(account);
    }
  }

  AccountManagerHandler(AccountManagerHandler&&) = delete;
  AccountManagerHandler& operator=(AccountManagerHandler&&) = delete;

  Task<Response> operator()(Request request, coro::stdx::stop_token stop_token);

  Task<> Quit();

 private:
  Task<Response> HandleRequest(Request request,
                               coro::stdx::stop_token stop_token);

  Response GetWebDAVRootResponse(
      std::span<const std::pair<std::string, std::string>> headers) const;

  void RemoveHandler(std::string_view account_id);

  template <typename F>
  Task<> RemoveCloudProvider(const F& predicate);

  struct AuthHandler2 {
    Task<Response> operator()(Request request,
                              stdx::stop_token stop_token) const {
      auto result = co_await d->factory2_->CreateAuth(type)
                        ->CreateAuthHandler()
                        ->OnRequest(std::move(request), stop_token);
      if (std::holds_alternative<Response>(result)) {
        co_return std::move(std::get<Response>(result));
      }
      auto* account = co_await d->Create2(
          std::get<AbstractCloudProvider::Auth::AuthToken>(std::move(result)),
          std::move(stop_token));
      co_return Response{
          .status = 302,
          .headers = {
              {"Location", util::StrCat("/", http::EncodeUri(account->id()))}}};
    }

    AbstractCloudProvider::Type type;
    AccountManagerHandler* d;
  };

  struct OnRemoveHandler {
    Task<Response> operator()(Request request,
                              stdx::stop_token stop_token) const;
    AccountManagerHandler* d;
    std::string account_id;
  };

  void OnCloudProviderCreated(CloudProviderAccount* account);

  CloudProviderAccount* CreateAccount2(
      AbstractCloudProvider::Auth::AuthToken auth_token,
      std::shared_ptr<std::optional<std::string>> username);

  Task<CloudProviderAccount*> Create2(
      AbstractCloudProvider::Auth::AuthToken auth_token,
      stdx::stop_token stop_token);

  struct Handler {
    std::string id;
    std::string prefix;
    stdx::any_invocable<Task<http::Response<>>(http::Request<>,
                                               stdx::stop_token)>
        handler;
  };

  Handler* ChooseHandler(std::string_view path);

  Generator<std::string> GetHomePage() const;

  const CloudFactory* factory_;
  const AbstractCloudFactory* factory2_;
  const ThumbnailGenerator* thumbnail_generator_;
  std::vector<Handler> handlers_;
  std::unique_ptr<AccountListener> account_listener_;
  SettingsManager settings_manager_;
  std::list<CloudProviderAccount> accounts_;
  int64_t version_ = 0;
};

template <typename Impl>
class AccountManagerHandler::AccountListenerImpl : public AccountListener {
 public:
  explicit AccountListenerImpl(Impl impl) : impl_(std::move(impl)) {}

  void OnCreate(CloudProviderAccount* d) override { impl_.OnCreate(d); }
  Task<> OnDestroy(CloudProviderAccount* d) override {
    return impl_.OnDestroy(d);
  }

 private:
  Impl impl_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
