#include "coro/cloudstorage/util/merged_cloud_provider.h"

#include <set>

#include "coro/cloudstorage/util/string_utils.h"

namespace coro::cloudstorage::util {

namespace {

template <typename T, typename Entry>
T ToItem(MergedCloudProvider::AccountId account_id, Entry entry) {
  T item;
  item.account_id = std::move(account_id);
  item.id =
      StrCat(item.account_id.type, '|', item.account_id.id, '|', entry.id);
  item.name = entry.name;
  item.timestamp = entry.timestamp;
  item.size = entry.size;
  item.item = std::move(entry);
  return item;
}

}  // namespace

bool MergedCloudProvider::IsFileContentSizeRequired(const Directory &d) const {
  auto *account = GetAccount(d.account_id);
  return account->provider->IsFileContentSizeRequired(d.item);
}

void MergedCloudProvider::AddAccount(std::string id,
                                     std::shared_ptr<AbstractCloudProvider> p) {
  accounts_.push_back(Account{.id = std::move(id), .provider = std::move(p)});
}

void MergedCloudProvider::RemoveAccount(
    std::shared_ptr<AbstractCloudProvider> p) {
  accounts_.erase(
      std::find_if(accounts_.begin(), accounts_.end(),
                   [&](Account &account) { return account.provider == p; }));
}

auto MergedCloudProvider::GetRoot(stdx::stop_token) const -> Task<Root> {
  co_return Root{.id = "root"};
}

auto MergedCloudProvider::ListDirectoryPage(Root, std::optional<std::string>,
                                            stdx::stop_token)
    -> Task<PageData> {
  std::set<std::string_view> account_type;
  for (const auto &account : accounts_) {
    account_type.insert(account.provider->GetId());
  }
  PageData page_data;
  for (std::string_view type : account_type) {
    page_data.items.emplace_back(
        ProviderTypeRoot{.id = std::string(type), .name = std::string(type)});
  }
  co_return page_data;
}

auto MergedCloudProvider::ListDirectoryPage(ProviderTypeRoot directory,
                                            std::optional<std::string>,
                                            stdx::stop_token stop_token)
    -> Task<PageData> {
  auto get_root = [&](Account *account,
                      stdx::stop_token stop_token) -> Task<Directory> {
    coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                          std::move(stop_token));
    auto item = co_await account->provider->GetRoot(stop_token_or.GetToken());
    Directory root;
    root.account_id = {directory.id, account->id};
    root.id = StrCat(directory.id, '|', account->id);
    root.name = account->id;
    root.item = std::move(item);
    co_return root;
  };
  std::vector<Task<Directory>> tasks;
  for (Account &account : accounts_) {
    if (account.provider->GetId() == directory.id) {
      tasks.emplace_back(get_root(&account, stop_token));
    }
  }
  PageData page_data;
  for (auto &d : co_await coro::WhenAll(std::move(tasks))) {
    page_data.items.emplace_back(std::move(d));
  }
  co_return page_data;
}

auto MergedCloudProvider::ListDirectoryPage(
    Directory directory, std::optional<std::string> page_token,
    stdx::stop_token stop_token) -> Task<PageData> {
  auto *account = GetAccount(directory.account_id);
  auto p = account->provider;
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

auto MergedCloudProvider::GetGeneralData(stdx::stop_token stop_token)
    -> Task<GeneralData> {
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

Generator<std::string> MergedCloudProvider::GetFileContent(
    File file, http::Range range, stdx::stop_token stop_token) {
  auto *account = GetAccount(file.account_id);
  coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                        std::move(stop_token));
  auto generator = account->provider->GetFileContent(
      std::move(file.item), range, stop_token_or.GetToken());
  FOR_CO_AWAIT(std::string & chunk, generator) { co_yield std::move(chunk); }
}

template <typename ItemT, typename>
Task<ItemT> MergedCloudProvider::RenameItem(ItemT item, std::string new_name,
                                            stdx::stop_token stop_token) {
  auto *account = GetAccount(item.account_id);
  auto p = account->provider;
  coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                        std::move(stop_token));
  co_return ToItem<ItemT>(
      item.account_id,
      co_await p->RenameItem(std::move(item.item), std::move(new_name),
                             stop_token_or.GetToken()));
}

auto MergedCloudProvider::CreateDirectory(Directory parent, std::string name,
                                          stdx::stop_token stop_token)
    -> Task<Directory> {
  auto *account = GetAccount(parent.account_id);
  auto p = account->provider;
  coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                        std::move(stop_token));
  co_return ToItem<Directory>(
      parent.account_id,
      co_await p->CreateDirectory(std::move(parent.item), std::move(name),
                                  stop_token_or.GetToken()));
}

template <typename ItemT, typename>
Task<> MergedCloudProvider::RemoveItem(ItemT item,
                                       stdx::stop_token stop_token) {
  auto *account = GetAccount(item.account_id);
  auto p = account->provider;
  coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                        std::move(stop_token));
  co_await p->RemoveItem(std::move(item.item), stop_token_or.GetToken());
}

template <typename ItemT, typename>
Task<ItemT> MergedCloudProvider::MoveItem(ItemT source, Directory destination,
                                          stdx::stop_token stop_token) {
  if (source.account_id != destination.account_id) {
    throw CloudException("can't move between accounts");
  } else {
    auto *account = GetAccount(source.account_id);
    auto p = account->provider;
    coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                          std::move(stop_token));
    co_return ToItem<ItemT>(source.account_id,
                            co_await p->MoveItem(std::move(source.item),
                                                 std::move(destination.item),
                                                 stop_token_or.GetToken()));
  }
}

auto MergedCloudProvider::CreateFile(Directory parent, std::string_view name,
                                     FileContent content,
                                     stdx::stop_token stop_token)
    -> Task<File> {
  auto *account = GetAccount(parent.account_id);
  auto p = account->provider;
  coro::util::StopTokenOr stop_token_or(account->stop_source.get_token(),
                                        std::move(stop_token));
  AbstractCloudProvider::FileContent ncontent{.data = std::move(content.data),
                                              .size = content.size};
  co_return ToItem<File>(
      parent.account_id,
      co_await p->CreateFile(std::move(parent.item), name, std::move(ncontent),
                             stop_token_or.GetToken()));
}

auto MergedCloudProvider::GetAccount(const AccountId &account_id) -> Account * {
  for (auto &account : accounts_) {
    if (account.provider->GetId() == account_id.type &&
        account.id == account_id.id) {
      return &account;
    }
  }
  throw CloudException(CloudException::Type::kNotFound);
}

auto MergedCloudProvider::GetAccount(const AccountId &account_id) const
    -> const Account * {
  return const_cast<MergedCloudProvider *>(this)->GetAccount(account_id);
}

template auto MergedCloudProvider::RenameItem(File item, std::string new_name,
                                              stdx::stop_token stop_token)
    -> Task<File>;

template auto MergedCloudProvider::RenameItem(Directory item,
                                              std::string new_name,
                                              stdx::stop_token stop_token)
    -> Task<Directory>;

template auto MergedCloudProvider::MoveItem(File, Directory, stdx::stop_token)
    -> Task<File>;

template auto MergedCloudProvider::MoveItem(Directory, Directory,
                                            stdx::stop_token)
    -> Task<Directory>;

template Task<> MergedCloudProvider::RemoveItem(File, stdx::stop_token);

template Task<> MergedCloudProvider::RemoveItem(Directory, stdx::stop_token);

}  // namespace coro::cloudstorage::util