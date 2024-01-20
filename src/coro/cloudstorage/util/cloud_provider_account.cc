#include "coro/cloudstorage/util/cloud_provider_account.h"

#include "coro/cloudstorage/util/cloud_provider_utils.h"
#include "coro/cloudstorage/util/generator_utils.h"

namespace coro::cloudstorage::util {

namespace {

using ::coro::RunTask;

constexpr const int64_t kThumbnailTimeToLive = 60LL * 60;

Task<> UpdateDirectoryListCache(
    CacheManager::AccountKey account, CacheManager* cache_manager,
    int64_t current_time,
    std::shared_ptr<
        Promise<std::optional<std::vector<AbstractCloudProvider::Item>>>>
        updated,
    AbstractCloudProvider::Directory directory,
    std::vector<AbstractCloudProvider::Item> previous,
    stdx::stop_token stop_token) {
  try {
    std::vector<AbstractCloudProvider::Item> items;
    std::optional<std::string> page_token;
    do {
      auto page_data = co_await account.provider->ListDirectoryPage(
          directory, page_token, stop_token);
      std::copy(page_data.items.begin(), page_data.items.end(),
                std::back_inserter(items));
      page_token = std::move(page_data.next_page_token);
    } while (page_token);
    if (!std::equal(items.begin(), items.end(), previous.begin(),
                    previous.end(), [&](const auto& item1, const auto& item2) {
                      return account.provider->ToJson(item1) ==
                             account.provider->ToJson(item2);
                    })) {
      co_await cache_manager->Put(
          std::move(account),
          CacheManager::DirectoryContent{
              .parent = directory, .items = items, .update_time = current_time},
          std::move(stop_token));
      updated->SetValue(std::move(items));
    } else {
      updated->SetValue(std::nullopt);
    }
  } catch (...) {
    updated->SetException(std::current_exception());
  }
}

}  // namespace

Task<VersionedDirectoryContent> CloudProviderAccount::ListDirectory(
    AbstractCloudProvider::Directory directory,
    stdx::stop_token stop_token) const {
  auto current_time = clock_->Now();
  auto cached = co_await cache_manager_->Get(
      account_key(), CacheManager::ParentDirectoryKey{directory.id},
      stop_token);
  auto updated = std::make_shared<
      Promise<std::optional<std::vector<AbstractCloudProvider::Item>>>>();
  if (!cached) {
    auto generator =
        [](auto* cache_manager, auto current_time, auto updated, auto account,
           auto directory,
           auto stop_token) -> Generator<AbstractCloudProvider::PageData> {
      std::optional<std::string> page_token;
      std::vector<AbstractCloudProvider::Item> items;
      try {
        do {
          auto page_data = co_await account.provider->ListDirectoryPage(
              directory, page_token, stop_token);
          std::copy(page_data.items.begin(), page_data.items.end(),
                    std::back_inserter(items));
          co_yield page_data;
          page_token = std::move(page_data.next_page_token);
        } while (page_token);
        co_await cache_manager->Put(
            std::move(account),
            CacheManager::DirectoryContent{.parent = std::move(directory),
                                           .items = std::move(items),
                                           .update_time = current_time},
            std::move(stop_token));
        updated->SetValue(std::nullopt);
      } catch (...) {
        updated->SetException(std::current_exception());
        throw;
      }
    }(cache_manager_, current_time, updated, account_key(),
                            std::move(directory), std::move(stop_token));
    co_return VersionedDirectoryContent{std::move(generator), current_time,
                                        std::move(updated)};
  } else {
    RunTask(UpdateDirectoryListCache, account_key(), cache_manager_,
            current_time, updated, std::move(directory), cached->items,
            stop_source_.get_token());
    co_return VersionedDirectoryContent{
        .content =
            [](auto items) -> Generator<AbstractCloudProvider::PageData> {
          co_yield AbstractCloudProvider::PageData{.items = std::move(items)};
        }(std::move(cached->items)),
        .update_time = cached->update_time,
        .updated = std::move(updated)};
  }
}

Task<VersionedItem> CloudProviderAccount::GetItemById(
    std::string id, stdx::stop_token stop_token) const {
  auto current_time = clock_->Now();
  auto updated =
      std::make_shared<Promise<std::optional<AbstractCloudProvider::Item>>>();
  auto item = co_await cache_manager_->Get(
      account_key(), CacheManager::ItemKey{id}, stop_token);
  if (item) {
    RunTask([account_key = account_key(), provider = provider_,
             cache_manager = cache_manager_, current_time, id = std::move(id),
             prev_item = item->item, stop_token = stop_source_.get_token(),
             updated]() mutable -> Task<> {
      try {
        auto item = co_await ::coro::cloudstorage::util ::GetItemById(
            provider.get(), id, stop_token);
        if (provider->ToJson(item) != provider->ToJson(prev_item)) {
          co_await cache_manager->Put(
              std::move(account_key), CacheManager::ItemKey{id},
              CacheManager::ItemData{.item = item, .update_time = current_time},
              std::move(stop_token));
          updated->SetValue(std::move(item));
        } else {
          updated->SetValue(std::nullopt);
        }
      } catch (...) {
        updated->SetException(std::current_exception());
      }
    });
    co_return VersionedItem{.item = std::move(item->item),
                            .update_time = item->update_time,
                            .updated = std::move(updated)};
  } else {
    try {
      auto item = co_await ::coro::cloudstorage::util::GetItemById(
          provider_.get(), id, stop_token);
      co_await cache_manager_->Put(
          account_key(), CacheManager::ItemKey{id},
          CacheManager::ItemData{.item = item, .update_time = current_time},
          std::move(stop_token));
      updated->SetValue(std::nullopt);
      co_return VersionedItem{.item = std::move(item),
                              .update_time = current_time,
                              .updated = updated};
    } catch (...) {
      updated->SetException(std::current_exception());
      throw;
    }
  }
}

template <typename Item>
Task<VersionedThumbnail> CloudProviderAccount::GetItemThumbnailWithFallback(
    Item item, ThumbnailQuality quality, http::Range range,
    stdx::stop_token stop_token) const {
  auto current_time = clock_->Now();
  std::optional<CacheManager::ImageData> image_data =
      co_await cache_manager_->Get(
          account_key(), CacheManager::ImageKey{item.id, quality}, stop_token);
  auto updated = std::make_shared<
      Promise<std::optional<AbstractCloudProvider::Thumbnail>>>();
  if (image_data) {
    if (current_time - image_data->update_time > kThumbnailTimeToLive) {
      RunTask([account_key = account_key(),
               thumbnail_generator = thumbnail_generator_,
               cache_manager = cache_manager_, current_time,
               provider = provider_, item = std::move(item), quality, range,
               stop_token = stop_source_.get_token(),
               updated]() mutable -> Task<> {
        try {
          AbstractCloudProvider::Thumbnail thumbnail =
              co_await ::coro::cloudstorage::util::GetItemThumbnailWithFallback(
                  thumbnail_generator, provider.get(), item, quality,
                  http::Range{}, stop_token);
          auto image_bytes = co_await http::GetBody(std::move(thumbnail.data));
          CacheManager::ImageData image_data{
              .image_bytes =
                  std::vector<char>(image_bytes.begin(), image_bytes.end()),
              .mime_type = thumbnail.mime_type,
              .update_time = current_time};
          co_await cache_manager->Put(std::move(account_key),
                                      CacheManager::ImageKey{item.id, quality},
                                      image_data, std::move(stop_token));
          int64_t size = static_cast<int64_t>(image_data.image_bytes.size());
          std::string data(image_data.image_bytes.begin(),
                           image_data.image_bytes.end());
          updated->SetValue(AbstractCloudProvider::Thumbnail{
              .data = ToGenerator(Trim(std::move(data), range)),
              .size = size,
              .mime_type = std::move(image_data.mime_type)});
        } catch (...) {
          updated->SetException(std::current_exception());
        }
      });
    }
    int64_t size = static_cast<int64_t>(image_data->image_bytes.size());
    std::string data(image_data->image_bytes.begin(),
                     image_data->image_bytes.end());
    updated->SetValue(std::nullopt);
    co_return VersionedThumbnail{
        .thumbnail =
            AbstractCloudProvider::Thumbnail{
                .data = ToGenerator(Trim(std::move(data), range)),
                .size = size,
                .mime_type = std::move(image_data->mime_type)},
        .update_time = image_data->update_time,
        .updated = std::move(updated)};
  }
  try {
    AbstractCloudProvider::Thumbnail thumbnail =
        co_await ::coro::cloudstorage::util::GetItemThumbnailWithFallback(
            thumbnail_generator_, provider_.get(), item, quality, http::Range{},
            stop_token);
    auto image_bytes = co_await http::GetBody(std::move(thumbnail.data));
    co_await cache_manager_->Put(
        account_key(), CacheManager::ImageKey{item.id, quality},
        CacheManager::ImageData{.image_bytes = std::vector<char>(
                                    image_bytes.begin(), image_bytes.end()),
                                .mime_type = thumbnail.mime_type,
                                .update_time = current_time},
        std::move(stop_token));
    updated->SetValue(std::nullopt);
    co_return VersionedThumbnail{
        .thumbnail =
            AbstractCloudProvider::Thumbnail{
                .data = ToGenerator(Trim(std::move(image_bytes), range)),
                .size = thumbnail.size,
                .mime_type = std::move(thumbnail.mime_type)},
        .update_time = current_time,
        .updated = updated};
  } catch (...) {
    updated->SetException(std::current_exception());
    throw;
  }
}

template Task<VersionedThumbnail>
    CloudProviderAccount::GetItemThumbnailWithFallback(
        AbstractCloudProvider::File, ThumbnailQuality, http::Range,
        stdx::stop_token) const;

template Task<VersionedThumbnail>
    CloudProviderAccount::GetItemThumbnailWithFallback(
        AbstractCloudProvider::Directory, ThumbnailQuality, http::Range,
        stdx::stop_token) const;

}  // namespace coro::cloudstorage::util