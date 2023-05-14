#ifndef CORO_CLOUDSTORAGE_CACHE_MANAGER_H
#define CORO_CLOUDSTORAGE_CACHE_MANAGER_H

#include <any>

#include "coro/cloudstorage/util/cloud_provider_account.h"
#include "coro/task.h"
#include "coro/util/event_loop.h"
#include "coro/util/thread_pool.h"

namespace coro::cloudstorage::util {

class CacheManager {
 public:
  struct ImageData {
    std::vector<char> image_bytes;
    std::string mime_type;
  };

  CacheManager(const coro::util::EventLoop* event_loop, std::string cache_path);

  Task<> Put(CloudProviderAccount, AbstractCloudProvider::Directory directory,
             std::vector<AbstractCloudProvider::Item> items,
             stdx::stop_token stop_token);

  Task<> Put(CloudProviderAccount, std::vector<std::string> path,
             AbstractCloudProvider::Item, stdx::stop_token);

  Task<> Put(CloudProviderAccount, AbstractCloudProvider::Item,
             ThumbnailQuality, std::vector<char> image_bytes,
             std::string mime_type, stdx::stop_token stop_token);

  Task<std::optional<std::vector<AbstractCloudProvider::Item>>> Get(
      CloudProviderAccount, AbstractCloudProvider::Directory directory,
      stdx::stop_token stop_token) const;

  Task<std::optional<ImageData>> Get(CloudProviderAccount,
                                     AbstractCloudProvider::Item,
                                     ThumbnailQuality,
                                     stdx::stop_token stop_token);

  Task<std::optional<AbstractCloudProvider::Item>> Get(
      CloudProviderAccount, std::vector<std::string> path,
      stdx::stop_token stop_token) const;

 private:
  mutable std::any db_;
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