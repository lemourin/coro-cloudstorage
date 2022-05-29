#ifndef CORO_CLOUDSTORAGE_FUSE_MERGED_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_FUSE_MERGED_CLOUD_PROVIDER_H

#include <iostream>

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/util/stop_token_or.h"
#include "coro/when_all.h"

namespace coro::cloudstorage::util {

namespace internal {

template <typename List, typename Key>
struct TypeAt;

template <typename HeadKey, typename HeadValue, typename... Ts, typename Key>
struct TypeAt<coro::util::TypeList<std::pair<HeadKey, HeadValue>, Ts...>, Key>
    : std::conditional_t<std::is_same_v<HeadKey, Key>,
                         std::type_identity<HeadValue>,
                         TypeAt<coro::util::TypeList<Ts...>, Key>> {};

template <typename List, typename Key>
using TypeAtT = typename TypeAt<List, Key>::type;

}  // namespace internal

template <typename>
struct MergedCloudProvider;

template <typename... CloudProviders>
struct MergedCloudProvider<coro::util::TypeList<CloudProviders...>> {
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
    template <typename T, typename CloudProvider>
    struct IsDirectoryT : std::bool_constant<IsDirectory<T, CloudProvider>> {};

    coro::util::FromTypeListT<
        std::variant, coro::util::ConcatT<coro::util::FilterT<
                          IsDirectoryT, typename CloudProviders::ItemTypeList,
                          CloudProviders>...>>
        item;
  };

  struct File : ItemData {
    template <typename T, typename CloudProvider>
    struct IsFileT : std::bool_constant<IsFile<T, CloudProvider>> {};

    coro::util::FromTypeListT<
        std::variant,
        coro::util::ConcatT<coro::util::FilterT<
            IsFileT, typename CloudProviders::ItemTypeList, CloudProviders>...>>
        item;
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

  class CloudProvider;
};

template <typename... CloudProviders>
class MergedCloudProvider<
    coro::util::TypeList<CloudProviders...>>::CloudProvider
    : public coro::cloudstorage::CloudProvider<MergedCloudProvider,
                                               CloudProvider> {
 public:
  template <typename DirectoryT>
  static inline constexpr bool IsFileContentSizeRequired(const DirectoryT &d) {
    return std::visit(
        []<typename Directory>(const Directory &d) {
          return ItemToCloudProviderT<Directory>::IsFileContentSizeRequired(d);
        },
        d.item);
  }

  template <typename CloudProvider>
  void AddAccount(std::string id, CloudProvider *p) {
    std::cerr << "CREATE " << id << "\n";
    accounts_.push_back(Account{.id = std::move(id), .provider = p});
  }

  template <typename CloudProvider>
  void RemoveAccount(CloudProvider *p) {
    accounts_.erase(
        std::find_if(accounts_.begin(), accounts_.end(), [&](Account &account) {
          return std::visit(
              [&]<typename F>(F *f) {
                if constexpr (std::is_same_v<F, CloudProvider>) {
                  if (f == p) {
                    std::cerr << "REMOVE " << account.id << "\n";
                    account.stop_source.request_stop();
                  }
                  return f == p;
                } else {
                  return false;
                }
              },
              account.provider);
        }));
  }

  Task<Root> GetRoot(stdx::stop_token) const { co_return Root{.id = "root"}; }

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

  template <typename DirectoryT>
  Task<Directory> CreateDirectory(DirectoryT parent, std::string name,
                                  stdx::stop_token stop_token);

  template <typename ItemT>
  Task<> RemoveItem(ItemT item, stdx::stop_token stop_token);

  template <typename ItemT, typename DirectoryT>
  Task<ItemT> MoveItem(ItemT source, DirectoryT destination,
                       stdx::stop_token stop_token);

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token);

 private:
  template <typename T1, typename T2>
  struct MakePair : std::type_identity<std::pair<T1, T2>> {};

  using ItemToCloudProvider = coro::util::ConcatT<coro::util::MapT<
      MakePair, typename CloudProviders::ItemTypeList, CloudProviders>...>;

  template <typename Item>
  using ItemToCloudProviderT = internal::TypeAtT<ItemToCloudProvider, Item>;

  struct Account {
    std::string id;
    std::variant<CloudProviders *...> provider;
    stdx::stop_source stop_source;
  };

  Account *GetAccount(std::string_view id) {
    for (auto &account : accounts_) {
      if (account.id == id) {
        return &account;
      }
    }
    throw CloudException(CloudException::Type::kNotFound);
  }

  template <typename T, typename Entry>
  static T ToItem(std::string account_id, Entry entry) {
    using CloudProviderT = ItemToCloudProviderT<Entry>;

    T item;
    item.account_id = std::move(account_id);
    item.id = util::StrCat(item.account_id, "|", entry.id);
    item.name = entry.name;
    item.timestamp = CloudProviderT::GetTimestamp(entry);
    item.size = CloudProviderT::GetSize(entry);
    item.item = std::move(entry);
    return item;
  }

  std::vector<Account> accounts_;
};

template <typename... CloudProviders>
auto MergedCloudProvider<coro::util::TypeList<CloudProviders...>>::
    CloudProvider::ListDirectoryPage(Root directory,
                                     std::optional<std::string> page_token,
                                     stdx::stop_token stop_token)
        -> Task<PageData> {
  auto get_root = [](Account *account,
                     stdx::stop_token stop_token) -> Task<Directory> {
    co_return co_await std::visit(
        [&](auto *p) -> Task<Directory> {
          coro::util::StopTokenOr stop_token_or(
              account->stop_source.get_token(), std::move(stop_token));
          auto item = co_await p->GetRoot(stop_token_or.GetToken());
          Directory root;
          root.account_id = account->id;
          root.id = account->id;
          root.name = account->id;
          root.item = std::move(item);
          co_return root;
        },
        account->provider);
  };
  std::vector<Task<Directory>> tasks;
  for (Account &account : accounts_) {
    tasks.emplace_back(get_root(&account, stop_token));
  }
  PageData page_data;
  for (auto &d : co_await coro::WhenAll(std::move(tasks))) {
    page_data.items.emplace_back(std::move(d));
  }
  co_return page_data;
}

template <typename... CloudProviders>
auto MergedCloudProvider<coro::util::TypeList<CloudProviders...>>::
    CloudProvider::ListDirectoryPage(Directory directory,
                                     std::optional<std::string> page_token,
                                     stdx::stop_token stop_token)
        -> Task<PageData> {
  co_return co_await std::visit(
      [&]<typename DirectoryT>(const DirectoryT &d) -> Task<PageData> {
        using CloudProviderT = ItemToCloudProviderT<DirectoryT>;
        auto *account = GetAccount(directory.account_id);
        auto *p = std::get<CloudProviderT *>(account->provider);
        coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                              std::move(stop_token));
        auto page_data = co_await p->ListDirectoryPage(
            d, std::move(page_token), stop_token_or.GetToken());
        PageData result{.next_page_token =
                            std::move(page_data.next_page_token)};
        for (auto &item : page_data.items) {
          result.items.emplace_back(std::visit(
              [&]<typename CloudItem>(CloudItem &item) -> Item {
                if constexpr (IsDirectory<CloudItem, CloudProviderT>) {
                  return ToItem<Directory>(directory.account_id,
                                           std::move(item));
                } else {
                  static_assert(IsFile<CloudItem, CloudProviderT>);
                  return ToItem<File>(directory.account_id, std::move(item));
                }
              },
              item));
        }
        co_return result;
      },
      directory.item);
}

template <typename... CloudProviders>
auto MergedCloudProvider<coro::util::TypeList<CloudProviders...>>::
    CloudProvider::GetGeneralData(stdx::stop_token stop_token)
        -> Task<GeneralData> {
  struct VolumeData {
    std::optional<int64_t> space_used;
    std::optional<int64_t> space_total;
  };
  auto get_volume_data = [&](Account &account) -> Task<VolumeData> {
    co_return co_await std::visit(
        [&](auto *provider) -> Task<VolumeData> {
          coro::util::StopTokenOr stop_token_or(account.stop_source.get_token(),
                                                std::move(stop_token));
          auto data =
              co_await provider->GetGeneralData(stop_token_or.GetToken());
          if constexpr (HasUsageData<decltype(data)>) {
            co_return VolumeData{.space_used = data.space_used,
                                 .space_total = data.space_total};
          } else {
            co_return VolumeData{.space_used = 0, .space_total = 0};
          }
        },
        account.provider);
  };
  std::vector<Task<VolumeData>> tasks;
  for (auto &account : accounts_) {
    tasks.emplace_back(get_volume_data(account));
  }
  GeneralData total = {.space_used = 0, .space_total = 0};
  for (const auto &data : co_await coro::WhenAll(std::move(tasks))) {
    if (data.space_used && total.space_used) {
      *total.space_used += *data.space_used;
    } else {
      total.space_used = std::nullopt;
    }
    if (data.space_total && total.space_total) {
      *total.space_total += *data.space_total;
    } else {
      total.space_total = std::nullopt;
    }
  }
  co_return total;
}

template <typename... CloudProviders>
Generator<std::string>
MergedCloudProvider<coro::util::TypeList<CloudProviders...>>::CloudProvider::
    GetFileContent(File file, http::Range range, stdx::stop_token stop_token) {
  auto *account = GetAccount(file.account_id);
  coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                        std::move(stop_token));
  auto generator = std::visit(
      [&]<typename FileT>(FileT d) mutable -> Generator<std::string> {
        using CloudProviderT = ItemToCloudProviderT<FileT>;
        return std::get<CloudProviderT *>(account->provider)
            ->GetFileContent(std::move(d), range, stop_token_or.GetToken());
      },
      file.item);
  FOR_CO_AWAIT(std::string & chunk, generator) { co_yield std::move(chunk); }
}

template <typename... CloudProviders>
template <typename ItemT>
auto MergedCloudProvider<coro::util::TypeList<CloudProviders...>>::
    CloudProvider::RenameItem(ItemT item, std::string new_name,
                              stdx::stop_token stop_token) -> Task<ItemT> {
  if constexpr (std::is_same_v<ItemT, Root>) {
    throw std::runtime_error("can't rename root");
  } else {
    co_return co_await std::visit(
        [&]<typename FileT>(const FileT &d) -> Task<ItemT> {
          using CloudProviderT = ItemToCloudProviderT<FileT>;
          if constexpr (CanRename<FileT, CloudProviderT>) {
            auto *account = GetAccount(item.account_id);
            auto *p = std::get<CloudProviderT *>(account->provider);
            coro::util::StopTokenOr stop_token_or(
                account->stop_source.get_token(), std::move(stop_token));
            co_return ToItem<ItemT>(
                item.account_id,
                co_await p->RenameItem(d, std::move(new_name),
                                       stop_token_or.GetToken()));
          } else {
            throw CloudException("can't rename item");
          }
        },
        item.item);
  }
}

template <typename... CloudProviders>
template <typename DirectoryT>
auto MergedCloudProvider<coro::util::TypeList<CloudProviders...>>::
    CloudProvider::CreateDirectory(DirectoryT parent, std::string name,
                                   stdx::stop_token stop_token)
        -> Task<Directory> {
  if constexpr (std::is_same_v<DirectoryT, Root>) {
    throw CloudException("can't create directory directly under root");
  } else {
    co_return co_await std::visit(
        [&]<typename FileT>(const FileT &d) -> Task<Directory> {
          using CloudProviderT = ItemToCloudProviderT<FileT>;
          if constexpr (CanCreateDirectory<FileT, CloudProviderT>) {
            auto *account = GetAccount(parent.account_id);
            auto *p = std::get<CloudProviderT *>(account->provider);
            coro::util::StopTokenOr stop_token_or(
                account->stop_source.get_token(), std::move(stop_token));
            co_return ToItem<Directory>(
                parent.account_id,
                co_await p->CreateDirectory(d, std::move(name),
                                            stop_token_or.GetToken()));
          } else {
            throw CloudException("create directory not supported");
          }
        },
        parent.item);
  }
}

template <typename... CloudProviders>
template <typename ItemT>
auto MergedCloudProvider<coro::util::TypeList<CloudProviders...>>::
    CloudProvider::RemoveItem(ItemT item, stdx::stop_token stop_token)
        -> Task<> {
  if constexpr (std::is_same_v<ItemT, Root>) {
    throw CloudException("can't remove root");
  } else {
    co_return co_await std::visit(
        [&]<typename FileT>(const FileT &d) -> Task<> {
          using CloudProviderT = ItemToCloudProviderT<FileT>;
          if constexpr (CanRemove<FileT, CloudProviderT>) {
            auto *account = GetAccount(item.account_id);
            auto *p = std::get<CloudProviderT *>(account->provider);
            coro::util::StopTokenOr stop_token_or(
                account->stop_source.get_token(), std::move(stop_token));
            co_return co_await p->RemoveItem(d, stop_token_or.GetToken());
          } else {
            throw CloudException("can't remove item");
          }
        },
        item.item);
  }
}

template <typename... CloudProviders>
template <typename ItemT, typename DirectoryT>
auto MergedCloudProvider<coro::util::TypeList<CloudProviders...>>::
    CloudProvider::MoveItem(ItemT source, DirectoryT destination,
                            stdx::stop_token stop_token) -> Task<ItemT> {
  if constexpr (std::is_same_v<ItemT, Root>) {
    throw CloudException("can't move root");
  } else if constexpr (std::is_same_v<DirectoryT, Root>) {
    throw CloudException("can't move directly under root");
  } else {
    co_return co_await std::visit(
        [&]<typename SourceT, typename DestinationT>(
            const SourceT &nsource,
            const DestinationT &ndestination) -> Task<ItemT> {
          using CloudProviderT = ItemToCloudProviderT<SourceT>;
          if constexpr (!std::is_same_v<CloudProviderT,
                                        ItemToCloudProviderT<DestinationT>>) {
            throw CloudException("can't move between accounts");
          } else if constexpr (CanMove<SourceT, DestinationT, CloudProviderT>) {
            auto *account = GetAccount(source.account_id);
            auto *p = std::get<CloudProviderT *>(account->provider);
            coro::util::StopTokenOr stop_token_or(
                account->stop_source.get_token(), std::move(stop_token));
            co_return ToItem<ItemT>(
                source.account_id,
                co_await p->MoveItem(nsource, ndestination,
                                     stop_token_or.GetToken()));
          } else {
            throw CloudException("can't move item");
          }
        },
        source.item, destination.item);
  }
}

template <typename... CloudProviders>
auto MergedCloudProvider<coro::util::TypeList<CloudProviders...>>::
    CloudProvider::CreateFile(Directory parent, std::string_view name,
                              FileContent content, stdx::stop_token stop_token)
        -> Task<File> {
  co_return co_await std::visit(
      [&]<typename Directory>(const Directory &d) -> Task<File> {
        using CloudProviderT = ItemToCloudProviderT<Directory>;
        if constexpr (CanCreateFile<Directory, CloudProviderT>) {
          auto *account = GetAccount(parent.account_id);
          auto *p = std::get<CloudProviderT *>(account->provider);
          coro::util::StopTokenOr stop_token_or(
              account->stop_source.get_token(), std::move(stop_token));
          typename CloudProviderT::FileContent ncontent;
          ncontent.data = std::move(content.data);
          if constexpr (std::is_convertible_v<decltype(ncontent.size),
                                              int64_t>) {
            ncontent.size = content.size.value();
          } else {
            ncontent.size = content.size;
          }
          co_return ToItem<File>(
              parent.account_id,
              co_await p->CreateFile(d, name, std::move(ncontent),
                                     stop_token_or.GetToken()));
        } else {
          throw CloudException("can't create file");
        }
      },
      parent.item);
}

struct MergedCloudProvider2 {
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

  class CloudProvider;
};

class MergedCloudProvider2::CloudProvider
    : public coro::cloudstorage::CloudProvider<MergedCloudProvider2,
                                               CloudProvider> {
 public:
  bool IsFileContentSizeRequired(const Directory &d) const;

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

  template <typename DirectoryT>
  Task<Directory> CreateDirectory(DirectoryT parent, std::string name,
                                  stdx::stop_token stop_token);

  template <typename ItemT>
  Task<> RemoveItem(ItemT item, stdx::stop_token stop_token);

  template <typename ItemT, typename DirectoryT>
  Task<ItemT> MoveItem(ItemT source, DirectoryT destination,
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
