#ifndef CORO_CLOUDSTORAGE_LOCAL_FILESYSTEM_H
#define CORO_CLOUDSTORAGE_LOCAL_FILESYSTEM_H

#include <filesystem>
#include <optional>

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/auth_token_manager.h"
#include "coro/util/thread_pool.h"

namespace coro::cloudstorage {

class LocalFileSystem {
 public:
  struct Auth {
    struct AuthToken {
      std::string root;
    };

    struct AuthHandler;
  };

  struct GeneralData {
    std::string username;
    int64_t space_used;
    std::optional<int64_t> space_total;
  };

  struct ItemData {
    std::string id;
    std::string name;
    int64_t timestamp;
  };

  struct Directory : ItemData {};

  struct File : ItemData {
    int64_t size;
  };

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct FileContent {
    Generator<std::string> data;
    std::optional<int64_t> size;
  };

  static constexpr std::string_view kId = "local";
  static inline constexpr const auto& kIcon = util::kAssetsProvidersLocalPng;

  LocalFileSystem(coro::util::ThreadPool* thread_pool,
                  Auth::AuthToken auth_token)
      : thread_pool_(thread_pool), auth_token_(std::move(auth_token)) {}

  Task<Directory> GetRoot(stdx::stop_token) const;

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string>,
                                   stdx::stop_token) const;

  Task<GeneralData> GetGeneralData(stdx::stop_token) const;

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) const;

  template <typename Item>
  Task<Item> RenameItem(Item item, std::string new_name,
                        stdx::stop_token stop_token);

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token);

  Task<> RemoveItem(Item item, stdx::stop_token stop_token);

  template <typename ItemT>
  Task<ItemT> MoveItem(ItemT source, Directory destination,
                       stdx::stop_token stop_token);

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token);

 private:
  coro::util::ThreadPool* thread_pool_;
  Auth::AuthToken auth_token_;
};

struct LocalFileSystem::Auth::AuthHandler {
  Task<std::variant<http::Response<>, Auth::AuthToken>> operator()(
      http::Request<> request, stdx::stop_token stop_token) const;
};

namespace util {

template <>
nlohmann::json ToJson<LocalFileSystem::Auth::AuthToken>(
    LocalFileSystem::Auth::AuthToken token);

template <>
LocalFileSystem::Auth::AuthToken ToAuthToken<LocalFileSystem::Auth::AuthToken>(
    const nlohmann::json& json);

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_LOCAL_FILESYSTEM_H
