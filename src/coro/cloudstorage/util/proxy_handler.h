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
  using File = typename CloudProvider::File;
  using Item = typename CloudProvider::Item;
  using Directory = typename CloudProvider::Directory;
  using Request = http::Request<>;
  using Response = http::Response<>;

  ProxyHandler(CloudProvider* provider, std::string path_prefix)
      : provider_(provider),
        path_prefix_(std::move(path_prefix)),
        item_cache_(32, GetItem{provider_}) {}

  ProxyHandler(ProxyHandler&& handler) = default;

  Task<Response> operator()(Request request,
                            coro::stdx::stop_token stop_token) {
    std::string path = GetEffectivePath(
        http::DecodeUri(http::ParseUri(request.url).path.value_or(""))
            .substr(path_prefix_.length()));
    if (path.empty() || path.front() != '/') {
      path = '/' + path;
    }
    auto item = co_await item_cache_.Get(path, stop_token);
    if (std::holds_alternative<File>(item)) {
      auto file = std::get<File>(item);
      std::vector<std::pair<std::string, std::string>> headers = {
          {"Content-Type", file.mime_type.value_or(coro::http::GetMimeType(
                               coro::http::GetExtension(file.name)))},
          {"Content-Disposition", "inline; filename=\"" + file.name + "\""},
          {"Access-Control-Allow-Origin", "*"},
          {"Access-Control-Allow-Headers", "*"}};
      auto range_str = coro::http::GetHeader(request.headers, "Range");
      coro::http::Range range =
          coro::http::ParseRange(range_str.value_or("bytes=0-"));
      if (file.size) {
        if (!range.end) {
          range.end = *file.size - 1;
        }
        headers.emplace_back("Accept-Ranges", "bytes");
        headers.emplace_back("Content-Length",
                             std::to_string(*range.end - range.start + 1));
        if (range_str) {
          std::stringstream stream;
          stream << "bytes " << range.start << "-" << *range.end << "/"
                 << *file.size;
          headers.emplace_back("Content-Range", std::move(stream).str());
        }
      }
      co_return Response{
          .status = !range_str || !file.size ? 200 : 206,
          .headers = std::move(headers),
          .body = provider_->GetFileContent(file, range, stop_token)};
    } else {
      auto directory = std::get<Directory>(item);
      if (!path.empty() && path.back() != '/') {
        path += '/';
      }
      if (request.method == http::Method::kPropfind) {
        co_return Response{
            .status = 207,
            .headers = {{"Content-Type", "text/xml"}},
            .body = GetWebDavResponse(
                directory,
                provider_->ListDirectory(directory, std::move(stop_token)),
                std::move(request), path_prefix_ + path)};
      } else {
        co_return Response{
            .status = 200,
            .headers = {{"Content-Type", "text/html"}},
            .body = GetDirectoryContent(
                provider_->ListDirectory(directory, std::move(stop_token)),
                path_prefix_ + path)};
      }
    }
  }

  static Generator<std::string> GetWebDavResponse(
      Directory directory,
      Generator<typename CloudProvider::PageData> page_data, Request request,
      std::string path) {
    co_yield R"(<?xml version="1.0" encoding="utf-8"?><d:multistatus xmlns:d="DAV:">)";
    ElementData current_element_data{
        .path = path, .name = directory.name, .is_directory = true};
    co_yield GetElement(std::move(current_element_data));
    if (::coro::http::GetHeader(request.headers, "Depth") == "1") {
      FOR_CO_AWAIT(const auto& page, page_data, {
        for (const auto& item : page.items) {
          auto name = std::visit([](auto item) { return item.name; }, item);
          auto timestamp = std::visit(
              [](auto item) -> std::optional<int64_t> {
                if constexpr (HasTimestamp<decltype(item)>) {
                  return item.timestamp;
                } else {
                  return std::nullopt;
                }
              },
              item);
          ElementData element_data(
              {.path = path + name,
               .name = name,
               .is_directory = std::holds_alternative<Directory>(item),
               .timestamp = timestamp});
          if (std::holds_alternative<File>(item)) {
            const File& file = std::get<File>(item);
            element_data.mime_type = file.mime_type.value_or(
                coro::http::GetMimeType(coro::http::GetExtension(file.name)));
            element_data.size = file.size;
          }
          co_yield GetElement(std::move(element_data));
        }
      });
    }
    co_yield "</d:multistatus>";
  }

  static Generator<std::string> GetDirectoryContent(
      Generator<typename CloudProvider::PageData> page_data, std::string path) {
    co_yield R"(<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body><table><tr><td>[DIR]</td><td><a href=')" +
        GetDirectoryPath(path) + "'>..</a></td></tr>";
    FOR_CO_AWAIT(const auto& page, page_data, {
      for (const auto& item : page.items) {
        auto name = std::visit([](auto item) { return item.name; }, item);
        std::string type =
            std::holds_alternative<Directory>(item) ? "DIR" : "FILE";
        std::string file_link = coro::http::EncodeUriPath(path + name);
        if (name.ends_with(".mpd")) {
          file_link = "/dash" + file_link;
        }
        co_yield "<tr><td>[" + type + "]</td><td><a href='" + file_link + "'>" +
            name + "</a></td></tr>";
      }
    });
    co_yield "</table></body></html>";
  }

 private:
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

  struct GetItem {
    Task<Item> operator()(std::string path, stdx::stop_token stop_token) {
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
