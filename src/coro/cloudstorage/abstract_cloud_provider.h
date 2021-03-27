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
    std::optional<std::string> mime_type;
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
      auto list_directory_page =
          BindFront(ListDirectoryPageF{}, p, std::move(page_token),
                    std::move(stop_token));
      co_return co_await std::visit(std::move(list_directory_page),
                                    std::move(std::get<ItemT>(directory)));
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
              BindFront(GetFileContentF{}, d, range, std::move(stop_token)),
              std::move(std::get<ItemT>(file)));
        },
        impl_);
  }

  Task<Item> CreateDirectory(Item parent, std::string_view name,
                             stdx::stop_token stop_token) {
    auto create_directory = [&](auto* d) -> Task<Item> {
      using CloudProviderT = std::remove_pointer_t<decltype(d)>;
      using ItemT = typename CloudProviderT::Item;
      auto create_directory =
          BindFront(CreateDirectoryF{}, d, name, std::move(stop_token));
      co_return co_await std::visit(std::move(create_directory),
                                    std::move(std::get<ItemT>(parent)));
    };
    co_return co_await std::visit(create_directory, impl_);
  }

  Task<> RemoveItem(Item item, stdx::stop_token stop_token) {
    auto remove_item = [&](auto* d) -> Task<> {
      using CloudProviderT = std::remove_pointer_t<decltype(d)>;
      using ItemT = typename CloudProviderT::Item;
      auto remove_item = BindFront(RemoveItemF{}, d, std::move(stop_token));
      co_await std::visit(std::move(remove_item),
                          std::move(std::get<ItemT>(item)));
    };
    co_await std::visit(remove_item, impl_);
  }

  Task<Item> RenameItem(Item item, std::string_view new_name,
                        stdx::stop_token stop_token) {
    auto rename_item = [&](auto* d) -> Task<Item> {
      using CloudProviderT = std::remove_pointer_t<decltype(d)>;
      using ItemT = typename CloudProviderT::Item;
      auto rename_item =
          BindFront(RenameItemF{}, d, new_name, std::move(stop_token));
      co_return co_await std::visit(std::move(rename_item),
                                    std::move(std::get<ItemT>(item)));
    };
    co_return co_await std::visit(rename_item, impl_);
  }

  Task<Item> MoveItem(Item source, Item destination,
                      stdx::stop_token stop_token) {
    auto move_item = [&](auto* d) -> Task<Item> {
      using CloudProviderT = std::remove_pointer_t<decltype(d)>;
      using ItemT = typename CloudProviderT::Item;

      auto move_item = [&](auto& destination) -> Task<Item> {
        auto move_item = BindFront(MoveItemF{}, d, std::move(destination),
                                   std::move(stop_token));
        co_return co_await std::visit(std::move(move_item),
                                      std::move(std::get<ItemT>(source)));
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
      auto create_file =
          BindFront(CreateFileF{}, d, name,
                    ToFileContent<FileContentT>(std::move(content)),
                    std::move(stop_token));
      co_return co_await std::visit(std::move(create_file),
                                    std::move(std::get<ItemT>(parent)));
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
                    .name = d.name,
                    .timestamp = CloudProviderT::GetTimestamp(d),
                    .size = CloudProviderT::GetSize(d),
                    .mime_type = [&] {
                      if constexpr (IsFile<decltype(d), CloudProviderT>) {
                        return CloudProviderT::GetMimeType(d);
                      } else {
                        return std::nullopt;
                      }
                    }(),
                    .type = IsDirectory<decltype(d), CloudProviderT>
                                ? GenericItem::Type::kDirectory
                                : GenericItem::Type::kFile};
              },
              d);
        },
        d);
  }

 private:
  template <typename FD, typename... Args>
  struct BindFrontF {
    FD func;
    std::tuple<std::decay_t<Args>...> args;

    template <typename... CallArgs>
    auto operator()(CallArgs&&... call_args) && {
      return std::apply(
          func,
          std::tuple_cat(std::move(args), std::forward_as_tuple(call_args...)));
    }
  };

  template <typename FD, typename... Args>
  auto BindFront(FD func, Args&&... args) {
    return BindFrontF<FD, Args...>{
        std::move(func), std::make_tuple(std::forward<Args>(args)...)};
  }

  struct RenameItemF {
    template <typename CloudProviderT, typename ItemT>
    Task<Item> operator()(CloudProviderT* d, std::string_view new_name,
                          stdx::stop_token stop_token, ItemT item) const {
      if constexpr (CanRename<ItemT, CloudProviderT>) {
        co_return co_await d->RenameItem(std::move(item), std::string(new_name),
                                         std::move(stop_token));
      } else {
        throw std::runtime_error("rename not supported");
      }
    }
  };

  struct RemoveItemF {
    template <typename CloudProviderT, typename ItemT>
    Task<> operator()(CloudProviderT* d, stdx::stop_token stop_token,
                      ItemT item) const {
      if constexpr (CanRemove<ItemT, CloudProviderT>) {
        co_await d->RemoveItem(item, std::move(stop_token));
      } else {
        throw std::runtime_error("remove not supported");
      }
    }
  };

  struct MoveItemF {
    template <typename CloudProviderT, typename Destination, typename Source>
    Task<Item> operator()(CloudProviderT* d, Destination destination,
                          stdx::stop_token stop_token, Source source) const {
      if constexpr (IsDirectory<decltype(destination), CloudProviderT>) {
        if constexpr (CanMove<Source, Destination, CloudProviderT>) {
          co_return co_await d->MoveItem(
              std::move(source), std::move(destination), std::move(stop_token));
        } else {
          throw std::runtime_error("move not supported");
        }
      } else {
        throw std::invalid_argument("cannot move into non directory");
      }
    }
  };

  struct CreateDirectoryF {
    template <typename CloudProviderT, typename ItemT>
    Task<Item> operator()(CloudProviderT* d, std::string_view name,
                          stdx::stop_token stop_token, ItemT item) const {
      if constexpr (IsDirectory<ItemT, CloudProviderT>) {
        if constexpr (CanCreateDirectory<ItemT, CloudProviderT>) {
          co_return co_await d->CreateDirectory(
              std::move(item), std::string(name), std::move(stop_token));
        } else {
          throw std::runtime_error("createdirectory not supported");
        }
      } else {
        throw std::invalid_argument("parent not a directory");
      }
    }
  };

  struct CreateFileF {
    template <typename CloudProviderT, typename FileContentT, typename ItemT>
    Task<Item> operator()(CloudProviderT* d, std::string_view name,
                          FileContentT content, stdx::stop_token stop_token,
                          ItemT parent) const {
      if constexpr (IsDirectory<ItemT, CloudProviderT> &&
                    CanCreateFile<ItemT, CloudProviderT>) {
        co_return co_await d->CreateFile(
            std::move(parent), name, std::move(content), std::move(stop_token));
      } else {
        throw std::invalid_argument("not supported");
      }
    }
  };

  struct ListDirectoryPageF {
    template <typename CloudProviderT, typename ItemT>
    Task<PageData> operator()(CloudProviderT* d,
                              std::optional<std::string> page_token,
                              stdx::stop_token stop_token, ItemT item) const {
      if constexpr (IsDirectory<ItemT, CloudProviderT>) {
        auto page_data = co_await d->ListDirectoryPage(
            std::move(item), std::move(page_token), std::move(stop_token));
        PageData result = {.next_page_token =
                               std::move(page_data.next_page_token)};
        std::copy(page_data.items.begin(), page_data.items.end(),
                  std::back_inserter(result.items));
        co_return result;
      } else {
        throw std::runtime_error("not a directory");
      }
    }
  };

  struct GetFileContentF {
    template <typename CloudProviderT, typename ItemT>
    Generator<std::string> operator()(CloudProviderT* d, http::Range range,
                                      stdx::stop_token stop_token,
                                      ItemT item) const {
      if constexpr (IsFile<ItemT, CloudProviderT>) {
        return d->GetFileContent(std::move(item), range, std::move(stop_token));
      } else {
        throw std::invalid_argument("not a file");
      }
    }
  };

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