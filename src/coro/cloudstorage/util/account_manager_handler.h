#ifndef CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
#define CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H

#include <coro/cloudstorage/util/auth_token_manager.h>
#include <coro/cloudstorage/util/proxy_handler.h>
#include <coro/cloudstorage/util/webdav_utils.h>
#include <coro/http/http.h>
#include <coro/stdx/any_invocable.h>
#include <coro/util/type_list.h>

#include <regex>

namespace coro::cloudstorage::util {

template <typename CloudProviderTypeList, typename CloudFactory,
          typename AccountListener,
          typename AuthTokenManagerT = AuthTokenManager>
class AccountManagerHandler;

template <typename... CloudProviders, typename CloudFactory,
          typename AccountListener, typename AuthTokenManagerT>
class AccountManagerHandler<coro::util::TypeList<CloudProviders...>,
                            CloudFactory, AccountListener, AuthTokenManagerT> {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;
  using HandlerType = coro::stdx::any_invocable<Task<Response>(
      Request, coro::stdx::stop_token)>;

  struct Data;

  template <typename CloudProvider>
  struct OnAuthTokenChanged {
    void operator()(typename CloudProvider::Auth::AuthToken auth_token) {
      d->auth_token_manager.template SaveToken<CloudProvider>(
          std::move(auth_token), account_id);
    }
    Data* d;
    std::string account_id;
  };

  struct CloudProviderAccount {
    template <typename T>
    using CloudProviderT =
        decltype(std::declval<CloudFactory>().template Create<T>(
            std::declval<typename T::Auth::AuthToken>(),
            std::declval<OnAuthTokenChanged<T>>()));

    std::string id;
    std::variant<CloudProviderT<CloudProviders>...> provider;
  };

  struct Handler {
    std::string id;
    std::regex regex;
    HandlerType handler;
  };

  struct Data {
    Data(const CloudFactory& factory, AccountListener account_listener,
         AuthTokenManagerT auth_token_manager)
        : factory(factory),
          account_listener(std::move(account_listener)),
          auth_token_manager(std::move(auth_token_manager)) {}

    ~Data() {
      for (auto& account : accounts) {
        account_listener.OnDestroy(&account);
      }
    }

    template <typename CloudProvider>
    void RemoveCloudProvider(std::string id) {
      for (auto it = std::begin(accounts); it != std::end(accounts);) {
        if (it->id == id) {
          account_listener.OnDestroy(&*it);
          it = accounts.erase(it);
        } else {
          it++;
        }
      }
      for (auto it = std::begin(handlers); it != std::end(handlers);) {
        if (it->id == id) {
          it = handlers.erase(it);
        } else {
          it++;
        }
      }
      auth_token_manager.template RemoveToken<CloudProvider>(id);
    }

    template <typename CloudProvider>
    void AddAuthHandler() {
      handlers.emplace_back(Handler{
          .regex = std::regex("/auth(/" +
                              std::string(GetCloudProviderId<CloudProvider>()) +
                              ".*$)"),
          .handler = factory.template CreateAuthHandler<CloudProvider>(
              OnAuthTokenCreated<CloudProvider>{this})});
    }

    const CloudFactory& factory;
    std::vector<Handler> handlers;
    AccountListener account_listener;
    AuthTokenManagerT auth_token_manager;
    std::list<CloudProviderAccount> accounts;
  };

  AccountManagerHandler(const CloudFactory& factory,
                        AccountListener account_listener,
                        AuthTokenManagerT auth_token_manager)
      : d_(std::make_unique<Data>(factory, std::move(account_listener),
                                  std::move(auth_token_manager))) {
    (d_->template AddAuthHandler<CloudProviders>(), ...);
    for (const auto& any_token : d_->auth_token_manager.template LoadTokenData<
                                 coro::util::TypeList<CloudProviders...>>()) {
      std::visit(
          [this](auto token) {
            OnAuthTokenCreated<typename decltype(token)::CloudProvider>{
                d_.get()}(std::move(token));
          },
          any_token);
    }
  }

  bool CanHandleUrl(std::string_view url) const {
    if (url.empty() || url == "/") {
      return true;
    }
    for (const auto& handler : d_->handlers) {
      if (std::regex_match(std::string(url), handler.regex)) {
        return true;
      }
    }
    return false;
  }

  Task<Response> operator()(Request request,
                            coro::stdx::stop_token stop_token) {
    if (request.method == coro::http::Method::kOptions) {
      co_return Response{
          .status = 204,
          .headers = {{"Allow", "OPTIONS, GET, HEAD, POST, PROPFIND"},
                      {"DAV", "1"}}};
    }
    for (auto& handler : d_->handlers) {
      if (std::regex_match(request.url, handler.regex)) {
        auto url = request.url;
        auto response =
            co_await handler.handler(std::move(request), stop_token);
        std::smatch match;
        if (response.status == 302 &&
            std::regex_match(url, match, std::regex("/auth(/[^?]*)?.*$"))) {
          co_return Response{.status = 302,
                             .headers = {{"Location", match[1].str() + "/"}}};
        } else {
          co_return response;
        }
      }
    }
    if (request.url.empty() || request.url == "/") {
      if (request.method == coro::http::Method::kPropfind) {
        std::vector<std::string> responses = {GetElement(
            ElementData{.path = "/", .name = "root", .is_directory = true})};
        if (coro::http::GetHeader(request.headers, "Depth") == "1") {
          for (const auto& account : d_->accounts) {
            responses.push_back(
                GetElement(ElementData{.path = "/" + account.id + "/",
                                       .name = account.id,
                                       .is_directory = true}));
          }
        }
        co_return coro::http::Response<>{
            .status = 207,
            .headers = {{"Content-Type", "text/xml"}},
            .body = CreateBody(GetMultiStatusResponse(responses))};
      } else {
        co_return coro::http::Response<>{.status = 200, .body = GetHomePage()};
      }
    } else {
      co_return coro::http::Response<>{.status = 302,
                                       .headers = {{"Location", "/"}}};
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
    stream << "<tr><td><a href='" << url << "'>"
           << GetCloudProviderId<CloudProvider>() << "</a></td>";
    stream << "</tr>";
  }

  template <typename CloudProvider>
  struct OnAuthTokenCreated {
    void operator()(typename CloudProvider::Auth::AuthToken auth_token) const {
      auto account_id = std::string(GetCloudProviderId<CloudProvider>());
      for (const auto& entry : d->accounts) {
        if (entry.id == account_id) {
          return;
        }
      }
      d->handlers.emplace_back(Handler{
          .id = account_id,
          .regex = std::regex("/remove(/" + account_id + ".*$)"),
          .handler = [d = this->d, id = account_id](
                         Request request,
                         coro::stdx::stop_token) -> Task<Response> {
            d->template RemoveCloudProvider<CloudProvider>(id);
            co_return Response{.status = 302, .headers = {{"Location", "/"}}};
          }});

      d->auth_token_manager.template SaveToken<CloudProvider>(auth_token,
                                                              account_id);

      d->accounts.emplace_back(CloudProviderAccount{
          .id = account_id,
          .provider = d->factory.template Create<CloudProvider>(
              auth_token, OnAuthTokenChanged<CloudProvider>{d, account_id})});

      auto* provider =
          &std::get<typename CloudProviderAccount::template CloudProviderT<
              CloudProvider>>(d->accounts.back().provider);
      d->handlers.emplace_back(Handler{
          .id = account_id,
          .regex = std::regex("/" + account_id + "(.*$)"),
          .handler = HandlerType(ProxyHandler(provider, "/" + account_id))});
      d->account_listener.OnCreate(&d->accounts.back());
    }
    Data* d;
  };

  Generator<std::string> GetHomePage() {
    std::stringstream result;
    result << "<html><body><table>";
    (AppendAuthUrl<CloudProviders>(d_->factory, result), ...);
    result << "</table><table>";

    for (const auto& account : d_->accounts) {
      result << "<tr><td><a href='/" << account.id << "/'>" << account.id
             << "</a></td>";
      result << "<td><form action='/remove/" << account.id
             << "' method='POST' style='margin: auto;'><input "
                "type='submit' value='remove'/></form></td>";
      result << "</tr>";
    }

    result << "</table></body></html>";
    co_yield result.str();
  }

  static Generator<std::string> CreateBody(std::string body) {
    co_yield std::move(body);
  }

  std::unique_ptr<Data> d_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
