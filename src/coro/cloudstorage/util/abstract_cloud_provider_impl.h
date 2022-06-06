#ifndef CORO_CLOUDSTORAGE_ABSTRACT_CLOUD_PROVIDER_IMPL_H
#define CORO_CLOUDSTORAGE_ABSTRACT_CLOUD_PROVIDER_IMPL_H

#include "coro/cloudstorage/util/abstract_cloud_provider.h"

namespace coro::cloudstorage::util {

template <typename T, typename CloudProvider>
concept IsFile = requires(CloudProvider& provider, T v, http::Range range,
                          stdx::stop_token stop_token) {
                   {
                     provider.GetFileContent(v, range, stop_token)
                     } -> GeneratorLike<std::string_view>;
                 };

template <typename T, typename CloudProvider>
concept IsDirectory = requires(CloudProvider& provider, T v,
                               std::optional<std::string> page_token,
                               stdx::stop_token stop_token) {
                        {
                          provider.ListDirectoryPage(v, page_token, stop_token)
                          } -> Awaitable<typename CloudProvider::PageData>;
                      };

template <typename Parent, typename CloudProvider>
concept CanCreateFile = requires(
    CloudProvider& provider, Parent parent, std::string_view name,
    typename CloudProvider::FileContent content, stdx::stop_token stop_token,
    decltype(provider.CreateFile(parent, name, std::move(content),
                                 stop_token)) item_promise,
    typename decltype(item_promise)::type item) {
                          { item } -> IsFile<CloudProvider>;
                        };

template <typename T, typename CloudProvider>
concept CanRename = requires(
    CloudProvider& provider, T v, std::string new_name,
    stdx::stop_token stop_token,
    decltype(provider.RenameItem(v, new_name, stop_token)) item_promise,
    typename decltype(item_promise)::type item) {
                      {
                        item
                        } -> stdx::convertible_to<typename CloudProvider::Item>;
                    };

template <typename T, typename CloudProvider>
concept CanRemove =
    requires(CloudProvider& provider, T v, stdx::stop_token stop_token) {
      { provider.RemoveItem(v, stop_token) } -> Awaitable<void>;
    };

template <typename Source, typename Destination, typename CloudProvider>
concept CanMove = requires(
    CloudProvider& provider, Source source, Destination destination,
    stdx::stop_token stop_token,
    decltype(provider.MoveItem(source, destination, stop_token)) item_promise,
    typename decltype(item_promise)::type item) {
                    {
                      item
                      } -> stdx::convertible_to<typename CloudProvider::Item>;
                  };

template <typename Parent, typename CloudProvider>
concept CanCreateDirectory =
    requires(
        CloudProvider& provider, Parent v, std::string name,
        stdx::stop_token stop_token,
        decltype(provider.CreateDirectory(v, name, stop_token)) item_promise,
        typename decltype(item_promise)::type item) {
      { item } -> stdx::convertible_to<typename CloudProvider::Item>;
    };

template <typename Item, typename CloudProvider>
concept HasThumbnail =
    requires(CloudProvider& provider, Item v, http::Range range,
             stdx::stop_token stop_token,
             decltype(provider.GetItemThumbnail(v, range,
                                                stop_token)) thumbnail_promise,
             typename decltype(thumbnail_promise)::type thumbnail) {
      {
        std::declval<decltype(thumbnail)>()
        } -> stdx::convertible_to<typename CloudProvider::Thumbnail>;
    };

template <typename Item, typename CloudProvider>
concept HasQualityThumbnail =
    requires(CloudProvider& provider, Item v, ThumbnailQuality quality,
             http::Range range, stdx::stop_token stop_token,
             decltype(provider.GetItemThumbnail(v, quality, range,
                                                stop_token)) thumbnail_promise,
             typename decltype(thumbnail_promise)::type thumbnail) {
      {
        std::declval<decltype(thumbnail)>()
        } -> stdx::convertible_to<typename CloudProvider::Thumbnail>;
    };

template <typename Directory, typename CloudProvider>
concept HasIsFileContentSizeRequired =
    requires(CloudProvider& provider, const Directory& d) {
      { provider.IsFileContentSizeRequired(d) } -> stdx::convertible_to<bool>;
    };

template <typename T>
concept HasMimeType =
    requires(T v) {
      { v.mime_type } -> stdx::convertible_to<std::optional<std::string_view>>;
    };

template <typename T>
concept HasSize = requires(T v) {
                    { v.size } -> stdx::convertible_to<std::optional<int64_t>>;
                  };

template <typename T>
concept HasTimestamp = requires(T v) {
                         {
                           v.timestamp
                           } -> stdx::convertible_to<std::optional<int64_t>>;
                       };

template <typename T>
concept HasUsageData = requires(T v) {
                         {
                           v.space_used
                           } -> stdx::convertible_to<std::optional<int64_t>>;
                         {
                           v.space_total
                           } -> stdx::convertible_to<std::optional<int64_t>>;
                       };

template <typename CloudProviderT>
class AbstractCloudProviderImplNonOwningSupplier {
 public:
  explicit AbstractCloudProviderImplNonOwningSupplier(CloudProviderT* impl)
      : impl_(impl) {}

  const auto* provider() const { return impl_; }
  auto* provider() { return impl_; }

 private:
  CloudProviderT* impl_;
};

template <typename CloudProviderT>
class AbstractCloudProviderImplOwningSupplier {
 public:
  explicit AbstractCloudProviderImplOwningSupplier(CloudProviderT impl)
      : impl_(std::move(impl)) {}

  const auto* provider() const { return &impl_; }
  auto* provider() { return &impl_; }

 private:
  CloudProviderT impl_;
};

template <typename CloudProviderT, typename CloudProviderSupplier>
class AbstractCloudProviderImpl : public AbstractCloudProvider,
                                  public CloudProviderSupplier {
 public:
  template <typename... Args>
  explicit AbstractCloudProviderImpl(Args&&... args)
      : CloudProviderSupplier(std::forward<Args>(args)...) {}

  std::string_view GetId() const override { return CloudProviderT::kId; }

  Task<Directory> GetRoot(stdx::stop_token stop_token) const override {
    co_return Convert(co_await provider()->GetRoot(std::move(stop_token)));
  }

  bool IsFileContentSizeRequired(const Directory& d) const override {
    return std::visit(IsFileContentSizeRequiredF{provider()},
                      std::any_cast<const ItemT&>(d.impl));
  }

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) const override {
    co_return co_await std::visit(
        [&]<typename DirectoryT>(DirectoryT directory) -> Task<PageData> {
          if constexpr (IsDirectory<DirectoryT, CloudProviderT>) {
            auto page = co_await provider()->ListDirectoryPage(
                directory, std::move(page_token), std::move(stop_token));

            PageData result;
            result.next_page_token = std::move(page.next_page_token);
            for (auto& p : page.items) {
              result.items.emplace_back(std::visit(
                  [&]<typename ItemT>(ItemT& entry) -> Item {
                    return Convert(std::move(entry));
                  },
                  p));
            }

            co_return result;
          } else {
            throw CloudException("not a directory");
          }
        },
        std::any_cast<ItemT&&>(std::move(directory.impl)));
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) const override {
    auto data = co_await provider()->GetGeneralData(std::move(stop_token));
    GeneralData result;
    result.username = std::move(data.username);
    if constexpr (HasUsageData<decltype(data)>) {
      result.space_used = data.space_used;
      result.space_total = data.space_total;
    }
    co_return result;
  }

  Generator<std::string> GetFileContent(
      File file, http::Range range,
      stdx::stop_token stop_token) const override {
    return std::visit(
        [&]<typename File>(File item) -> Generator<std::string> {
          if constexpr (IsFile<File, CloudProviderT>) {
            return provider()->GetFileContent(std::move(item), range,
                                              std::move(stop_token));
          } else {
            throw CloudException("not a file");
          }
        },
        std::any_cast<ItemT&&>(std::move(file.impl)));
  }

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token) const override {
    co_return co_await std::visit(
        CreateDirectoryF{provider(), std::move(name), std::move(stop_token)},
        std::any_cast<ItemT&&>(std::move(parent.impl)));
  }

  Task<Directory> RenameItem(Directory item, std::string new_name,
                             stdx::stop_token stop_token) const override {
    co_return co_await std::visit(
        RenameDirectoryF{provider(), std::move(new_name),
                         std::move(stop_token)},
        std::any_cast<ItemT&&>(std::move(item.impl)));
  }

  Task<File> RenameItem(File item, std::string new_name,
                        stdx::stop_token stop_token) const override {
    co_return co_await std::visit(
        RenameFileF{provider(), std::move(new_name), std::move(stop_token)},
        std::any_cast<ItemT&&>(std::move(item.impl)));
  }

  Task<> RemoveItem(Directory item,
                    stdx::stop_token stop_token) const override {
    return Remove(std::move(item), std::move(stop_token));
  }

  Task<> RemoveItem(File item, stdx::stop_token stop_token) const override {
    return Remove(std::move(item), std::move(stop_token));
  }

  Task<File> MoveItem(File source, Directory destination,
                      stdx::stop_token stop_token) const override {
    co_return co_await std::visit(
        MoveFileF{provider(), std::move(stop_token)},
        std::any_cast<ItemT&&>(std::move(source.impl)),
        std::any_cast<ItemT&&>(std::move(destination.impl)));
  }

  Task<Directory> MoveItem(Directory source, Directory destination,
                           stdx::stop_token stop_token) const override {
    co_return co_await std::visit(
        MoveDirectoryF{provider(), std::move(stop_token)},
        std::any_cast<ItemT&&>(std::move(source.impl)),
        std::any_cast<ItemT&&>(std::move(destination.impl)));
  }

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content,
                        stdx::stop_token stop_token) const override {
    co_return co_await std::visit(
        CreateFileF{provider(), std::string(name), std::move(content),
                    std::move(stop_token)},
        std::any_cast<ItemT&&>(std::move(parent.impl)));
  }

  Task<Thumbnail> GetItemThumbnail(File item, http::Range range,
                                   stdx::stop_token stop_token) const override {
    return GetThumbnail(std::move(item), range, std::move(stop_token));
  }

  Task<Thumbnail> GetItemThumbnail(Directory item, http::Range range,
                                   stdx::stop_token stop_token) const override {
    return GetThumbnail(std::move(item), range, std::move(stop_token));
  }

  Task<Thumbnail> GetItemThumbnail(File item, ThumbnailQuality quality,
                                   http::Range range,
                                   stdx::stop_token stop_token) const override {
    return GetThumbnail(std::move(item), quality, range, std::move(stop_token));
  }

  Task<Thumbnail> GetItemThumbnail(Directory item, ThumbnailQuality quality,
                                   http::Range range,
                                   stdx::stop_token stop_token) const override {
    return GetThumbnail(std::move(item), quality, range, std::move(stop_token));
  }

  template <typename From,
            typename To = std::conditional_t<IsDirectory<From, CloudProviderT>,
                                             Directory, File>>
  static To Convert(From d) {
    To result;
    result.id = [&] {
      std::stringstream stream;
      stream << d.id;
      return std::move(stream).str();
    }();
    result.name = d.name;
    result.size = GetSize(d);
    result.timestamp = GetTimestamp(d);
    if constexpr (std::is_same_v<To, File> && IsFile<From, CloudProviderT>) {
      result.mime_type = GetMimeType(d);
    }
    result.impl.template emplace<ItemT>(std::move(d));
    return result;
  }

 private:
  using ItemT = typename CloudProviderT::Item;
  using FileContentT = typename CloudProviderT::FileContent;

  template <typename T>
  static std::string GetMimeType(const T& d) {
    static_assert(IsFile<T, CloudProviderT>);
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

  auto* provider() const {
    return const_cast<CloudProviderT*>(CloudProviderSupplier::provider());
  }

  struct CreateFileF {
    template <typename DirectoryT>
    Task<File> operator()(DirectoryT parent) && {
      if constexpr (CanCreateFile<DirectoryT, CloudProviderT>) {
        FileContentT ncontent;
        ncontent.data = std::move(content.data);
        if constexpr (std::is_convertible_v<decltype(ncontent.size), int64_t>) {
          ncontent.size = content.size.value();
        } else {
          ncontent.size = content.size;
        }
        co_return Convert(co_await provider->CreateFile(
            std::move(parent), std::move(name), std::move(ncontent),
            std::move(stop_token)));
      } else {
        throw CloudException("can't create file");
      }
    }
    CloudProviderT* provider;
    std::string name;
    FileContent content;
    stdx::stop_token stop_token;
  };

  struct CreateDirectoryF {
    template <typename DirectoryT>
    Task<Directory> operator()(DirectoryT parent) {
      if constexpr (CanCreateDirectory<DirectoryT, CloudProviderT>) {
        co_return Convert(co_await provider->CreateDirectory(
            std::move(parent), std::move(name), std::move(stop_token)));
      } else {
        throw CloudException("can't create directory");
      }
    }
    CloudProviderT* provider;
    std::string name;
    stdx::stop_token stop_token;
  };

  struct RenameDirectoryF {
    template <typename Entry>
    Task<Directory> operator()(Entry entry) && {
      if constexpr (IsDirectory<Entry, CloudProviderT> &&
                    CanRename<Entry, CloudProviderT>) {
        co_return Convert(co_await provider->RenameItem(
            std::move(entry), std::move(new_name), std::move(stop_token)));
      } else {
        throw CloudException("can't rename");
      }
    }
    CloudProviderT* provider;
    std::string new_name;
    stdx::stop_token stop_token;
  };

  struct RenameFileF {
    template <typename Entry>
    Task<File> operator()(Entry entry) && {
      if constexpr (IsFile<Entry, CloudProviderT> &&
                    CanRename<Entry, CloudProviderT>) {
        co_return Convert(co_await provider->RenameItem(
            std::move(entry), std::move(new_name), std::move(stop_token)));
      } else {
        throw CloudException("can't rename");
      }
    }
    CloudProviderT* provider;
    std::string new_name;
    stdx::stop_token stop_token;
  };

  struct RemoveItemF {
    template <typename Entry>
    Task<> operator()(Entry entry) && {
      if constexpr (CanRemove<Entry, CloudProviderT>) {
        co_await provider->RemoveItem(std::move(entry), std::move(stop_token));
      } else {
        throw CloudException("can't remove");
      }
    }
    CloudProviderT* provider;
    stdx::stop_token stop_token;
  };

  template <typename Item>
  Task<> Remove(Item item, stdx::stop_token stop_token) const {
    co_return co_await std::visit(
        RemoveItemF{provider(), std::move(stop_token)},
        std::any_cast<ItemT&&>(std::move(item.impl)));
  }

  struct MoveDirectoryF {
    template <typename SourceT, typename DestinationT>
    Task<Directory> operator()(SourceT source, DestinationT destination) && {
      if constexpr (IsDirectory<SourceT, CloudProviderT> &&
                    CanMove<SourceT, DestinationT, CloudProviderT>) {
        co_return Convert(co_await provider->MoveItem(
            std::move(source), std::move(destination), std::move(stop_token)));
      } else {
        throw CloudException("can't move");
      }
    }
    CloudProviderT* provider;
    stdx::stop_token stop_token;
  };

  struct MoveFileF {
    template <typename SourceT, typename DestinationT>
    Task<File> operator()(SourceT source, DestinationT destination) && {
      if constexpr (IsFile<SourceT, CloudProviderT> &&
                    CanMove<SourceT, DestinationT, CloudProviderT>) {
        co_return Convert(co_await provider->MoveItem(
            std::move(source), std::move(destination), std::move(stop_token)));
      } else {
        throw CloudException("can't move");
      }
    }
    CloudProviderT* provider;
    stdx::stop_token stop_token;
  };

  struct GetThumbnailF {
    template <typename Item>
    Task<Thumbnail> operator()(Item entry) && {
      if constexpr (HasThumbnail<Item, CloudProviderT>) {
        auto provider_thumbnail = co_await provider->GetItemThumbnail(
            std::move(entry), range, std::move(stop_token));
        Thumbnail thumbnail{
            .data = std::move(provider_thumbnail.data),
            .size = provider_thumbnail.size,
            .mime_type = std::string(std::move(provider_thumbnail.mime_type))};
        co_return thumbnail;
      } else {
        throw CloudException("thumbnail not available");
      }
    }
    CloudProviderT* provider;
    http::Range range;
    stdx::stop_token stop_token;
  };

  template <typename Item>
  Task<Thumbnail> GetThumbnail(Item item, http::Range range,
                               stdx::stop_token stop_token) const {
    co_return co_await std::visit(
        GetThumbnailF{provider(), range, std::move(stop_token)},
        std::any_cast<ItemT&&>(std::move(item.impl)));
  }

  struct GetQualityThumbnailF {
    template <typename Item>
    Task<Thumbnail> operator()(Item entry) && {
      if constexpr (HasQualityThumbnail<Item, CloudProviderT>) {
        auto provider_thumbnail = co_await provider->GetItemThumbnail(
            std::move(entry), quality, range, std::move(stop_token));
        Thumbnail thumbnail{
            .data = std::move(provider_thumbnail.data),
            .size = provider_thumbnail.size,
            .mime_type = std::string(std::move(provider_thumbnail.mime_type))};
        co_return thumbnail;
      } else {
        co_return co_await GetThumbnailF{
            provider, range, std::move(stop_token)}(std::move(entry));
      }
    }
    CloudProviderT* provider;
    ThumbnailQuality quality;
    http::Range range;
    stdx::stop_token stop_token;
  };

  template <typename Item>
  Task<Thumbnail> GetThumbnail(Item item, ThumbnailQuality quality,
                               http::Range range,
                               stdx::stop_token stop_token) const {
    co_return co_await std::visit(
        GetQualityThumbnailF{provider(), quality, range, std::move(stop_token)},
        std::any_cast<ItemT&&>(std::move(item.impl)));
  }

  struct IsFileContentSizeRequiredF {
    template <typename Item>
    bool operator()(const Item& directory) && {
      if constexpr (IsDirectory<Item, CloudProviderT>) {
        if constexpr (HasIsFileContentSizeRequired<Item, CloudProviderT>) {
          return provider_->IsFileContentSizeRequired(directory);
        } else {
          return std::is_convertible_v<
              decltype(std::declval<FileContentT>().size), int64_t>;
        }
      } else {
        throw CloudException("not a directory here");
      }
    }
    CloudProviderT* provider_;
  };
};

template <typename T>
auto CreateAbstractCloudProviderImpl(T impl) {
  if constexpr (std::is_pointer_v<T>) {
    using CloudProviderT = std::remove_pointer_t<T>;
    return AbstractCloudProviderImpl<
        CloudProviderT,
        AbstractCloudProviderImplNonOwningSupplier<CloudProviderT>>(impl);
  } else {
    using CloudProviderT = std::remove_cvref_t<T>;
    return AbstractCloudProviderImpl<
        CloudProviderT,
        AbstractCloudProviderImplOwningSupplier<CloudProviderT>>(
        std::move(impl));
  }
}

template <typename T>
std::unique_ptr<AbstractCloudProvider> CreateAbstractCloudProvider(T impl) {
  auto d = CreateAbstractCloudProviderImpl(std::move(impl));
  return std::make_unique<decltype(d)>(std::move(d));
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_ABSTRACT_CLOUD_PROVIDER_IMPL_H
