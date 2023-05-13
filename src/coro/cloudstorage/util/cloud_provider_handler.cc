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

class ThumbnailGeneratorException : public std::exception {};

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

Generator<std::string> GetDashPlayer(std::string host, std::string path) {
  std::stringstream stream;
  stream << "<source src='" << path << "'>";
  std::string content = fmt::format(
      fmt::runtime(kDashPlayerHtml),
      fmt::arg("poster",
               RewriteThumbnailUrl(host, StrCat(path, "?hq_thumbnail=true"))),
      fmt::arg("source", std::move(stream).str()));
  co_yield std::move(content);
}

std::string_view GetIconName(AbstractCloudProvider::Directory) {
  return "folder";
}

std::string_view GetIconName(AbstractCloudProvider::File file) {
  switch (GetFileType(file.mime_type)) {
    case FileType::kUnknown:
      return "unknown";
    case FileType::kImage:
      return "image-x-generic";
    case FileType::kAudio:
      return "audio-x-generic";
    case FileType::kVideo:
      return "video-x-generic";
  }
  throw RuntimeError("invalid file type");
}

template <typename Item>
std::string GetItemEntry(std::string_view host, const Item& item,
                         std::string_view path, bool use_dash_player) {
  std::string file_link = StrCat(path, http::EncodeUri(item.name));
  return fmt::format(
      fmt::runtime(kItemEntryHtml), fmt::arg("name", item.name),
      fmt::arg("size", SizeToString(item.size)),
      fmt::arg("timestamp", TimeStampToString(item.timestamp)),
      fmt::arg("url",
               StrCat(file_link, use_dash_player ? "?dash_player=true" : "")),
      fmt::arg(
          "thumbnail_url",
          RewriteThumbnailUrl(host, StrCat(file_link, "?thumbnail=true"))));
}

std::string GetItemEntry(std::string_view host,
                         const AbstractCloudProvider::Item& item,
                         std::string_view path, bool use_dash_player) {
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
        return GetItemEntry(host, item, path, effective_use_dash_player);
      },
      item);
}

}  // namespace

auto CloudProviderHandler::operator()(Request request,
                                      stdx::stop_token stop_token)
    -> Task<Response> {
  try {
    if (request.method == http::Method::kPropfind ||
        request.method == http::Method::kMove ||
        request.method == http::Method::kProppatch ||
        request.method == http::Method::kMkcol ||
        request.method == http::Method::kDelete ||
        request.method == http::Method::kPut) {
      co_return co_await WebDAVHandler(provider_)(std::move(request),
                                                  std::move(stop_token));
    }
    auto uri = http::ParseUri(request.url);
    auto path = GetEffectivePath(uri.path.value());
    if (request.method == http::Method::kGet && uri.query) {
      auto query = http::ParseQuery(*uri.query);
      if (auto it = query.find("thumbnail");
          it != query.end() && it->second == "true") {
        co_return co_await std::visit(
            [&]<typename T>(T&& d) {
              return GetItemThumbnail(std::forward<T>(d),
                                      ThumbnailQuality::kLow, stop_token);
            },
            co_await GetItemByPathComponents(provider_, path, stop_token));
      }
      if (auto it = query.find("hq_thumbnail");
          it != query.end() && it->second == "true") {
        co_return co_await std::visit(
            [&]<typename T>(T&& d) {
              return GetItemThumbnail(std::forward<T>(d),
                                      ThumbnailQuality::kHigh, stop_token);
            },
            co_await GetItemByPathComponents(provider_, path, stop_token));
      }
      if (auto it = query.find("dash_player");
          it != query.end() && it->second == "true") {
        co_return Response{
            .status = 200,
            .headers = {{"Content-Type", "text/html; charset=UTF-8"}},
            .body = GetDashPlayer(
                http::GetHeader(request.headers, "Host").value(),
                StrCat(GetItemPathPrefix(request.headers), uri.path.value()))};
      }
    }
    co_return co_await std::visit(
        [&]<typename T>(T&& d) {
          return HandleExistingItem(std::move(request), std::forward<T>(d),
                                    stop_token);
        },
        co_await GetItemByPathComponents(provider_, path, stop_token));
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

Task<std::string> CloudProviderHandler::GenerateThumbnail(
    const AbstractCloudProvider::File& item,
    stdx::stop_token stop_token) const {
  switch (GetFileType(item.mime_type)) {
    case FileType::kImage:
    case FileType::kVideo:
      try {
        co_return co_await (*thumbnail_generator_)(
            provider_, item,
            ThumbnailOptions{.codec = ThumbnailOptions::Codec::PNG},
            std::move(stop_token));
      } catch (const std::exception& e) {
        std::cerr << "FAILED TO GENERATE THUMBNAIL: " << e.what() << '\n';
        throw ThumbnailGeneratorException();
      }
    default:
      throw CloudException(CloudException::Type::kNotFound);
  }
}

template <typename Item>
auto CloudProviderHandler::GetStaticIcon(const Item& item, int http_code) const
    -> Response {
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Location", StrCat("/static/", GetIconName(item), ".svg")}};
  if (http_code == 301) {
    headers.push_back({"Cache-Control", "private"});
    headers.push_back({"Cache-Control", "max-age=604800"});
  }
  return Response{.status = http_code, .headers = std::move(headers)};
}

template <typename Item>
auto CloudProviderHandler::GetIcon(const Item& item,
                                   stdx::stop_token stop_token) const
    -> Task<Response> {
  if constexpr (std::is_same_v<Item, AbstractCloudProvider::File>) {
    try {
      std::string content =
          co_await GenerateThumbnail(item, std::move(stop_token));
      co_return Response{
          .status = 200,
          .headers = {{"Cache-Control", "private"},
                      {"Cache-Control", "max-age=604800"},
                      {"Content-Type", "image/png"},
                      {"Content-Length", std::to_string(content.size())}},
          .body = http::CreateBody(std::move(content))};
    } catch (const ThumbnailGeneratorException&) {
      co_return GetStaticIcon(item, 302);
    } catch (const CloudException& e) {
      co_return GetStaticIcon(
          item, /*http_code=*/e.type() == CloudException::Type::kNotFound
                    ? 301
                    : 302);
    } catch (...) {
      co_return GetStaticIcon(item, /*http_code=*/302);
    }
  } else {
    co_return GetStaticIcon(item, /*http_code=*/301);
  }
}

template <typename Item>
auto CloudProviderHandler::GetItemThumbnail(Item d, ThumbnailQuality quality,
                                            stdx::stop_token stop_token) const
    -> Task<Response> {
  try {
    auto thumbnail = co_await provider_->GetItemThumbnail(
        d, quality, http::Range{}, stop_token);
    co_return Response{
        .status = 200,
        .headers = {{"Cache-Control", "private"},
                    {"Cache-Control", "max-age=604800"},
                    {"Content-Type", std::string(thumbnail.mime_type)},
                    {"Content-Length", std::to_string(thumbnail.size)}},
        .body = std::move(thumbnail.data)};
  } catch (...) {
  }
  co_return co_await GetIcon(d, std::move(stop_token));
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
          ListDirectory(provider_, d, stop_token),
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
  std::string parent_entry = fmt::format(
      fmt::runtime(kItemEntryHtml), fmt::arg("name", ".."),
      fmt::arg("size", ""), fmt::arg("timestamp", ""),
      fmt::arg("url", GetDirectoryPath(path)),
      fmt::arg("thumbnail_url",
               RewriteThumbnailUrl(
                   host, StrCat(IsRoot(path) ? path : GetDirectoryPath(path),
                                "?thumbnail=true"))));
  co_yield std::move(parent_entry);
  FOR_CO_AWAIT(const auto& page, page_data) {
    for (const auto& item : page.items) {
      co_yield GetItemEntry(host, item, path, use_dash_player);
    }
  }
  co_yield "</table>"
      "</body>"
      "</html>";
}

}  // namespace coro::cloudstorage::util
