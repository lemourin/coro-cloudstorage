#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H

#include <coro/cloudstorage/util/auth_manager.h>
#include <coro/cloudstorage/util/generator_utils.h>
#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/promise.h>
#include <coro/shared_promise.h>
#include <coro/stdx/stop_callback.h>
#include <coro/stdx/stop_source.h>
#include <coro/util/raii_utils.h>
#include <coro/util/stop_token_or.h>

namespace coro::cloudstorage {

struct FileContent {
  Generator<std::string> data;
  std::optional<int64_t> size;
};

template <typename T>
concept RandomAccessFileContent = requires(T v, int64_t offset, int64_t size,
                                           stdx::stop_token stop_token) {
  { v.size }
  ->stdx::convertible_to<int64_t>;
  { v(offset, size, stop_token) }
  ->Awaitable<std::string>;
};

template <typename T>
concept HasTimestamp = requires(T v) {
  { v.timestamp }
  ->stdx::convertible_to<std::optional<int64_t>>;
};

template <typename T>
concept HasSize = requires(T v) {
  { v.size }
  ->stdx::convertible_to<std::optional<int64_t>>;
};

template <typename T>
concept HasMimeType = requires(T v) {
  { v.mime_type }
  ->stdx::convertible_to<std::optional<std::string_view>>;
};

template <typename T, typename CloudProvider>
concept IsDirectory = requires(typename CloudProvider::Impl provider, T v,
                               std::optional<std::string> page_token,
                               stdx::stop_token stop_token) {
  { provider.ListDirectoryPage(v, page_token, stop_token) }
  ->Awaitable<typename CloudProvider::PageData>;
};

template <typename T, typename CloudProvider>
concept IsFile = requires(typename CloudProvider::Impl provider, T v,
                          http::Range range, stdx::stop_token stop_token) {
  { provider.GetFileContent(v, range, stop_token) }
  ->GeneratorLike<std::string_view>;
};

template <typename Parent, typename FileContent, typename CloudProvider>
concept CanCreateSmallFile = requires(
    typename CloudProvider::Impl provider, Parent parent, std::string_view name,
    FileContent content, stdx::stop_token stop_token,
    decltype(provider.CreateSmallFile(parent, name, std::move(content),
                                      stop_token)) item_promise,
    typename decltype(item_promise)::type item) {
  { item }
  ->IsFile<CloudProvider>;
};

template <typename Parent, typename FileContent, typename CloudProvider>
concept CanCreateUploadSession = requires(
    typename CloudProvider::Impl provider, Parent parent, std::string_view name,
    FileContent content, int64_t offset, stdx::stop_token stop_token,
    typename CloudProvider::Type::UploadSession session,
    decltype(provider.FinishUploadSession(session, std::move(content), offset,
                                          stop_token)) finish_promise,
    typename decltype(finish_promise)::type item) {
  { provider.CreateUploadSession(parent, name, std::move(content), stop_token) }
  ->Awaitable<typename CloudProvider::Type::UploadSession>;
  { provider.WriteChunk(session, std::move(content), offset, stop_token) }
  ->Awaitable<typename CloudProvider::Type::UploadSession>;
  { item }
  ->IsFile<CloudProvider>;
};

template <typename Parent, typename FileContent, typename CloudProvider>
concept CanCreateFile =
    CanCreateSmallFile<Parent, FileContent, CloudProvider> ||
    CanCreateUploadSession<Parent, FileContent, CloudProvider>;

template <typename T, typename CloudProvider>
concept CanRename = requires(
    typename CloudProvider::Impl provider, T v, std::string new_name,
    stdx::stop_token stop_token,
    decltype(provider.RenameItem(v, new_name, stop_token)) item_promise,
    typename decltype(item_promise)::type item) {
  { item }
  ->stdx::convertible_to<typename CloudProvider::Item>;
};

template <typename T, typename CloudProvider>
concept CanRemove = requires(typename CloudProvider::Impl provider, T v,
                             stdx::stop_token stop_token) {
  { provider.RemoveItem(v, stop_token) }
  ->Awaitable<void>;
};

template <typename Source, typename Destination, typename CloudProvider>
concept CanMove = requires(typename CloudProvider::Impl provider, Source source,
                           Destination destination, stdx::stop_token stop_token,
                           decltype(provider.MoveItem(source, destination,
                                                      stop_token)) item_promise,
                           typename decltype(item_promise)::type item) {
  { item }
  ->stdx::convertible_to<typename CloudProvider::Item>;
};

template <typename Parent, typename CloudProvider>
concept CanCreateDirectory = requires(
    typename CloudProvider::Impl provider, Parent v, std::string name,
    stdx::stop_token stop_token,
    decltype(provider.CreateDirectory(v, name, stop_token)) item_promise,
    typename decltype(item_promise)::type item) {
  { item }
  ->stdx::convertible_to<typename CloudProvider::Item>;
};

template <typename CloudProviderT, typename ImplT = CloudProviderT>
class CloudProvider {
 public:
  using Type = CloudProviderT;
  using Impl = ImplT;
  using Item = typename CloudProviderT::Item;
  using PageData = typename CloudProviderT::PageData;

  Task<Item> GetItemByPath(std::string path, stdx::stop_token stop_token) {
    co_return co_await Get()->GetItemByPath(co_await Get()->GetRoot(stop_token),
                                            std::move(path), stop_token);
  }

  template <typename Directory>
  Generator<PageData> ListDirectory(Directory directory,
                                    stdx::stop_token stop_token) {
    std::optional<std::string> current_page_token;
    do {
      auto page_data = co_await Get()->ListDirectoryPage(
          directory, std::move(current_page_token), stop_token);
      co_yield page_data;
      current_page_token = page_data.next_page_token;
    } while (current_page_token);
  }

  template <typename T>
  static std::string GetMimeType(const T& d) {
    static_assert(IsFile<T, CloudProvider>);
    if constexpr (HasMimeType<T>) {
      if constexpr (std::is_convertible_v<decltype(d.mime_type), std::string>) {
        return d.mime_type;
      } else if constexpr (std::is_constructible_v<std::string,
                                                   decltype(d.mime_type)>) {
        return std::string(d.mime_type);
      } else {
        return d.mime_type.value_or(
            coro::http::GetMimeType(coro::http::GetExtension(d.name)));
      }
    } else {
      return coro::http::GetMimeType(coro::http::GetExtension(d.name));
    }
  }

  template <typename T>
  static std::optional<int64_t> GetSize(const T& d) {
    if constexpr (HasSize<T>) {
      return d.size;
    } else {
      return std::nullopt;
    }
  }

  template <typename T>
  static std::optional<int64_t> GetTimestamp(const T& d) {
    if constexpr (HasTimestamp<T>) {
      return d.timestamp;
    } else {
      return std::nullopt;
    }
  }

  template <IsDirectory<CloudProvider> Directory, typename FileContent,
            typename T = CloudProvider>
  requires CanCreateFile<Directory, FileContent, T> auto CreateFile(
      Directory parent, std::string_view name, FileContent content,
      stdx::stop_token stop_token) {
    constexpr int kChunkSize = 1024 * 1024 * 150;
    if constexpr (CanCreateSmallFile<Directory, FileContent, T> &&
                  CanCreateUploadSession<Directory, FileContent, T>) {
      if (content.size <= kChunkSize) {
        return Get()->CreateSmallFile(
            std::move(parent), name, std::move(content), std::move(stop_token));
      } else {
        return CreateFileWithUploadSession(std::move(parent), name,
                                           std::move(content), kChunkSize,
                                           std::move(stop_token));
      }
    } else if constexpr (CanCreateSmallFile<Directory, FileContent, T>) {
      return Get()->CreateSmallFile(std::move(parent), name, std::move(content),
                                    std::move(stop_token));
    } else {
      static_assert(CanCreateUploadSession<Directory, FileContent, T>);
      return CreateFileWithUploadSession(std::move(parent), name,
                                         std::move(content), kChunkSize,
                                         std::move(stop_token));
    }
  }

 private:
  auto Get() { return static_cast<Impl*>(this); }

  template <IsDirectory<CloudProvider> Directory, typename T>
  using UploadSessionT =
      typename decltype(std::declval<typename T::Impl>().CreateUploadSession(
          std::declval<Directory>(), std::string_view(),
          std::declval<FileContent>(), stdx::stop_token()))::type;

  template <IsDirectory<CloudProvider> Directory, typename T>
  using NewFileT =
      typename decltype(std::declval<typename T::Impl>().FinishUploadSession(
          std::declval<UploadSessionT<Directory, T>>(),
          std::declval<FileContent>(), 0, stdx::stop_token()))::type;

  template <IsDirectory<CloudProvider> Directory, typename FileContent,
            typename T = CloudProvider>
  requires CanCreateUploadSession<Directory, FileContent, T> auto
  CreateFileWithUploadSession(Directory parent, std::string_view name,
                              FileContent content, int64_t upload_chunk_size,
                              stdx::stop_token stop_token)
      -> Task<NewFileT<Directory, T>> {
    int64_t offset = 0;
    std::optional<UploadSessionT<Directory, T>> session;
    auto it = co_await content.data.begin();
    while (true) {
      auto chunk_size = std::min<int64_t>(
          upload_chunk_size, content.size.value_or(INT64_MAX) - offset);
      FileContent chunk{.data = util::Take(it, chunk_size), .size = chunk_size};
      if (!session) {
        session = co_await Get()->CreateUploadSession(
            std::move(parent), name, std::move(chunk), stop_token);
      } else if (offset + chunk_size < content.size) {
        session = co_await Get()->WriteChunk(
            std::move(*session), std::move(chunk), offset, stop_token);
      } else {
        co_return co_await Get()->FinishUploadSession(std::move(*session),
                                                      std::move(chunk), offset,
                                                      std::move(stop_token));
      }
      offset += chunk_size;
    }
  }

  template <typename Directory>
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
    FOR_CO_AWAIT(auto& page, ListDirectory(current_directory, stop_token)) {
      for (auto& item : page.items) {
        auto r = std::visit(
            [&](auto& d) -> std::variant<std::monostate, Task<Item>, Item> {
              if constexpr (IsDirectory<decltype(d), CloudProvider>) {
                if (d.name == path_component) {
                  return GetItemByPath(d, rest_component, stop_token);
                }
              } else {
                if (d.name == path_component) {
                  return std::move(d);
                }
              }
              return std::monostate();
            },
            item);
        if (std::holds_alternative<Task<Item>>(r)) {
          co_return co_await std::get<Task<Item>>(r);
        } else if (std::holds_alternative<Item>(r)) {
          co_return std::move(std::get<Item>(r));
        }
      }
    }

    throw CloudException(CloudException::Type::kNotFound);
  }
};

template <typename CloudProvider>
struct CreateCloudProvider {
  template <typename CloudFactory, typename OnTokenUpdated = void (*)(
                                       typename CloudProvider::Auth::AuthToken)>
  auto operator()(
      const CloudFactory& factory,
      typename CloudProvider::Auth::AuthToken auth_token,
      OnTokenUpdated on_token_updated =
          [](typename CloudProvider::Auth::AuthToken) {}) const {
    util::AuthManager<std::remove_pointer_t<decltype(factory.http_)>,
                      typename CloudProvider::Auth, OnTokenUpdated>
        auth_manager(*factory.http_, std::move(auth_token),
                     factory.auth_data_.template operator()<CloudProvider>(),
                     std::move(on_token_updated));
    using Impl =
        typename CloudProvider::template CloudProvider<decltype(auth_manager)>;
    return Impl(std::move(auth_manager));
  }
};

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
