#ifndef CORO_CLOUDSTORAGE_PROXY_HANDLER_H
#define CORO_CLOUDSTORAGE_PROXY_HANDLER_H

#include <coro/cloudstorage/util/webdav_utils.h>
#include <coro/http/http_parse.h>
#include <coro/util/lru_cache.h>

#include <sstream>

namespace coro::cloudstorage::util {

template <typename CloudProvider>
class ProxyHandler {
 public:
  using Request = http::Request<>;
  using Response = http::Response<>;

  ProxyHandler(CloudProvider* provider, std::string path_prefix)
      : provider_(provider),
        path_prefix_(std::move(path_prefix)),
        item_cache_(32, GetItem{provider_}) {}

  template <typename Item>
  Task<Response> HandleExistingItem(Request request, std::string path,
                                    const Item& d,
                                    stdx::stop_token stop_token) {
    if (request.method == http::Method::kProppatch) {
      co_return Response{.status = 207,
                         .headers = {{"Content-Type", "text/xml"}},
                         .body = GetWebDavItemResponse(path_prefix_ + path, d)};
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
      auto destination_header =
          ::coro::http::GetHeader(request.headers, "Destination");
      if (!destination_header) {
        co_return Response{.status = 400};
      }
      auto destination = GetEffectivePath(
          http::DecodeUri(http::ParseUri(*destination_header).path.value_or(""))
              .substr(path_prefix_.length()));
      auto destination_path = GetDirectoryPath(destination);
      auto destination_name = GetFileName(destination);
      typename CloudProvider::Item new_item = d;
      if (GetDirectoryPath(path) != destination_path) {
        auto destination_directory =
            co_await item_cache_.Get(destination_path, stop_token);
        auto move_item = stdx::BindFront(MoveItemF{}, provider_, d, stop_token);
        auto item =
            co_await std::visit(std::move(move_item), destination_directory);
        if (!item) {
          co_return Response{.status = 501};
        } else {
          new_item = std::move(*item);
        }
      }
      if (GetFileName(path) != destination_name) {
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
      if (!path.empty() && path.back() != '/') {
        path += '/';
      }
      if (request.method == http::Method::kPropfind) {
        co_return Response{
            .status = 207,
            .headers = {{"Content-Type", "text/xml"}},
            .body = GetWebDavResponse(
                d, provider_->ListDirectory(d, std::move(stop_token)),
                std::move(request), path_prefix_ + path)};
      } else {
        co_return Response{
            .status = 200,
            .headers = {{"Content-Type", "text/html"}},
            .body = GetDirectoryContent(
                provider_->ListDirectory(d, std::move(stop_token)),
                path_prefix_ + path)};
      }
    } else {
      if (request.method == http::Method::kPropfind) {
        co_return Response{
            .status = 207,
            .headers = {{"Content-Type", "text/html"}},
            .body = GetWebDavItemResponse(path_prefix_ + path, d)};
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

  Task<Response> operator()(Request request, stdx::stop_token stop_token) {
    std::string path = GetEffectivePath(
        http::DecodeUri(http::ParseUri(request.url).path.value_or(""))
            .substr(path_prefix_.length()));
    if (path.empty() || path.front() != '/') {
      path = '/' + path;
    }
    try {
      if (request.method == http::Method::kMkcol) {
        auto create_directory =
            stdx::BindFront(CreateDirectoryF{}, provider_, path, stop_token);
        co_return co_await std::visit(std::move(create_directory),
                                      co_await provider_->GetItemByPath(
                                          GetDirectoryPath(path), stop_token));
      }
      if (request.method == http::Method::kPut) {
        auto create_file = stdx::BindFront(CreateFileF{}, provider_, path,
                                           std::move(request), stop_token);
        co_return co_await std::visit(std::move(create_file),
                                      co_await provider_->GetItemByPath(
                                          GetDirectoryPath(path), stop_token));
      }
      if ((request.method == http::Method::kPropfind &&
           ::coro::http::GetHeader(request.headers, "Depth") == "0") ||
          request.method == http::Method::kMove ||
          request.method == http::Method::kDelete) {
        item_cache_.Invalidate(path);
      }
      co_return co_await std::visit(
          [&](const auto& d) {
            return HandleExistingItem(std::move(request), std::move(path), d,
                                      std::move(stop_token));
          },
          co_await item_cache_.Get(path, stop_token));
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

  template <typename Item>
  static Generator<std::string> GetWebDavItemResponse(std::string path,
                                                      Item item) {
    co_yield R"(<?xml version="1.0" encoding="utf-8"?><d:multistatus xmlns:d="DAV:">)";
    ElementData current_element_data{
        .path = std::move(path), .name = item.name, .is_directory = false};
    co_yield GetElement(std::move(current_element_data));
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
    co_yield GetElement(std::move(current_element_data));
    if (::coro::http::GetHeader(request.headers, "Depth") == "1") {
      FOR_CO_AWAIT(const auto& page, page_data) {
        for (const auto& item : page.items) {
          auto name = std::visit([](auto item) { return item.name; }, item);
          auto timestamp = std::visit(
              [](const auto& item) {
                return CloudProvider::GetTimestamp(item);
              },
              item);
          ElementData element_data(
              {.path = path + name,
               .name = name,
               .is_directory = std::holds_alternative<Directory>(item),
               .timestamp = timestamp});
          std::visit(
              [&](const auto& item) {
                if constexpr (IsFile<decltype(item), CloudProvider>) {
                  element_data.mime_type = CloudProvider::GetMimeType(item);
                  element_data.size = item.size;
                }
              },
              item);
          co_yield GetElement(std::move(element_data));
        }
      }
    }
    co_yield "</d:multistatus>";
  }

  static Generator<std::string> GetDirectoryContent(
      Generator<typename CloudProvider::PageData> page_data, std::string path) {
    co_yield R"(<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body><table><tr><td>[DIR]</td><td><a href=')" +
        GetDirectoryPath(path) + "'>..</a></td></tr>";
    FOR_CO_AWAIT(const auto& page, page_data) {
      for (const auto& item : page.items) {
        auto name =
            std::visit([](const auto& item) { return item.name; }, item);
        std::string type = std::visit(
            [](const auto& item) {
              if constexpr (IsDirectory<decltype(item), CloudProvider>) {
                return "DIR";
              } else {
                return "FILE";
              }
            },
            item);
        std::string file_link = coro::http::EncodeUriPath(path + name);
        if (name.ends_with(".mpd")) {
          file_link = "/dash" + file_link;
        }
        co_yield "<tr><td>[" + type + "]</td><td><a href='" + file_link + "'>" +
            name + "</a></td></tr>";
      }
    }
    co_yield "</table></body></html>";
  }

 private:
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
      return "";
    }
    path.pop_back();
    auto it = path.find_last_of('/');
    if (it == std::string_view::npos) {
      return "";
    }
    return std::string(path.begin(), path.begin() + it + 1);
  }

  static std::string GetFileName(std::string path) {
    if (path.empty()) {
      return "";
    }
    if (path.back() == '/') {
      path.pop_back();
    }
    auto it = path.find_last_of('/');
    if (it == std::string_view::npos) {
      return "";
    }
    return std::string(path.begin() + it + 1, path.end());
  }

  static std::string GetEffectivePath(std::string_view path) {
    std::vector<std::string> components;
    std::string current;
    size_t it = 0;
    while (it < path.size()) {
      if (path[it] == '/') {
        if (current == "..") {
          if (components.empty()) {
            throw std::invalid_argument("invalid path");
          } else {
            components.pop_back();
          }
        } else if (current != "." && current != "") {
          components.emplace_back(std::move(current));
        }
        current.clear();
      } else {
        current += path[it];
      }
      it++;
    }
    std::string result;
    for (std::string& component : components) {
      result += "/" + std::move(component);
    }
    if (!current.empty()) {
      result += "/" + std::move(current);
    }
    return result;
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
    Task<Response> operator()(CloudProvider* provider, std::string path,
                              stdx::stop_token stop_token, const Item& item) {
      if constexpr (IsDirectory<Item, CloudProvider> &&
                    CanCreateDirectory<Item, CloudProvider>) {
        co_await provider->CreateDirectory(item, GetFileName(std::move(path)),
                                           std::move(stop_token));
        co_return Response{.status = 201};
      } else {
        co_return Response{.status = 501};
      }
    }
  };

  struct CreateFileF {
    template <typename Item>
    Task<Response> operator()(CloudProvider* provider, std::string path,
                              Request request, stdx::stop_token stop_token,
                              const Item& item) {
      if constexpr (IsDirectory<Item, CloudProvider> &&
                    CanCreateFile<Item, CloudProvider>) {
        co_await provider->CreateFile(item, GetFileName(std::move(path)),
                                      ToFileContent(std::move(request)),
                                      std::move(stop_token));
        co_return Response{.status = 201};
      } else {
        co_return Response{.status = 501};
      }
    }
  };

  struct GetItem {
    auto operator()(std::string path, stdx::stop_token stop_token) {
      return provider->GetItemByPath(std::move(path), stop_token);
    }
    CloudProvider* provider;
  };

  CloudProvider* provider_;
  std::string path_prefix_;
  ::coro::util::LRUCache<std::string, GetItem> item_cache_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_PROXY_HANDLER_H
