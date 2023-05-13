#include "coro/cloudstorage/util/stale_cloud_provider.h"

#include <iostream>

namespace coro::cloudstorage::util {

namespace {

Task<> UpdateCache(AbstractCloudProvider* provider,
                   CloudProviderCacheManager cache_manager,
                   AbstractCloudProvider::Directory directory) {
  try {
    std::vector<AbstractCloudProvider::Item> items;
    std::optional<std::string> page_token;
    do {
      auto page_data = co_await provider->ListDirectoryPage(
          directory, page_token, stdx::stop_token());
      std::copy(page_data.items.begin(), page_data.items.end(),
                std::back_inserter(items));
      page_token = std::move(page_data.next_page_token);
    } while (page_token);
    co_await cache_manager.Put(directory, std::move(items), stdx::stop_token());
  } catch (const std::exception& e) {
    std::cerr << "COULDN'T RELOAD DIRECTORY PAGE: " << e.what() << '\n';
  }
}

}  // namespace

auto StaleCloudProvider::ListDirectoryPage(
    Directory directory, std::optional<std::string> page_token,
    stdx::stop_token stop_token) const -> Task<PageData> {
  if (page_token) {
    auto page_data = co_await provider_->ListDirectoryPage(
        std::move(directory), std::move(page_token), std::move(stop_token));
    if (!page_data.next_page_token) {
      RunTask(UpdateCache, provider_, cache_manager_, std::move(directory));
    }
    co_return page_data;
  }

  auto cached = co_await cache_manager_.Get(directory, stop_token);
  if (!cached) {
    auto page_data = co_await provider_->ListDirectoryPage(
        directory, std::move(page_token), std::move(stop_token));
    if (!page_data.next_page_token) {
      RunTask([directory = std::move(directory), page_data,
               cache_manager = cache_manager_]() mutable -> Task<> {
        return cache_manager.Put(std::move(directory),
                                 std::move(page_data.items),
                                 stdx::stop_token());
      });
    }
    co_return page_data;
  }
  RunTask(UpdateCache, provider_, cache_manager_, std::move(directory));
  co_return PageData{.items = std::move(*cached)};
}

auto StaleCloudProvider::GetItemThumbnail(File item, ThumbnailQuality quality,
                                          http::Range range,
                                          stdx::stop_token stop_token) const
    -> Task<Thumbnail> {
  return provider_->GetItemThumbnail(std::move(item), quality, range,
                                     std::move(stop_token));
}

auto StaleCloudProvider::GetItemThumbnail(Directory item,
                                          ThumbnailQuality quality,
                                          http::Range range,
                                          stdx::stop_token stop_token) const
    -> Task<Thumbnail> {
  return provider_->GetItemThumbnail(std::move(item), quality, range,
                                     std::move(stop_token));
}

}  // namespace coro::cloudstorage::util