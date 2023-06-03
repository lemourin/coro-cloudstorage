#ifndef CORO_CLOUDSTORAGE_CACHE_MANAGER_H
#define CORO_CLOUDSTORAGE_CACHE_MANAGER_H

#include <any>

#include "coro/cloudstorage/util/cloud_provider_account.h"
#include "coro/task.h"
#include "coro/util/event_loop.h"
#include "coro/util/thread_pool.h"

namespace coro::cloudstorage::util {

struct CacheDatabase;

struct CacheDatabaseDeleter {
  void operator()(CacheDatabase*) const;
};

std::unique_ptr<CacheDatabase, CacheDatabaseDeleter> CreateCacheDatabase(
    std::string path);

class CacheManager {
 public:
  struct ImageKey {
    std::string item_id;
    ThumbnailQuality quality;
  };

  struct ImageData {
    std::vector<char> image_bytes;
    std::string mime_type;
    int64_t update_time;
  };

  struct ItemKey {
    std::string item_id;
  };

  struct ItemData {
    AbstractCloudProvider::Item item;
    int64_t update_time;
  };

  struct ParentDirectoryKey {
    std::string item_id;
  };

  struct DirectoryContent {
    AbstractCloudProvider::Directory parent;
    std::vector<AbstractCloudProvider::Item> items;
    int64_t update_time;
  };

  CacheManager(CacheDatabase*, const coro::util::EventLoop* event_loop);

  Task<> Put(CloudProviderAccount, DirectoryContent,
             stdx::stop_token stop_token);

  Task<> Put(CloudProviderAccount, ItemData, stdx::stop_token);

  Task<> Put(CloudProviderAccount, std::string id, ThumbnailQuality, ImageData,
             stdx::stop_token stop_token);

  Task<std::optional<DirectoryContent>> Get(CloudProviderAccount,
                                            ParentDirectoryKey,
                                            stdx::stop_token stop_token) const;

  Task<std::optional<ImageData>> Get(CloudProviderAccount, ImageKey,
                                     stdx::stop_token stop_token);

  Task<std::optional<ItemData>> Get(CloudProviderAccount, ItemKey id,
                                    stdx::stop_token stop_token) const;

 private:
  CacheDatabase* db_;
  mutable coro::util::ThreadPool worker_;
};

class CloudProviderCacheManager {
 public:
  CloudProviderCacheManager(CloudProviderAccount account,
                            CacheManager* cache_manager)
      : account_(std::move(account)), cache_manager_(cache_manager) {}

  template <typename... Args>
  auto Put(Args&&... args) {
    return cache_manager_->Put(account_, std::forward<Args>(args)...);
  }

  template <typename... Args>
  auto Get(Args&&... args) const {
    return cache_manager_->Get(account_, std::forward<Args>(args)...);
  }

 private:
  CloudProviderAccount account_;
  CacheManager* cache_manager_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_CACHE_MANAGER_H