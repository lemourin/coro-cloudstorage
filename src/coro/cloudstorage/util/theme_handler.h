#ifndef CORO_CLOUDSTORAGE_THEME_HANDLER_H
#define CORO_CLOUDSTORAGE_THEME_HANDLER_H

#include <coro/cloudstorage/util/string_utils.h>
#include <coro/http/http_parse.h>

#include <algorithm>
#include <span>

namespace coro::cloudstorage::util {

enum class Theme { kDark, kLight };

inline std::string_view ToString(Theme theme) {
  switch (theme) {
    case Theme::kDark:
      return "dark";
    case Theme::kLight:
      return "light";
  }
  throw std::runtime_error("invalid theme");
}

inline Theme GetTheme(
    std::span<const std::pair<std::string, std::string>> headers) {
  if (auto cookie = http::GetHeader(headers, "Cookie");
      cookie && cookie->find("theme=dark") != std::string::npos) {
    return Theme::kDark;
  } else {
    return Theme::kLight;
  }
}

struct ThemeHandler {
  using Request = http::Request<>;
  using Response = http::Response<>;

  Task<Response> operator()(Request request, stdx::stop_token) const {
    Theme current_theme = GetTheme(request.headers);
    co_return Response{
        .status = 302,
        .headers = {{"Location", "/"},
                    {"Set-Cookie",
                     util::StrCat("theme=",
                                  ToString(current_theme == Theme::kLight
                                               ? Theme::kDark
                                               : Theme::kLight),
                                  "; Expires=2038-01-19 04:14:07")}},
    };
  }
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_THEME_HANDLER_H
