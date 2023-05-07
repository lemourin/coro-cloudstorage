#include "coro/cloudstorage/util/cache_manager.h"

#include <sqlite_orm/sqlite_orm.h>

namespace coro::cloudstorage::util {

namespace {

using ::sqlite_orm::make_column;
using ::sqlite_orm::make_storage;
using ::sqlite_orm::make_table;

struct CloudItem {
  std::string account_type;
  std::string account_username;
  std::string id;
  int64_t timestamp;
  std::string content;
};

auto CreateStorage(std::string path) {
  return sqlite_orm::make_storage(
      std::move(path),
      make_table("items", make_column("account_type", &CloudItem::account_type),
                 make_column("account_username", &CloudItem::account_username),
                 make_column("id", &CloudItem::id),
                 make_column("timestamp", &CloudItem::timestamp),
                 make_column("content", &CloudItem::content)));
}

const auto& GetDb(const std::any& any) {
  return std::any_cast<const decltype(CreateStorage(""))&>(any);
}

auto& GetDb(std::any& any) {
  return std::any_cast<decltype(CreateStorage(""))&>(any);
}

}  // namespace

CacheManager::CacheManager(coro::util::ThreadPool* thread_pool,
                           std::string cache_path)
    : thread_pool_(thread_pool), db_(CreateStorage(std::move(cache_path))) {}

Task<> CacheManager::Put(CloudProviderAccount,
                         AbstractCloudProvider::Directory directory,
                         std::vector<AbstractCloudProvider::Item> items,
                         stdx::stop_token stop_token) {
  auto& db = GetDb(db_);
  co_return;
}

Task<std::optional<std::vector<AbstractCloudProvider::Item>>> CacheManager::Get(
    CloudProviderAccount, AbstractCloudProvider::Directory directory,
    stdx::stop_token stop_token) const {
  co_return std::nullopt;
}

}  // namespace coro::cloudstorage::util