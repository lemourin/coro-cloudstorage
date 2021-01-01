#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H

#include <coro/cloudstorage/util/auth_manager.h>
#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/promise.h>
#include <coro/shared_promise.h>
#include <coro/stdx/stop_callback.h>
#include <coro/stdx/stop_source.h>
#include <coro/util/make_pointer.h>

namespace coro::cloudstorage {

template <typename T>
concept CloudProviderImpl = requires(http::HttpStub& http, std::string code,
                                     typename T::AuthData auth_data,
                                     stdx::stop_token stop_token) {
  { T::ExchangeAuthorizationCode(http, auth_data, code, stop_token) }
  ->Awaitable;
};

template <typename T>
concept HasTimestamp = requires(T v) {
  { v.timestamp }
  ->stdx::convertible_to<std::optional<int64_t>>;
};

template <typename T>
concept HasSize = requires(T v) {
  { v.size }
  ->stdx::convertible_to<std::optional<int64_t>>;
};

template <typename T, typename CloudProvider>
concept IsDirectory = requires(typename CloudProvider::Impl provider, T v,
                               std::optional<std::string> page_token,
                               stdx::stop_token stop_token) {
  { provider.ListDirectoryPage(v, page_token, stop_token).await_resume() }
  ->std::convertible_to<typename CloudProvider::PageData>;
};

template <typename T, typename CloudProvider>
concept IsFile = requires(typename CloudProvider::Impl provider, T v,
                          http::Range range, stdx::stop_token stop_token) {
  { provider.GetFileContent(v, range, stop_token) }
  ->GeneratorLike;
};

template <typename ImplT>
class CloudProvider {
 public:
  using Impl = ImplT;
  using Item = typename Impl::Item;
  using PageData = typename Impl::PageData;

  explicit CloudProvider(Impl impl) : impl_(std::move(impl)) {}

  CloudProvider(const CloudProvider&) = delete;
  CloudProvider(CloudProvider&&) noexcept = default;
  CloudProvider& operator=(const CloudProvider&) = delete;
  CloudProvider& operator=(CloudProvider&&) noexcept = default;

  ~CloudProvider() { stop_source_.request_stop(); }

  auto GetRoot(stdx::stop_token stop_token = stdx::stop_token()) {
    return Do(
        [this, stop_token]() mutable {
          return impl_.GetRoot(std::move(stop_token));
        },
        stop_token);
  }

  auto GetGeneralData(stdx::stop_token stop_token = stdx::stop_token()) {
    return Do(
        [this, stop_token]() mutable {
          return impl_.GetGeneralData(std::move(stop_token));
        },
        stop_token);
  }

  auto GetItem(std::string id,
               stdx::stop_token stop_token = stdx::stop_token()) {
    return Do(
        [this, id = std::move(id), stop_token]() mutable {
          return impl_.GetItem(std::move(id), std::move(stop_token));
        },
        stop_token);
  }

  Task<Item> GetItemByPath(std::string path,
                           stdx::stop_token stop_token = stdx::stop_token()) {
    co_return co_await GetItemByPath(co_await GetRoot(stop_token),
                                     std::move(path), stop_token);
  }

  template <typename File>
  auto GetFileContent(File file, http::Range range = http::Range{},
                      stdx::stop_token stop_token = stdx::stop_token()) {
    return Do(
        [this, file = std::move(file), range = std::move(range),
         stop_token]() mutable {
          return impl_.GetFileContent(std::move(file), std::move(range),
                                      std::move(stop_token));
        },
        stop_token);
  }

  template <typename Directory>
  Generator<PageData> ListDirectory(
      Directory directory, stdx::stop_token stop_token = stdx::stop_token()) {
    std::optional<std::string> current_page_token;
    do {
      auto page_data = co_await ListDirectoryPage(
          directory, std::move(current_page_token), stop_token);
      co_yield page_data;
      current_page_token = page_data.next_page_token;
    } while (current_page_token);
  }

  template <typename Directory>
  auto ListDirectoryPage(Directory directory,
                         std::optional<std::string> page_token = std::nullopt,
                         stdx::stop_token stop_token = stdx::stop_token()) {
    return Do(
        [this, directory = std::move(directory),
         page_token = std::move(page_token), stop_token] {
          return impl_.ListDirectoryPage(std::move(directory),
                                         std::move(page_token),
                                         std::move(stop_token));
        },
        stop_token);
  }

 private:
  template <typename Directory>
  Task<Item> GetItemByPath(Directory current_directory, std::string path,
                           stdx::stop_token stop_token) {
    if (path.empty() || path == "/") {
      co_return current_directory;
    }
    auto delimiter_index = path.find_first_of('/', 1);
    std::string path_component(path.begin() + 1,
                               delimiter_index == std::string::npos
                                   ? path.end()
                                   : path.begin() + delimiter_index);
    std::string rest_component(delimiter_index == std::string::npos
                                   ? path.end()
                                   : path.begin() + delimiter_index,
                               path.end());
    FOR_CO_AWAIT(auto& page, ListDirectory(current_directory, stop_token)) {
      for (auto& item : page.items) {
        auto r = std::visit(
            [&](auto& d) -> std::variant<std::monostate, Task<Item>, Item> {
              if constexpr (IsDirectory<decltype(d), CloudProvider>) {
                if (d.name == path_component) {
                  return GetItemByPath(d, rest_component, stop_token);
                }
              } else {
                if (d.name == path_component) {
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

  template <typename F>
  auto Do(F func, stdx::stop_token stop_token)
      -> Task<decltype(func().await_resume())> {
    stdx::stop_source stop_source;
    stdx::stop_callback callback_fst(stop_token,
                                     [&] { stop_source.request_stop(); });
    stdx::stop_callback callback_nd(stop_source_.get_token(),
                                    [&] { stop_source.request_stop(); });
    stop_token = stop_source.get_token();
    auto result = co_await func();
    if (stop_token.stop_requested()) {
      throw InterruptedException();
    }
    co_return std::move(result);
  }

  template <typename F>
  auto Do(F func, stdx::stop_token stop_token) -> Generator<
      std::remove_reference_t<decltype(*(func().begin().await_resume()))>> {
    stdx::stop_source stop_source;
    stdx::stop_callback callback_fst(stop_token,
                                     [&] { stop_source.request_stop(); });
    stdx::stop_callback callback_nd(stop_source_.get_token(),
                                    [&] { stop_source.request_stop(); });
    stop_token = stop_source.get_token();
    FOR_CO_AWAIT(auto& entry, func()) {
      if (stop_token.stop_requested()) {
        throw InterruptedException();
      }
      co_yield std::move(entry);
    }
    if (stop_token.stop_requested()) {
      throw InterruptedException();
    }
  }

  stdx::stop_source stop_source_;
  Impl impl_;
};

template <typename CloudProvider>
struct CreateCloudProvider {
  template <typename CloudFactory, typename OnTokenUpdated = void (*)(
                                       typename CloudProvider::Auth::AuthToken)>
  auto operator()(
      const CloudFactory& factory,
      typename CloudProvider::Auth::AuthToken auth_token,
      OnTokenUpdated on_token_updated =
          [](typename CloudProvider::Auth::AuthToken) {}) const {
    util::AuthManager<std::remove_pointer_t<decltype(factory.http_)>,
                      typename CloudProvider::Auth, OnTokenUpdated>
        auth_manager(*factory.http_, std::move(auth_token),
                     factory.auth_data_.template operator()<CloudProvider>(),
                     std::move(on_token_updated));
    using InternalImpl =
        typename CloudProvider::template Impl<decltype(auth_manager)>;
    return ::coro::cloudstorage::CloudProvider(
        InternalImpl(std::move(auth_manager)));
  }
};

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
