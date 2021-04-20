#ifndef CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
#define CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H

#include <coro/cloudstorage/abstract_cloud_provider.h>
#include <coro/cloudstorage/util/auth_handler.h>
#include <coro/cloudstorage/util/auth_token_manager.h>
#include <coro/cloudstorage/util/proxy_handler.h>
#include <coro/cloudstorage/util/serialize_utils.h>
#include <coro/cloudstorage/util/string_utils.h>
#include <coro/cloudstorage/util/webdav_utils.h>
#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/util/type_list.h>

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

  struct Data;

  template <typename CloudProvider>
  struct OnAuthTokenChanged {
    void operator()(typename CloudProvider::Auth::AuthToken auth_token) {
      if (*account_id) {
        d->auth_token_manager.template SaveToken<CloudProvider>(
            std::move(auth_token), **account_id);
      }
    }
    Data* d;
    std::shared_ptr<std::optional<std::string>> account_id;
  };

  template <typename CloudProvider>
  static std::string GetAccountId(std::string_view username) {
    return StrCat("[", CloudProvider::kId, "] ", username);
  }

  struct CloudProviderAccount {
    template <typename T>
    using CloudProviderT =
        decltype(std::declval<CloudFactory>().template Create<T>(
            std::declval<typename T::Auth::AuthToken>(),
            std::declval<OnAuthTokenChanged<T>>()));

    std::string GetId() const {
      return std::visit(
          [&](const auto& p) {
            return GetAccountId<
                typename std::remove_cvref_t<decltype(p)>::Type>(username);
          },
          provider);
    }

    std::string username;
    std::variant<CloudProviderT<CloudProviders>...> provider;
    stdx::stop_source stop_source;
  };

  using AbstractCloudProviderT = AbstractCloudProvider<
      ::coro::util::TypeList<typename CloudProviderAccount::
                                 template CloudProviderT<CloudProviders>...>>;

  struct Data {
    Data(const CloudFactory& factory,
         const ThumbnailGenerator& thumbnail_generator,
         AccountListener account_listener, AuthTokenManagerT auth_token_manager)
        : factory(factory),
          thumbnail_generator(thumbnail_generator),
          account_listener(std::move(account_listener)),
          auth_token_manager(std::move(auth_token_manager)) {
      handlers.emplace_back(
          Handler{.prefix = "/static/", .handler = StaticFileHandler{}});
    }

    ~Data() {
      for (auto& account : accounts) {
        account_listener.OnDestroy(&account);
      }
    }

    template <typename CloudProvider>
    void RemoveCloudProvider(std::string_view username) {
      auth_token_manager.template RemoveToken<CloudProvider>(username);
      auto account_id = GetAccountId<CloudProvider>(username);
      for (auto it = std::begin(accounts); it != std::end(accounts);) {
        if (it->GetId() == account_id) {
          it->stop_source.request_stop();
          account_listener.OnDestroy(&*it);
          it = accounts.erase(it);
        } else {
          it++;
        }
      }
      for (auto it = std::begin(handlers); it != std::end(handlers);) {
        if (it->id == account_id) {
          it = handlers.erase(it);
        } else {
          it++;
        }
      }
    }

    struct StaticFileHandler {
      template <typename CloudProvider>
      static bool GetIcon(std::string_view name,
                          std::optional<std::string_view>& output) {
        if (name == StrCat("/static/", CloudProvider::kId, ".png")) {
          output = CloudProvider::kIcon;
          return true;
        } else {
          return false;
        }
      }

      Task<Response> operator()(Request request, stdx::stop_token) const {
        std::optional<std::string_view> content;
        std::string mime_type;
        (GetIcon<CloudProviders>(request.url, content) || ...);
        if (content) {
          mime_type = "image/png";
        } else if (request.url == "/static/default.css") {
          content = util::kAssetsStylesDefaultCss;
          mime_type = "text/css";
        } else if (request.url == "/static/user-trash.svg") {
          content = util::kAssetsIconsUserTrashSvg;
          mime_type = "image/svg+xml";
        }
        if (!content) {
          co_return Response{.status = 404};
        }
        co_return Response{
            .status = 200,
            .headers = {{"Content-Type", std::move(mime_type)},
                        {"Content-Length", std::to_string(content->size())},
                        {"Cache-Control", "public"},
                        {"Cache-Control", "max-age=604800"}},
            .body = CreateBody(std::string(*content))};
      }
    };

    template <typename CloudProvider>
    struct AuthHandler {
      using AuthToken = typename CloudProvider::Auth::AuthToken;

      Task<Response> operator()(Request request,
                                stdx::stop_token stop_token) const {
        auto result =
            co_await d->factory.template CreateAuthHandler<CloudProvider>()(
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
        auto username =
            std::make_shared<std::optional<std::string>>(std::nullopt);
        auto provider = d->factory.template Create<CloudProvider>(
            auth_token, OnAuthTokenChanged<CloudProvider>{d, username});
        auto general_data =
            co_await provider.GetGeneralData(std::move(stop_token));
        *username = std::move(general_data.username);
        d->template RemoveCloudProvider<CloudProvider>(
            GetAccountId<CloudProvider>(**username));
        d->auth_token_manager.template SaveToken<CloudProvider>(
            std::move(auth_token), **username);
        d->OnCloudProviderCreated<CloudProvider>(std::move(provider),
                                                 **username);
        co_return Response{
            .status = 302,
            .headers = {{"Location",
                         "/" + http::EncodeUri(
                                   GetAccountId<CloudProvider>(**username))}}};
      }

      Data* d;
    };

    template <typename CloudProvider>
    struct OnRemoveHandler {
      Task<Response> operator()(Request request,
                                stdx::stop_token stop_token) const {
        d->template RemoveCloudProvider<CloudProvider>(username);
        co_return Response{.status = 302, .headers = {{"Location", "/"}}};
      }
      Data* d;
      std::string username;
    };

    template <typename CloudProvider>
    void AddAuthHandler() {
      handlers.emplace_back(Handler{
          .prefix = "/auth/" + std::string(GetCloudProviderId<CloudProvider>()),
          .handler = AuthHandler<CloudProvider>{this}});
    }

    template <typename CloudProvider, typename CloudProviderT>
    void OnCloudProviderCreated(CloudProviderT provider_impl,
                                std::string_view username) {
      if (username.empty()) {
        return;
      }
      std::string account_id = GetAccountId<CloudProvider>(username);
      for (const auto& entry : accounts) {
        if (entry.GetId() == account_id) {
          return;
        }
      }
      handlers.emplace_back(
          Handler{.id = std::string(account_id),
                  .prefix = StrCat("/remove/", account_id),
                  .handler = OnRemoveHandler<CloudProvider>{
                      .d = this, .username = std::string(username)}});

      accounts.emplace_back(
          CloudProviderAccount{.username = std::string(username),
                               .provider = std::move(provider_impl)});

      auto* provider =
          &std::get<typename CloudProviderAccount::template CloudProviderT<
              CloudProvider>>(accounts.back().provider);
      handlers.emplace_back(
          Handler{.id = std::string(account_id),
                  .prefix = StrCat("/", account_id),
                  .handler = ProxyHandler(thumbnail_generator, provider,
                                          StrCat("/", account_id))});
      account_listener.OnCreate(&accounts.back());
    }

    struct Handler {
      std::string id;
      std::string prefix;
      std::variant<StaticFileHandler, AuthHandler<CloudProviders>...,
                   OnRemoveHandler<CloudProviders>...,
                   ProxyHandler<typename CloudProviderAccount::
                                    template CloudProviderT<CloudProviders>,
                                ThumbnailGenerator>...>
          handler;
    };

    const CloudFactory& factory;
    const ThumbnailGenerator& thumbnail_generator;
    std::vector<Handler> handlers;
    AccountListener account_listener;
    AuthTokenManagerT auth_token_manager;
    std::list<CloudProviderAccount> accounts;
  };

  AccountManagerHandler(
      const CloudFactory& factory,
      const ThumbnailGenerator& thumbnail_generator,
      AccountListener account_listener,
      AuthTokenManagerT auth_token_manager = AuthTokenManagerT{})
      : d_(std::make_unique<Data>(factory, thumbnail_generator,
                                  std::move(account_listener),
                                  std::move(auth_token_manager))) {
    (d_->template AddAuthHandler<CloudProviders>(), ...);
    for (const auto& any_token : d_->auth_token_manager.template LoadTokenData<
                                 coro::util::TypeList<CloudProviders...>>()) {
      std::visit(
          [d = d_.get()](auto token) {
            using CloudProvider = typename decltype(token)::CloudProvider;
            auto id = std::move(token.id);
            d->template OnCloudProviderCreated<CloudProvider>(
                d->factory.template Create<CloudProvider>(
                    std::move(token),
                    OnAuthTokenChanged<CloudProvider>{
                        d, std::make_shared<std::optional<std::string>>(id)}),
                id);
          },
          any_token);
    }
  }

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
    for (auto& handler : d_->handlers) {
      if (path.starts_with(handler.prefix)) {
        if (auto account_it =
                std::find_if(d_->accounts.begin(), d_->accounts.end(),
                             [&](const auto& account) {
                               return account.GetId() == handler.id;
                             });
            account_it != d_->accounts.end()) {
          ::coro::util::StopTokenOr stop_token_or(
              account_it->stop_source.get_token(), std::move(stop_token));
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
          for (const auto& account : d_->accounts) {
            responses.push_back(
                GetElement(ElementData{.path = "/" + account.GetId() + "/",
                                       .name = account.GetId(),
                                       .is_directory = true}));
          }
        }
        co_return Response{
            .status = 207,
            .headers = {{"Content-Type", "text/xml"}},
            .body = CreateBody(GetMultiStatusResponse(responses))};
      } else {
        co_return Response{.status = 200, .body = GetHomePage()};
      }
    } else {
      co_return Response{.status = 302, .headers = {{"Location", "/"}}};
    }
  }

 private:
  template <typename CloudProvider>
  static void AppendAuthUrl(const CloudFactory& factory,
                            std::stringstream& stream) {
    std::string id(GetCloudProviderId<CloudProvider>());
    std::string url =
        factory.template GetAuthorizationUrl<CloudProvider>().value_or(
            "/auth/" + id);
    stream << "<div class='thumbnail-container'>"
              "<a href='"
           << url
           << "'>"
              "<img class='provider-icon' src='/static/"
           << GetCloudProviderId<CloudProvider>() << ".png'></a>";
    stream << "</div>";
  }

  Generator<std::string> GetHomePage() {
    std::stringstream result;
    result << "<html><meta name='viewport' content='width=device-width, "
              "initial-scale=1'>"
              "<link rel=stylesheet href='/static/default.css'>"
              "<body>"
              "<table class='content-table'>"
              "<tr><td>"
              "<h3 class='table-header'>ADD CLOUD PROVIDERS</h3>"
              "</td></tr>"
              "<tr><td><div class='supported-providers'>";
    (AppendAuthUrl<CloudProviders>(d_->factory, result), ...);
    result << "</div></td></tr></table>"
              "<h3 class='table-header'>ACCOUNT LIST</h3>"
              "<table class='content-table'>";
    for (const auto& account : d_->accounts) {
      result << "<tr>";
      std::visit(
          [&](const auto& provider) {
            result << "<td class='thumbnail-container'>"
                      "<image class='provider-icon' src='/static/"
                   << std::remove_cvref_t<decltype(provider)>::Type::kId
                   << ".png'></td>";
          },
          account.provider);
      result << "<td><a href='/" << http::EncodeUri(account.GetId()) << "/'>"
             << account.username << "</a></td>";
      result << "<td class='trash-container'>"
                "<form action='/remove/"
             << http::EncodeUri(account.GetId())
             << "' method='POST' style='margin: auto;'>"
                "<button type='submit'>"
                "<img class='trash-icon' src='/static/user-trash.svg'>"
                "</input>"
                "</form>"
                "</td>"
                "</tr>";
    }

    result << "</table></body></html>";
    co_yield result.str();
  }

  static Generator<std::string> CreateBody(std::string body) {
    co_yield std::move(body);
  }

  std::unique_ptr<Data> d_;
};

template <typename CloudProviderTypeList, typename CloudFactory,
          typename AccountListener,
          typename AuthTokenManagerT = AuthTokenManager>
auto MakeAccountManagerHandler(
    const CloudFactory& factory, AccountListener account_listener,
    AuthTokenManagerT auth_token_manager = AuthTokenManagerT()) {
  return AccountManagerHandler<CloudProviderTypeList, CloudFactory,
                               AccountListener, AuthTokenManagerT>(
      factory, std::move(account_listener), std::move(auth_token_manager));
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
