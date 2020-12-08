#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H

#include <coro/cloudstorage/util/auth_manager.h>
#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/promise.h>
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
  enum class Type { kNotFound, kUnknown };

  explicit CloudStorageException(std::string message)
      : type_(Type::kUnknown), message_(std::move(message)) {}

  explicit CloudStorageException(Type type)
      : type_(type),
        message_(std::string("CloudStorageException: ") + TypeToString(type)) {}

  Type type() const { return type_; }
  const char* what() const noexcept final { return message_.c_str(); }

  static const char* TypeToString(Type type) {
    switch (type) {
      case Type::kNotFound:
        return "NotFound";
      default:
        return "Unknown";
    }
  }

 private:
  Type type_;
  std::string message_;
};

template <typename Impl>
class CloudProvider {
 public:
  using Directory = typename Impl::Directory;
  using File = typename Impl::File;
  using Item = typename Impl::Item;
  using PageData = typename Impl::PageData;

  explicit CloudProvider(Impl impl) : impl_(std::move(impl)) {}

  auto GetRoot(stdx::stop_token stop_token = stdx::stop_token()) {
    return impl_.GetRoot(std::move(stop_token));
  }

  auto GetGeneralData(stdx::stop_token stop_token = stdx::stop_token()) {
    return impl_.GetGeneralData(std::move(stop_token));
  }

  auto GetItem(std::string id,
               stdx::stop_token stop_token = stdx::stop_token()) {
    return impl_.GetItem(id, std::move(stop_token));
  }

  Task<Item> GetItemByPath(std::string path,
                           stdx::stop_token stop_token = stdx::stop_token()) {
    co_return co_await GetItemByPath(co_await impl_.GetRoot(stop_token),
                                     std::move(path), stop_token);
  }

  auto GetFileContent(File file, http::Range range = http::Range{},
                      stdx::stop_token stop_token = stdx::stop_token()) {
    return impl_.GetFileContent(file, range, std::move(stop_token));
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
    return impl_.ListDirectoryPage(std::move(directory), std::move(page_token),
                                   std::move(stop_token));
  }

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

  Impl impl_;
};

template <typename CloudProviderImpl, typename... Args>
auto MakeCloudProvider(Args&&... args) {
  auto auth_manager = util::MakeAuthManager<typename CloudProviderImpl::Auth>(
      std::forward<Args>(args)...);
  using InternalImpl =
      typename CloudProviderImpl::template Impl<decltype(auth_manager)>;
  return CloudProvider<InternalImpl>(InternalImpl(std::move(auth_manager)));
}

template <typename CloudProviderImpl>
auto MakeCloudProvider(CloudProviderImpl impl) {
  return CloudProvider<CloudProviderImpl>(std::move(impl));
}

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
