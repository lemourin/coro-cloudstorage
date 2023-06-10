#ifndef CORO_CLOUDSTORAGE_CACHE_MANAGER_H
#define CORO_CLOUDSTORAGE_CACHE_MANAGER_H

#include <any>

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
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
  struct AccountKey {
    std::shared_ptr<AbstractCloudProvider> provider;
    std::string username;
  };

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

  Task<> Put(AccountKey, DirectoryContent, stdx::stop_token stop_token);

  Task<> Put(AccountKey, ItemKey, ItemData, stdx::stop_token);

  Task<> Put(AccountKey, ImageKey, ImageData, stdx::stop_token stop_token);

  Task<std::optional<DirectoryContent>> Get(AccountKey, ParentDirectoryKey,
                                            stdx::stop_token stop_token) const;

  Task<std::optional<ImageData>> Get(AccountKey, ImageKey,
                                     stdx::stop_token stop_token);

  Task<std::optional<ItemData>> Get(AccountKey, ItemKey id,
                                    stdx::stop_token stop_token) const;

 private:
  CacheDatabase* db_;
  mutable coro::util::ThreadPool worker_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_CACHE_MANAGER_H