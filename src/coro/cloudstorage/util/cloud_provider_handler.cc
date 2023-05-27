#include "coro/cloudstorage/util/cloud_provider_handler.h"

#include <fmt/format.h>

#include <iomanip>
#include <iostream>
#include <sstream>

#include "coro/cloudstorage/util/cloud_provider_utils.h"
#include "coro/cloudstorage/util/net_utils.h"
#include "coro/cloudstorage/util/serialize_utils.h"
#include "coro/cloudstorage/util/thumbnail_quality.h"
#include "coro/util/regex.h"

namespace coro::cloudstorage::util {

namespace {

std::string GetItemPathPrefix(
    std::span<const std::pair<std::string, std::string>> headers) {
  namespace re = coro::util::re;
  std::optional<std::vector<std::string>> host_addresses;
  auto host = [&]() -> std::string {
    auto host = http::GetCookie(headers, "host");
    if (!host || host->empty()) {
      return "";
    }
    host_addresses = GetHostAddresses();
    if (std::find(host_addresses->begin(), host_addresses->end(), *host) ==
        host_addresses->end()) {
      return "";
    }
    return *host;
  }();
  if (host.empty()) {
    if (!host_addresses) {
      host_addresses = GetHostAddresses();
    }
    std::optional<size_t> idx;
    bool ambiguous = false;
    for (size_t i = 0; i < host_addresses->size(); i++) {
      if ((*host_addresses)[i] != "127.0.0.1") {
        if (idx) {
          ambiguous = true;
          break;
        }
        idx = i;
      }
    }
    if (!ambiguous && idx) {
      host = std::move((*host_addresses)[*idx]);
    }
  }
  if (host.empty()) {
    return "";
  }
  std::string host_header = http::GetHeader(headers, "Host").value();
  auto port = [&]() -> std::string_view {
    re::regex regex(R"((\:\d{1,5})$)");
    re::match_results<std::string::const_iterator> match;
    if (re::regex_search(host_header, match, regex)) {
      return std::string_view(&*match[1].begin(), match[1].length());
    } else {
      return "";
    }
  }();
  return StrCat("http://", host, port);
}

bool IsRoot(std::string_view path) { return GetEffectivePath(path).empty(); }

std::string RewriteThumbnailUrl(std::string_view host, std::string url) {
  auto host_uri = http::ParseUri(StrCat("//", host));
  if (!host_uri.host->ends_with(".localhost")) {
    return url;
  }
  auto uri = http::ParseUri(url);
  uri.host = StrCat("img.", *host_uri.host);
  uri.port = host_uri.port;
  std::string rewritten = http::ToString(uri);
  return rewritten;
}

Generator<std::string> GetDashPlayer(std::string host, std::string path,
                                     std::string poster_url) {
  std::stringstream stream;
  stream << "<source src='" << path << "'>";
  std::string content =
      fmt::format(fmt::runtime(kDashPlayerHtml), fmt::arg("poster", poster_url),
                  fmt::arg("source", std::move(stream).str()));
  co_yield std::move(content);
}

template <typename Item>
std::string GetItemEntry(
    std::string_view host, const Item& item, std::string_view path,
    bool use_dash_player,
    const stdx::any_invocable<std::string(std::string_view item_id) const>&
        thumbnail_url_generator,
    const stdx::any_invocable<std::string(std::string_view item_id) const>&
        content_url_generator) {
  std::string file_link = StrCat(path, http::EncodeUri(item.name));
  return fmt::format(
      fmt::runtime(kItemEntryHtml), fmt::arg("name", item.name),
      fmt::arg("size", SizeToString(item.size)),
      fmt::arg("timestamp", TimeStampToString(item.timestamp)),
      fmt::arg("url", StrCat(content_url_generator(item.id),
                             use_dash_player ? "?dash_player=true" : "")),
      fmt::arg("thumbnail_url",
               RewriteThumbnailUrl(host, thumbnail_url_generator(item.id))));
}

std::string GetItemEntry(
    std::string_view host, const AbstractCloudProvider::Item& item,
    std::string_view path, bool use_dash_player,
    const stdx::any_invocable<std::string(std::string_view item_id) const>&
        thumbnail_url_generator,
    const stdx::any_invocable<std::string(std::string_view item_id) const>&
        content_url_generator) {
  return std::visit(
      [&]<typename Item>(const Item& item) {
        bool effective_use_dash_player = [&] {
          if constexpr (std::is_same_v<Item, AbstractCloudProvider::File>) {
            return item.name.ends_with(".mpd") ||
                   (use_dash_player && item.mime_type.starts_with("video"));
          } else {
            return false;
          }
        }();
        return GetItemEntry(host, item, path, effective_use_dash_player,
                            thumbnail_url_generator, content_url_generator);
      },
      item);
}

}  // namespace

auto CloudProviderHandler::operator()(Request request,
                                      stdx::stop_token stop_token)
    -> Task<Response> {
  try {
    auto uri = http::ParseUri(request.url);
    auto path = GetEffectivePath(uri.path.value());
    auto item = co_await GetItemByPathComponents(cache_manager_, provider_,
                                                 path, stop_token);
    if (request.method == http::Method::kGet && uri.query) {
      auto query = http::ParseQuery(*uri.query);
      if (auto it = query.find("dash_player");
          it != query.end() && it->second == "true") {
        co_return Response{
            .status = 200,
            .headers = {{"Content-Type", "text/html; charset=UTF-8"}},
            .body = GetDashPlayer(
                http::GetHeader(request.headers, "Host").value(),
                StrCat(GetItemPathPrefix(request.headers), uri.path.value()),
                RewriteThumbnailUrl(
                    http::GetHeader(request.headers, "Host").value(),
                    StrCat(thumbnail_url_generator_(
                               std::get<AbstractCloudProvider::File>(item).id),
                           "?quality=high")))};
      }
    }
    co_return co_await std::visit(
        [&]<typename T>(T&& d) {
          return HandleExistingItem(std::move(request), std::forward<T>(d),
                                    stop_token);
        },
        std::move(item));
  } catch (const CloudException& e) {
    switch (e.type()) {
      case CloudException::Type::kNotFound:
        co_return Response{.status = 404};
      case CloudException::Type::kUnauthorized:
        co_return Response{.status = 401};
      default:
        throw;
    }
  }
}

std::string CloudProviderHandler::GetItemPathPrefix(
    std::span<const std::pair<std::string, std::string>> headers) const {
  if (!settings_manager_->EffectiveIsPublicNetworkEnabled()) {
    return "";
  }
  return ::coro::cloudstorage::util::GetItemPathPrefix(headers);
}

auto CloudProviderHandler::HandleExistingItem(Request request,
                                              AbstractCloudProvider::File d,
                                              stdx::stop_token stop_token)
    -> Task<Response> {
  return GetFileContentResponse(
      provider_, std::move(d),
      [&]() -> std::optional<http::Range> {
        if (auto header = http::GetHeader(request.headers, "Range")) {
          return http::ParseRange(std::move(*header));
        } else {
          return std::nullopt;
        }
      }(),
      std::move(stop_token));
}

auto CloudProviderHandler::HandleExistingItem(
    Request request, AbstractCloudProvider::Directory d,
    stdx::stop_token stop_token) -> Task<Response> {
  std::string directory_path = GetPath(request);
  if (directory_path.empty() || directory_path.back() != '/') {
    directory_path += '/';
  }
  co_return Response{
      .status = 200,
      .headers = {{"Content-Type", "text/html"}},
      .body = GetDirectoryContent(
          http::GetHeader(request.headers, "Host").value(), directory_path, d,
          ListDirectory(cache_manager_, provider_, d, stop_token),
          /*use_dash_player=*/!GetItemPathPrefix(request.headers).empty(),
          stop_token)};
}

Generator<std::string> CloudProviderHandler::GetDirectoryContent(
    std::string host, std::string path, AbstractCloudProvider::Directory parent,
    Generator<AbstractCloudProvider::PageData> page_data, bool use_dash_player,
    stdx::stop_token stop_token) const {
  co_yield "<!DOCTYPE html>"
      "<html lang='en-us'>"
      "<head>"
      "  <title>coro-cloudstorage</title>"
      "  <meta charset='UTF-8'>"
      "  <meta name='viewport' "
      "        content='width=device-width, initial-scale=1'>"
      "  <link rel=stylesheet href='/static/layout.css'>"
      "  <link rel=stylesheet href='/static/colors.css'>"
      "  <link rel='icon' type='image/x-icon' href='/static/favicon.ico'>"
      "</head>"
      "<body class='root-container'>"
      "<table class='content-table'>";
  std::string parent_entry =
      fmt::format(fmt::runtime(kItemEntryHtml), fmt::arg("name", ".."),
                  fmt::arg("size", ""), fmt::arg("timestamp", ""),
                  fmt::arg("url", GetDirectoryPath(path)),
                  fmt::arg("thumbnail_url",
                           RewriteThumbnailUrl(host, "/static/folder.svg")));
  co_yield std::move(parent_entry);
  FOR_CO_AWAIT(const auto& page, page_data) {
    for (const auto& item : page.items) {
      co_yield GetItemEntry(host, item, path, use_dash_player,
                            thumbnail_url_generator_, content_url_generator_);
    }
  }
  co_yield "</table>"
      "</body>"
      "</html>";
}

}  // namespace coro::cloudstorage::util
