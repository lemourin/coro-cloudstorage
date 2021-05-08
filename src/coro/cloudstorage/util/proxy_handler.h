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
               CloudProvider* provider, std::string path_prefix)
      : thumbnail_generator_(&thumbnail_generator),
        provider_(provider),
        path_prefix_(std::move(path_prefix)) {}

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
        auto create_directory = stdx::BindFront(CreateDirectoryF{}, provider_,
                                                path.back(), stop_token);
        co_return co_await std::visit(
            std::move(create_directory),
            co_await provider_->GetItemByPathComponents(GetDirectoryPath(path),
                                                        stop_token));
      }
      if (request.method == http::Method::kPut) {
        if (path.empty()) {
          throw CloudException("invalid path");
        }
        auto create_file =
            stdx::BindFront(CreateFileF{}, provider_, path.back(),
                            std::move(request), stop_token);
        co_return co_await std::visit(
            std::move(create_file), co_await provider_->GetItemByPathComponents(
                                        GetDirectoryPath(path), stop_token));
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
  static Generator<std::string> GetGenerator(std::string content) {
    co_yield std::move(content);
  }

  static Generator<std::string> GetDashPlayer(std::string path) {
    co_yield fmt::format(kAssetsHtmlDashPlayerHtml,
                         fmt::arg("video_url", path));
  }

  template <typename Item>
  Task<Response> GetIcon(const Item& item, stdx::stop_token stop_token) const {
    std::string content;
    std::string mime_type = "image/svg+xml";
    if constexpr (IsFile<Item, CloudProvider>) {
      try {
        switch (CloudProvider::GetFileType(item)) {
          case FileType::kImage:
          case FileType::kVideo: {
            content = co_await(*thumbnail_generator_)(
                provider_, item,
                ThumbnailOptions{.codec = ThumbnailOptions::Codec::PNG},
                std::move(stop_token));
            mime_type = "image/png";
            break;
          }
          default:
            throw CloudException(CloudException::Type::kNotFound);
        }
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
        .body = GetGenerator(std::string(content))};
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
      typename CloudProvider::Item new_item = d;
      if (!Equal(GetDirectoryPath(path), destination_path)) {
        auto destination_directory =
            co_await provider_->GetItemByPathComponents(destination_path,
                                                        stop_token);
        auto move_item = stdx::BindFront(MoveItemF{}, provider_, d, stop_token);
        auto item =
            co_await std::visit(std::move(move_item), destination_directory);
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
        auto rename_item = stdx::BindFront(
            RenameItemF{}, provider_, std::move(destination_name), stop_token);
        auto item = co_await std::visit(std::move(rename_item), new_item);
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
    std::stringstream row;
    row << "<tr>";
    row << "<td class='thumbnail-container'><image class='thumbnail' src='"
        << file_link << "?thumbnail=true"
        << "'/></td>";
    if (item.name.ends_with(".mpd")) {
      file_link += "?dash_player=true";
    }
    row << "<td class='item-metadata'><table>";
    row << "<tr><td><a class='title' href='" << file_link << "'>" << item.name
        << "</a></td></tr>";
    row << "<tr><td><small>"
        << TimeStampToString(CloudProvider::GetTimestamp(item))
        << "</small></td></tr>";
    row << "</table></td>";
    row << "<td class='size'><small>"
        << SizeToString(CloudProvider::GetSize(item)) << "</small></td>";
    row << "</tr>";
    return std::move(row).str();
  }

  Generator<std::string> GetDirectoryContent(
      Generator<typename CloudProvider::PageData> page_data,
      std::string path) const {
    std::stringstream header;
    header << "<!DOCTYPE html><html><head>"
              "<meta charset='UTF-8'>"
              "<meta name='viewport' content='width=device-width, "
              "initial-scale=1'>"
              "<link rel=stylesheet href='/static/default.css'>"
              "</head>"
              "<body><table class='content-table'>"
              "<tr>"
              "<td class='thumbnail-container'>"
              "<image class='thumbnail' src='"
           << (IsRoot(path) ? path : GetDirectoryPath(path))
           << "?thumbnail=true'/>"
              "</td>"
              "<td class='item-metadata'><a href='"
           << GetDirectoryPath(path)
           << "'>..</a></td><td class='size'/>"
              "</tr>";
    co_yield std::move(header).str();
    FOR_CO_AWAIT(const auto& page, page_data) {
      for (const auto& item : page.items) {
        co_yield std::visit(
            [&](const auto& item) { return GetItemEntry(item, path); }, item);
      }
    }
    co_yield "</table></body></html>";
  }

  static auto ToFileContent(Request request) {
    if (!request.body) {
      throw http::HttpException(http::HttpException::kBadRequest);
    }
    typename CloudProvider::FileContent content{.data =
                                                    std::move(*request.body)};
    auto header = ::coro::http::GetHeader(request.headers, "Content-Length");
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
    if (path.size() <= 1) {
      throw CloudException("root has no parent");
    }
    return path.subspan(0, path.size() - 2);
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

  struct RenameItemF {
    template <typename Item>
    Task<std::optional<typename CloudProvider::Item>> operator()(
        CloudProvider* provider, std::string new_name,
        stdx::stop_token stop_token, Item item) {
      if constexpr (CanRename<Item, CloudProvider>) {
        co_return co_await provider->RenameItem(
            std::move(item), std::move(new_name), std::move(stop_token));
      } else {
        co_return std::nullopt;
      }
    }
  };

  struct MoveItemF {
    template <typename Destination, typename Item>
    Task<std::optional<typename CloudProvider::Item>> operator()(
        CloudProvider* provider, Item item, stdx::stop_token stop_token,
        Destination destination) {
      if constexpr (CanMove<Item, Destination, CloudProvider>) {
        co_return co_await provider->MoveItem(
            std::move(item), std::move(destination), std::move(stop_token));
      } else {
        co_return std::nullopt;
      }
    }
  };

  struct CreateDirectoryF {
    template <typename Item>
    Task<Response> operator()(CloudProvider* provider, std::string name,
                              stdx::stop_token stop_token, const Item& item) {
      if constexpr (IsDirectory<Item, CloudProvider> &&
                    CanCreateDirectory<Item, CloudProvider>) {
        co_await provider->CreateDirectory(item, std::move(name),
                                           std::move(stop_token));
        co_return Response{.status = 201};
      } else {
        co_return Response{.status = 501};
      }
    }
  };

  struct CreateFileF {
    template <typename Item>
    Task<Response> operator()(CloudProvider* provider, std::string_view name,
                              Request request, stdx::stop_token stop_token,
                              const Item& item) {
      if constexpr (IsDirectory<Item, CloudProvider> &&
                    CanCreateFile<Item, CloudProvider>) {
        co_await provider->CreateFile(item, name,
                                      ToFileContent(std::move(request)),
                                      std::move(stop_token));
        co_return Response{.status = 201};
      } else {
        co_return Response{.status = 501};
      }
    }
  };

  const ThumbnailGenerator* thumbnail_generator_;
  CloudProvider* provider_;
  std::string path_prefix_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_PROXY_HANDLER_H
