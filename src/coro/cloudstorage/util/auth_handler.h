#ifndef CORO_CLOUDSTORAGE_AUTH_HANDLER_H
#define CORO_CLOUDSTORAGE_AUTH_HANDLER_H

#include <coro/http/http.h>
#include <coro/http/http_parse.h>

namespace coro::cloudstorage::util {

template <template <typename> typename AuthData, typename CloudProvider,
          coro::http::HttpClient HttpClient, typename OnAuthTokenCreated>
class AuthHandler {
 public:
  AuthHandler(event_base* event_loop, HttpClient& http,
              OnAuthTokenCreated on_auth_token_created)
      : event_loop_(event_loop),
        http_(http),
        on_auth_token_created_(std::move(on_auth_token_created)) {}

  Task<http::Response<>> operator()(coro::http::Request<> request,
                                    coro::stdx::stop_token stop_token) const {
    if constexpr (std::is_same_v<CloudProvider, Mega>) {
      if (request.method == "POST") {
        auto query =
            http::ParseQuery(co_await http::GetBody(std::move(*request.body)));
        auto it1 = query.find("email");
        auto it2 = query.find("password");
        if (it1 != std::end(query) && it2 != std::end(query)) {
          auto it3 = query.find("twofactor");
          Mega::UserCredential credential = {
              .email = it1->second,
              .password_hash = Mega::GetPasswordHash(it2->second),
              .twofactor = it3 != std::end(query)
                               ? std::make_optional(it3->second)
                               : std::nullopt};
          auto session = co_await Mega::GetSession(
              event_loop_, http_, std::move(credential), AuthData<Mega>{}(),
              stop_token);
          on_auth_token_created_(Mega::AuthToken{std::move(session)});
          co_return http::Response<>{.status = 302};
        }
      } else {
        co_return http::Response<>{.status = 200, .body = GenerateLoginPage()};
      }
    } else {
      auto query =
          http::ParseQuery(http::ParseUri(request.url).query.value_or(""));
      auto it = query.find("code");
      if (it != std::end(query)) {
        on_auth_token_created_(
            co_await CloudProvider::Auth::ExchangeAuthorizationCode(
                http_, AuthData<CloudProvider>{}(), it->second, stop_token));
        co_return http::Response<>{.status = 302};
      }
    }
    co_return http::Response<>{.status = 400};
  }

 private:
  Generator<std::string> GenerateLoginPage() const {
    co_yield R"(
      <html>
        <body>
          <form method="post">
            <table>
              <tr>
                <td><label for="email">email:</label></td>
                <td><input type="text" id="email" name="email"/></td>
              </tr>
              <tr>
                <td><label for="password">password:</label></td>
                <td><input type="password" id="password" name="password"/></td>
              </tr>
              <tr><td><input type="submit" value="Submit"/></td></tr>
            </table>
          </form>
        </body>
      </html>
    )";
  }

  event_base* event_loop_;
  HttpClient& http_;
  OnAuthTokenCreated on_auth_token_created_;
};

template <template <typename> typename AuthData, typename CloudProvider,
          coro::http::HttpClient HttpClient, typename OnAuthTokenCreated>
auto MakeAuthHandler(event_base* event_loop, HttpClient& http,
                     OnAuthTokenCreated on_auth_token_created) {
  return AuthHandler<AuthData, CloudProvider, HttpClient, OnAuthTokenCreated>(
      event_loop, http, std::move(on_auth_token_created));
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_AUTH_HANDLER_H
