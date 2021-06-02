#ifndef CORO_CLOUDSTORAGE_UTIL_ABSTRACT_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_UTIL_ABSTRACT_CLOUD_PROVIDER_H

#include <coro/cloudstorage/cloud_exception.h>
#include <coro/cloudstorage/cloud_provider.h>
#include <coro/generator.h>
#include <coro/http/http.h>
#include <coro/stdx/stop_token.h>
#include <coro/task.h>
#include <coro/util/type_list.h>

#include <any>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace coro::cloudstorage::util {

class AbstractCloudProvider {
 private:
  struct ItemData {
    std::string id;
    std::string name;
    std::optional<int64_t> size;
    std::optional<int64_t> timestamp;
    std::any impl;
  };

 public:
  struct File : ItemData {
    std::string mime_type;
  };

  struct Directory : ItemData {};

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct GeneralData {
    std::string username;
    std::optional<int64_t> space_used;
    std::optional<int64_t> space_total;
  };

  struct FileContent {
    Generator<std::string> data;
    std::optional<int64_t> size;
  };

  class CloudProvider;

  template <typename T>
  class CloudProviderImpl;
};

class AbstractCloudProvider::CloudProvider
    : public coro::cloudstorage::CloudProvider<AbstractCloudProvider,
                                               CloudProvider> {
 public:
  virtual intptr_t GetId() const = 0;

  virtual Task<Directory> GetRoot(stdx::stop_token) const = 0;

  virtual bool IsFileContentSizeRequired(const Directory&) const = 0;

  virtual Task<PageData> ListDirectoryPage(
      Directory directory, std::optional<std::string> page_token,
      stdx::stop_token stop_token) const = 0;

  virtual Task<GeneralData> GetGeneralData(stdx::stop_token) const = 0;

  virtual Generator<std::string> GetFileContent(
      File file, http::Range range, stdx::stop_token stop_token) const = 0;

  virtual Task<Directory> RenameItem(Directory item, std::string new_name,
                                     stdx::stop_token stop_token) const = 0;
  virtual Task<File> RenameItem(File item, std::string new_name,
                                stdx::stop_token stop_token) const = 0;

  virtual Task<Directory> CreateDirectory(
      Directory parent, std::string name,
      stdx::stop_token stop_token) const = 0;

  virtual Task<> RemoveItem(Directory item,
                            stdx::stop_token stop_token) const = 0;
  virtual Task<> RemoveItem(File item, stdx::stop_token stop_token) const = 0;

  virtual Task<File> MoveItem(File source, Directory destination,
                              stdx::stop_token stop_token) const = 0;
  virtual Task<Directory> MoveItem(Directory source, Directory destination,
                                   stdx::stop_token stop_token) const = 0;

  virtual Task<File> CreateFile(Directory parent, std::string_view name,
                                FileContent content,
                                stdx::stop_token stop_token) const = 0;
};

template <typename CloudProviderT>
class AbstractCloudProvider::CloudProviderImpl
    : public AbstractCloudProvider::CloudProvider {
 public:
  explicit CloudProviderImpl(CloudProviderT* provider) : provider_(provider) {}

  intptr_t GetId() const override {
    return reinterpret_cast<intptr_t>(provider_);
  }

  Task<Directory> GetRoot(stdx::stop_token stop_token) const override {
    co_return Convert<Directory>(
        co_await provider_->GetRoot(std::move(stop_token)));
  }

  bool IsFileContentSizeRequired(const Directory& d) const override {
    return std::visit(
        [&]<typename Item>(const Item& directory) -> bool {
          if constexpr (IsDirectory<Item, CloudProviderT>) {
            return provider_->IsFileContentSizeRequired(directory);
          } else {
            throw CloudException("not a directory");
          }
        },
        std::any_cast<const ItemT&>(d.impl));
  }

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) const override {
    co_return co_await std::visit(
        [&]<typename DirectoryT>(DirectoryT directory) -> Task<PageData> {
          if constexpr (IsDirectory<DirectoryT, CloudProviderT>) {
            auto page = co_await provider_->ListDirectoryPage(
                directory, std::move(page_token), std::move(stop_token));

            PageData result;
            result.next_page_token = std::move(page.next_page_token);
            for (auto& p : page.items) {
              result.items.emplace_back(std::visit(
                  [&]<typename ItemT>(ItemT& entry) -> Item {
                    if constexpr (IsFile<ItemT, CloudProviderT>) {
                      return Convert<File>(std::move(entry));
                    } else {
                      static_assert(IsDirectory<ItemT, CloudProviderT>);
                      return Convert<Directory>(std::move(entry));
                    }
                  },
                  p));
            }

            co_return result;
          } else {
            throw CloudException("not a directory");
          }
        },
        std::any_cast<ItemT&&>(std::move(directory.impl)));
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) const override {
    auto data = co_await provider_->GetGeneralData(std::move(stop_token));
    GeneralData result;
    result.username = std::move(data.username);
    if constexpr (HasUsageData<decltype(data)>) {
      result.space_used = data.space_used;
      result.space_total = data.space_total;
    }
    co_return result;
  }

  Generator<std::string> GetFileContent(
      File file, http::Range range,
      stdx::stop_token stop_token) const override {
    return std::visit(
        [&]<typename File>(File item) -> Generator<std::string> {
          if constexpr (IsFile<File, CloudProviderT>) {
            return provider_->GetFileContent(std::move(item), range,
                                             std::move(stop_token));
          } else {
            throw CloudException("not a file");
          }
        },
        std::any_cast<ItemT&&>(std::move(file.impl)));
  }

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token) const override {
    co_return co_await std::visit(
        [&]<typename DirectoryT>(DirectoryT parent) -> Task<Directory> {
          if constexpr (CanCreateDirectory<DirectoryT, CloudProviderT>) {
            co_return Convert<Directory>(co_await provider_->CreateDirectory(
                std::move(parent), std::move(name), std::move(stop_token)));
          } else {
            throw CloudException("can't create directory");
          }
        },
        std::any_cast<ItemT&&>(std::move(parent.impl)));
  }

  Task<Directory> RenameItem(Directory item, std::string new_name,
                             stdx::stop_token stop_token) const override {
    return Rename(std::move(item), std::move(new_name), std::move(stop_token));
  }

  Task<File> RenameItem(File item, std::string new_name,
                        stdx::stop_token stop_token) const override {
    return Rename(std::move(item), std::move(new_name), std::move(stop_token));
  }

  Task<> RemoveItem(Directory item,
                    stdx::stop_token stop_token) const override {
    return Remove(std::move(item), std::move(stop_token));
  }

  Task<> RemoveItem(File item, stdx::stop_token stop_token) const override {
    return Remove(std::move(item), std::move(stop_token));
  }

  Task<File> MoveItem(File source, Directory destination,
                      stdx::stop_token stop_token) const override {
    return Move(std::move(source), std::move(destination),
                std::move(stop_token));
  }

  Task<Directory> MoveItem(Directory source, Directory destination,
                           stdx::stop_token stop_token) const override {
    return Move(std::move(source), std::move(destination),
                std::move(stop_token));
  }

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content,
                        stdx::stop_token stop_token) const override {
    co_return co_await std::visit(
        [&]<typename DirectoryT>(DirectoryT parent) -> Task<File> {
          if constexpr (CanCreateFile<DirectoryT, CloudProviderT>) {
            typename CloudProviderT::FileContent ncontent;
            ncontent.data = std::move(content.data);
            if constexpr (std::is_convertible_v<decltype(ncontent.size),
                                                int64_t>) {
              ncontent.size = content.size.value();
            } else {
              ncontent.size = content.size;
            }
            co_return Convert<File>(co_await provider_->CreateFile(
                std::move(parent), name, std::move(ncontent),
                std::move(stop_token)));
          } else {
            throw CloudException("can't create file");
          }
        },
        std::any_cast<ItemT&&>(std::move(parent.impl)));
  }

 private:
  using ItemT = typename CloudProviderT::Item;

  template <typename Item>
  Task<Item> Rename(Item item, std::string new_name,
                    stdx::stop_token stop_token) const {
    co_return co_await std::visit(
        [&]<typename Entry>(Entry entry) -> Task<Item> {
          if constexpr (CanRename<Entry, CloudProviderT>) {
            co_return Convert<Item>(co_await provider_->RenameItem(
                std::move(entry), std::move(new_name), std::move(stop_token)));
          } else {
            throw CloudException("can't rename");
          }
        },
        std::any_cast<ItemT&&>(std::move(item.impl)));
  }

  template <typename Item>
  Task<> Remove(Item item, stdx::stop_token stop_token) const {
    co_return co_await std::visit(
        [&]<typename Entry>(Entry entry) -> Task<> {
          if constexpr (CanRemove<Entry, CloudProviderT>) {
            co_await provider_->RemoveItem(std::move(entry),
                                           std::move(stop_token));
          } else {
            throw CloudException("can't remove");
          }
        },
        std::any_cast<ItemT&&>(std::move(item.impl)));
  }

  template <typename Item>
  Task<Item> Move(Item source, Directory destination,
                  stdx::stop_token stop_token) const {
    co_return co_await std::visit(
        [&]<typename SourceT, typename DestinationT>(
            SourceT source, DestinationT destination) -> Task<Item> {
          if constexpr (CanMove<SourceT, DestinationT, CloudProviderT>) {
            co_return Convert<Item>(co_await provider_->MoveItem(
                std::move(source), std::move(destination),
                std::move(stop_token)));
          } else {
            throw CloudException("can't move");
          }
        },
        std::any_cast<ItemT&&>(std::move(source.impl)),
        std::any_cast<ItemT&&>(std::move(destination.impl)));
  }

  template <typename To, typename From>
  static To Convert(From d) {
    To result;
    result.id = [&] {
      std::stringstream stream;
      stream << d.id;
      return std::move(stream).str();
    }();
    result.name = d.name;
    result.size = CloudProviderT::GetSize(d);
    result.timestamp = CloudProviderT::GetTimestamp(d);
    if constexpr (IsFile<From, CloudProvider>) {
      result.mime_type = CloudProviderT::GetMimetype(d);
    }
    result.impl = ItemT(std::move(d));
    return result;
  }

  CloudProviderT* provider_;
};

}  // namespace coro::cloudstorage::util

#endif