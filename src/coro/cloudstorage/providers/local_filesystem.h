#ifndef CORO_CLOUDSTORAGE_LOCAL_FILESYSTEM_H
#define CORO_CLOUDSTORAGE_LOCAL_FILESYSTEM_H

#include <filesystem>
#include <optional>

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/auth_token_manager.h"
#include "coro/util/thread_pool.h"

namespace coro::cloudstorage {

struct LocalFileSystem {
  struct Auth {
    struct AuthToken {
      std::string root;
    };

    struct AuthHandler;
  };

  template <typename ThreadPool = class ThreadPoolT>
  struct CloudProvider;

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

 private:
  static bool IsFileHidden(const std::filesystem::directory_entry& e);
};

template <typename ThreadPool>
class LocalFileSystem::CloudProvider
    : public coro::cloudstorage::CloudProvider<LocalFileSystem,
                                               CloudProvider<ThreadPool>> {
 public:
  CloudProvider(ThreadPool* thread_pool, Auth::AuthToken auth_token)
      : thread_pool_(thread_pool), auth_token_(std::move(auth_token)) {}

  Task<Directory> GetRoot(stdx::stop_token) const {
    Directory d{};
    d.id = auth_token_.root;
    d.name = "root";
    co_return d;
  }

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string>,
                                   stdx::stop_token) const {
    co_return co_await thread_pool_->Do([&] {
      PageData page_data;
      for (const auto& e : std::filesystem::directory_iterator(
               std::filesystem::path(directory.id))) {
        if (IsFileHidden(e)) {
          continue;
        }
        if (std::filesystem::is_directory(e)) {
          page_data.items.emplace_back(ToItem<Directory>(e));
        } else {
          page_data.items.emplace_back(ToItem<File>(e));
        }
      }
      return page_data;
    });
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token) const {
    co_return co_await thread_pool_->Do([&] {
      auto space = std::filesystem::space(auth_token_.root);
      return GeneralData{
          .username = auth_token_.root,
          .space_used = static_cast<int64_t>(space.capacity - space.free),
          .space_total = static_cast<int64_t>(space.capacity)};
    });
  }

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) const {
    std::ifstream stream = co_await thread_pool_->Do(
        [&] { return std::ifstream(file.id, std::ifstream::binary); });
    if (!range.end) {
      range.end = file.size - 1;
    }
    std::string buffer(kBufferSize, 0);
    co_await thread_pool_->Do([&] { stream.seekg(range.start); });
    int64_t bytes_read = 0;
    int64_t size = *range.end - range.start + 1;
    while (bytes_read < size) {
      if (stop_token.stop_requested()) {
        throw InterruptedException();
      }
      bool read_status = co_await thread_pool_->Do([&] {
        return bool(stream.read(
            buffer.data(), std::min<int64_t>(size - bytes_read, kBufferSize)));
      });
      if (!read_status) {
        throw std::runtime_error("couldn't read file");
      }
      co_yield std::string(buffer.data(), stream.gcount());
      bytes_read += stream.gcount();
    }
  }

  template <typename Item>
  Task<Item> RenameItem(Item item, std::string new_name,
                        stdx::stop_token stop_token) {
    throw std::runtime_error("unimplemented");
  }

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token) {
    throw std::runtime_error("unimplemented");
  }

  Task<> RemoveItem(Item item, stdx::stop_token stop_token) {
    throw std::runtime_error("unimplemented");
  }

  template <typename ItemT>
  Task<ItemT> MoveItem(ItemT source, Directory destination,
                       stdx::stop_token stop_token) {
    throw std::runtime_error("unimplemented");
  }

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token) {
    throw std::runtime_error("unimplemented");
  }

 private:
  static inline constexpr const int kBufferSize = 4096;

  template <typename T>
  static T ToItem(const std::filesystem::directory_entry& entry) {
    T item;
    item.id = entry.path().string();
    item.name = entry.path().filename().string();
    if constexpr (std::is_same_v<T, File>) {
      item.size = std::filesystem::file_size(entry.path());
    }
#ifdef CORO_CLOUDSTORAGE_HAVE_CLOCK_CAST
    item.timestamp = std::chrono::system_clock::to_time_t(
        std::chrono::clock_cast<std::chrono::system_clock>(
            std::filesystem::last_write_time(entry.path())));
#else
    item.timestamp =
        std::filesystem::last_write_time(entry.path()).time_since_epoch() /
        std::chrono::seconds(1);
#endif
    return item;
  }

  ThreadPool* thread_pool_;
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
