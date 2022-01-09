#ifndef CORO_CLOUDSTORAGE_THEME_HANDLER_H
#define CORO_CLOUDSTORAGE_THEME_HANDLER_H

#include <algorithm>
#include <span>

#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"

namespace coro::cloudstorage::util {

enum class Theme { kDark, kLight };

std::string_view ToString(Theme theme);
Theme GetTheme(std::span<const std::pair<std::string, std::string>> headers);

struct ThemeHandler {
  using Request = http::Request<>;
  using Response = http::Response<>;

  Task<Response> operator()(Request request, stdx::stop_token) const;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_THEME_HANDLER_H
