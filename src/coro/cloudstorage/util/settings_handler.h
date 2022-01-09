#ifndef CORO_CLOUDSTORAGE_SETTINGS_HANDLER_H
#define CORO_CLOUDSTORAGE_SETTINGS_HANDLER_H

#include <fmt/format.h>

#include "coro/http/http.h"

namespace coro::cloudstorage::util {

struct SettingsHandler {
  using Request = http::Request<>;
  using Response = http::Response<>;

  Task<Response> operator()(Request request, stdx::stop_token) const;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_SETTINGS_HANDLER_H
