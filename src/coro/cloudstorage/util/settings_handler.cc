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

using Request = http::Request<>;
using Response = http::Response<>;

struct SettingsHandlerData {
  std::string_view path;
  std::span<const std::pair<std::string, std::string>> headers;
  std::optional<Generator<std::string>> request_body;
  bool public_network;
  bool effective_public_network;
  stdx::stop_token stop_token;
};

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

}  // namespace

auto SettingsHandler::operator()(Request request,
                                 stdx::stop_token stop_token) const
    -> Task<Response> {
  auto uri = http::ParseUri(request.url);
  if (!uri.path) {
    co_return Response{.status = 400};
  }
  if (request.method == http::Method::kPost) {
    if (*uri.path == "/settings/public-network") {
      auto body = co_await http::GetBody(std::move(request.body).value());
      auto query = http::ParseQuery(body);
      if (auto it = query.find("value"); it != query.end()) {
        settings_manager_->SetEnablePublicNetwork(it->second == "true");
        co_return Response{.status = 200};
      } else {
        co_return Response{.status = 400};
      }
    }
  }
  co_return co_await GetSettingsHandlerResponse(SettingsHandlerData{
      .path = *uri.path,
      .headers = request.headers,
      .request_body = std::move(request.body),
      .public_network = settings_manager_->IsPublicNetworkEnabled(),
      .effective_public_network =
          settings_manager_->EffectiveIsPublicNetworkEnabled(),
      .stop_token = std::move(stop_token)});
}

}  // namespace coro::cloudstorage::util