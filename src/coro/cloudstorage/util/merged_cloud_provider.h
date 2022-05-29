#ifndef CORO_CLOUDSTORAGE_FUSE_MERGED_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_FUSE_MERGED_CLOUD_PROVIDER_H

#include <iostream>

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/util/stop_token_or.h"
#include "coro/when_all.h"

namespace coro::cloudstorage::util {

struct MergedCloudProvider {
  struct GeneralData {
    std::string username;
    std::optional<int64_t> space_used;
    std::optional<int64_t> space_total;
  };

  struct ItemData {
    std::string account_id;
    std::string id;
    std::string name;
    std::optional<int64_t> timestamp;
    std::optional<int64_t> size;
  };

  struct Directory : ItemData {
    AbstractCloudProvider::Directory item;
  };

  struct File : ItemData {
    AbstractCloudProvider::File item;
  };

  struct Root {
    std::string id;
    std::string name;
  };

  using Item = std::variant<File, Directory, Root>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct FileContent {
    Generator<std::string> data;
    std::optional<int64_t> size;
  };

  static inline constexpr std::string_view kId = "merged";

  class CloudProvider;
};

class MergedCloudProvider::CloudProvider
    : public coro::cloudstorage::CloudProvider<MergedCloudProvider,
                                               CloudProvider> {
 public:
  bool IsFileContentSizeRequired(const Directory &d) const;
  bool IsFileContentSizeRequired(const Root &d) const;

  void AddAccount(std::string id, AbstractCloudProvider::CloudProvider *p);

  void RemoveAccount(AbstractCloudProvider::CloudProvider *p);

  Task<Root> GetRoot(stdx::stop_token) const;

  Task<PageData> ListDirectoryPage(Root directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token);

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token);

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token);

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token);

  template <typename ItemT>
  Task<ItemT> RenameItem(ItemT item, std::string new_name,
                         stdx::stop_token stop_token);

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token);

  template <typename ItemT>
  Task<> RemoveItem(ItemT item, stdx::stop_token stop_token);

  template <typename ItemT>
  Task<ItemT> MoveItem(ItemT source, Directory destination,
                       stdx::stop_token stop_token);

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token);

 private:
  struct Account {
    std::string id;
    AbstractCloudProvider::CloudProvider *provider;
    stdx::stop_source stop_source;
  };

  Account *GetAccount(std::string_view id);
  const Account *GetAccount(std::string_view id) const;

  std::vector<Account> accounts_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_MERGED_CLOUD_PROVIDER_H
