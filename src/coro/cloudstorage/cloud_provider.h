#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H

#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/semaphore.h>
#include <coro/stdx/stop_callback.h>
#include <coro/stdx/stop_source.h>
#include <coro/util/make_pointer.h>

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

template <CloudProviderImpl Impl, http::HttpClient HttpClient,
          typename OnAuthTokenUpdated>
class CloudProvider {
 public:
  using AuthToken = typename Impl::AuthToken;
  using AuthData = typename Impl::AuthData;
  using Directory = typename Impl::Directory;
  using File = typename Impl::File;
  using Item = typename Impl::Item;
  using PageData = typename Impl::PageData;

  CloudProvider(HttpClient& http, AuthToken auth_token, AuthData auth_data,
                OnAuthTokenUpdated on_auth_token_updated)
      : http_(http),
        shared_data_(std::make_shared<SharedData>(
            SharedData{.auth_token = std::move(auth_token)})),
        auth_data_(std::move(auth_data)),
        on_auth_token_updated_(std::move(on_auth_token_updated)) {}

  ~CloudProvider() { stop_source_.request_stop(); }

  CloudProvider(const CloudProvider&) = delete;
  CloudProvider(CloudProvider&&) = delete;
  CloudProvider& operator=(const CloudProvider&) = delete;
  CloudProvider& operator=(CloudProvider&&) = delete;

  auto GetGeneralData(stdx::stop_token stop_token = stdx::stop_token()) {
    return RefreshAuthTokenIfNeeded(
        [this, stop_token] {
          return Impl::GetGeneralData(
              http_, shared_data_->auth_token.access_token, stop_token);
        },
        stop_token);
  }

  auto GetItem(std::string id,
               stdx::stop_token stop_token = stdx::stop_token()) {
    return RefreshAuthTokenIfNeeded(
        [this, id, stop_token] {
          return Impl::GetItem(http_, shared_data_->auth_token, id, stop_token);
        },
        stop_token);
  }

  Task<Item> GetItemByPath(std::string path,
                           stdx::stop_token stop_token = stdx::stop_token()) {
    co_return co_await GetItemByPath(co_await Impl::GetRoot(), std::move(path),
                                     std::move(stop_token));
  }

  auto GetFileContent(File file, http::Range range = http::Range{},
                      stdx::stop_token stop_token = stdx::stop_token()) {
    return RefreshAuthTokenIfNeeded(
        [this, file, range, stop_token] {
          return Impl::GetFileContent(http_,
                                      shared_data_->auth_token.access_token,
                                      file, range, stop_token);
        },
        stop_token);
  }

  Generator<PageData> ListDirectory(
      Directory directory, stdx::stop_token stop_token = stdx::stop_token()) {
    std::optional<std::string> current_page_token;
    do {
      auto page_data = co_await ListDirectoryPage(
          directory, std::move(current_page_token), stop_token);
      co_yield page_data;
      current_page_token = page_data.next_page_token;
    } while (current_page_token);
  }

  auto ListDirectoryPage(Directory directory,
                         std::optional<std::string> page_token = std::nullopt,
                         stdx::stop_token stop_token = stdx::stop_token()) {
    return RefreshAuthTokenIfNeeded(
        [this, directory, page_token, stop_token] {
          return Impl::ListDirectoryPage(http_,
                                         shared_data_->auth_token.access_token,
                                         directory, page_token, stop_token);
        },
        stop_token);
  }

  auto RefreshAccessToken(
      stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::RefreshAccessToken(http_, auth_data_,
                                    shared_data_->auth_token.refresh_token,
                                    std::move(stop_token));
  }

  const AuthToken& GetAuthToken() const { return shared_data_->auth_token; }

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

  template <typename Operation>
  requires requires(Operation op) {
    op().begin();
  }
  auto RefreshAuthTokenIfNeeded(Operation operation,
                                stdx::stop_token stop_token)
      -> decltype(operation()) {
    bool success = false;
    try {
      auto generator = operation();
      auto begin = co_await generator.begin();
      success = true;
      auto end = generator.end();
      auto it = begin;
      while (it != end) {
        co_yield* it;
        co_await ++it;
      }
    } catch (const http::HttpException& exception) {
      if (exception.status() != 401 || success) {
        throw;
      }
    }
    co_await RefreshAuthToken(std::move(stop_token));
    FOR_CO_AWAIT(auto chunk, operation(), { co_yield chunk; });
  }

  template <typename Operation>
  auto RefreshAuthTokenIfNeeded(Operation operation,
                                stdx::stop_token stop_token)
      -> decltype(operation()) {
    try {
      co_return co_await operation();
    } catch (const http::HttpException& exception) {
      if (exception.status() != 401) {
        throw;
      }
    }
    co_await RefreshAuthToken(std::move(stop_token));
    co_return co_await operation();
  }

  Task<> RefreshAuthToken(stdx::stop_token stop_token) {
    Semaphore semaphore;
    shared_data_->semaphore.insert(&semaphore);
    auto scope_guard = util::MakePointer(
        &semaphore, [shared_data = shared_data_](Semaphore* semaphore) {
          shared_data->semaphore.erase(semaphore);
        });
    stdx::stop_callback stop_callback(stop_token, [&] { semaphore.resume(); });
    if (!shared_data_->pending_auth_token_refresh) {
      shared_data_->pending_auth_token_refresh = true;
      [this]() -> Task<> {
        auto shared_data = shared_data_;
        auto stop_token = stop_source_.get_token();
        shared_data->auth_token = co_await RefreshAccessToken(stop_token);
        shared_data->pending_auth_token_refresh = false;
        if (!stop_token.stop_requested()) {
          on_auth_token_updated_(shared_data->auth_token);
        }
        while (!shared_data->semaphore.empty()) {
          (*shared_data->semaphore.begin())->resume();
        }
      }();
    }
    co_await semaphore;
    if (stop_token.stop_requested()) {
      throw http::HttpException(http::HttpException::kAborted);
    }
  }

  struct SharedData {
    bool pending_auth_token_refresh = false;
    std::unordered_set<Semaphore*> semaphore;
    AuthToken auth_token;
  };

  HttpClient& http_;
  std::shared_ptr<SharedData> shared_data_;
  AuthData auth_data_;
  stdx::stop_source stop_source_;
  OnAuthTokenUpdated on_auth_token_updated_;
};

template <CloudProviderImpl Impl, http::HttpClient HttpClient,
          typename OnAuthTokenUpdated>
CloudProvider<Impl, HttpClient, OnAuthTokenUpdated> MakeCloudProvider(
    HttpClient& http, typename Impl::AuthToken auth_token,
    typename Impl::AuthData auth_data,
    OnAuthTokenUpdated on_auth_token_updated) {
  return CloudProvider<Impl, HttpClient, OnAuthTokenUpdated>(
      http, std::move(auth_token), std::move(auth_data),
      std::move(on_auth_token_updated));
}

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
