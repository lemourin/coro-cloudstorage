#include "coro/cloudstorage/util/stale_cloud_provider.h"

#include <iostream>

#include "coro/cloudstorage/util/generator_utils.h"

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

template <typename Item>
Task<AbstractCloudProvider::Thumbnail> GetThumbnail(
    AbstractCloudProvider* provider, CloudProviderCacheManager cache_manager,
    Item item, ThumbnailQuality quality, stdx::stop_token stop_token) {
  std::optional<CacheManager::ImageData> image_data =
      co_await cache_manager.Get(item, quality, stop_token);
  if (image_data) {
    int64_t size = image_data->image_bytes.size();
    std::string data(image_data->image_bytes.begin(),
                     image_data->image_bytes.end());
    co_return AbstractCloudProvider::Thumbnail{
        .data = ToGenerator(std::move(data)),
        .size = size,
        .mime_type = std::move(image_data->mime_type)};
  } else {
    auto image_data = co_await provider->GetItemThumbnail(
        item, quality, http::Range{}, std::move(stop_token));
    auto image_bytes = co_await http::GetBody(std::move(image_data.data));
    RunTask([cache_manager, item = std::move(item), quality,
             image_bytes =
                 std::vector<char>(image_bytes.begin(), image_bytes.end()),
             mime_type = image_data.mime_type]() mutable {
      return cache_manager.Put(std::move(item), quality, std::move(image_bytes),
                               std::move(mime_type), stdx::stop_token());
    });
    co_return AbstractCloudProvider::Thumbnail{
        .data = ToGenerator(std::move(image_bytes)),
        .size = image_data.size,
        .mime_type = std::move(image_data.mime_type)};
  }
}

template <typename Item>
Task<AbstractCloudProvider::Thumbnail> GetThumbnail(
    AbstractCloudProvider* provider, CloudProviderCacheManager cache_manager,
    Item item, ThumbnailQuality quality, http::Range range,
    stdx::stop_token stop_token) {
  auto thumbnail =
      co_await GetThumbnail(provider, std::move(cache_manager), std::move(item),
                            quality, std::move(stop_token));
  std::string image_bytes = co_await http::GetBody(std::move(thumbnail.data));
  thumbnail.data = ToGenerator(
      std::move(image_bytes)
          .substr(range.start, range.end ? *range.end - range.start + 1
                                         : std::string::npos));
  co_return thumbnail;
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
  return GetThumbnail(provider_, cache_manager_, std::move(item), quality,
                      range, std::move(stop_token));
}

auto StaleCloudProvider::GetItemThumbnail(Directory item,
                                          ThumbnailQuality quality,
                                          http::Range range,
                                          stdx::stop_token stop_token) const
    -> Task<Thumbnail> {
  return GetThumbnail(provider_, cache_manager_, std::move(item), quality,
                      range, std::move(stop_token));
}

}  // namespace coro::cloudstorage::util