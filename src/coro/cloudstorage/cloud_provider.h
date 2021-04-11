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

template <typename Parent, typename CloudProvider>
concept CanCreateFile = requires(
    typename CloudProvider::Impl provider, Parent parent, std::string_view name,
    typename CloudProvider::FileContent content, stdx::stop_token stop_token,
    decltype(provider.CreateFile(parent, name, std::move(content),
                                 stop_token)) item_promise,
    typename decltype(item_promise)::type item) {
  { item }
  ->IsFile<CloudProvider>;
};

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

template <typename Item, typename CloudProvider>
concept HasThumbnail = requires(
    typename CloudProvider::Impl provider, Item v, http::Range range,
    stdx::stop_token stop_token,
    decltype(provider.GetItemThumbnail(v, range, stop_token)) thumbnail_promise,
    typename decltype(thumbnail_promise)::type thumbnail) {
  { std::declval<decltype(thumbnail)>() }
  ->stdx::convertible_to<typename CloudProvider::Type::Thumbnail>;
};

enum class FileType { kUnknown, kVideo, kAudio, kImage };

template <typename CloudProviderT, typename ImplT = CloudProviderT>
class CloudProvider {
 public:
  using Type = CloudProviderT;
  using Impl = ImplT;
  using Item = typename CloudProviderT::Item;
  using PageData = typename CloudProviderT::PageData;
  using FileContent = typename CloudProviderT::FileContent;

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

  template <IsFile<CloudProvider> T>
  static FileType GetFileType(const T& d) {
    auto mime_type = GetMimeType(d);
    if (mime_type.find("audio") == 0) {
      return FileType::kAudio;
    } else if (mime_type.find("image") == 0) {
      return FileType::kImage;
    } else if (mime_type.find("video") == 0) {
      return FileType::kVideo;
    } else {
      return FileType::kUnknown;
    }
  }

 private:
  auto Get() { return static_cast<Impl*>(this); }

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
