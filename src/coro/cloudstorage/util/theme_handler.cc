#include "coro/cloudstorage/util/theme_handler.h"

namespace coro::cloudstorage::util {

std::string_view ToString(Theme theme) {
  switch (theme) {
    case Theme::kDark:
      return "dark";
    case Theme::kLight:
      return "light";
  }
  throw RuntimeError("invalid theme");
}

Theme GetTheme(std::span<const std::pair<std::string, std::string>> headers) {
  if (auto cookie = http::GetHeader(headers, "Cookie")) {
    if (cookie->find("theme=dark") != std::string::npos) {
      return Theme::kDark;
    } else if (cookie->find("theme=light") != std::string::npos) {
      return Theme::kLight;
    }
  }
  if (auto hint = http::GetHeader(headers, "Sec-CH-Prefers-Color-Scheme")) {
    if (*hint == "dark") {
      return Theme::kDark;
    } else if (*hint == "light") {
      return Theme::kLight;
    }
  }
  return Theme::kLight;
}

auto ThemeHandler::operator()(Request request, stdx::stop_token) const
    -> Task<Response> {
  Theme current_theme = GetTheme(request.headers);
  std::string cookie = StrCat(
      "theme=",
      ToString(current_theme == Theme::kLight ? Theme::kDark : Theme::kLight),
      ";path=/;Expires=Mon, 01 Jan 9999 00:00:00 GMT");
  if (std::optional<std::string> host =
          http::GetHeader(request.headers, "Host")) {
    auto uri = http::ParseUri(StrCat("//", *host));
    if (uri.host.value().ends_with(".localhost")) {
      cookie += StrCat(";domain=.", *uri.host);
    }
  }
  co_return Response{
      .status = 302,
      .headers = {{"Location", "/settings"}, {"Set-Cookie", std::move(cookie)}},
  };
}

}  // namespace coro::cloudstorage::util