#ifndef CORO_CLOUDSTORAGE_CACHE_MANAGER_H
#define CORO_CLOUDSTORAGE_CACHE_MANAGER_H

#include <any>

#include "coro/cloudstorage/util/cloud_provider_account.h"
#include "coro/task.h"
#include "coro/util/thread_pool.h"

namespace coro::cloudstorage::util {

class CacheManager {
 public:
  CacheManager(coro::util::ThreadPool* thread_pool, std::string cache_path);

  Task<> Put(CloudProviderAccount, AbstractCloudProvider::Directory directory,
             std::vector<AbstractCloudProvider::Item> items,
             stdx::stop_token stop_token);

  Task<std::optional<std::vector<AbstractCloudProvider::Item>>> Get(
      CloudProviderAccount, AbstractCloudProvider::Directory directory,
      stdx::stop_token stop_token) const;

 private:
  coro::util::ThreadPool* thread_pool_;
  std::any db_;
};

class CloudProviderCacheManager {
 public:
  CloudProviderCacheManager(CloudProviderAccount account,
                            CacheManager* cache_manager)
      : account_(std::move(account)), cache_manager_(cache_manager) {}

  Task<> Put(AbstractCloudProvider::Directory directory,
             std::vector<AbstractCloudProvider::Item> items,
             stdx::stop_token stop_token) {
    return cache_manager_->Put(account_, std::move(directory), std::move(items),
                               std::move(stop_token));
  }

  Task<std::optional<std::vector<AbstractCloudProvider::Item>>> Get(
      AbstractCloudProvider::Directory directory,
      stdx::stop_token stop_token) const {
    return cache_manager_->Get(account_, std::move(directory),
                               std::move(stop_token));
  }

 private:
  CloudProviderAccount account_;
  CacheManager* cache_manager_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_CACHE_MANAGER_H