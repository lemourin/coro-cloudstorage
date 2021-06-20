#ifndef CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
#define CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H

#include <coro/cloudstorage/util/assets.h>
#include <coro/cloudstorage/util/auth_handler.h>
#include <coro/cloudstorage/util/auth_token_manager.h>
#include <coro/cloudstorage/util/cloud_provider_account.h>
#include <coro/cloudstorage/util/get_size_handler.h>
#include <coro/cloudstorage/util/proxy_handler.h>
#include <coro/cloudstorage/util/serialize_utils.h>
#include <coro/cloudstorage/util/static_file_handler.h>
#include <coro/cloudstorage/util/string_utils.h>
#include <coro/cloudstorage/util/webdav_utils.h>
#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/util/type_list.h>
#include <coro/when_all.h>
#include <fmt/format.h>

#include <list>

namespace coro::cloudstorage::util {

template <typename CloudProviderTypeList, typename CloudFactory,
          typename ThumbnailGenerator, typename AccountListener,
          typename AuthTokenManagerT = AuthTokenManager>
class AccountManagerHandler;

template <typename... CloudProviders, typename CloudFactory,
          typename ThumbnailGenerator, typename AccountListener,
          typename AuthTokenManagerT>
class AccountManagerHandler<coro::util::TypeList<CloudProviders...>,
                            CloudFactory, ThumbnailGenerator, AccountListener,
                            AuthTokenManagerT> {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;

  using CloudProviderAccount = coro::cloudstorage::util::CloudProviderAccount<
      coro::util::TypeList<CloudProviders...>, CloudFactory, AuthTokenManagerT>;

  Task<> Quit() {
    std::vector<Task<>> tasks;
    for (auto& account : accounts_) {
      if (!account.stop_token().stop_requested()) {
        account.stop_source_.request_stop();
        tasks.emplace_back(account_listener_.OnDestroy(&account));
      }
    }
    co_await WhenAll(std::move(tasks));
    accounts_.clear();
  }

  AccountManagerHandler(
      const CloudFactory& factory,
      const ThumbnailGenerator& thumbnail_generator,
      AccountListener account_listener,
      AuthTokenManagerT auth_token_manager = AuthTokenManagerT{})
      : factory_(factory),
        thumbnail_generator_(thumbnail_generator),
        account_listener_(std::move(account_listener)),
        auth_token_manager_(std::move(auth_token_manager)) {
    handlers_.emplace_back(
        Handler{.prefix = "/static/", .handler = StaticFileHandler{}});
    handlers_.emplace_back(
        Handler{.prefix = "/size", .handler = GetSizeHandler{&accounts_}});
    (AddAuthHandler<CloudProviders>(), ...);
    for (const auto& any_token : auth_token_manager_.template LoadTokenData<
                                 coro::util::TypeList<CloudProviders...>>()) {
      std::visit(
          [&]<typename AuthToken>(AuthToken token) {
            using CloudProvider = typename AuthToken::CloudProvider;
            auto id = std::move(token.id);
            auto* account = CreateAccount<CloudProvider>(
                std::move(token),
                std::make_shared<std::optional<std::string>>(id));
            OnCloudProviderCreated<CloudProvider>(account);
          },
          any_token);
    }
  }

  AccountManagerHandler(AccountManagerHandler&&) = delete;
  AccountManagerHandler& operator=(AccountManagerHandler&&) = delete;

  Task<Response> operator()(Request request,
                            coro::stdx::stop_token stop_token) {
    if (request.method == coro::http::Method::kOptions) {
      co_return Response{
          .status = 204,
          .headers = {{"Allow",
                       "OPTIONS, GET, HEAD, POST, PUT, DELETE, MOVE, "
                       "MKCOL, PROPFIND, PATCH, PROPPATCH"},
                      {"DAV", "1"},
                      {"Access-Control-Allow-Origin", "*"},
                      {"Access-Control-Allow-Headers", "*"}}};
    }
    auto path_opt = http::ParseUri(request.url).path;
    if (!path_opt) {
      co_return Response{.status = 400};
    }
    auto path = http::DecodeUri(std::move(*path_opt));
    for (auto& handler : handlers_) {
      if (path.starts_with(handler.prefix)) {
        if (auto account_it = std::find_if(accounts_.begin(), accounts_.end(),
                                           [&](const auto& account) {
                                             return account.GetId() ==
                                                    handler.id;
                                           });
            account_it != accounts_.end()) {
          coro::util::StopTokenOr stop_token_or(account_it->stop_token(),
                                                std::move(stop_token));
          co_return co_await std::visit(
              [request = std::move(request),
               stop_token = stop_token_or.GetToken()](auto& d) mutable {
                return d(std::move(request), std::move(stop_token));
              },
              handler.handler);
        } else {
          co_return co_await std::visit(
              [request = std::move(request),
               stop_token = std::move(stop_token)](auto& d) mutable {
                return d(std::move(request), std::move(stop_token));
              },
              handler.handler);
        }
      }
    }
    if (path.empty() || path == "/") {
      if (request.method == coro::http::Method::kPropfind) {
        std::vector<std::string> responses = {GetElement(
            ElementData{.path = "/", .name = "root", .is_directory = true})};
        if (coro::http::GetHeader(request.headers, "Depth") == "1") {
          for (const auto& account : accounts_) {
            responses.push_back(
                GetElement(ElementData{.path = "/" + account.GetId() + "/",
                                       .name = account.GetId(),
                                       .is_directory = true}));
          }
        }
        co_return Response{
            .status = 207,
            .headers = {{"Content-Type", "text/xml"}},
            .body = http::CreateBody(GetMultiStatusResponse(responses))};
      } else {
        co_return Response{.status = 200,
                           .body = GetHomePage(std::move(stop_token))};
      }
    } else {
      co_return Response{.status = 302, .headers = {{"Location", "/"}}};
    }
  }

 private:
  template <typename CloudProvider>
  using CloudProviderT =
      typename CloudProviderAccount::template CloudProviderT<CloudProvider>;

  template <typename CloudProvider>
  using OnAuthTokenChanged =
      internal::OnAuthTokenChanged<AuthTokenManagerT, CloudProvider>;

  void RemoveHandler(std::string_view account_id) {
    for (auto it = std::begin(handlers_); it != std::end(handlers_);) {
      if (it->id == account_id) {
        it = handlers_.erase(it);
      } else {
        it++;
      }
    }
  }

  template <typename CloudProvider, typename F>
  Task<> RemoveCloudProvider(const F& predicate) {
    for (auto it = std::begin(accounts_); it != std::end(accounts_);) {
      if (predicate(*it) && !it->stop_token().stop_requested()) {
        it->stop_source_.request_stop();
        co_await account_listener_.OnDestroy(&*it);
        auth_token_manager_.template RemoveToken<CloudProvider>(it->username());
        RemoveHandler(it->GetId());
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
      auto result =
          co_await d->factory_.template CreateAuthHandler<CloudProvider>()(
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

  template <typename CloudProvider>
  struct OnRemoveHandler {
    Task<Response> operator()(Request request,
                              stdx::stop_token stop_token) const {
      co_await d->template RemoveCloudProvider<CloudProvider>(
          [id = GetAccountId<CloudProvider>(username)](const auto& account) {
            return account.GetId() == id;
          });
      co_return Response{.status = 302, .headers = {{"Location", "/"}}};
    }
    AccountManagerHandler* d;
    std::string username;
  };

  template <typename CloudProvider>
  void AddAuthHandler() {
    handlers_.emplace_back(Handler{
        .prefix = "/auth/" + std::string(GetCloudProviderId<CloudProvider>()),
        .handler = AuthHandler<CloudProvider>{this}});
  }

  template <typename CloudProvider>
  void OnCloudProviderCreated(CloudProviderAccount* account) {
    std::string account_id = GetAccountId<CloudProvider>(account->username());
    try {
      handlers_.emplace_back(Handler{
          .id = std::string(account_id),
          .prefix = StrCat("/remove/", account_id),
          .handler = OnRemoveHandler<CloudProvider>{
              .d = this, .username = std::string(account->username())}});

      auto& provider =
          std::get<CloudProviderT<CloudProvider>>(account->provider());
      handlers_.emplace_back(
          Handler{.id = std::string(account_id),
                  .prefix = StrCat("/", account_id),
                  .handler = ProxyHandler(thumbnail_generator_, &provider,
                                          StrCat("/", account_id))});

      account_listener_.OnCreate(account);
    } catch (...) {
      RemoveHandler(account_id);
      throw;
    }
  }

  struct CreateF {
    template <typename CloudProviderT, typename... Args>
    CloudProviderAccount* operator()(Args&&... args) const {
      return &accounts.emplace_back(username, version,
                                    std::in_place_type_t<CloudProviderT>{},
                                    std::forward<Args>(args)...);
    }

    std::string username;
    int64_t version;
    std::list<CloudProviderAccount>& accounts;
  };

  template <typename CloudProvider>
  CloudProviderAccount* CreateAccount(
      typename CloudProvider::Auth::AuthToken auth_token,
      std::shared_ptr<std::optional<std::string>> username) {
    return CreateCloudProvider<CloudProvider>{}(
        CreateF{username->value_or(""), version_++, accounts_}, factory_,
        std::move(auth_token),
        OnAuthTokenChanged<CloudProvider>{&auth_token_manager_, username});
  }

  template <typename CloudProvider>
  Task<CloudProviderAccount*> Create(
      typename CloudProvider::Auth::AuthToken auth_token,
      stdx::stop_token stop_token) {
    auto username = std::make_shared<std::optional<std::string>>(std::nullopt);
    auto* account = CreateAccount<CloudProvider>(auth_token, username);
    auto version = account->version_;
    auto& provider =
        std::get<CloudProviderT<CloudProvider>>(account->provider());
    bool on_create_called = false;
    std::exception_ptr exception;
    try {
      auto general_data =
          co_await provider.GetGeneralData(std::move(stop_token));
      *username = std::move(general_data.username);
      account->username_ = **username;
      co_await RemoveCloudProvider<CloudProvider>([&](const auto& entry) {
        return entry.version_ < version &&
               entry.GetId() == GetAccountId<CloudProvider>(**username);
      });
      for (const auto& entry : accounts_) {
        if (entry.version_ == version) {
          OnCloudProviderCreated<CloudProvider>(account);
          on_create_called = true;
          auth_token_manager_.template SaveToken<CloudProvider>(
              std::move(auth_token), **username);
          break;
        }
      }
      co_return account;
    } catch (...) {
      exception = std::current_exception();
    }
    co_await RemoveCloudProvider<CloudProvider>(
        [&](const auto& entry) { return entry.version_ == version; });
    std::rethrow_exception(exception);
  }

  using StaticFileHandler =
      coro::cloudstorage::util::StaticFileHandler<CloudProviders...>;

  using GetSizeHandler =
      coro::cloudstorage::util::GetSizeHandler<CloudProviderAccount>;

  struct Handler {
    std::string id;
    std::string prefix;
    std::variant<
        StaticFileHandler, GetSizeHandler, AuthHandler<CloudProviders>...,
        OnRemoveHandler<CloudProviders>...,
        ProxyHandler<CloudProviderT<CloudProviders>, ThumbnailGenerator>...>
        handler;
  };

  template <typename CloudProvider>
  static void AppendAuthUrl(const CloudFactory& factory,
                            std::stringstream& stream) {
    std::string id(GetCloudProviderId<CloudProvider>());
    std::string url =
        factory.template GetAuthorizationUrl<CloudProvider>().value_or(
            util::StrCat("/auth/", id));
    stream << fmt::format(
        kAssetsHtmlProviderEntryHtml, fmt::arg("provider_url", url),
        fmt::arg("image_url", util::StrCat("/static/", id, ".png")));
  }

  Generator<std::string> GetHomePage(stdx::stop_token stop_token) {
    std::stringstream result;
    result << "<html>"
              "<head>"
              "  <title>coro-cloudstorage</title>"
              "  <meta name='viewport' "
              "        content='width=device-width, initial-scale=1'>"
              "  <link rel=stylesheet href='/static/default.css'>"
              "  <script src='/static/account_list_main.js'></script>"
              "</head>"
              "<body>"
              "<table class='content-table'>"
              "<tr><td>"
              "<h3 class='table-header'>ADD CLOUD PROVIDERS</h3>"
              "</td></tr>"
              "<tr><td><div class='supported-providers'>";
    (AppendAuthUrl<CloudProviders>(factory_, result), ...);
    result << "</div></td></tr>"
              "</table>"
              "<h3 class='table-header'>ACCOUNT LIST</h3>"
              "<table class='content-table'>";
    for (const auto& account : accounts_) {
      auto provider_id = std::visit(
          []<typename CloudProvider>(const CloudProvider&) {
            return CloudProvider::Type::kId;
          },
          account.provider());
      std::string provider_size;
      result << fmt::format(
          kAssetsHtmlAccountEntryHtml,
          fmt::arg("provider_icon",
                   util::StrCat("/static/", provider_id, ".png")),
          fmt::arg("provider_url",
                   util::StrCat("/", http::EncodeUri(account.GetId()), "/")),
          fmt::arg("provider_name", account.username()),
          fmt::arg("provider_remove_url",
                   util::StrCat("/remove/", http::EncodeUri(account.GetId()))),
          fmt::arg("provider_id", http::EncodeUri(account.GetId())));
    }

    result << "</table>"
              "</body>"
              "</html>";
    co_yield std::move(result).str();
  }

  const CloudFactory& factory_;
  const ThumbnailGenerator& thumbnail_generator_;
  std::vector<Handler> handlers_;
  AccountListener account_listener_;
  AuthTokenManagerT auth_token_manager_;
  std::list<CloudProviderAccount> accounts_;
  int64_t version_ = 0;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
