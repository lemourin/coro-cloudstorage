#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_HANDLER_H
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_HANDLER_H

#include <fmt/format.h>

#include <iomanip>
#include <sstream>

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/handler_utils.h"
#include "coro/cloudstorage/util/net_utils.h"
#include "coro/cloudstorage/util/thumbnail_options.h"
#include "coro/cloudstorage/util/webdav_handler.h"
#include "coro/http/http_parse.h"
#include "coro/util/lru_cache.h"
#include "coro/util/regex.h"

namespace coro::cloudstorage::util {

template <typename CloudProvider, typename ThumbnailGenerator>
class CloudProviderHandler {
 public:
  using Request = http::Request<>;
  using Response = http::Response<>;

  CloudProviderHandler(const ThumbnailGenerator* thumbnail_generator,
                       CloudProvider* provider)
      : thumbnail_generator_(thumbnail_generator), provider_(provider) {}

  Task<Response> operator()(Request request, stdx::stop_token stop_token) {
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
                return GetItemThumbnail(std::forward<T>(d), stop_token);
              },
              co_await provider_->GetItemByPathComponents(path, stop_token));
        }
        if (auto it = query.find("dash_player");
            it != query.end() && it->second == "true") {
          co_return Response{
              .status = 200,
              .headers = {{"Content-Type", "text/html; charset=UTF-8"}},
              .body = GetDashPlayer(
                  http::GetHeader(request.headers, "Host").value(),
                  uri.path.value())};
        }
      }
      co_return co_await std::visit(
          [&]<typename T>(T&& d) {
            return HandleExistingItem(std::move(request), std::forward<T>(d),
                                      stop_token);
          },
          co_await provider_->GetItemByPathComponents(path, stop_token));
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

 private:
  static Generator<std::string> GetDashPlayer(std::string host,
                                              std::string path) {
    namespace re = coro::util::re;
    re::regex regex(R"((\:\d{1,5})$)");
    re::match_results<std::string::const_iterator> match;
    std::string_view port = "";
    if (re::regex_search(host, match, regex)) {
      port = std::string_view(match[1].begin(), match[1].end());
    }
    std::stringstream stream;
    stream << "<source src='" << path << "'>";
    for (std::string address : GetHostAddresses()) {
      stream << "<source src='"
             << "http://" << address << port << path << "'>";
    }
    co_yield fmt::format(fmt::runtime(kAssetsHtmlDashPlayerHtml),
                         fmt::arg("source", std::move(stream).str()));
  }

  static bool IsRoot(std::string_view path) {
    return GetEffectivePath(path).empty();
  }

  template <typename Item>
  Task<std::string> GenerateThumbnail(const Item& item,
                                      stdx::stop_token stop_token) const {
    switch (CloudProvider::GetFileType(item)) {
      case FileType::kImage:
      case FileType::kVideo:
        co_return co_await (*thumbnail_generator_)(
            provider_, item,
            ThumbnailOptions{.codec = ThumbnailOptions::Codec::PNG},
            std::move(stop_token));
      default:
        throw CloudException(CloudException::Type::kNotFound);
    }
  }

  template <typename Item>
  Response GetStaticIcon(const Item& item) const {
    std::string_view icon_name = [&] {
      if constexpr (IsFile<Item, CloudProvider>) {
        switch (CloudProvider::GetFileType(item)) {
          case FileType::kUnknown:
            return "unknown";
          case FileType::kImage:
            return "image-x-generic";
          case FileType::kAudio:
            return "audio-x-generic";
          case FileType::kVideo:
            return "video-x-generic";
        }
        throw std::runtime_error("invalid file type");
      } else {
        return "folder";
      }
    }();
    return Response{
        .status = 302,
        .headers = {{"Location", StrCat("/static/", icon_name, ".svg")}}};
  }

  template <typename Item>
  Task<Response> GetIcon(const Item& item, stdx::stop_token stop_token) const {
    if constexpr (IsFile<Item, CloudProvider>) {
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
      } catch (...) {
        co_return GetStaticIcon(item);
      }
    } else {
      co_return GetStaticIcon(item);
    }
  }

  template <typename Item>
  Task<Response> GetItemThumbnail(Item d, stdx::stop_token stop_token) const {
    if constexpr (HasThumbnail<Item, CloudProvider>) {
      try {
        auto thumbnail =
            co_await provider_->GetItemThumbnail(d, http::Range{}, stop_token);
        co_return Response{
            .status = 200,
            .headers = {{"Cache-Control", "private"},
                        {"Cache-Control", "max-age=604800"},
                        {"Content-Type", std::string(thumbnail.mime_type)},
                        {"Content-Length", std::to_string(thumbnail.size)}},
            .body = std::move(thumbnail.data)};
      } catch (...) {
      }
    }
    co_return co_await GetIcon(d, std::move(stop_token));
  }

  template <typename Item>
  Task<Response> HandleExistingItem(Request request, Item d,
                                    stdx::stop_token stop_token) {
    if constexpr (IsDirectory<Item, CloudProvider>) {
      std::string directory_path = GetPath(request);
      if (directory_path.empty() || directory_path.back() != '/') {
        directory_path += '/';
      }
      auto hostname = http::GetCookie(request.headers, "host");
      co_return Response{
          .status = 200,
          .headers = {{"Content-Type", "text/html"}},
          .body = GetDirectoryContent(
              hostname && !hostname->empty() ? &*hostname : nullptr,
              provider_->ListDirectory(d, std::move(stop_token)),
              directory_path)};
    } else {
      co_return GetFileContentResponse(
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
  }

  template <typename Item>
  std::string GetItemEntry(const std::string* hostname, const Item& item,
                           std::string_view path) const {
    std::string file_link = StrCat(path, http::EncodeUri(item.name));
    bool use_dash_player = [&] {
      if constexpr (IsFile<Item, CloudProvider>) {
        return item.name.ends_with(".mpd") ||
               (hostname &&
                CloudProvider::GetMimeType(item).starts_with("video"));
      } else {
        return false;
      }
    }();
    return fmt::format(
        fmt::runtime(kAssetsHtmlItemEntryHtml), fmt::arg("name", item.name),
        fmt::arg("size", SizeToString(CloudProvider::GetSize(item))),
        fmt::arg("timestamp",
                 TimeStampToString(CloudProvider::GetTimestamp(item))),
        fmt::arg("url",
                 StrCat(file_link, use_dash_player ? "?dash_player=true" : "")),
        fmt::arg("thumbnail_url", StrCat(file_link, "?thumbnail=true")));
  }

  Generator<std::string> GetDirectoryContent(
      const std::string* hostname,
      Generator<typename CloudProvider::PageData> page_data,
      std::string path) const {
    co_yield "<!DOCTYPE html>"
             "<html lang='en-us'>"
             "<head>"
             "  <title>coro-cloudstorage</title>"
             "  <meta charset='UTF-8'>"
             "  <meta name='viewport' "
             "        content='width=device-width, initial-scale=1'>"
             "  <link rel=stylesheet href='/static/layout.css'>"
             "  <link rel=stylesheet href='/static/colors.css'>"
             "</head>"
             "<body class='root-container'>"
             "<table class='content-table'>";
    co_yield fmt::format(
        fmt::runtime(kAssetsHtmlItemEntryHtml), fmt::arg("name", ".."),
        fmt::arg("size", ""), fmt::arg("timestamp", ""),
        fmt::arg("url", GetDirectoryPath(path)),
        fmt::arg("thumbnail_url",
                 StrCat(IsRoot(path) ? path : GetDirectoryPath(path),
                        "?thumbnail=true")));
    FOR_CO_AWAIT(const auto& page, page_data) {
      for (const auto& item : page.items) {
        co_yield std::visit(
            [&](const auto& item) {
              return GetItemEntry(hostname, item, path);
            },
            item);
      }
    }
    co_yield "</table>"
             "</body>"
             "</html>";
  }

  using ItemT = typename CloudProvider::Item;

  const ThumbnailGenerator* thumbnail_generator_;
  CloudProvider* provider_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_HANDLER_H
