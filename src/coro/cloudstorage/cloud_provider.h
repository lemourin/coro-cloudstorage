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

template <CloudProviderImpl Impl>
class CloudProvider : public Impl {
 public:
  using Impl::Impl;
  using Directory = typename Impl::Directory;
  using File = typename Impl::File;
  using Item = typename Impl::Item;

  template <http::HttpClient HttpClient>
  auto GetGeneralData(HttpClient& http, std::string access_token,
                      stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::GetGeneralData(http, std::move(access_token),
                                std::move(stop_token));
  }

  template <http::HttpClient HttpClient>
  auto GetItem(HttpClient& http, std::string access_token, std::string id,
               stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::GetItem(http, std::move(access_token), std::move(id),
                         std::move(stop_token));
  }

  template <http::HttpClient HttpClient>
  Task<Item> GetItemByPath(
      HttpClient& http, std::string access_token, std::string path,
      stdx::stop_token stop_token = stdx::stop_token()) const {
    co_return co_await GetItemByPath(http, std::move(access_token),
                                     co_await Impl::GetRoot(), std::move(path),
                                     std::move(stop_token));
  }

  template <http::HttpClient HttpClient>
  auto GetFileContent(HttpClient& http, std::string access_token, File file,
                      http::Range range = http::Range{},
                      stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::GetFileContent(http, std::move(access_token), std::move(file),
                                range, std::move(stop_token));
  }

  template <http::HttpClient HttpClient>
  Generator<typename Impl::PageData> ListDirectory(
      HttpClient& http, std::string access_token, Directory directory,
      stdx::stop_token stop_token = stdx::stop_token()) const {
    std::optional<std::string> current_page_token;
    do {
      auto page_data = co_await Impl::ListDirectoryPage(
          http, access_token, directory, std::move(current_page_token),
          stop_token);
      co_yield page_data;
      current_page_token = page_data.next_page_token;
    } while (current_page_token);
  }

  template <http::HttpClient HttpClient>
  auto ListDirectoryPage(
      HttpClient& http, std::string access_token, Directory directory,
      std::optional<std::string_view> page_token = std::nullopt,
      stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::ListDirectoryPage(http, std::move(access_token),
                                   std::move(directory), page_token,
                                   std::move(stop_token));
  }

  template <http::HttpClient HttpClient>
  auto ExchangeAuthorizationCode(
      HttpClient& http, std::string code,
      stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::ExchangeAuthorizationCode(http, std::move(code),
                                           std::move(stop_token));
  }

  template <http::HttpClient HttpClient>
  auto RefreshAccessToken(
      HttpClient& http, std::string refresh_token,
      stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::RefreshAccessToken(http, std::move(refresh_token),
                                    std::move(stop_token));
  }

 private:
  template <http::HttpClient HttpClient>
  Task<Item> GetItemByPath(
      HttpClient& http, std::string access_token, Directory current_directory,
      std::string path,
      stdx::stop_token stop_token = stdx::stop_token()) const {
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
    FOR_CO_AWAIT(
        const auto& page,
        ListDirectory(http, access_token, current_directory, stop_token), {
          for (const auto& item : page.items) {
            if (std::holds_alternative<Directory>(item)) {
              const auto& directory = std::get<Directory>(item);
              if (directory.name == path_component) {
                co_return co_await GetItemByPath(http, access_token, directory,
                                                 rest_component);
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
};

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
