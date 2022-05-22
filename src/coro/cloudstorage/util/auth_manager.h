#ifndef CORO_CLOUDSTORAGE_AUTH_MANAGER_H
#define CORO_CLOUDSTORAGE_AUTH_MANAGER_H

#include <nlohmann/json.hpp>

#include "coro/cloudstorage/cloud_exception.h"
#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http.h"
#include "coro/shared_promise.h"
#include "coro/stdx/stop_source.h"

namespace coro::cloudstorage::util {

template <typename Auth>
class RefreshToken {
 public:
  using AuthToken = typename Auth::AuthToken;
  using AuthData = typename Auth::AuthData;

  RefreshToken(const coro::http::Http* http, AuthData auth_data)
      : http_(http), auth_data_(std::move(auth_data)) {}

  Task<AuthToken> operator()(AuthToken auth_token,
                             stdx::stop_token stop_token) const {
    return Auth::RefreshAccessToken(*http_, auth_data_, auth_token, stop_token);
  }

 private:
  const coro::http::Http* http_;
  AuthData auth_data_;
};

struct AuthorizeRequest {
  template <typename Request, typename AuthToken>
  Request operator()(Request request, const AuthToken& auth_token) const {
    request.headers.emplace_back("Authorization",
                                 StrCat("Bearer ", auth_token.access_token));
    return request;
  }
};

template <typename Auth, typename OnAuthTokenUpdatedT,
          typename RefreshTokenT = RefreshToken<Auth>,
          typename AuthorizeRequestT = AuthorizeRequest>
class AuthManager {
 public:
  using AuthToken = typename Auth::AuthToken;
  using Http = coro::http::Http;

  AuthManager(const coro::http::Http* http, AuthToken auth_token,
              OnAuthTokenUpdatedT on_auth_token_updated,
              RefreshTokenT refresh_token, AuthorizeRequestT authorize_request)
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

  template <typename Request>
  Task<http::Response<>> Fetch(Request request, stdx::stop_token stop_token) {
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

  template <typename Request>
  Task<nlohmann::json> FetchJson(Request request, stdx::stop_token stop_token) {
    if (!http::HasHeader(request.headers, "Accept", "application/json")) {
      request.headers.emplace_back("Accept", "application/json");
    }
    http::ResponseLike auto response =
        co_await Fetch(std::move(request), std::move(stop_token));
    std::string body = co_await http::GetBody(std::move(response.body));
    co_return nlohmann::json::parse(std::move(body));
  }

  const AuthToken& GetAuthToken() const { return auth_token_; }
  const coro::http::Http& GetHttp() const { return *http_; }

  void OnAuthTokenUpdated(AuthToken auth_token) {
    auth_token_ = std::move(auth_token);
    on_auth_token_updated_(auth_token_);
  }

  RefreshTokenT& refresh_token() { return refresh_token_; }
  const RefreshTokenT& refresh_token() const { return refresh_token_; }

 private:
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
  OnAuthTokenUpdatedT on_auth_token_updated_;
  RefreshTokenT refresh_token_;
  AuthorizeRequestT authorize_request_;
  stdx::stop_source stop_source_;
};

template <typename Auth>
class AuthManager2 {
 public:
  virtual ~AuthManager2() = default;

  using AuthToken = typename Auth::AuthToken;

  virtual const AuthToken& GetAuthToken() const = 0;

  virtual Task<http::Response<>> Fetch(http::Request<std::string> request,
                                       stdx::stop_token stop_token) const = 0;

  virtual Task<nlohmann::json> FetchJson(http::Request<std::string> request,
                                         stdx::stop_token stop_token) const = 0;

  virtual void OnAuthTokenUpdated(typename Auth::AuthToken auth_token) = 0;
};

template <typename Auth>
class AuthManager3 : public AuthManager2<Auth> {
 public:
  template <typename Impl>
  explicit AuthManager3(Impl impl)
      : d_(std::make_unique<AuthManager2Impl<Impl>>(std::move(impl))) {}

  const typename Auth::AuthToken& GetAuthToken() const override {
    return d_->GetAuthToken();
  }

  Task<http::Response<>> Fetch(http::Request<std::string> request,
                               stdx::stop_token stop_token) const override {
    return d_->Fetch(std::move(request), std::move(stop_token));
  }

  Task<nlohmann::json> FetchJson(http::Request<std::string> request,
                                 stdx::stop_token stop_token) const override {
    return d_->FetchJson(std::move(request), std::move(stop_token));
  }

  void OnAuthTokenUpdated(typename Auth::AuthToken auth_token) override {
    d_->OnAuthTokenUpdated(std::move(auth_token));
  }

 private:
  template <typename Impl>
  class AuthManager2Impl : public AuthManager2<Auth> {
   public:
    explicit AuthManager2Impl(Impl impl) : impl_(std::move(impl)) {}

    const typename Auth::AuthToken& GetAuthToken() const override {
      return impl_.GetAuthToken();
    }

    Task<http::Response<>> Fetch(http::Request<std::string> request,
                                 stdx::stop_token stop_token) const override {
      return impl_.Fetch(std::move(request), std::move(stop_token));
    }

    Task<nlohmann::json> FetchJson(http::Request<std::string> request,
                                   stdx::stop_token stop_token) const override {
      return impl_.FetchJson(std::move(request), std::move(stop_token));
    }

    void OnAuthTokenUpdated(typename Auth::AuthToken auth_token) override {
      impl_.OnAuthTokenUpdated(std::move(auth_token));
    }

   private:
    mutable Impl impl_;
  };

  std::unique_ptr<AuthManager2<Auth>> d_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_AUTH_MANAGER_H
