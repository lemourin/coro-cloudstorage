#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_ACCOUNT_H
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_ACCOUNT_H

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/cloudstorage/util/cache_manager.h"
#include "coro/cloudstorage/util/clock.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"
#include "coro/stdx/stop_source.h"
#include "coro/stdx/stop_token.h"
#include "coro/util/type_list.h"

namespace coro::cloudstorage::util {

struct VersionedDirectoryContent {
  Generator<AbstractCloudProvider::PageData> content;
  int64_t update_time;
  std::shared_ptr<
      Promise<std::optional<std::vector<AbstractCloudProvider::Item>>>>
      updated;
};

struct VersionedItem {
  AbstractCloudProvider::Item item;
  int64_t update_time;
  std::shared_ptr<Promise<std::optional<AbstractCloudProvider::Item>>> updated;
};

struct VersionedThumbnail {
  AbstractCloudProvider::Thumbnail thumbnail;
  int64_t update_time;
  std::shared_ptr<Promise<std::optional<AbstractCloudProvider::Thumbnail>>>
      updated;
};

class CloudProviderAccount {
 public:
  struct Id {
    std::string type;
    std::string username;

    friend bool operator==(const Id& a, const Id& b) {
      return std::tie(a.type, a.username) == std::tie(b.type, b.username);
    }
  };

  std::string_view type() const { return type_; }
  Id id() const { return {type_, std::string(username())}; }
  std::string_view username() const { return username_; }
  auto& provider() { return provider_; }
  const auto& provider() const { return provider_; }
  stdx::stop_token stop_token() const { return stop_source_.get_token(); }

  Task<VersionedDirectoryContent> ListDirectory(
      AbstractCloudProvider::Directory, stdx::stop_token) const;

  Task<VersionedItem> GetItemById(std::string id,
                                  stdx::stop_token stop_token) const;

  template <typename Item>
  Task<VersionedThumbnail> GetItemThumbnailWithFallback(Item, ThumbnailQuality,
                                                        http::Range,
                                                        stdx::stop_token) const;

 private:
  CloudProviderAccount(std::string username, int64_t version,
                       std::unique_ptr<AbstractCloudProvider> account,
                       CacheManager* cache_manager, const Clock* clock,
                       const ThumbnailGenerator* thumbnail_generator)
      : username_(std::move(username)),
        version_(version),
        type_(account->GetId()),
        provider_(std::move(account)),
        cache_manager_(cache_manager),
        clock_(clock),
        thumbnail_generator_(thumbnail_generator) {}

  friend class AccountManagerHandler;

  CacheManager::AccountKey account_key() const {
    return {provider_, username_};
  }

  std::string username_;
  int64_t version_;
  std::string type_;
  std::shared_ptr<AbstractCloudProvider> provider_;
  CacheManager* cache_manager_;
  const Clock* clock_;
  const ThumbnailGenerator* thumbnail_generator_;
  stdx::stop_source stop_source_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_ACCOUNT_H