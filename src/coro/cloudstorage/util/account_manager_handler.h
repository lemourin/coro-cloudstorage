#ifndef CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
#define CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H

#include <coro/cloudstorage/util/webdav_utils.h>
#include <coro/http/http.h>
#include <coro/stdx/any_invocable.h>
#include <coro/util/for_each.h>

#include <regex>

namespace coro::cloudstorage::util {

template <typename CloudProviders, typename CloudFactory,
          typename AuthTokenManager>
class AccountManagerHandler {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;
  using HandlerType = coro::stdx::any_invocable<Task<Response>(
      Request, coro::stdx::stop_token)>;

  AccountManagerHandler(const CloudFactory& factory,
                        AuthTokenManager auth_token_manager)
      : factory_(factory), auth_token_manager_(std::move(auth_token_manager)) {
    ::coro::util::ForEach<CloudProviders>{}(AddAuthHandlerFunctor{this});
  }

  AccountManagerHandler(const AccountManagerHandler&) = delete;

  bool CanHandleUrl(std::string_view url) const {
    if (url.empty() || url == "/") {
      return true;
    }
    for (const auto& handler : handlers_) {
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
    for (auto& handler : handlers_) {
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
          ::coro::util::ForEach<CloudProviders>{}(
              GenerateRootContent{this, responses});
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
  struct AddAuthHandlerFunctor {
    template <typename CloudProvider>
    void operator()() const {
      handler->AddAuthHandler<CloudProvider>();
    }
    AccountManagerHandler* handler;
  };

  struct GenerateAuthUrlTable {
    template <typename CloudProvider>
    void operator()() const {
      auto auth_token =
          handler->auth_token_manager_.template LoadToken<CloudProvider>();
      std::string id(GetCloudProviderId<CloudProvider>());
      std::string url =
          auth_token
              ? "/" + id + "/"
              : handler->factory_.template GetAuthorizationUrl<CloudProvider>()
                    .value_or("/auth/" + id);
      stream << "<tr><td><a href='" << url << "'>"
             << GetCloudProviderId<CloudProvider>() << "</a></td>";
      if (auth_token) {
        stream << "<td><form action='/remove/" << id
               << "' method='POST' style='margin: auto;'><input type='submit' "
                  "value='remove'/></form></td>";
      }
      stream << "</tr>";
    }
    AccountManagerHandler* handler;
    std::stringstream& stream;
  };

  struct GenerateRootContent {
    template <typename CloudProvider>
    void operator()() const {
      auto auth_token =
          handler->auth_token_manager_.template LoadToken<CloudProvider>();
      std::string id(GetCloudProviderId<CloudProvider>());
      if (auth_token) {
        stream.push_back(GetElement(ElementData{
            .path = "/" + id + "/", .name = id, .is_directory = true}));
      }
    }
    AccountManagerHandler* handler;
    std::vector<std::string>& stream;
  };

  template <typename CloudProvider>
  void AddAuthHandler() {
    auto auth_token = auth_token_manager_.template LoadToken<CloudProvider>();
    if (auth_token) {
      auth_token_manager_.template OnAuthTokenCreated<CloudProvider>(
          *auth_token);
    }
    handlers_.emplace_back(Handler{
        .regex = std::regex("/auth(/" +
                            std::string(GetCloudProviderId<CloudProvider>()) +
                            ".*$)"),
        .handler =
            factory_.template CreateAuthHandler<CloudProvider>([this](auto d) {
              auth_token_manager_.template OnAuthTokenCreated<CloudProvider>(d);
            })});
    handlers_.emplace_back(Handler{
        .regex = std::regex("/remove(/" +
                            std::string(GetCloudProviderId<CloudProvider>()) +
                            ".*$)"),
        .handler = [this](Request request,
                          coro::stdx::stop_token) -> Task<Response> {
          auth_token_manager_.template OnCloudProviderRemoved<CloudProvider>();
          co_return Response{.status = 302, .headers = {{"Location", "/"}}};
        }});
  }

  Generator<std::string> GetHomePage() {
    std::stringstream result;
    result << "<html><body><table>";
    ::coro::util::ForEach<CloudProviders>{}(GenerateAuthUrlTable{this, result});
    result << "</table></body></html>";
    co_yield result.str();
  }

  static Generator<std::string> CreateBody(std::string body) {
    co_yield std::move(body);
  }

  struct Handler {
    std::regex regex;
    HandlerType handler;
  };
  const CloudFactory& factory_;
  std::vector<Handler> handlers_;
  AuthTokenManager auth_token_manager_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_ACCOUNT_MANAGER_HANDLER_H
