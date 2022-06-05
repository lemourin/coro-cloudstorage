#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H

#include <span>

#include "coro/cloudstorage/cloud_exception.h"
#include "coro/cloudstorage/util/generator_utils.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"
#include "coro/promise.h"
#include "coro/shared_promise.h"
#include "coro/stdx/any_invocable.h"
#include "coro/stdx/stop_callback.h"
#include "coro/stdx/stop_source.h"
#include "coro/util/raii_utils.h"
#include "coro/util/stop_token_or.h"
#include "coro/util/type_list.h"

namespace coro::cloudstorage {

enum class ThumbnailQuality {
  kLow,
  kHigh,
};

template <typename T, typename CloudProvider>
concept IsDirectory = requires(typename CloudProvider::Impl& provider, T v,
                               std::optional<std::string> page_token,
                               stdx::stop_token stop_token) {
  {
    provider.ListDirectoryPage(v, page_token, stop_token)
    } -> Awaitable<typename CloudProvider::PageData>;
};

template <typename AuthToken>
class OnAuthTokenUpdated {
 public:
  template <typename F>
  explicit OnAuthTokenUpdated(F&& func) : impl_(std::forward<F>(func)) {}

  void operator()(const AuthToken& auth_token) { impl_(auth_token); }

 private:
  stdx::any_invocable<void(const AuthToken&)> impl_;
};

template <typename CloudProviderT, typename ImplT = CloudProviderT>
class CloudProvider {
 public:
  using Type = CloudProviderT;
  using Impl = ImplT;
  using Item = typename CloudProviderT::Item;
  using PageData = typename CloudProviderT::PageData;
  using FileContent = typename CloudProviderT::FileContent;
  using ItemTypeList = coro::util::ToTypeListT<std::variant, Item>;

  template <typename DirectoryT>
  static constexpr bool IsFileContentSizeRequired(const DirectoryT&) {
    return std::is_convertible_v<decltype(std::declval<FileContent>().size),
                                 int64_t>;
  }

  Task<Item> GetItemByPath(std::string path, stdx::stop_token stop_token) {
    co_return co_await Get()->GetItemByPath(co_await Get()->GetRoot(stop_token),
                                            std::move(path), stop_token);
  }

  Task<Item> GetItemByPathComponents(std::span<const std::string> components,
                                     stdx::stop_token stop_token) {
    co_return co_await Get()->GetItemByPathComponents(
        co_await Get()->GetRoot(stop_token), components, stop_token);
  }

  template <typename Directory>
  Generator<PageData> ListDirectory(Directory directory,
                                    stdx::stop_token stop_token) {
    std::optional<std::string> current_page_token;
    do {
      auto page_data = co_await Get()->ListDirectoryPage(
          directory, std::move(current_page_token), stop_token);
      co_yield page_data;
      current_page_token = page_data.next_page_token;
    } while (current_page_token);
  }

 private:
  auto Get() { return static_cast<Impl*>(this); }
  auto Get() const { return static_cast<const Impl*>(this); }

  template <typename Directory>
  Task<Item> GetItemByPathComponents(Directory current_directory,
                                     std::span<const std::string> components,
                                     stdx::stop_token stop_token) {
    if (components.empty()) {
      co_return current_directory;
    }
    FOR_CO_AWAIT(auto& page, ListDirectory(current_directory, stop_token)) {
      for (auto& item : page.items) {
        auto r = std::visit(
            [&](auto& d) -> std::variant<std::monostate, Task<Item>, Item> {
              if constexpr (IsDirectory<decltype(d), CloudProvider>) {
                if (d.name == components.front()) {
                  return GetItemByPathComponents(
                      std::move(d), components.subspan(1), stop_token);
                }
              } else {
                if (d.name == components.front()) {
                  return std::move(d);
                }
              }
              return std::monostate();
            },
            item);
        if (std::holds_alternative<Task<Item>>(r)) {
          co_return co_await std::get<Task<Item>>(r);
        } else if (std::holds_alternative<Item>(r)) {
          co_return std::move(std::get<Item>(r));
        }
      }
    }
    throw CloudException(CloudException::Type::kNotFound);
  }

  template <typename Directory>
  Task<Item> GetItemByPath(Directory current_directory, std::string_view path,
                           stdx::stop_token stop_token) {
    co_return co_await GetItemByPathComponents(
        std::move(current_directory), util::SplitString(std::string(path), '/'),
        std::move(stop_token));
  }
};

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
