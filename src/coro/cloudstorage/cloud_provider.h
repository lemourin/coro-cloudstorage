#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H

#include <coro/http/http.h>
#include <coro/http/http_parse.h>

#include <iostream>

namespace coro::cloudstorage {

template <typename T>
concept CloudProviderImpl = requires(http::HttpStub& http, std::string code,
                                     typename T::AuthData auth_data,
                                     stdx::stop_token stop_token) {
  { T::ExchangeAuthorizationCode(http, auth_data, code, stop_token) }
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
class CloudProvider {
 public:
  using AuthToken = typename Impl::AuthToken;
  using AuthData = typename Impl::AuthData;
  using Directory = typename Impl::Directory;
  using File = typename Impl::File;
  using Item = typename Impl::Item;
  using PageData = typename Impl::PageData;

  CloudProvider(HttpClient& http, AuthToken auth_token, AuthData auth_data)
      : http_(http),
        auth_token_(std::move(auth_token)),
        auth_data_(std::move(auth_data)) {}

  auto GetGeneralData(stdx::stop_token stop_token = stdx::stop_token()) {
    return Impl::GetGeneralData(http_, auth_token_.access_token,
                                std::move(stop_token));
  }

  auto GetItem(std::string id,
               stdx::stop_token stop_token = stdx::stop_token()) {
    return Impl::GetItem(http_, std::move(auth_token_), std::move(id),
                         std::move(stop_token));
  }

  Task<Item> GetItemByPath(std::string path,
                           stdx::stop_token stop_token = stdx::stop_token()) {
    co_return co_await GetItemByPath(co_await Impl::GetRoot(), std::move(path),
                                     std::move(stop_token));
  }

  auto GetFileContent(File file, http::Range range = http::Range{},
                      stdx::stop_token stop_token = stdx::stop_token()) {
    return Impl::GetFileContent(http_, auth_token_.access_token,
                                std::move(file), range, std::move(stop_token));
  }

  Generator<PageData> ListDirectory(
      Directory directory, stdx::stop_token stop_token = stdx::stop_token()) {
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
      stdx::stop_token stop_token = stdx::stop_token()) {
    return Impl::ListDirectoryPage(http, std::move(access_token),
                                   std::move(directory), page_token,
                                   std::move(stop_token));
  }

  auto RefreshAccessToken(
      stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::RefreshAccessToken(http_, auth_data_, auth_token_.access_token,
                                    std::move(stop_token));
  }

  const AuthToken& GetAuthToken() const { return auth_token_; }

 private:
  Task<Item> GetItemByPath(Directory current_directory, std::string path,
                           stdx::stop_token stop_token) {
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

  HttpClient& http_;
  AuthToken auth_token_;
  AuthData auth_data_;
};

template <CloudProviderImpl Impl, http::HttpClient HttpClient>
CloudProvider<Impl, HttpClient> MakeCloudProvider(
    HttpClient& http, typename Impl::AuthToken auth_token,
    typename Impl::AuthData auth_data) {
  return CloudProvider<Impl, HttpClient>(http, std::move(auth_token),
                                         std::move(auth_data));
}

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
