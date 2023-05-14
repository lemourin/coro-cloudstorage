#include "coro/cloudstorage/util/webdav_handler.h"

#include "coro/cloudstorage/util/cloud_provider_utils.h"
#include "coro/cloudstorage/util/string_utils.h"

namespace coro::cloudstorage::util {

namespace {

using Request = http::Request<>;
using Response = http::Response<>;
using CloudProvider = AbstractCloudProvider;
using ItemT = AbstractCloudProvider::Item;

struct CreateDirectoryF {
  Task<Response> operator()(AbstractCloudProvider::Directory item) && {
    co_await provider->CreateDirectory(std::move(item), name,
                                       std::move(stop_token));
    co_return Response{.status = 201};
  }

  Task<Response> operator()(AbstractCloudProvider::File) && {
    co_return Response{.status = 501};
  }

  CloudProvider* provider;
  std::string name;
  stdx::stop_token stop_token;
};

struct CreateFileF {
  Task<Response> operator()(AbstractCloudProvider::Directory item) && {
    auto content = ToFileContent(provider, item, std::move(request));
    co_await provider->CreateFile(std::move(item), std::move(name),
                                  std::move(content), std::move(stop_token));
    co_return Response{.status = 201};
  }

  Task<Response> operator()(AbstractCloudProvider::File) && {
    co_return Response{.status = 501};
  }

  CloudProvider* provider;
  std::string name;
  Request request;
  stdx::stop_token stop_token;
};

template <typename Item>
struct MoveItemF {
  Task<std::optional<ItemT>> operator()(
      AbstractCloudProvider::Directory destination) && {
    co_return co_await provider->MoveItem(
        std::move(source), std::move(destination), std::move(stop_token));
  }

  Task<std::optional<ItemT>> operator()(AbstractCloudProvider::File) && {
    co_return std::nullopt;
  }

  CloudProvider* provider;
  Item source;
  stdx::stop_token stop_token;
};

struct RenameItemF {
  template <typename Source>
  Task<std::optional<ItemT>> operator()(Source item) && {
    co_return co_await provider->RenameItem(
        std::move(item), std::move(destination_name), std::move(stop_token));
  }
  CloudProvider* provider;
  std::string destination_name;
  stdx::stop_token stop_token;
};

template <typename Item>
Generator<std::string> GetWebDavItemResponse(std::string path, Item item) {
  co_yield R"(<?xml version="1.0" encoding="utf-8"?><d:multistatus xmlns:d="DAV:">)";
  ElementData current_element_data{.path = std::move(path),
                                   .name = item.name,
                                   .size = item.size,
                                   .timestamp = item.timestamp};
  if constexpr (std::is_same_v<Item, AbstractCloudProvider::File>) {
    current_element_data.mime_type = item.mime_type;
    current_element_data.size = item.size;
  } else {
    current_element_data.is_directory = true;
  }
  co_yield GetElement(current_element_data);
  co_yield "</d:multistatus>";
}

Generator<std::string> GetWebDavResponse(
    AbstractCloudProvider::Directory directory,
    Generator<AbstractCloudProvider::PageData> page_data, Request request,
    std::string path) {
  co_yield R"(<?xml version="1.0" encoding="utf-8"?><d:multistatus xmlns:d="DAV:">)";
  ElementData current_element_data{
      .path = path, .name = directory.name, .is_directory = true};
  co_yield GetElement(current_element_data);
  if (coro::http::GetHeader(request.headers, "Depth") == "1") {
    FOR_CO_AWAIT(const auto& page, page_data) {
      for (const auto& item : page.items) {
        auto name = std::visit([](auto item) { return item.name; }, item);
        auto timestamp =
            std::visit([](const auto& item) { return item.timestamp; }, item);
        ElementData element_data(
            {.path = util::StrCat(path, http::EncodeUri(name)),
             .name = name,
             .is_directory = std::visit(
                 []<typename T>(const T& d) {
                   return std::is_same_v<T, AbstractCloudProvider::Directory>;
                 },
                 item),
             .timestamp = timestamp});
        std::visit(
            [&]<typename T>(const T& item) {
              if constexpr (std::is_same_v<T, AbstractCloudProvider::File>) {
                element_data.mime_type = item.mime_type;
                element_data.size = item.size;
              }
            },
            item);
        co_yield GetElement(element_data);
      }
    }
  }
  co_yield "</d:multistatus>";
}

template <typename Item>
Task<Response> HandleExistingItem(CloudProvider* provider, Request request,
                                  std::span<const std::string> path, Item d,
                                  stdx::stop_token stop_token) {
  if (request.method == http::Method::kProppatch) {
    co_return Response{.status = 207,
                       .headers = {{"Content-Type", "text/xml"}},
                       .body = GetWebDavItemResponse(GetPath(request), d)};
  } else if (request.method == http::Method::kDelete) {
    co_await provider->RemoveItem(d, std::move(stop_token));
    co_return Response{.status = 204};
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
      auto destination_directory = co_await GetItemByPathComponents(
          provider,
          std::vector<std::string>(destination_path.begin(),
                                   destination_path.end()),
          stop_token);
      auto item = co_await std::visit(
          MoveItemF<Item>{provider, std::move(d), std::move(stop_token)},
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
      auto item =
          co_await std::visit(RenameItemF{provider, std::move(destination_name),
                                          std::move(stop_token)},
                              std::move(new_item));
      if (!item) {
        co_return Response{.status = 501};
      }
    }
    co_return Response{.status = 201};
  } else if (request.method == http::Method::kPropfind) {
    if constexpr (std::is_same_v<Item, AbstractCloudProvider::Directory>) {
      std::string directory_path = GetPath(request);
      if (directory_path.empty() || directory_path.back() != '/') {
        directory_path += '/';
      }
      co_return Response{
          .status = 207,
          .headers = {{"Content-Type", "text/xml"}},
          .body = GetWebDavResponse(
              d, ListDirectory(provider, d, std::move(stop_token)),
              std::move(request), directory_path)};
    } else {
      co_return Response{.status = 207,
                         .headers = {{"Content-Type", "text/html"}},
                         .body = GetWebDavItemResponse(GetPath(request), d)};
    }
  } else {
    throw RuntimeError("unsupported method");
  }
}

}  // namespace

auto WebDAVHandler::operator()(Request request,
                               stdx::stop_token stop_token) const
    -> Task<Response> {
  auto uri = http::ParseUri(request.url);
  auto path = GetEffectivePath(uri.path.value());
  if (request.method == http::Method::kMkcol) {
    if (path.empty()) {
      throw CloudException("invalid path");
    }
    auto parent_path = GetDirectoryPath(path);
    co_return co_await std::visit(
        CreateDirectoryF{provider_, path.back(), stop_token},
        co_await GetItemByPathComponents(
            provider_,
            std::vector<std::string>(parent_path.begin(), parent_path.end()),
            stop_token));
  } else if (request.method == http::Method::kPut) {
    if (path.empty()) {
      throw CloudException("invalid path");
    }
    auto parent_path = GetDirectoryPath(path);
    co_return co_await std::visit(
        CreateFileF{provider_, path.back(), std::move(request), stop_token},
        co_await GetItemByPathComponents(
            provider_,
            std::vector<std::string>(parent_path.begin(), parent_path.end()),
            stop_token));
  } else {
    co_return co_await std::visit(
        [&](const auto& d) {
          return HandleExistingItem(provider_, std::move(request), path, d,
                                    stop_token);
        },
        co_await GetItemByPathComponents(provider_, path, stop_token));
  }
}

}  // namespace coro::cloudstorage::util