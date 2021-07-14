#ifndef CORO_CLOUDSTORAGE_PROXY_HANDLER_H
#define CORO_CLOUDSTORAGE_PROXY_HANDLER_H

#include <coro/cloudstorage/util/assets.h>
#include <coro/cloudstorage/util/thumbnail_options.h>
#include <coro/cloudstorage/util/webdav_utils.h>
#include <coro/http/http_parse.h>
#include <coro/util/lru_cache.h>
#include <fmt/format.h>

#include <iomanip>
#include <sstream>

namespace coro::cloudstorage::util {

template <typename CloudProvider, typename ThumbnailGenerator>
class ProxyHandler {
 public:
  using Request = http::Request<>;
  using Response = http::Response<>;

  ProxyHandler(const ThumbnailGenerator& thumbnail_generator,
               CloudProvider* provider)
      : thumbnail_generator_(&thumbnail_generator), provider_(provider) {}

  Task<Response> operator()(Request request, stdx::stop_token stop_token) {
    auto uri = http::ParseUri(request.url);
    auto path = GetEffectivePath(uri.path.value());
    try {
      if (request.method == http::Method::kGet && uri.query) {
        auto query = http::ParseQuery(*uri.query);
        if (auto it = query.find("thumbnail");
            it != query.end() && it->second == "true") {
          co_return co_await std::visit(
              [&](const auto& d) {
                return GetItemThumbnail(std::move(request), d, stop_token);
              },
              co_await provider_->GetItemByPathComponents(path, stop_token));
        }
        if (auto it = query.find("dash_player");
            it != query.end() && it->second == "true") {
          co_return Response{
              .status = 200,
              .headers = {{"Content-Type", "text/html; charset=UTF-8"}},
              .body = GetDashPlayer(uri.path.value())};
        }
      }
      if (request.method == http::Method::kMkcol) {
        if (path.empty()) {
          throw CloudException("invalid path");
        }
        co_return co_await std::visit(
            CreateDirectoryF{provider_, path.back(), stop_token},
            co_await provider_->GetItemByPathComponents(GetDirectoryPath(path),
                                                        stop_token));
      }
      if (request.method == http::Method::kPut) {
        if (path.empty()) {
          throw CloudException("invalid path");
        }
        co_return co_await std::visit(
            CreateFileF{provider_, path.back(),
                        ToFileContent(std::move(request)), stop_token},
            co_await provider_->GetItemByPathComponents(GetDirectoryPath(path),
                                                        stop_token));
      }
      co_return co_await std::visit(
          [&](const auto& d) {
            return HandleExistingItem(std::move(request), path, d, stop_token);
          },
          co_await provider_->GetItemByPathComponents(path, stop_token));
    } catch (const CloudException& e) {
      switch (e.type()) {
        case CloudException::Type::kNotFound:
          co_return Response{.status = 404};
        case CloudException::Type::kUnauthorized:
          co_return Response{.status = 401};
        case CloudException::Type::kUnknown:
          throw;
      }
    }
  }

 private:
  static Generator<std::string> GetDashPlayer(std::string path) {
    co_yield fmt::format(fmt::runtime(kAssetsHtmlDashPlayerHtml),
                         fmt::arg("video_url", path));
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
  Task<Response> GetIcon(const Item& item, stdx::stop_token stop_token) const {
    std::string content;
    std::string mime_type = "image/svg+xml";
    if constexpr (IsFile<Item, CloudProvider>) {
      try {
        content = co_await GenerateThumbnail(item, std::move(stop_token));
        mime_type = "image/png";
      } catch (...) {
        content = [&] {
          switch (CloudProvider::GetFileType(item)) {
            case FileType::kUnknown:
              return kAssetsIconsGtkFileSvg;
            case FileType::kImage:
              return kAssetsIconsImageSvg;
            case FileType::kAudio:
              return kAssetsIconsAudioXGenericSvg;
            case FileType::kVideo:
              return kAssetsIconsVideoSvg;
          }
          throw std::runtime_error("invalid file type");
        }();
      }
    } else {
      content = kAssetsIconsFolderSvg;
    }
    co_return Response{
        .status = 200,
        .headers = {{"Cache-Control", "private"},
                    {"Cache-Control", "max-age=604800"},
                    {"Content-Type", std::move(mime_type)},
                    {"Content-Length", std::to_string(content.size())}},
        .body = http::CreateBody(std::string(content))};
  }

  template <typename Item>
  Task<Response> GetItemThumbnail(Request request, Item d,
                                  stdx::stop_token stop_token) const {
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

  std::string GetPath(const Request& request) const {
    return http::ParseUri(request.url).path.value();
  }

  template <typename T>
  static bool Equal(std::span<const T> s1, std::span<const T> s2) {
    return std::equal(s1.begin(), s1.end(), s2.begin());
  }

  template <typename Item>
  Task<Response> HandleExistingItem(Request request,
                                    std::span<const std::string> path, Item d,
                                    stdx::stop_token stop_token) {
    if (request.method == http::Method::kProppatch) {
      co_return Response{.status = 207,
                         .headers = {{"Content-Type", "text/xml"}},
                         .body = GetWebDavItemResponse(GetPath(request), d)};
    }
    if (request.method == http::Method::kDelete) {
      if constexpr (CanRemove<Item, CloudProvider>) {
        co_await provider_->RemoveItem(d, std::move(stop_token));
        co_return Response{.status = 204};
      } else {
        co_return Response{.status = 501};
      }
    }
    if (request.method == http::Method::kMove) {
      auto destination_header = http::GetHeader(request.headers, "Destination");
      if (!destination_header) {
        co_return Response{.status = 400};
      }
      auto destination =
          GetEffectivePath(http::ParseUri(*destination_header).path.value());
      if (destination.empty()) {
        throw CloudException("invalid destination");
      }
      auto destination_path = GetDirectoryPath(destination);
      auto destination_name = destination.back();
      ItemT new_item = d;
      if (!Equal(GetDirectoryPath(path), destination_path)) {
        auto destination_directory =
            co_await provider_->GetItemByPathComponents(destination_path,
                                                        stop_token);
        auto item = co_await std::visit(
            MoveItemF<Item>{provider_, std::move(d), std::move(stop_token)},
            std::move(destination_directory));
        if (!item) {
          co_return Response{.status = 501};
        } else {
          new_item = std::move(*item);
        }
      }
      if (path.empty()) {
        throw CloudException("invalid path");
      }
      if (path.back() != destination_name) {
        auto item = co_await std::visit(
            RenameItemF{provider_, std::move(destination_name),
                        std::move(stop_token)},
            std::move(new_item));
        if (!item) {
          co_return Response{.status = 501};
        } else {
          new_item = std::move(*item);
        }
      }
      co_return Response{.status = 201};
    }
    if constexpr (IsDirectory<Item, CloudProvider>) {
      std::string directory_path = GetPath(request);
      if (directory_path.empty() || directory_path.back() != '/') {
        directory_path += '/';
      }
      if (request.method == http::Method::kPropfind) {
        co_return Response{
            .status = 207,
            .headers = {{"Content-Type", "text/xml"}},
            .body = GetWebDavResponse(
                d, provider_->ListDirectory(d, std::move(stop_token)),
                std::move(request), directory_path)};
      } else {
        co_return Response{
            .status = 200,
            .headers = {{"Content-Type", "text/html"}},
            .body = GetDirectoryContent(
                provider_->ListDirectory(d, std::move(stop_token)),
                directory_path)};
      }
    } else {
      if (request.method == http::Method::kPropfind) {
        co_return Response{.status = 207,
                           .headers = {{"Content-Type", "text/html"}},
                           .body = GetWebDavItemResponse(GetPath(request), d)};
      }
      std::vector<std::pair<std::string, std::string>> headers = {
          {"Content-Type", CloudProvider::GetMimeType(d)},
          {"Content-Disposition", "inline; filename=\"" + d.name + "\""},
          {"Access-Control-Allow-Origin", "*"},
          {"Access-Control-Allow-Headers", "*"}};
      auto range_str = coro::http::GetHeader(request.headers, "Range");
      coro::http::Range range =
          coro::http::ParseRange(range_str.value_or("bytes=0-"));
      auto size = CloudProvider::GetSize(d);
      if (size) {
        if (!range.end) {
          range.end = *size - 1;
        }
        headers.emplace_back("Accept-Ranges", "bytes");
        headers.emplace_back("Content-Length",
                             std::to_string(*range.end - range.start + 1));
        if (range_str) {
          std::stringstream stream;
          stream << "bytes " << range.start << "-" << *range.end << "/"
                 << *size;
          headers.emplace_back("Content-Range", std::move(stream).str());
        }
      }
      co_return Response{
          .status = !range_str || !size ? 200 : 206,
          .headers = std::move(headers),
          .body = provider_->GetFileContent(d, range, std::move(stop_token))};
    }
  }

  template <typename Item>
  static Generator<std::string> GetWebDavItemResponse(std::string path,
                                                      Item item) {
    co_yield R"(<?xml version="1.0" encoding="utf-8"?><d:multistatus xmlns:d="DAV:">)";
    ElementData current_element_data{
        .path = std::move(path),
        .name = item.name,
        .size = CloudProvider::GetSize(item),
        .timestamp = CloudProvider::GetTimestamp(item)};
    if constexpr (IsFile<Item, CloudProvider>) {
      current_element_data.mime_type = CloudProvider::GetMimeType(item);
      current_element_data.size = CloudProvider::GetSize(item);
    } else {
      current_element_data.is_directory = true;
    }
    co_yield GetElement(current_element_data);
    co_yield "</d:multistatus>";
  }

  template <IsDirectory<CloudProvider> Directory>
  static Generator<std::string> GetWebDavResponse(
      Directory directory,
      Generator<typename CloudProvider::PageData> page_data, Request request,
      std::string path) {
    co_yield R"(<?xml version="1.0" encoding="utf-8"?><d:multistatus xmlns:d="DAV:">)";
    ElementData current_element_data{
        .path = path, .name = directory.name, .is_directory = true};
    co_yield GetElement(current_element_data);
    if (coro::http::GetHeader(request.headers, "Depth") == "1") {
      FOR_CO_AWAIT(const auto& page, page_data) {
        for (const auto& item : page.items) {
          auto name = std::visit([](auto item) { return item.name; }, item);
          auto timestamp = std::visit(
              [](const auto& item) {
                return CloudProvider::GetTimestamp(item);
              },
              item);
          ElementData element_data(
              {.path = util::StrCat(path, http::EncodeUri(name)),
               .name = name,
               .is_directory = std::visit(
                   [](const auto& d) {
                     return IsDirectory<std::remove_cvref_t<decltype(d)>,
                                        CloudProvider>;
                   },
                   item),
               .timestamp = timestamp});
          std::visit(
              [&](const auto& item) {
                if constexpr (IsFile<decltype(item), CloudProvider>) {
                  element_data.mime_type = CloudProvider::GetMimeType(item);
                  element_data.size = CloudProvider::GetSize(item);
                }
              },
              item);
          co_yield GetElement(element_data);
        }
      }
    }
    co_yield "</d:multistatus>";
  }

  bool IsRoot(std::string_view path) const {
    return GetEffectivePath(path).empty();
  }

  template <typename Item>
  std::string GetItemEntry(const Item& item, std::string_view path) const {
    std::string file_link = util::StrCat(path, http::EncodeUri(item.name));
    return fmt::format(
        fmt::runtime(kAssetsHtmlItemEntryHtml), fmt::arg("name", item.name),
        fmt::arg("size", SizeToString(CloudProvider::GetSize(item))),
        fmt::arg("timestamp",
                 TimeStampToString(CloudProvider::GetTimestamp(item))),
        fmt::arg("url", util::StrCat(file_link, item.name.ends_with(".mpd")
                                                    ? "?dash_player=true"
                                                    : "")),
        fmt::arg("thumbnail_url", util::StrCat(file_link, "?thumbnail=true")));
  }

  Generator<std::string> GetDirectoryContent(
      Generator<typename CloudProvider::PageData> page_data,
      std::string path) const {
    co_yield "<!DOCTYPE html>"
              "<html>"
              "<head>"
              "  <title>coro-cloudstorage</title>"
              "  <meta charset='UTF-8'>"
              "  <meta name='viewport' "
              "        content='width=device-width, initial-scale=1'>"
              "  <link rel=stylesheet href='/static/default.css'>"
              "</head>"
              "<body>"
              "<table class='content-table'>";
    co_yield fmt::format(
        fmt::runtime(kAssetsHtmlItemEntryHtml), fmt::arg("name", ".."),
        fmt::arg("size", ""), fmt::arg("timestamp", ""),
        fmt::arg("url", GetDirectoryPath(path)),
        fmt::arg("thumbnail_url",
                 util::StrCat(IsRoot(path) ? path : GetDirectoryPath(path),
                              "?thumbnail=true")));
    FOR_CO_AWAIT(const auto& page, page_data) {
      for (const auto& item : page.items) {
        co_yield std::visit(
            [&](const auto& item) { return GetItemEntry(item, path); }, item);
      }
    }
    co_yield "</table>"
             "</body>"
             "</html>";
  }

  static auto ToFileContent(Request request) {
    if (!request.body) {
      throw http::HttpException(http::HttpException::kBadRequest);
    }
    typename CloudProvider::FileContent content{.data =
                                                    std::move(*request.body)};
    auto header = http::GetHeader(request.headers, "Content-Length");
    if (std::is_convertible_v<decltype(content.size), int64_t> && !header) {
      throw http::HttpException(http::HttpException::kBadRequest);
    }
    if (header) {
      content.size = std::stoll(*header);
    }
    return content;
  }

  static std::string GetDirectoryPath(std::string path) {
    if (path.empty()) {
      throw CloudException("invalid path");
    }
    if (path.back() == '/') {
      path.pop_back();
    }
    auto it = path.find_last_of('/');
    if (it == std::string_view::npos) {
      throw CloudException("root has no parent");
    }
    return path.substr(0, it + 1);
  }

  static std::span<const std::string> GetDirectoryPath(
      std::span<const std::string> path) {
    if (path.empty()) {
      throw CloudException("root has no parent");
    }
    return path.subspan(0, path.size() - 1);
  }

  static std::vector<std::string> GetEffectivePath(std::string_view uri_path) {
    std::vector<std::string> components;
    for (std::string_view component :
         util::SplitString(std::string(uri_path), '/')) {
      if (component.empty() || component == ".") {
        continue;
      } else if (component == "..") {
        if (components.empty()) {
          throw CloudException("invalid path");
        } else {
          components.pop_back();
        }
      } else {
        components.emplace_back(component);
      }
    }
    for (auto& d : components) {
      d = http::DecodeUri(d);
    }
    if (components.empty()) {
      throw CloudException("invalid path");
    }
    components.erase(components.begin());
    return components;
  }

  struct CreateDirectoryF {
    template <typename Item>
    Task<Response> operator()(Item item) && {
      if constexpr (IsDirectory<Item, CloudProvider> &&
                    CanCreateDirectory<Item, CloudProvider>) {
        co_await provider->CreateDirectory(std::move(item), name,
                                           std::move(stop_token));
        co_return Response{.status = 201};
      } else {
        co_return Response{.status = 501};
      }
    }
    CloudProvider* provider;
    std::string name;
    stdx::stop_token stop_token;
  };

  struct CreateFileF {
    template <typename Item>
    Task<Response> operator()(Item item) && {
      if constexpr (IsDirectory<Item, CloudProvider> &&
                    CanCreateFile<Item, CloudProvider>) {
        co_await provider->CreateFile(std::move(item), std::move(name),
                                      std::move(content),
                                      std::move(stop_token));
        co_return Response{.status = 201};
      } else {
        co_return Response{.status = 501};
      }
    }

    CloudProvider* provider;
    std::string name;
    typename CloudProvider::FileContent content;
    stdx::stop_token stop_token;
  };

  using ItemT = typename CloudProvider::Item;

  template <typename Item>
  struct MoveItemF {
    template <typename Destination>
    Task<std::optional<ItemT>> operator()(Destination destination) && {
      if constexpr (CanMove<Item, Destination, CloudProvider>) {
        co_return co_await provider->MoveItem(
            std::move(source), std::move(destination), std::move(stop_token));
      } else {
        co_return std::nullopt;
      }
    }
    CloudProvider* provider;
    Item source;
    stdx::stop_token stop_token;
  };

  struct RenameItemF {
    template <typename Source>
    Task<std::optional<ItemT>> operator()(Source item) && {
      if constexpr (CanRename<Source, CloudProvider>) {
        co_return co_await provider->RenameItem(std::move(item),
                                                std::move(destination_name),
                                                std::move(stop_token));
      } else {
        co_return std::nullopt;
      }
    }
    CloudProvider* provider;
    std::string destination_name;
    stdx::stop_token stop_token;
  };

  const ThumbnailGenerator* thumbnail_generator_;
  CloudProvider* provider_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_PROXY_HANDLER_H
