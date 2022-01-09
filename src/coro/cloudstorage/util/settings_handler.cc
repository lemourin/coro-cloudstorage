#include "coro/cloudstorage/util/settings_handler.h"

#include <fmt/format.h>

#include <span>

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/net_utils.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"

namespace coro::cloudstorage::util {

namespace {

std::string GetHostSelector(
    std::span<const std::pair<std::string, std::string>> headers) {
  auto host_addresses = GetHostAddresses();
  std::stringstream stream;
  stream << "<select name='host' id='host'>";
  stream << "<option value=''></option>";
  auto host = http::GetCookie(headers, "host");
  for (std::string_view address : GetHostAddresses()) {
    stream << "<option";
    if (host && *host == address) {
      stream << " selected=true";
    }
    stream << " value='" << address << "'>" << address << "</option>";
  }
  stream << "</select>";
  return std::move(stream).str();
}

}  // namespace

auto SettingsHandler::operator()(Request request, stdx::stop_token) const
    -> Task<Response> {
  auto uri = http::ParseUri(request.url);
  if (!uri.path) {
    co_return Response{.status = 400};
  }
  if (uri.path == "/settings/host-set") {
    if (!request.body) {
      co_return Response{.status = 400};
    }
    auto body = co_await http::GetBody(std::move(*request.body));
    auto query = http::ParseQuery(body);
    std::string cookie = [&] {
      if (auto it = query.find("host");
          it != query.end() && !it->second.empty()) {
        return StrCat("host=", http::EncodeUri(it->second),
                      ";path=/;Expires=Mon, 01 Jan 9999 00:00:00 GMT");
      } else {
        return StrCat("host=;path=/;Expires=Mon, 01 Jan 1970 00:00:00 GMT");
      }
    }();
    co_return Response{.status = 302,
                       .headers = {{"Location", "/settings"},
                                   {"Set-Cookie", std::move(cookie)}}};
  }
  co_return Response{
      .status = 200,
      .body = http::CreateBody(fmt::format(
          fmt::runtime(util::kAssetsHtmlSettingsPageHtml),
          fmt::arg("host_selector", GetHostSelector(request.headers))))};
}

}  // namespace coro::cloudstorage::util