#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_UTILS_H_
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_UTILS_H_

#include <span>
#include <string_view>

#include "coro/cloudstorage/util/abstract_cloud_provider.h"

namespace coro::cloudstorage::util {

enum class FileType { kUnknown, kVideo, kAudio, kImage };

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
    current_page_token = page_data.next_page_token;
  } while (current_page_token);
}

Task<AbstractCloudProvider::Item> GetItemByPathComponents(
    const AbstractCloudProvider::CloudProvider*,
    std::span<const std::string> components, stdx::stop_token stop_token);

Task<AbstractCloudProvider::Item> GetItemByPath(
    const AbstractCloudProvider::CloudProvider*, std::string path,
    stdx::stop_token stop_token);

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_UTILS_H_