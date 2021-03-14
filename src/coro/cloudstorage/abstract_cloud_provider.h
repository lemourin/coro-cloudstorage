#ifndef CORO_CLOUDSTORAGE_ABSTRACT_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_ABSTRACT_CLOUD_PROVIDER_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/stdx/stop_token.h>
#include <coro/util/type_list.h>

#include <any>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <variant>

namespace coro::cloudstorage {

namespace internal {

template <typename List, typename Key>
struct TypeAt;

template <typename HeadKey, typename HeadValue, typename... Ts, typename Key>
struct TypeAt<::coro::util::TypeList<std::pair<HeadKey, HeadValue>, Ts...>, Key>
    : std::conditional_t<std::is_same_v<HeadKey, Key>,
                         std::type_identity<HeadValue>,
                         TypeAt<::coro::util::TypeList<Ts...>, Key>> {};

template <typename List, typename Key>
using TypeAtT = typename TypeAt<List, Key>::type;

}  // namespace internal

template <typename>
class AbstractCloudProvider;

template <typename... Ts>
class AbstractCloudProvider<::coro::util::TypeList<Ts...>> {
 public:
  struct GenericItem {
    std::string id;
    std::string name;
    std::optional<int64_t> timestamp;
    std::optional<int64_t> size;
    enum class Type { kFile, kDirectory } type;
  };

  using Item = std::variant<typename Ts::Item...>;

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

template <typename... Ts>
class AbstractCloudProvider<::coro::util::TypeList<Ts...>>::CloudProvider
    : public ::coro::cloudstorage::CloudProvider<AbstractCloudProvider,
                                                 CloudProvider> {
 public:
  template <typename T>
  explicit CloudProvider(T* impl) : impl_(impl) {}

  intptr_t id() const {
    return std::visit([](auto* p) { return reinterpret_cast<intptr_t>(p); },
                      impl_);
  }

  auto type() const { return impl_.index(); }

  Task<Item> GetRoot(stdx::stop_token stop_token) {
    auto get_root = [&](auto* p) -> Task<Item> {
      co_return co_await p->GetRoot(std::move(stop_token));
    };
    co_return co_await std::visit(get_root, impl_);
  }

  Task<PageData> ListDirectoryPage(Item directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
    auto list_directory_page = [&](auto* p) -> Task<PageData> {
      using CloudProviderT = std::remove_pointer_t<decltype(p)>;
      using ItemT = typename CloudProviderT::Item;

      auto list_directory_page = [&](auto& d) -> Task<PageData> {
        if constexpr (IsDirectory<decltype(d), CloudProviderT>) {
          auto page_data = co_await p->ListDirectoryPage(
              std::move(d), std::move(page_token), std::move(stop_token));
          PageData result = {.next_page_token =
                                 std::move(page_data.next_page_token)};
          std::copy(page_data.items.begin(), page_data.items.end(),
                    std::back_inserter(result.items));
          co_return std::move(result);
        } else {
          throw std::runtime_error("not a directory");
        }
      };
      co_return co_await std::visit(list_directory_page,
                                    std::get<ItemT>(directory));
    };
    co_return co_await std::visit(list_directory_page, impl_);
  }

  Generator<std::string> GetFileContent(Item file, http::Range range,
                                        stdx::stop_token stop_token) {
    return std::visit(
        [&](auto* d) {
          using CloudProviderT = std::remove_pointer_t<decltype(d)>;
          using ItemT = typename CloudProviderT::Item;

          return std::visit(
              [&](auto& item) -> Generator<std::string> {
                if constexpr (IsFile<decltype(item), CloudProviderT>) {
                  return d->GetFileContent(std::move(item), range,
                                           std::move(stop_token));
                } else {
                  throw std::invalid_argument("not a file");
                }
              },
              std::get<ItemT>(file));
        },
        impl_);
  }

  Task<Item> CreateDirectory(Item parent, std::string_view name,
                             stdx::stop_token stop_token) {
    auto create_directory = [&](auto* d) -> Task<Item> {
      using CloudProviderT = std::remove_pointer_t<decltype(d)>;
      using ItemT = typename CloudProviderT::Item;

      auto create_directory = [&](auto& item) -> Task<Item> {
        if constexpr (IsDirectory<decltype(item), CloudProviderT>) {
          if constexpr (CanCreateDirectory<decltype(item), CloudProviderT>) {
            co_return co_await d->CreateDirectory(
                std::move(item), std::string(name), std::move(stop_token));
          } else {
            throw std::runtime_error("createdirectory not supported");
          }
        } else {
          throw std::invalid_argument("parent not a directory");
        }
      };
      co_return co_await std::visit(create_directory, std::get<ItemT>(parent));
    };
    co_return co_await std::visit(create_directory, impl_);
  }

  Task<> RemoveItem(Item item, stdx::stop_token stop_token) {
    auto remove_item = [&](auto* d) -> Task<> {
      using CloudProviderT = std::remove_pointer_t<decltype(d)>;
      using ItemT = typename CloudProviderT::Item;

      auto remove_item = [&](auto& item) -> Task<> {
        if constexpr (CanRemove<decltype(item), CloudProviderT>) {
          co_await d->RemoveItem(item, std::move(stop_token));
        } else {
          throw std::runtime_error("remove not supported");
        }
      };
      co_await std::visit(remove_item, std::get<ItemT>(item));
    };
    co_await std::visit(remove_item, impl_);
  }

  Task<Item> RenameItem(Item item, std::string_view new_name,
                        stdx::stop_token stop_token) {
    auto rename_item = [&](auto* d) -> Task<Item> {
      using CloudProviderT = std::remove_pointer_t<decltype(d)>;
      using ItemT = typename CloudProviderT::Item;
      auto rename_item = [&](auto& item) -> Task<Item> {
        if constexpr (CanRename<decltype(item), CloudProviderT>) {
          co_return co_await d->RenameItem(
              std::move(item), std::string(new_name), std::move(stop_token));
        } else {
          throw std::runtime_error("rename not supported");
        }
      };
      co_return co_await std::visit(rename_item, std::get<ItemT>(item));
    };
    co_return co_await std::visit(rename_item, impl_);
  }

  Task<Item> MoveItem(Item source, Item destination,
                      stdx::stop_token stop_token) {
    auto move_item = [&](auto* d) -> Task<Item> {
      using CloudProviderT = std::remove_pointer_t<decltype(d)>;
      using ItemT = typename CloudProviderT::Item;

      auto move_item = [&](auto& destination) -> Task<Item> {
        if constexpr (IsDirectory<decltype(destination), CloudProviderT>) {
          auto move_item = [&](auto& source) -> Task<Item> {
            if constexpr (CanMove<decltype(source), decltype(destination),
                                  CloudProviderT>) {
              co_return co_await d->MoveItem(std::move(source),
                                             std::move(destination),
                                             std::move(stop_token));
            } else {
              throw std::runtime_error("move not supported");
            }
          };
          co_return co_await std::visit(move_item, std::get<ItemT>(source));
        } else {
          throw std::invalid_argument("cannot move into non directory");
        }
      };
      co_return co_await std::visit(move_item, std::get<ItemT>(destination));
    };
    co_return co_await std::visit(move_item, impl_);
  }

  bool IsFileContentSizeRequired() const {
    return std::visit(
        [&](const auto* d) {
          using CloudProviderT = std::remove_pointer_t<decltype(d)>;
          using FileContentT = typename CloudProviderT::FileContent;
          return std::is_convertible_v<
              decltype(std::declval<FileContentT>().size), int64_t>;
        },
        impl_);
  }

  Task<Item> CreateFile(Item parent, std::string_view name, FileContent content,
                        stdx::stop_token stop_token) {
    auto create_file = [&](auto* d) -> Task<Item> {
      using CloudProviderT = std::remove_pointer_t<decltype(d)>;
      using ItemT = typename CloudProviderT::Item;
      using FileContentT = typename CloudProviderT::FileContent;

      auto create_file = [&](auto& parent) -> Task<Item> {
        if constexpr (IsDirectory<decltype(parent), CloudProviderT> &&
                      CanCreateFile<decltype(parent), CloudProviderT>) {
          co_return co_await d->CreateFile(
              std::move(parent), name,
              ToFileContent<FileContentT>(std::move(content)),
              std::move(stop_token));
        } else {
          throw std::invalid_argument("not supported");
        }
      };
      co_return co_await std::visit(create_file, std::get<ItemT>(parent));
    };
    co_return co_await std::visit(create_file, impl_);
  }

  static GenericItem ToGenericItem(Item d) {
    return std::visit(
        [](auto& d) {
          using CloudProviderT =
              ItemToCloudProviderT<std::remove_cvref_t<decltype(d)>>;
          return std::visit(
              [](auto& d) {
                return GenericItem{
                    .id =
                        [&] {
                          std::stringstream stream;
                          stream << std::move(d.id);
                          return std::move(stream.str());
                        }(),
                    .name = std::move(d.name),
                    .timestamp = CloudProviderT::GetTimestamp(d),
                    .size = CloudProviderT::GetSize(d),
                    .type = IsDirectory<decltype(d), CloudProviderT>
                                ? GenericItem::Type::kDirectory
                                : GenericItem::Type::kFile};
              },
              d);
        },
        d);
  }

 private:
  template <typename T>
  static T ToFileContent(FileContent content) {
    if constexpr (std::is_convertible_v<decltype(std::declval<T>().size),
                                        int64_t>) {
      return T{.data = std::move(content.data), .size = content.size.value()};
    } else {
      return T{.data = std::move(content.data), .size = content.size};
    }
  }

  using ItemToCloudProvider =
      ::coro::util::TypeList<std::pair<typename Ts::Item, Ts>...>;

  template <typename Item>
  using ItemToCloudProviderT = internal::TypeAtT<ItemToCloudProvider, Item>;

  std::variant<Ts*...> impl_;
};

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_ABSTRACT_CLOUD_PROVIDER_H