#include "coro/cloudstorage/util/cache_manager.h"

#include <sqlite_orm/sqlite_orm.h>

#include <nlohmann/json.hpp>

namespace coro::cloudstorage::util {

namespace {

using ::sqlite_orm::and_;
using ::sqlite_orm::c;
using ::sqlite_orm::columns;
using ::sqlite_orm::foreign_key;
using ::sqlite_orm::join;
using ::sqlite_orm::make_column;
using ::sqlite_orm::make_storage;
using ::sqlite_orm::make_table;
using ::sqlite_orm::on;
using ::sqlite_orm::order_by;
using ::sqlite_orm::primary_key;
using ::sqlite_orm::where;

struct DbItem {
  std::string account_type;
  std::string account_username;
  std::string id;
  std::vector<char> content;
  int64_t update_time;
};

struct DbDirectoryMetadata {
  std::string account_type;
  std::string account_username;
  std::string parent_item_id;
  int64_t update_time;
};

struct DbDirectoryContent {
  std::string account_type;
  std::string account_username;
  std::string parent_item_id;
  std::string child_item_id;
  int32_t order;
};

struct DbImage {
  std::string account_type;
  std::string account_username;
  std::string item_id;
  int quality;
  std::string mime_type;
  std::vector<char> image_bytes;
  int64_t update_time;
};

auto CreateStorage(std::string path) {
  auto storage = make_storage(
      std::move(path),
      make_table("item", make_column("account_type", &DbItem::account_type),
                 make_column("account_username", &DbItem::account_username),
                 make_column("id", &DbItem::id),
                 make_column("content", &DbItem::content),
                 make_column("update_time", &DbItem::update_time),
                 primary_key(&DbItem::account_type, &DbItem::account_username,
                             &DbItem::id)),
      make_table(
          "directory_metadata",
          make_column("account_type", &DbDirectoryMetadata::account_type),
          make_column("account_username",
                      &DbDirectoryMetadata::account_username),
          make_column("parent_item_id", &DbDirectoryMetadata::parent_item_id),
          make_column("update_time", &DbDirectoryMetadata::update_time),
          primary_key(&DbDirectoryMetadata::account_type,
                      &DbDirectoryMetadata::account_username,
                      &DbDirectoryMetadata::parent_item_id),
          foreign_key(&DbDirectoryMetadata::account_type,
                      &DbDirectoryMetadata::account_username,
                      &DbDirectoryMetadata::parent_item_id)
              .references(&DbItem::account_type, &DbItem::account_username,
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
                          &DbItem::id)),
      make_table("image", make_column("account_type", &DbImage::account_type),
                 make_column("account_username", &DbImage::account_username),
                 make_column("item_id", &DbImage::item_id),
                 make_column("quality", &DbImage::quality),
                 make_column("mime_type", &DbImage::mime_type),
                 make_column("image_bytes", &DbImage::image_bytes),
                 make_column("update_time", &DbImage::update_time),
                 primary_key(&DbImage::account_type, &DbImage::account_username,
                             &DbImage::item_id)));
  storage.sync_schema();
  return storage;
}

using CacheDatabaseT = decltype(CreateStorage(""));

auto* GetDb(CacheDatabase* any) {
  return reinterpret_cast<CacheDatabaseT*>(any);
}

std::vector<char> ToCbor(const nlohmann::json& json) {
  std::vector<char> output;
  nlohmann::json::to_cbor(json, output);
  return output;
}

}  // namespace

void CacheDatabaseDeleter::operator()(CacheDatabase* db) const {
  delete reinterpret_cast<CacheDatabaseT*>(db);
}

std::unique_ptr<CacheDatabase, CacheDatabaseDeleter> CreateCacheDatabase(
    std::string path) {
  return std::unique_ptr<CacheDatabase, CacheDatabaseDeleter>(
      reinterpret_cast<CacheDatabase*>(
          new CacheDatabaseT{CreateStorage(std::move(path))}));
}

CacheManager::CacheManager(CacheDatabase* db,
                           const coro::util::EventLoop* event_loop)
    : db_(db), worker_(event_loop, /*thread_count=*/1, "db") {}

Task<> CacheManager::Put(AccountKey account, DirectoryContent content,
                         stdx::stop_token stop_token) {
  auto* db = GetDb(db_);
  std::vector<DbItem> db_items(content.items.size() + 1);
  std::vector<DbDirectoryContent> db_directory_content(content.items.size());
  std::string account_type{account.provider->GetId()};
  db_items[0] =
      DbItem{.account_type = account_type,
             .account_username = account.username,
             .id = content.parent.id,
             .content = ToCbor(account.provider->ToJson(content.parent)),
             .update_time = content.update_time};
  int32_t order = 0;
  for (size_t i = 0; i < content.items.size(); i++) {
    db_items[i + 1] = std::visit(
        [&](const auto& e) {
          return DbItem{
              .account_type = account_type,
              .account_username = account.username,
              .id = e.id,
              .content = ToCbor(account.provider->ToJson(content.items[i])),
              .update_time = content.update_time};
        },
        content.items[i]);
    db_directory_content[i] = std::visit(
        [&](const auto& i) {
          return DbDirectoryContent{.account_type = account_type,
                                    .account_username = account.username,
                                    .parent_item_id = content.parent.id,
                                    .child_item_id = i.id,
                                    .order = order++};
        },
        content.items[i]);
  }
  DbDirectoryMetadata metadata{.account_type = account_type,
                               .account_username = account.username,
                               .parent_item_id = content.parent.id,
                               .update_time = content.update_time};
  co_return co_await worker_.Do(std::move(stop_token), [&] {
    db->transaction([&] {
      db->remove_all<DbDirectoryContent>(where(and_(
          c(&DbDirectoryContent::account_type) == account_type,
          and_(c(&DbDirectoryContent::account_username) == account.username,
               c(&DbDirectoryContent::parent_item_id) == content.parent.id))));
      for (const auto& d : db_items) {
        db->replace(d);
      }
      for (const auto& d : db_directory_content) {
        db->replace(d);
      }
      db->replace(metadata);
      return true;
    });
  });
}

Task<> CacheManager::Put(AccountKey account, ItemKey key, ItemData item,
                         stdx::stop_token stop_token) {
  auto* db = GetDb(db_);
  DbItem db_item =
      DbItem{.account_type = std::string{account.provider->GetId()},
             .account_username = account.username,
             .id = std::move(key.item_id),
             .content = ToCbor(account.provider->ToJson(item.item)),
             .update_time = item.update_time};
  co_return co_await worker_.Do(std::move(stop_token),
                                [&] { db->replace(db_item); });
}

auto CacheManager::Get(AccountKey account, ParentDirectoryKey key,
                       stdx::stop_token stop_token) const
    -> Task<std::optional<DirectoryContent>> {
  auto* db = GetDb(db_);
  auto result = co_await worker_.Do(
      std::move(stop_token),
      [&]() -> std::optional<std::pair<DbDirectoryMetadata,
                                       std::vector<std::vector<char>>>> {
        auto lock = db->transaction_guard();
        auto metadata = db->get_all<DbDirectoryMetadata>(where(and_(
            c(&DbDirectoryMetadata::account_type) == account.provider->GetId(),
            and_(c(&DbDirectoryMetadata::account_username) == account.username,
                 c(&DbDirectoryMetadata::parent_item_id) == key.item_id))));
        if (metadata.empty()) {
          return std::nullopt;
        }
        return std::make_pair(
            std::move(metadata[0]),
            db->select(
                &DbItem::content,
                join<DbDirectoryContent>(on(and_(
                    and_(c(&DbItem::account_type) ==
                             &DbDirectoryContent::account_type,
                         c(&DbItem::account_username) ==
                             &DbDirectoryContent::account_username),
                    c(&DbItem::id) == &DbDirectoryContent::child_item_id))),
                where(and_(c(&DbDirectoryContent::account_type) ==
                               account.provider->GetId(),
                           and_(c(&DbDirectoryContent::account_username) ==
                                    account.username,
                                c(&DbDirectoryContent::parent_item_id) ==
                                    key.item_id))),
                order_by(&DbDirectoryContent::order)));
      });
  if (!result) {
    co_return std::nullopt;
  }

  std::vector<AbstractCloudProvider::Item> items{result->second.size()};
  for (size_t i = 0; i < items.size(); i++) {
    items[i] =
        account.provider->ToItem(nlohmann::json::from_cbor(result->second[i]));
  }
  co_return DirectoryContent{.items = std::move(items),
                             .update_time = result->first.update_time};
}

Task<> CacheManager::Put(AccountKey account, ImageKey key, ImageData image,
                         stdx::stop_token stop_token) {
  co_await worker_.Do(
      std::move(stop_token),
      [db = GetDb(db_),
       entry = DbImage{.account_type = std::string{account.provider->GetId()},
                       .account_username = std::move(account.username),
                       .item_id = std::move(key.item_id),
                       .quality = static_cast<int>(key.quality),
                       .mime_type = std::move(image.mime_type),
                       .image_bytes = std::move(image.image_bytes),
                       .update_time = image.update_time}]() mutable {
        db->replace(entry);
      });
}

auto CacheManager::Get(AccountKey account, ImageKey key,
                       stdx::stop_token stop_token)
    -> Task<std::optional<ImageData>> {
  auto* db = GetDb(db_);
  auto result = co_await worker_.Do(std::move(stop_token), [&] {
    return db->select(
        columns(&DbImage::image_bytes, &DbImage::mime_type,
                &DbImage::update_time),
        where(and_(c(&DbImage::account_type) == account.provider->GetId(),
                   and_(c(&DbImage::account_username) == account.username,
                        and_(c(&DbImage::item_id) == key.item_id,
                             c(&DbImage::quality) ==
                                 static_cast<int>(key.quality))))));
  });
  if (result.empty()) {
    co_return std::nullopt;
  }
  co_return ImageData{.image_bytes = std::move(std::get<0>(result[0])),
                      .mime_type = std::move(std::get<1>(result[0])),
                      .update_time = std::get<2>(result[0])};
}

Task<std::optional<CacheManager::ItemData>> CacheManager::Get(
    AccountKey account, ItemKey key, stdx::stop_token stop_token) const {
  auto* db = GetDb(db_);
  auto item = co_await worker_.Do(
      std::move(stop_token), [&]() -> std::optional<DbItem> {
        auto result = db->get_all<DbItem>(where(
            and_(and_(c(&DbItem::id) == key.item_id,
                      c(&DbItem::account_type) == account.provider->GetId()),
                 c(&DbItem::account_username) == account.username)));
        if (result.empty()) {
          return std::nullopt;
        } else {
          return result[0];
        }
      });
  if (item) {
    co_return ItemData{.item = account.provider->ToItem(
                           nlohmann::json::from_cbor(item->content)),
                       .update_time = item->update_time};
  } else {
    co_return std::nullopt;
  }
}

}  // namespace coro::cloudstorage::util