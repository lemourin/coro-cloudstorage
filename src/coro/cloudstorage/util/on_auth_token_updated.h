#ifndef CORO_CLOUDSTORAGE_ON_AUTH_TOKEN_UPDATED_H
#define CORO_CLOUDSTORAGE_ON_AUTH_TOKEN_UPDATED_H

#include <utility>

#include "coro/stdx/any_invocable.h"

namespace coro::cloudstorage::util {

template <typename AuthToken>
class OnAuthTokenUpdated {
 public:
  template <typename F>
  explicit OnAuthTokenUpdated(F&& func) : impl_(std::forward<F>(func)) {}

  void operator()(const AuthToken& auth_token) { impl_(auth_token); }

 private:
  stdx::any_invocable<void(const AuthToken&)> impl_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_ON_AUTH_TOKEN_UPDATED_H