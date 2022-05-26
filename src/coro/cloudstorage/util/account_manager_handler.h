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
                        const ThumbnailGenerator* thumbnail_generator,
                        AccountListenerT account_listener,
                        SettingsManager settings_manager = SettingsManager{})
      : factory_(factory),
        thumbnail_generator_(thumbnail_generator),
        account_listener_(
            std::make_unique<AccountListenerImpl<AccountListenerT>>(
                std::move(account_listener))),
        settings_manager_(std::move(settings_manager)),
        append_auth_urls_(
            [](const CloudFactory* factory, std::stringstream& sstream) {
              (AppendAuthUrl<CloudProviders>(factory, sstream), ...);
            }) {
    using StaticFileHandler =
        coro::cloudstorage::util::StaticFileHandler<CloudProviders...>;
    handlers_.emplace_back(
        Handler{.prefix = "/static/", .handler = StaticFileHandler{}});
    handlers_.emplace_back(
        Handler{.prefix = "/size", .handler = GetSizeHandler{&accounts_}});
    handlers_.emplace_back(Handler{
        .prefix = "/settings", .handler = SettingsHandler(&settings_manager_)});
    handlers_.emplace_back(
        Handler{.prefix = "/settings/theme-toggle", .handler = ThemeHandler{}});
    (AddAuthHandler<CloudProviders>(), ...);
    for (const auto& any_token : settings_manager_.template LoadTokenData<
                                 coro::util::TypeList<CloudProviders...>>()) {
      std::visit(
          [&]<typename AuthToken>(AuthToken token) {
            using CloudProvider = typename AuthToken::CloudProvider;
            auto id = std::move(token.id);
            auto* account = CreateAccount<CloudProvider>(
                std::move(token),
                std::make_shared<std::optional<std::string>>(id));
            OnCloudProviderCreated(account);
          },
          any_token);
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
  Task<> RemoveCloudProvider(const F& predicate) {
    for (auto it = std::begin(accounts_); it != std::end(accounts_);) {
      if (predicate(*it) && !it->stop_token().stop_requested()) {
        it->stop_source_.request_stop();
        co_await account_listener_->OnDestroy(&*it);
        settings_manager_.RemoveToken(it->type(), it->username());
        RemoveHandler(it->id());
        it = accounts_.erase(it);
      } else {
        it++;
      }
    }
  }

  template <typename CloudProvider>
  struct AuthHandler {
    using AuthToken = typename CloudProvider::Auth::AuthToken;

    Task<Response> operator()(Request request,
                              stdx::stop_token stop_token) const {
      auto result = co_await d->factory_->CreateAuthHandler<CloudProvider>()(
          std::move(request), stop_token);
      AuthToken auth_token;
      if constexpr (std::is_same_v<decltype(result), AuthToken>) {
        auth_token = std::move(result);
      } else {
        if (std::holds_alternative<Response>(result)) {
          co_return std::move(std::get<Response>(result));
        } else {
          auth_token = std::move(std::get<AuthToken>(result));
        }
      }
      auto* account = co_await d->Create<CloudProvider>(std::move(auth_token),
                                                        std::move(stop_token));
      co_return Response{
          .status = 302,
          .headers = {
              {"Location", "/" + http::EncodeUri(GetAccountId<CloudProvider>(
                                     account->username()))}}};
    }

    AccountManagerHandler* d;
  };

  struct OnRemoveHandler {
    Task<Response> operator()(Request request,
                              stdx::stop_token stop_token) const;
    AccountManagerHandler* d;
    std::string account_id;
  };

  template <typename CloudProvider>
  void AddAuthHandler() {
    handlers_.emplace_back(Handler{
        .prefix = "/auth/" + std::string(GetCloudProviderId<CloudProvider>()),
        .handler = AuthHandler<CloudProvider>{this}});
  }

  void OnCloudProviderCreated(CloudProviderAccount* account);

  template <typename CloudProvider>
  CloudProviderAccount* CreateAccount(
      typename CloudProvider::Auth::AuthToken auth_token,
      std::shared_ptr<std::optional<std::string>> username) {
    return &accounts_.emplace_back(
        username->value_or(""), version_++,
        factory_->template Create<CloudProvider>(
            std::move(auth_token), internal::OnAuthTokenChanged<CloudProvider>{
                                       &settings_manager_, username}));
  }

  template <typename CloudProvider>
  Task<CloudProviderAccount*> Create(
      typename CloudProvider::Auth::AuthToken auth_token,
      stdx::stop_token stop_token) {
    auto username = std::make_shared<std::optional<std::string>>(std::nullopt);
    auto* account = CreateAccount<CloudProvider>(auth_token, username);
    auto version = account->version_;
    auto& provider = account->provider();
    bool on_create_called = false;
    std::exception_ptr exception;
    try {
      auto general_data =
          co_await provider.GetGeneralData(std::move(stop_token));
      *username = std::move(general_data.username);
      account->username_ = **username;
      co_await RemoveCloudProvider([&](const auto& entry) {
        return entry.version_ < version &&
               entry.id() == GetAccountId<CloudProvider>(**username);
      });
      for (const auto& entry : accounts_) {
        if (entry.version_ == version) {
          OnCloudProviderCreated(account);
          on_create_called = true;
          settings_manager_.template SaveToken<CloudProvider>(
              std::move(auth_token), **username);
          break;
        }
      }
      co_return account;
    } catch (...) {
      exception = std::current_exception();
    }
    co_await RemoveCloudProvider(
        [&](const auto& entry) { return entry.version_ == version; });
    std::rethrow_exception(exception);
  }

  struct Handler {
    std::string id;
    std::string prefix;
    stdx::any_invocable<Task<http::Response<>>(http::Request<>,
                                               stdx::stop_token)>
        handler;
  };

  Handler* ChooseHandler(std::string_view path);

  template <typename CloudProvider>
  static void AppendAuthUrl(const CloudFactory* factory,
                            std::stringstream& stream) {
    std::string id(GetCloudProviderId<CloudProvider>());
    std::string url =
        factory->template GetAuthorizationUrl<CloudProvider>().value_or(
            util::StrCat("/auth/", id));
    stream << fmt::format(
        fmt::runtime(kAssetsHtmlProviderEntryHtml),
        fmt::arg("provider_url", url),
        fmt::arg("image_url", util::StrCat("/static/", id, ".png")));
  }

  Generator<std::string> GetHomePage() const;

  const CloudFactory* factory_;
  const ThumbnailGenerator* thumbnail_generator_;
  std::vector<Handler> handlers_;
  std::unique_ptr<AccountListener> account_listener_;
  SettingsManager settings_manager_;
  std::list<CloudProviderAccount> accounts_;
  int64_t version_ = 0;
  stdx::any_invocable<void(const CloudFactory*, std::stringstream&) const>
      append_auth_urls_;
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
