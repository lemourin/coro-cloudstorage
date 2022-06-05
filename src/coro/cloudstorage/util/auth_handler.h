#ifndef CORO_CLOUDSTORAGE_AUTH_HANDLER_H
#define CORO_CLOUDSTORAGE_AUTH_HANDLER_H

#include "coro/http/http.h"
#include "coro/http/http_parse.h"

namespace coro::cloudstorage::util {

template <typename Auth>
class AuthHandler {
 public:
  AuthHandler(const http::Http* http, typename Auth::AuthData auth_data)
      : http_(http), auth_data_(std::move(auth_data)) {}

  Task<typename Auth::AuthToken> operator()(
      coro::http::Request<> request, coro::stdx::stop_token stop_token) const {
    auto query =
        http::ParseQuery(http::ParseUri(request.url).query.value_or(""));
    auto it = query.find("code");
    if (it != std::end(query)) {
      co_return co_await Auth::ExchangeAuthorizationCode(
          *http_, auth_data_, it->second, stop_token);
    } else {
      throw http::HttpException(http::HttpException::kBadRequest);
    }
  }

 private:
  const http::Http* http_;
  typename Auth::AuthData auth_data_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_AUTH_HANDLER_H
