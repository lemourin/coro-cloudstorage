#include "coro/cloudstorage/util/merged_cloud_provider.h"

#include <iostream>

namespace coro::cloudstorage::util {

namespace {

template <typename T, typename Entry>
T ToItem(std::string account_id, Entry entry) {
  T item;
  item.account_id = std::move(account_id);
  item.id = util::StrCat(item.account_id, "|", entry.id);
  item.name = entry.name;
  item.timestamp = entry.timestamp;
  item.size = entry.size;
  item.item = std::move(entry);
  return item;
}

}  // namespace

bool MergedCloudProvider::CloudProvider::IsFileContentSizeRequired(
    const Directory &d) const {
  auto *account = GetAccount(d.account_id);
  return account->provider->IsFileContentSizeRequired(d.item);
}

bool MergedCloudProvider::CloudProvider::IsFileContentSizeRequired(
    const Root &) const {
  throw CloudException("can't upload into root");
}

void MergedCloudProvider::CloudProvider::AddAccount(
    std::string id, AbstractCloudProvider::CloudProvider *p) {
  std::cerr << "CREATE " << id << "\n";
  accounts_.push_back(Account{.id = std::move(id), .provider = p});
}

void MergedCloudProvider::CloudProvider::RemoveAccount(
    AbstractCloudProvider::CloudProvider *p) {
  accounts_.erase(
      std::find_if(accounts_.begin(), accounts_.end(), [&](Account &account) {
        if (account.provider == p) {
          std::cerr << "REMOVE " << account.id << "\n";
          return true;
        } else {
          return false;
        }
      }));
}

auto MergedCloudProvider::CloudProvider::GetRoot(stdx::stop_token) const
    -> Task<Root> {
  co_return Root{.id = "root"};
}

auto MergedCloudProvider::CloudProvider::ListDirectoryPage(
    Root directory, std::optional<std::string> page_token,
    stdx::stop_token stop_token) -> Task<PageData> {
  auto get_root = [](Account *account,
                     stdx::stop_token stop_token) -> Task<Directory> {
    coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                          std::move(stop_token));
    auto item = co_await account->provider->GetRoot(stop_token_or.GetToken());
    Directory root;
    root.account_id = account->id;
    root.id = account->id;
    root.name = account->id;
    root.item = std::move(item);
    co_return root;
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

auto MergedCloudProvider::CloudProvider::ListDirectoryPage(
    Directory directory, std::optional<std::string> page_token,
    stdx::stop_token stop_token) -> Task<PageData> {
  auto *account = GetAccount(directory.account_id);
  auto *p = account->provider;
  coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                        std::move(stop_token));
  auto page_data = co_await p->ListDirectoryPage(
      directory.item, std::move(page_token), stop_token_or.GetToken());
  PageData result{.next_page_token = std::move(page_data.next_page_token)};
  for (auto &item : page_data.items) {
    result.items.emplace_back(std::visit(
        [&]<typename CloudItem>(CloudItem &item) -> Item {
          if constexpr (std::is_same_v<AbstractCloudProvider::Directory,
                                       CloudItem>) {
            return ToItem<Directory>(directory.account_id, std::move(item));
          } else {
            static_assert(
                std::is_same_v<AbstractCloudProvider::File, CloudItem>);
            return ToItem<File>(directory.account_id, std::move(item));
          }
        },
        item));
  }
  co_return result;
}

auto MergedCloudProvider::CloudProvider::GetGeneralData(
    stdx::stop_token stop_token) -> Task<GeneralData> {
  auto get_volume_data =
      [&](Account &account) -> Task<AbstractCloudProvider::GeneralData> {
    coro::util::StopTokenOr stop_token_or(account.stop_source.get_token(),
                                          std::move(stop_token));
    co_return co_await account.provider->GetGeneralData(
        stop_token_or.GetToken());
  };
  std::vector<Task<AbstractCloudProvider::GeneralData>> tasks;
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

Generator<std::string> MergedCloudProvider::CloudProvider::GetFileContent(
    File file, http::Range range, stdx::stop_token stop_token) {
  auto *account = GetAccount(file.account_id);
  coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                        std::move(stop_token));
  auto generator = account->provider->GetFileContent(
      std::move(file.item), range, stop_token_or.GetToken());
  FOR_CO_AWAIT(std::string & chunk, generator) { co_yield std::move(chunk); }
}

template <typename ItemT>
Task<ItemT> MergedCloudProvider::CloudProvider::RenameItem(
    ItemT item, std::string new_name, stdx::stop_token stop_token) {
  if constexpr (std::is_same_v<ItemT, Root>) {
    throw std::runtime_error("can't rename root");
  } else {
    auto *account = GetAccount(item.account_id);
    auto *p = account->provider;
    coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                          std::move(stop_token));
    co_return ToItem<ItemT>(
        item.account_id,
        co_await p->RenameItem(std::move(item.item), std::move(new_name),
                               stop_token_or.GetToken()));
  }
}

auto MergedCloudProvider::CloudProvider::CreateDirectory(
    Directory parent, std::string name, stdx::stop_token stop_token)
    -> Task<Directory> {
  auto *account = GetAccount(parent.account_id);
  auto *p = account->provider;
  coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                        std::move(stop_token));
  co_return ToItem<Directory>(
      parent.account_id,
      co_await p->CreateDirectory(std::move(parent.item), std::move(name),
                                  stop_token_or.GetToken()));
}

template <typename ItemT>
Task<> MergedCloudProvider::CloudProvider::RemoveItem(
    ItemT item, stdx::stop_token stop_token) {
  if constexpr (std::is_same_v<ItemT, Root>) {
    throw CloudException("can't remove root");
  } else {
    auto *account = GetAccount(item.account_id);
    auto *p = account->provider;
    coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                          std::move(stop_token));
    co_await p->RemoveItem(std::move(item.item), stop_token_or.GetToken());
  }
}

template <typename ItemT>
Task<ItemT> MergedCloudProvider::CloudProvider::MoveItem(
    ItemT source, Directory destination, stdx::stop_token stop_token) {
  if constexpr (std::is_same_v<ItemT, Root>) {
    throw CloudException("can't move root");
  } else if (source.account_id != destination.account_id) {
    throw CloudException("can't move between accounts");
  } else {
    auto *account = GetAccount(source.account_id);
    auto *p = account->provider;
    coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                          std::move(stop_token));
    co_return ToItem<ItemT>(source.account_id,
                            co_await p->MoveItem(std::move(source.item),
                                                 std::move(destination.item),
                                                 stop_token_or.GetToken()));
  }
}

auto MergedCloudProvider::CloudProvider::CreateFile(Directory parent,
                                                    std::string_view name,
                                                    FileContent content,
                                                    stdx::stop_token stop_token)
    -> Task<File> {
  auto *account = GetAccount(parent.account_id);
  auto *p = account->provider;
  coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                        std::move(stop_token));
  AbstractCloudProvider::FileContent ncontent{.data = std::move(content.data),
                                              .size = content.size};
  co_return ToItem<File>(
      parent.account_id,
      co_await p->CreateFile(std::move(parent.item), name, std::move(ncontent),
                             stop_token_or.GetToken()));
}

auto MergedCloudProvider::CloudProvider::GetAccount(std::string_view id)
    -> Account * {
  for (auto &account : accounts_) {
    if (account.id == id) {
      return &account;
    }
  }
  throw CloudException(CloudException::Type::kNotFound);
}

auto MergedCloudProvider::CloudProvider::GetAccount(std::string_view id) const
    -> const Account * {
  return const_cast<CloudProvider *>(this)->GetAccount(id);
}

template auto MergedCloudProvider::CloudProvider::RenameItem(
    File item, std::string new_name, stdx::stop_token stop_token) -> Task<File>;

template auto MergedCloudProvider::CloudProvider::RenameItem(
    Directory item, std::string new_name, stdx::stop_token stop_token)
    -> Task<Directory>;

template auto MergedCloudProvider::CloudProvider::RenameItem(
    Root item, std::string new_name, stdx::stop_token stop_token) -> Task<Root>;

template auto MergedCloudProvider::CloudProvider::MoveItem(File, Directory,
                                                           stdx::stop_token)
    -> Task<File>;

template auto MergedCloudProvider::CloudProvider::MoveItem(Directory, Directory,
                                                           stdx::stop_token)
    -> Task<Directory>;

template auto MergedCloudProvider::CloudProvider::MoveItem(Root, Directory,
                                                           stdx::stop_token)
    -> Task<Root>;

template Task<> MergedCloudProvider::CloudProvider::RemoveItem(
    File, stdx::stop_token);

template Task<> MergedCloudProvider::CloudProvider::RemoveItem(
    Directory, stdx::stop_token);

template Task<> MergedCloudProvider::CloudProvider::RemoveItem(
    Root, stdx::stop_token);

}  // namespace coro::cloudstorage::util