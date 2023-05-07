#include "coro/cloudstorage/util/cache_manager.h"

#include <sqlite_orm/sqlite_orm.h>

namespace coro::cloudstorage::util {

namespace {

using ::sqlite_orm::and_;
using ::sqlite_orm::c;
using ::sqlite_orm::foreign_key;
using ::sqlite_orm::join;
using ::sqlite_orm::make_column;
using ::sqlite_orm::make_storage;
using ::sqlite_orm::make_table;
using ::sqlite_orm::on;
using ::sqlite_orm::order_by;
using ::sqlite_orm::primary_key;
using ::sqlite_orm::select;
using ::sqlite_orm::where;

struct DbItem {
  std::string account_type;
  std::string account_username;
  std::string id;
  std::optional<int64_t> timestamp;
  std::string content;
};

struct DbDirectoryContent {
  std::string account_type;
  std::string account_username;
  std::string parent_item_id;
  std::string child_item_id;
  int32_t order;
};

auto CreateStorage(std::string path) {
  auto storage = sqlite_orm::make_storage(
      std::move(path),
      make_table("item", make_column("account_type", &DbItem::account_type),
                 make_column("account_username", &DbItem::account_username),
                 make_column("id", &DbItem::id),
                 make_column("timestamp", &DbItem::timestamp),
                 make_column("content", &DbItem::content),
                 primary_key(&DbItem::account_type, &DbItem::account_username,
                             &DbItem::id)),
      make_table(
          "directory_content",
          make_column("account_type", &DbDirectoryContent::account_type),
          make_column("account_username",
                      &DbDirectoryContent::account_username),
          make_column("parent_item_id", &DbDirectoryContent::parent_item_id),
          make_column("child_item_id", &DbDirectoryContent::child_item_id),
          make_column("order", &DbDirectoryContent::order),
          primary_key(&DbDirectoryContent::account_type,
                      &DbDirectoryContent::account_username,
                      &DbDirectoryContent::parent_item_id,
                      &DbDirectoryContent::child_item_id),
          foreign_key(&DbDirectoryContent::account_type,
                      &DbDirectoryContent::account_username,
                      &DbDirectoryContent::parent_item_id)
              .references(&DbItem::account_type, &DbItem::account_username,
                          &DbItem::id),
          foreign_key(&DbDirectoryContent::account_type,
                      &DbDirectoryContent::account_username,
                      &DbDirectoryContent::child_item_id)
              .references(&DbItem::account_type, &DbItem::account_username,
                          &DbItem::id)));
  storage.sync_schema();
  return storage;
}

auto& GetDb(std::any& any) {
  return std::any_cast<decltype(CreateStorage(""))&>(any);
}

}  // namespace

CacheManager::CacheManager(coro::util::ThreadPool* thread_pool,
                           std::string cache_path)
    : thread_pool_(thread_pool), db_(CreateStorage(std::move(cache_path))) {}

Task<> CacheManager::Put(CloudProviderAccount account,
                         AbstractCloudProvider::Directory directory,
                         std::vector<AbstractCloudProvider::Item> items,
                         stdx::stop_token stop_token) {
  auto& db = GetDb(db_);
  auto account_id = account.id();
  std::vector<DbItem> db_items(items.size() + 1);
  std::vector<DbDirectoryContent> db_directory_content(items.size());
  db_items[0] = DbItem{.account_type = account_id.type,
                       .account_username = account_id.username,
                       .id = directory.id,
                       .timestamp = directory.timestamp,
                       .content = account.provider()->ToString(directory)};
  int32_t order = 0;
  for (size_t i = 0; i < items.size(); i++) {
    db_items[i + 1] = std::visit(
        [&](const auto& e) {
          return DbItem{.account_type = account_id.type,
                        .account_username = account_id.username,
                        .id = e.id,
                        .timestamp = e.timestamp,
                        .content = account.provider()->ToString(items[i])};
        },
        items[i]);
    db_directory_content[i] = std::visit(
        [&](const auto& i) {
          return DbDirectoryContent{.account_type = account_id.type,
                                    .account_username = account_id.username,
                                    .parent_item_id = directory.id,
                                    .child_item_id = i.id,
                                    .order = order++};
        },
        items[i]);
  }
  co_return co_await thread_pool_->Do(std::move(stop_token), [&] {
    db.transaction([&] {
      for (const auto& d : db_items) {
        db.replace(d);
      }
      for (const auto& d : db_directory_content) {
        db.replace(d);
      }
      return true;
    });
  });
}

Task<std::optional<std::vector<AbstractCloudProvider::Item>>> CacheManager::Get(
    CloudProviderAccount account, AbstractCloudProvider::Directory directory,
    stdx::stop_token stop_token) const {
  auto& db = GetDb(db_);
  auto account_id = account.id();
  auto result = co_await thread_pool_->Do(std::move(stop_token), [&] {
    return db.select(
        &DbItem::content,
        join<DbDirectoryContent>(on(and_(
            and_(c(&DbItem::account_type) == &DbDirectoryContent::account_type,
                 c(&DbItem::account_username) ==
                     &DbDirectoryContent::account_username),
            c(&DbItem::id) == &DbDirectoryContent::child_item_id))),
        where(and_(
            c(&DbDirectoryContent::account_type) == account_id.type,
            and_(
                c(&DbDirectoryContent::account_username) == account_id.username,
                c(&DbDirectoryContent::parent_item_id) == directory.id))),
        order_by(&DbDirectoryContent::order));
  });
  if (result.empty()) {
    co_return std::nullopt;
  }

  std::vector<AbstractCloudProvider::Item> items{result.size()};
  for (size_t i = 0; i < items.size(); i++) {
    items[i] = account.provider()->ToItem(result[i]);
  }
  co_return items;
}

}  // namespace coro::cloudstorage::util