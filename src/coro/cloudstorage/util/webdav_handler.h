#ifndef CORO_CLOUDSTORAGE_WEBDAV_HANDLER_H
#define CORO_CLOUDSTORAGE_WEBDAV_HANDLER_H

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/file_utils.h"
#include "coro/cloudstorage/util/handler_utils.h"
#include "coro/cloudstorage/util/webdav_utils.h"
#include "coro/http/http.h"

namespace coro::cloudstorage::util {

template <typename CloudProvider>
class WebDAVHandler {
 public:
  using Request = http::Request<>;
  using Response = http::Response<>;

  explicit WebDAVHandler(CloudProvider* provider) : provider_(provider) {}

  Task<Response> operator()(Request request,
                            stdx::stop_token stop_token) const {
    auto uri = http::ParseUri(request.url);
    auto path = GetEffectivePath(uri.path.value());
    if (request.method == http::Method::kMkcol) {
      if (path.empty()) {
        throw CloudException("invalid path");
      }
      co_return co_await std::visit(
          CreateDirectoryF{provider_, path.back(), stop_token},
          co_await provider_->GetItemByPathComponents(GetDirectoryPath(path),
                                                      stop_token));
    } else if (request.method == http::Method::kPut) {
      if (path.empty()) {
        throw CloudException("invalid path");
      }
      co_return co_await std::visit(
          CreateFileF{provider_, path.back(),
                      ToFileContent<CloudProvider>(std::move(request)),
                      stop_token},
          co_await provider_->GetItemByPathComponents(GetDirectoryPath(path),
                                                      stop_token));
    } else {
      co_return co_await std::visit(
          [&](const auto& d) {
            return HandleExistingItem(std::move(request), path, d, stop_token);
          },
          co_await provider_->GetItemByPathComponents(path, stop_token));
    }
  }

 private:
  using ItemT = typename CloudProvider::Item;

  template <typename Item>
  Task<Response> HandleExistingItem(Request request,
                                    std::span<const std::string> path, Item d,
                                    stdx::stop_token stop_token) const {
    if (request.method == http::Method::kProppatch) {
      co_return Response{.status = 207,
                         .headers = {{"Content-Type", "text/xml"}},
                         .body = GetWebDavItemResponse(GetPath(request), d)};
    } else if (request.method == http::Method::kDelete) {
      if constexpr (CanRemove<Item, CloudProvider>) {
        co_await provider_->RemoveItem(d, std::move(stop_token));
        co_return Response{.status = 204};
      } else {
        co_return Response{.status = 501};
      }
    } else if (request.method == http::Method::kMove) {
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
        }
      }
      co_return Response{.status = 201};
    } else if (request.method == http::Method::kPropfind) {
      if constexpr (IsDirectory<Item, CloudProvider>) {
        std::string directory_path = GetPath(request);
        if (directory_path.empty() || directory_path.back() != '/') {
          directory_path += '/';
        }
        co_return Response{
            .status = 207,
            .headers = {{"Content-Type", "text/xml"}},
            .body = GetWebDavResponse(
                d, provider_->ListDirectory(d, std::move(stop_token)),
                std::move(request), directory_path)};
      } else {
        co_return Response{.status = 207,
                           .headers = {{"Content-Type", "text/html"}},
                           .body = GetWebDavItemResponse(GetPath(request), d)};
      }
    } else {
      throw std::runtime_error("unsupported method");
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

  CloudProvider* provider_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_WEBDAV_HANDLER_H
