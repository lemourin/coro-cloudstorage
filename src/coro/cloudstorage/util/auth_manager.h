#ifndef CORO_CLOUDSTORAGE_AUTH_MANAGER_H
#define CORO_CLOUDSTORAGE_AUTH_MANAGER_H

#include <coro/cloudstorage/cloud_exception.h>
#include <coro/http/http.h>
#include <coro/promise.h>
#include <coro/stdx/stop_source.h>

#include <nlohmann/json.hpp>

namespace coro::cloudstorage::util {

template <typename Auth, http::HttpClient Http, typename OnAuthTokenUpdated>
class AuthManager {
 public:
  using AuthToken = typename Auth::AuthToken;
  using AuthData = typename Auth::AuthData;

  AuthManager(Http& http, AuthToken auth_token, AuthData auth_data,
              OnAuthTokenUpdated on_auth_token_updated)
      : http_(http),
        auth_token_(std::move(auth_token)),
        auth_data_(std::move(auth_data)),
        on_auth_token_updated_(std::move(on_auth_token_updated)) {}

  ~AuthManager() { stop_source_.request_stop(); }

  AuthManager(AuthManager&& manager) noexcept = default;

  template <typename Request>
  Task<typename Http::ResponseType> Fetch(Request request,
                                          stdx::stop_token stop_token) {
    auto response = co_await http_.Fetch(AuthorizeRequest(request), stop_token);
    if (response.status == 401) {
      try {
        co_await RefreshAuthToken(stop_token);
      } catch (const http::HttpException&) {
        throw CloudException(CloudException::Type::kUnauthorized);
      }
      co_return co_await http_.Fetch(AuthorizeRequest(request),
                                     std::move(stop_token));
    } else if (response.status / 100 == 2) {
      co_return response;
    } else {
      throw coro::http::HttpException(
          response.status, co_await http::GetBody(std::move(response.body)));
    }
  }

  template <typename... Args>
  Task<nlohmann::json> FetchJson(Args&&... args) {
    http::ResponseLike auto response =
        co_await Fetch(std::forward<Args>(args)...);
    std::string body = co_await http::GetBody(std::move(response.body));
    co_return nlohmann::json::parse(std::move(body));
  }

 private:
  Task<> RefreshAuthToken(stdx::stop_token stop_token) {
    if (!current_auth_refresh_) {
      current_auth_refresh_ =
          std::make_unique<Promise<AuthToken>>([this]() -> Task<AuthToken> {
            auto stop_token = stop_source_.get_token();
            auto d = this;
            auto auth_token = co_await Auth::RefreshAccessToken(
                d->http_, d->auth_data_, d->auth_token_.refresh_token,
                stop_token);
            if (!stop_token.stop_requested()) {
              d->current_auth_refresh_ = nullptr;
              d->auth_token_ = auth_token;
              d->on_auth_token_updated_(d->auth_token_);
            }
            co_return auth_token;
          });
    }
    co_await current_auth_refresh_->Get(stop_token);
  }

  template <typename Request>
  Request AuthorizeRequest(Request request) {
    request.headers.insert(
        {"Authorization", "Bearer " + auth_token_.access_token});
    return request;
  }

  Http& http_;
  AuthToken auth_token_;
  AuthData auth_data_;
  std::unique_ptr<Promise<AuthToken>> current_auth_refresh_;
  OnAuthTokenUpdated on_auth_token_updated_;
  stdx::stop_source stop_source_;
};

template <typename Auth, http::HttpClient Http, typename OnAuthTokenUpdated>
auto MakeAuthManager(Http& http, typename Auth::AuthToken auth_token,
                     typename Auth::AuthData auth_data,
                     OnAuthTokenUpdated on_auth_token_updated) {
  return AuthManager<Auth, Http, OnAuthTokenUpdated>(
      http, std::move(auth_token), std::move(auth_data),
      std::move(on_auth_token_updated));
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_AUTH_MANAGER_H
