#ifndef CORO_CLOUDSTORAGE_ON_AUTH_TOKEN_UPDATED_H
#define CORO_CLOUDSTORAGE_ON_AUTH_TOKEN_UPDATED_H

#include <functional>
#include <utility>

namespace coro::cloudstorage::util {

template <typename AuthToken>
class OnAuthTokenUpdated {
 public:
  template <typename F>
  explicit OnAuthTokenUpdated(F func) : impl_(std::move(func)) {}

  void operator()(const AuthToken& auth_token) { impl_(auth_token); }

 private:
  std::function<void(const AuthToken&)> impl_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_ON_AUTH_TOKEN_UPDATED_H