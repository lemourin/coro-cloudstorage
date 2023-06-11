#ifndef CORO_CLOUDSTORAGE_AUTH_MANAGER_H
#define CORO_CLOUDSTORAGE_AUTH_MANAGER_H

#include <nlohmann/json.hpp>

#include "coro/cloudstorage/cloud_exception.h"
#include "coro/cloudstorage/util/on_auth_token_updated.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http.h"
#include "coro/shared_promise.h"
#include "coro/stdx/stop_source.h"

namespace coro::cloudstorage::util {

template <typename Auth>
class RefreshToken {
 public:
  using AuthToken = typename Auth::AuthToken;

  template <typename F>
  explicit RefreshToken(F&& f) : impl_(std::forward<F>(f)) {}

  Task<AuthToken> operator()(AuthToken auth_token,
                             stdx::stop_token stop_token) const {
    return impl_(std::move(auth_token), std::move(stop_token));
  }

 private:
  stdx::any_invocable<Task<AuthToken>(AuthToken, stdx::stop_token) const> impl_;
};

template <typename Auth>
class AuthorizeRequest {
 public:
  using AuthToken = typename Auth::AuthToken;

  template <typename F>
  explicit AuthorizeRequest(F&& f) : impl_(std::forward<F>(f)) {}

  http::Request<std::string> operator()(http::Request<std::string> request,
                                        AuthToken auth_token) const {
    return impl_(std::move(request), std::move(auth_token));
  }

 private:
  stdx::any_invocable<http::Request<std::string>(http::Request<std::string>,
                                                 AuthToken) const>
      impl_;
};

template <typename Auth>
class AuthManager {
 public:
  using AuthToken = typename Auth::AuthToken;
  using Http = coro::http::Http;

  AuthManager(const coro::http::Http* http, AuthToken auth_token,
              OnAuthTokenUpdated<AuthToken> on_auth_token_updated,
              RefreshToken<Auth> refresh_token,
              AuthorizeRequest<Auth> authorize_request)
      : http_(http),
        auth_token_(std::move(auth_token)),
        on_auth_token_updated_(std::move(on_auth_token_updated)),
        refresh_token_(std::move(refresh_token)),
        authorize_request_(std::move(authorize_request)) {}

  AuthManager(const AuthManager&) = delete;
  AuthManager(AuthManager&&) noexcept = default;
  AuthManager& operator=(const AuthManager&) = delete;
  AuthManager& operator=(AuthManager&&) noexcept = default;

  ~AuthManager() { stop_source_.request_stop(); }

  Task<http::Response<>> Fetch(http::Request<std::string> request,
                               stdx::stop_token stop_token) {
    auto response =
        co_await http_->Fetch(AuthorizeRequest(request), stop_token);
    if (response.status == 401) {
      try {
        co_await RefreshAuthToken(stop_token);
      } catch (const http::HttpException&) {
        throw CloudException(CloudException::Type::kUnauthorized);
      }
      co_return co_await http_->Fetch(AuthorizeRequest(request),
                                      std::move(stop_token));
    } else if (response.status / 100 == 2 || response.status / 100 == 3) {
      co_return response;
    } else {
      auto message = co_await http::GetBody(std::move(response.body));
      throw coro::http::HttpException(response.status, std::move(message));
    }
  }

  Task<nlohmann::json> FetchJson(http::Request<std::string> request,
                                 stdx::stop_token stop_token) {
    if (!http::HasHeader(request.headers, "Accept", "application/json")) {
      request.headers.emplace_back("Accept", "application/json");
    }
    auto response = co_await Fetch(std::move(request), std::move(stop_token));
    std::string body = co_await http::GetBody(std::move(response.body));
    co_return nlohmann::json::parse(std::move(body));
  }

  const AuthToken& GetAuthToken() const { return auth_token_; }

 private:
  void OnAuthTokenUpdated(AuthToken auth_token) {
    auth_token_ = std::move(auth_token);
    on_auth_token_updated_(auth_token_);
  }

  Task<> RefreshAuthToken(stdx::stop_token stop_token) {
    if (!current_auth_refresh_) {
      current_auth_refresh_.emplace(RefreshToken{this});
    }
    co_await current_auth_refresh_->Get(stop_token);
  }

  template <typename Request>
  Request AuthorizeRequest(Request request) {
    return authorize_request_(std::move(request), auth_token_);
  }

  struct RefreshToken {
    Task<AuthToken> operator()() const {
      auto stop_token = d->stop_source_.get_token();
      auto auth_token = co_await d->refresh_token_(d->auth_token_, stop_token);
      if (!stop_token.stop_requested()) {
        d->current_auth_refresh_ = std::nullopt;
        d->auth_token_ = auth_token;
        d->on_auth_token_updated_(d->auth_token_);
      }
      co_return auth_token;
    }
    AuthManager* d;
  };

  const coro::http::Http* http_;
  AuthToken auth_token_;
  std::optional<SharedPromise<RefreshToken>> current_auth_refresh_;
  coro::cloudstorage::util::OnAuthTokenUpdated<AuthToken>
      on_auth_token_updated_;
  coro::cloudstorage::util::RefreshToken<Auth> refresh_token_;
  coro::cloudstorage::util::AuthorizeRequest<Auth> authorize_request_;
  stdx::stop_source stop_source_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_AUTH_MANAGER_H
