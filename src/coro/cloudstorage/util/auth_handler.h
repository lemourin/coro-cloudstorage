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
    auto query =
        http::ParseQuery(http::ParseUri(request.url).query.value_or(""));
    if (request.body) {
      auto body = co_await http::GetBody(std::move(*request.body));
    }
    if constexpr (std::is_same_v<CloudProvider, Mega>) {
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
        auto session =
            co_await Mega::GetSession(event_loop_, http_, std::move(credential),
                                      AuthData<Mega>{}(), stop_token);
        on_auth_token_created_(Mega::AuthToken{std::move(session)});
      } else {
        throw std::logic_error("invalid credentials");
      }
    } else {
      auto it = query.find("code");
      if (it != std::end(query)) {
        on_auth_token_created_(
            co_await CloudProvider::Auth::ExchangeAuthorizationCode(
                http_, AuthData<CloudProvider>{}(), it->second, stop_token));
      }
    }
    co_return http::Response<>{.status = 200};
  }

 private:
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
