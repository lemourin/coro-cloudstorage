#ifndef CORO_CLOUDSTORAGE_AUTH_MANAGER_H
#define CORO_CLOUDSTORAGE_AUTH_MANAGER_H

#include <coro/cloudstorage/cloud_exception.h>
#include <coro/cloudstorage/util/string_utils.h>
#include <coro/http/http.h>
#include <coro/shared_promise.h>
#include <coro/stdx/stop_source.h>

#include <nlohmann/json.hpp>

namespace coro::cloudstorage::util {

template <http::HttpClient HttpT, typename Auth>
struct RefreshToken {
  using AuthToken = typename Auth::AuthToken;
  using AuthData = typename Auth::AuthData;

  Task<AuthToken> operator()(AuthToken auth_token,
                             stdx::stop_token stop_token) const {
    return Auth::RefreshAccessToken(*http, auth_data, auth_token, stop_token);
  }

  const HttpT* http;
  AuthData auth_data;
};

struct AuthorizeRequest {
  template <typename Request, typename AuthToken>
  Request operator()(Request request, const AuthToken& auth_token) const {
    request.headers.emplace_back("Authorization",
                                 StrCat("Bearer ", auth_token.access_token));
    return request;
  }
};

template <http::HttpClient HttpT, typename Auth, typename OnAuthTokenUpdated,
          typename RefreshTokenT = RefreshToken<HttpT, Auth>,
          typename AuthorizeRequestT = AuthorizeRequest>
class AuthManager {
 public:
  using AuthToken = typename Auth::AuthToken;
  using Http = HttpT;

  AuthManager(const Http& http, AuthToken auth_token,
              OnAuthTokenUpdated on_auth_token_updated,
              RefreshTokenT refresh_token, AuthorizeRequestT authorize_request)
      : http_(&http),
        auth_token_(std::move(auth_token)),
        on_auth_token_updated_(std::move(on_auth_token_updated)),
        refresh_token_(std::move(refresh_token)),
        authorize_request_(std::move(authorize_request)) {}

  ~AuthManager() { stop_source_.request_stop(); }

  AuthManager(AuthManager&&) noexcept = default;
  AuthManager& operator=(AuthManager&&) noexcept = default;

  template <typename Request>
  Task<typename Http::ResponseType> Fetch(Request request,
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
      throw coro::http::HttpException(
          response.status, co_await http::GetBody(std::move(response.body)));
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
  const Http& GetHttp() const { return *http_; }

 private:
  Task<> RefreshAuthToken(stdx::stop_token stop_token) {
    if (!current_auth_refresh_) {
      current_auth_refresh_ = SharedPromise(RefreshToken{this});
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

  const Http* http_;
  AuthToken auth_token_;
  std::optional<SharedPromise<RefreshToken>> current_auth_refresh_;
  OnAuthTokenUpdated on_auth_token_updated_;
  RefreshTokenT refresh_token_;
  AuthorizeRequestT authorize_request_;
  stdx::stop_source stop_source_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_AUTH_MANAGER_H
