#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_UTILS_H_
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_UTILS_H_

#include <span>
#include <string_view>

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/cloudstorage/util/cloud_provider_account.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"

namespace coro::cloudstorage::util {

enum class FileType { kUnknown, kVideo, kAudio, kImage };

inline constexpr std::string_view kRootId = "";

FileType GetFileType(std::string_view mime_type);

template <typename CloudProviderT, typename DirectoryT>
auto ListDirectory(CloudProviderT* d, DirectoryT directory,
                   stdx::stop_token stop_token)
    -> Generator<typename decltype(d->ListDirectoryPage(directory, std::nullopt,
                                                        stop_token))::type> {
  std::optional<std::string> current_page_token;
  do {
    auto page_data = co_await d->ListDirectoryPage(
        directory, std::move(current_page_token), stop_token);
    co_yield page_data;
    current_page_token = std::move(page_data.next_page_token);
  } while (current_page_token);
}

Task<AbstractCloudProvider::Item> GetItemByPathComponents(
    const AbstractCloudProvider*, std::vector<std::string> components,
    stdx::stop_token stop_token);

Task<AbstractCloudProvider::Item> GetItemByPath(const AbstractCloudProvider*,
                                                std::string path,
                                                stdx::stop_token stop_token);

Task<AbstractCloudProvider::Item> GetItemById(const AbstractCloudProvider*,
                                              std::string id,
                                              stdx::stop_token stop_token);

template <typename Item>
Task<AbstractCloudProvider::Thumbnail> GetItemThumbnailWithFallback(
    const ThumbnailGenerator*, const AbstractCloudProvider*, Item,
    ThumbnailQuality, http::Range, stdx::stop_token) = delete;

template <>
Task<AbstractCloudProvider::Thumbnail>
GetItemThumbnailWithFallback<AbstractCloudProvider::File>(
    const ThumbnailGenerator*, const AbstractCloudProvider*,
    AbstractCloudProvider::File, ThumbnailQuality, http::Range,
    stdx::stop_token);

template <>
Task<AbstractCloudProvider::Thumbnail>
GetItemThumbnailWithFallback<AbstractCloudProvider::Directory>(
    const ThumbnailGenerator*, const AbstractCloudProvider*,
    AbstractCloudProvider::Directory, ThumbnailQuality, http::Range,
    stdx::stop_token);

template <typename T>
struct TypedItemId {
  enum class Type { kFile, kDirectory } type;
  T id;
};

template <typename T>
struct FromStringT<TypedItemId<T>> {
  TypedItemId<T> operator()(std::string id) const {
    auto type = id[0] == 'F' ? TypedItemId<T>::Type::kFile
                             : TypedItemId<T>::Type::kDirectory;
    return TypedItemId<T>{.type = type,
                          .id = FromString<T>(std::move(id).substr(1))};
  }
};

template <typename T>
std::string ToString(const TypedItemId<T>& id) {
  return StrCat(id.type == TypedItemId<T>::Type::kFile ? 'F' : 'D', id.id);
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_UTILS_H_