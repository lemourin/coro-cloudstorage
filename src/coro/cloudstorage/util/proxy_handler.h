#ifndef CORO_CLOUDSTORAGE_PROXY_HANDLER_H
#define CORO_CLOUDSTORAGE_PROXY_HANDLER_H

#include <coro/http/http_parse.h>
#include <coro/util/lru_cache.h>

namespace coro::cloudstorage::util {

template <typename CloudProvider>
class ProxyHandler {
 public:
  using File = typename CloudProvider::File;
  using Item = typename CloudProvider::Item;
  using Directory = typename CloudProvider::Directory;

  ProxyHandler(CloudProvider provider, std::string path_prefix)
      : provider_(std::make_unique<CloudProvider>(std::move(provider))),
        path_prefix_(std::move(path_prefix)),
        item_cache_(32, GetItem{provider_.get()}) {}

  ProxyHandler(ProxyHandler&& handler) = default;

  Task<http::Response<>> operator()(const coro::http::Request<>& request,
                                    coro::stdx::stop_token stop_token) {
    std::cerr << "[" << request.method << "] " << request.url << " ";
    std::string path =
        http::DecodeUri(http::ParseUri(request.url).path.value_or(""))
            .substr(path_prefix_.length());
    auto range_str = coro::http::GetHeader(request.headers, "Range");
    coro::http::Range range = {};
    if (range_str) {
      range = http::ParseRange(*range_str);
      std::cerr << *range_str;
    }
    std::cerr << "\n";
    auto item = co_await item_cache_.Get(path, stop_token);
    if (std::holds_alternative<File>(item)) {
      auto file = std::get<File>(item);
      std::vector<std::pair<std::string, std::string>> headers = {
          {"Content-Type", file.mime_type.value_or(coro::http::GetMimeType(
                               coro::http::GetExtension(file.name)))},
          {"Content-Disposition", "inline; filename=\"" + file.name + "\""},
          {"Access-Control-Allow-Origin", "*"},
          {"Access-Control-Allow-Headers", "*"}};
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
      co_return http::Response<>{
          .status = !range_str || !file.size ? 200 : 206,
          .headers = std::move(headers),
          .body = provider_->GetFileContent(file, range, stop_token)};
    } else {
      auto directory = std::get<Directory>(item);
      co_return http::Response<>{
          .status = 200,
          .headers = {{"Content-Type", "text/html"}},
          .body = GetDirectoryContent(path, directory, stop_token)};
    }
  }

  Generator<std::string> GetDirectoryContent(
      std::string path, Directory directory,
      coro::stdx::stop_token stop_token) {
    co_yield "<!DOCTYPE html>"
             "<html><head><meta charset='UTF-8'></head><body><table>";
    if (!path.empty() && path.back() != '/') {
      path += '/';
    }
    co_yield "<tr><td>[DIR]</td><td><a href='" +
        GetDirectoryPath(path_prefix_ + path) + "'>..</a></td></tr>";
    FOR_CO_AWAIT(
        const auto& page, provider_->ListDirectory(directory, stop_token), {
          for (const auto& item : page.items) {
            auto name = std::visit([](auto item) { return item.name; }, item);
            std::string type =
                std::holds_alternative<Directory>(item) ? "DIR" : "FILE";
            co_yield "<tr><td>[" + type + "]</td><td><a href='" + path_prefix_ +
                path + coro::http::EncodeUri(name) + "'>" + name +
                "</a></td></tr>";
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

  struct GetItem {
    Task<Item> operator()(std::string path, stdx::stop_token stop_token) {
      return provider->GetItemByPath(std::move(path), stop_token);
    }
    CloudProvider* provider;
  };

  std::unique_ptr<CloudProvider> provider_;
  std::string path_prefix_;
  ::coro::util::LRUCache<std::string, GetItem> item_cache_;
};

template <typename CloudProvider>
auto MakeProxyHandler(CloudProvider cloud_provider, std::string path_prefix) {
  return ProxyHandler<CloudProvider>(std::move(cloud_provider),
                                     std::move(path_prefix));
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_PROXY_HANDLER_H
