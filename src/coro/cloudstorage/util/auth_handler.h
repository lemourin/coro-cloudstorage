#ifndef CORO_CLOUDSTORAGE_AUTH_HANDLER_H
#define CORO_CLOUDSTORAGE_AUTH_HANDLER_H

#include <coro/http/http.h>
#include <coro/http/http_parse.h>

namespace coro::cloudstorage::util {

template <typename CloudProvider, coro::http::HttpClient HttpClient>
class AuthHandler {
 public:
  AuthHandler(event_base* event_loop, const HttpClient& http,
              typename CloudProvider::Auth::AuthData auth_data)
      : event_loop_(event_loop),
        http_(&http),
        auth_data_(std::move(auth_data)) {}

  Task<typename CloudProvider::Auth::AuthToken> operator()(
      coro::http::Request<> request, coro::stdx::stop_token stop_token) const {
    auto query =
        http::ParseQuery(http::ParseUri(request.url).query.value_or(""));
    auto it = query.find("code");
    if (it != std::end(query)) {
      co_return co_await CloudProvider::Auth::ExchangeAuthorizationCode(
          *http_, auth_data_, it->second, stop_token);
    } else {
      throw http::HttpException(http::HttpException::kBadRequest);
    }
  }

 private:
  event_base* event_loop_;
  const HttpClient* http_;
  typename CloudProvider::Auth::AuthData auth_data_;
};

template <typename CloudProvider, coro::http::HttpClient HttpClient>
auto MakeAuthHandler(event_base* event_loop, const HttpClient& http,
                     typename CloudProvider::Auth::AuthData auth_data) {
  return AuthHandler<CloudProvider, HttpClient>(event_loop, http,
                                                std::move(auth_data));
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_AUTH_HANDLER_H
