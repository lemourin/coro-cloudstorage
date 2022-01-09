#include "coro/cloudstorage/util/settings_handler.h"

#include <fmt/format.h>

#include <span>

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/net_utils.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"

namespace coro::cloudstorage::util::internal {

namespace {

using Request = http::Request<>;
using Response = http::Response<>;

std::string GetHostSelector(
    std::span<const std::pair<std::string, std::string>> headers) {
  auto host_addresses = GetHostAddresses();
  std::stringstream stream;
  auto host = http::GetCookie(headers, "host");
  for (std::string_view address : GetHostAddresses()) {
    stream << "<option";
    if (host && *host == address) {
      stream << " selected=true";
    }
    stream << " value='" << address << "'>" << address << "</option>";
  }
  return std::move(stream).str();
}

}  // namespace

Task<Response> GetSettingsHandlerResponse(SettingsHandlerData d) {
  if (d.path == "/settings/host-set") {
    if (!d.request_body) {
      co_return Response{.status = 400};
    }
    auto body = co_await http::GetBody(std::move(*d.request_body));
    auto query = http::ParseQuery(body);
    std::string cookie = [&] {
      if (auto it = query.find("value");
          it != query.end() && !it->second.empty()) {
        return StrCat("host=", http::EncodeUri(it->second),
                      ";path=/;Expires=Mon, 01 Jan 9999 00:00:00 GMT");
      } else {
        return StrCat("host=;path=/;Expires=Mon, 01 Jan 1970 00:00:00 GMT");
      }
    }();
    co_return Response{.status = 200,
                       .headers = {{"Set-Cookie", std::move(cookie)}}};
  }
  co_return Response{
      .status = 200,
      .body = http::CreateBody(fmt::format(
          fmt::runtime(util::kAssetsHtmlSettingsPageHtml),
          fmt::arg("host_class", d.effective_public_network ? "" : "hidden"),
          fmt::arg("host_selector", d.effective_public_network
                                        ? GetHostSelector(d.headers)
                                        : ""),
          fmt::arg("public_network_checked", d.public_network ? "checked" : ""),
          fmt::arg("public_network_requires_restart_class",
                   d.effective_public_network == d.public_network ? "hidden"
                                                                  : "")))};
}

}  // namespace coro::cloudstorage::util::internal