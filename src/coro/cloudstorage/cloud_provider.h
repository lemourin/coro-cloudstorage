#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H

#include <coro/http/http.h>
#include <coro/http/http_parse.h>

#include <iostream>

namespace coro::cloudstorage {

template <typename T>
concept CloudProviderImpl = requires(const T i, http::HttpStub& http,
                                     std::string string,
                                     stdx::stop_token stop_token) {
  { i.ExchangeAuthorizationCode(http, string, stop_token) }
  ->Awaitable;
};

class CloudStorageException : public std::exception {
 public:
  enum class Type { kNotFound };

  explicit CloudStorageException(Type type)
      : type_(type),
        message_(std::string("CloudStorageException: ") + TypeToString(type)) {}

  Type type() const { return type_; }
  const char* what() const noexcept final { return message_.c_str(); }

  static const char* TypeToString(Type type) {
    switch (type) {
      case Type::kNotFound:
        return "NotFound";
    }
  }

 private:
  Type type_;
  std::string message_;
};

template <CloudProviderImpl Impl, http::HttpClient HttpClient>
class CloudProvider : public Impl {
 public:
  using AuthToken = typename Impl::AuthToken;
  using Directory = typename Impl::Directory;
  using File = typename Impl::File;
  using Item = typename Impl::Item;
  using PageData = typename Impl::PageData;

  template <typename... Args>
  CloudProvider(AuthToken auth_token, HttpClient& http, Args&&... args)
      : Impl(std::forward<Args>(args)...),
        auth_token_(std::move(auth_token)),
        http_(http) {}

  auto GetGeneralData(stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::GetGeneralData(http_, auth_token_.access_token,
                                std::move(stop_token));
  }

  auto GetItem(std::string id,
               stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::GetItem(http_, std::move(auth_token_), std::move(id),
                         std::move(stop_token));
  }

  Task<Item> GetItemByPath(std::string path, stdx::stop_token stop_token =
                                                 stdx::stop_token()) const {
    co_return co_await GetItemByPath(co_await Impl::GetRoot(), std::move(path),
                                     std::move(stop_token));
  }

  auto GetFileContent(File file, http::Range range = http::Range{},
                      stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::GetFileContent(http_, auth_token_.access_token,
                                std::move(file), range, std::move(stop_token));
  }

  Generator<PageData> ListDirectory(
      Directory directory,
      stdx::stop_token stop_token = stdx::stop_token()) const {
    std::optional<std::string> current_page_token;
    do {
      auto page_data = co_await Impl::ListDirectoryPage(
          http_, auth_token_.access_token, directory,
          std::move(current_page_token), stop_token);
      co_yield page_data;
      current_page_token = page_data.next_page_token;
    } while (current_page_token);
  }

  auto ListDirectoryPage(
      HttpClient& http, std::string access_token, Directory directory,
      std::optional<std::string_view> page_token = std::nullopt,
      stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::ListDirectoryPage(http, std::move(access_token),
                                   std::move(directory), page_token,
                                   std::move(stop_token));
  }

  auto RefreshAccessToken(
      stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::RefreshAccessToken(http_, auth_token_.access_token,
                                    std::move(stop_token));
  }

 private:
  Task<Item> GetItemByPath(Directory current_directory, std::string path,
                           stdx::stop_token stop_token) const {
    if (path.empty() || path == "/") {
      co_return current_directory;
    }
    auto delimiter_index = path.find_first_of('/', 1);
    std::string path_component(path.begin() + 1,
                               delimiter_index == std::string::npos
                                   ? path.end()
                                   : path.begin() + delimiter_index);
    std::string rest_component(delimiter_index == std::string::npos
                                   ? path.end()
                                   : path.begin() + delimiter_index,
                               path.end());
    FOR_CO_AWAIT(const auto& page, ListDirectory(current_directory, stop_token),
                 {
                   for (const auto& item : page.items) {
                     if (std::holds_alternative<Directory>(item)) {
                       const auto& directory = std::get<Directory>(item);
                       if (directory.name == path_component) {
                         co_return co_await GetItemByPath(
                             directory, rest_component, stop_token);
                       }
                     } else if (rest_component.empty() &&
                                std::holds_alternative<File>(item)) {
                       const auto& file = std::get<File>(item);
                       if (file.name == path_component) {
                         co_return file;
                       }
                     }
                   }
                 });

    throw CloudStorageException(CloudStorageException::Type::kNotFound);
  }

  mutable AuthToken auth_token_;
  HttpClient& http_;
};

template <CloudProviderImpl Impl, http::HttpClient HttpClient, typename... Args>
CloudProvider<Impl, HttpClient> MakeCloudProvider(
    typename Impl::AuthToken auth_token, HttpClient& http, Args&&... args) {
  return CloudProvider<Impl, HttpClient>(std::move(auth_token), http,
                                         std::forward<Args>(args)...);
}

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
