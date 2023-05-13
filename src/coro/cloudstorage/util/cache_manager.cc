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
using ::sqlite_orm::select;
using ::sqlite_orm::where;

struct DbItem {
  std::string account_type;
  std::string account_username;
  std::string id;
  std::vector<char> content;
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
};

auto CreateStorage(std::string path) {
  auto storage = sqlite_orm::make_storage(
      std::move(path),
      make_table("item", make_column("account_type", &DbItem::account_type),
                 make_column("account_username", &DbItem::account_username),
                 make_column("id", &DbItem::id),
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
                          &DbItem::id)),
      make_table("image", make_column("account_type", &DbImage::account_type),
                 make_column("account_username", &DbImage::account_username),
                 make_column("item_id", &DbImage::item_id),
                 make_column("quality", &DbImage::quality),
                 make_column("mime_type", &DbImage::mime_type),
                 make_column("image_bytes", &DbImage::image_bytes),
                 primary_key(&DbImage::account_type, &DbImage::account_username,
                             &DbImage::item_id)));
  storage.sync_schema();
  return storage;
}

auto& GetDb(std::any& any) {
  return std::any_cast<decltype(CreateStorage(""))&>(any);
}

std::vector<char> ToCbor(const nlohmann::json& json) {
  std::vector<char> output;
  nlohmann::json::to_cbor(json, output);
  return output;
}

}  // namespace

CacheManager::CacheManager(const coro::util::EventLoop* event_loop,
                           std::string cache_path)
    : db_(CreateStorage(std::move(cache_path))),
      worker_(event_loop, /*thread_count=*/1) {}

Task<> CacheManager::Put(CloudProviderAccount account,
                         AbstractCloudProvider::Directory directory,
                         std::vector<AbstractCloudProvider::Item> items,
                         stdx::stop_token stop_token) {
  auto& db = GetDb(db_);
  auto account_id = account.id();
  std::vector<DbItem> db_items(items.size() + 1);
  std::vector<DbDirectoryContent> db_directory_content(items.size());
  db_items[0] =
      DbItem{.account_type = account_id.type,
             .account_username = account_id.username,
             .id = directory.id,
             .content = ToCbor(account.provider()->ToJson(directory))};
  int32_t order = 0;
  for (size_t i = 0; i < items.size(); i++) {
    db_items[i + 1] = std::visit(
        [&](const auto& e) {
          return DbItem{
              .account_type = account_id.type,
              .account_username = account_id.username,
              .id = e.id,
              .content = ToCbor(account.provider()->ToJson(items[i]))};
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
  co_return co_await worker_.Do(std::move(stop_token), [&] {
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
  auto result = co_await worker_.Do(std::move(stop_token), [&] {
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
    items[i] = account.provider()->ToItem(nlohmann::json::from_cbor(result[i]));
  }
  co_return items;
}

Task<> CacheManager::Put(CloudProviderAccount account,
                         AbstractCloudProvider::Item item,
                         ThumbnailQuality quality,
                         std::vector<char> image_bytes, std::string mime_type,
                         stdx::stop_token stop_token) {
  auto account_id = account.id();
  co_await worker_.Do(
      std::move(stop_token),
      [db = GetDb(db_),
       entry = DbImage{
           .account_type = std::move(account_id.type),
           .account_username = std::move(account_id.username),
           .item_id = std::visit([](auto&& i) { return std::move(i.id); },
                                 std::move(item)),
           .quality = static_cast<int>(quality),
           .mime_type = std::move(mime_type),
           .image_bytes = std::move(image_bytes)}]() mutable {
        db.replace(std::move(entry));
      });
}

auto CacheManager::Get(CloudProviderAccount account,
                       AbstractCloudProvider::Item item, ThumbnailQuality,
                       stdx::stop_token stop_token)
    -> Task<std::optional<ImageData>> {
  auto& db = GetDb(db_);
  auto account_id = account.id();
  auto item_id = std::visit([](auto i) { return i.id; }, std::move(item));
  auto result = co_await worker_.Do(std::move(stop_token), [&] {
    return db.select(
        columns(&DbImage::image_bytes, &DbImage::mime_type),
        where(and_(c(&DbImage::account_type) == account_id.type,
                   and_(c(&DbImage::account_username) == account_id.username,
                        c(&DbImage::item_id) == item_id))));
  });
  if (result.empty()) {
    co_return std::nullopt;
  }
  co_return ImageData{.image_bytes = std::get<0>(std::move(result[0])),
                      .mime_type = std::get<1>(std::move(result[0]))};
}

}  // namespace coro::cloudstorage::util