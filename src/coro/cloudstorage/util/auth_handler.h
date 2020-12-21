#ifndef CORO_CLOUDSTORAGE_AUTH_HANDLER_H
#define CORO_CLOUDSTORAGE_AUTH_HANDLER_H

#include <coro/http/http.h>
#include <coro/http/http_parse.h>

namespace coro::cloudstorage::util {

template <typename CloudProvider, coro::http::HttpClient HttpClient,
          typename OnAuthTokenCreated>
class AuthHandler {
 public:
  AuthHandler(event_base* event_loop, const HttpClient& http,
              typename CloudProvider::Auth::AuthData auth_data,
              OnAuthTokenCreated on_auth_token_created)
      : event_loop_(event_loop),
        http_(&http),
        auth_data_(std::move(auth_data)),
        on_auth_token_created_(std::move(on_auth_token_created)) {}

  Task<http::Response<>> operator()(coro::http::Request<> request,
                                    coro::stdx::stop_token stop_token) const {
    auto query =
        http::ParseQuery(http::ParseUri(request.url).query.value_or(""));
    auto it = query.find("code");
    if (it != std::end(query)) {
      on_auth_token_created_(
          co_await CloudProvider::Auth::ExchangeAuthorizationCode(
              *http_, auth_data_, it->second, stop_token));
      co_return http::Response<>{.status = 302};
    }
    co_return http::Response<>{.status = 400};
  }

 private:
  event_base* event_loop_;
  const HttpClient* http_;
  typename CloudProvider::Auth::AuthData auth_data_;
  OnAuthTokenCreated on_auth_token_created_;
};

template <typename CloudProvider, coro::http::HttpClient HttpClient,
          typename OnAuthTokenCreated>
auto MakeAuthHandler(event_base* event_loop, const HttpClient& http,
                     typename CloudProvider::Auth::AuthData auth_data,
                     OnAuthTokenCreated on_auth_token_created) {
  return AuthHandler<CloudProvider, HttpClient, OnAuthTokenCreated>(
      event_loop, http, std::move(auth_data), std::move(on_auth_token_created));
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_AUTH_HANDLER_H
