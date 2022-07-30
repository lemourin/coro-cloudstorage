#include "coro/cloudstorage/providers/local_filesystem.h"

#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"

#ifdef WIN32

#include <windows.h>

#undef CreateDirectory
#undef CreateFile

#else
#include <sys/stat.h>
#endif

#include <fmt/format.h>

#include <cstdlib>

namespace coro::cloudstorage {

namespace {

constexpr const int kBufferSize = 4096;

std::string GetHomeDirectory() {
#ifdef WINRT
  return ".";
#elif _WIN32
  const char* drive = std::getenv("Homedrive");
  const char* path = std::getenv("Homepath");
  return (drive && path) ? std::string(drive) + path : ".";
#elif __ANDROID__
  return "/storage/emulated/0";
#else
  const char* home = std::getenv("HOME");
  return home ? home : ".";
#endif
}

bool IsFileHidden(const std::filesystem::directory_entry& e) {
#ifdef WIN32
  return GetFileAttributesW(e.path().c_str()) &
         (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
#else
  return e.path().filename().c_str()[0] == '.' ||
         e.path().filename() == "lost+found";
#endif
}

int64_t GetTimestamp(const std::filesystem::directory_entry& e) {
#if defined(WIN32)
  struct _stat64 file_info;
  if (_wstati64(e.path().wstring().c_str(), &file_info) != 0) {
    throw RuntimeError("failed to get last write time");
  }
  return file_info.st_mtime;
#elif defined(__APPLE__)
  struct stat file_info;
  if (stat(e.path().c_str(), &file_info) != 0) {
    throw RuntimeError("failed to get last write time");
  }
  return file_info.st_mtimespec.tv_sec;
#else
  struct stat64 file_info;
  if (stat64(e.path().c_str(), &file_info) != 0) {
    throw RuntimeError("failed to get last write time");
  }
  return file_info.st_mtim.tv_sec;
#endif
}

template <typename T>
T ToItem(const std::filesystem::directory_entry& entry) {
  T item;
  item.id = entry.path().string();
  item.name = entry.path().filename().string();
  if constexpr (std::is_same_v<T, LocalFileSystem::File>) {
    item.size = std::filesystem::file_size(entry.path());
  }
  item.timestamp = GetTimestamp(entry);
  return item;
}

}  // namespace

namespace util {

template <>
nlohmann::json ToJson<LocalFileSystem::Auth::AuthToken>(
    LocalFileSystem::Auth::AuthToken token) {
  nlohmann::json json;
  json["root"] = std::move(token.root);
  return json;
}

template <>
LocalFileSystem::Auth::AuthToken ToAuthToken<LocalFileSystem::Auth::AuthToken>(
    const nlohmann::json& json) {
  LocalFileSystem::Auth::AuthToken auth_token;
  auth_token.root = json.at("root");
  return auth_token;
}

}  // namespace util

Task<std::variant<http::Response<>, LocalFileSystem::Auth::AuthToken>>
LocalFileSystem::Auth::AuthHandler::operator()(http::Request<> request,
                                               stdx::stop_token) const {
  if (request.method == http::Method::kGet) {
    co_return http::Response<>{
        .status = 200,
        .body = http::CreateBody(
            fmt::format(fmt::runtime(util::kAssetsHtmlLocalLoginHtml),
                        fmt::arg("root", GetHomeDirectory())))};
  } else if (request.method == http::Method::kPost) {
    auto query =
        http::ParseQuery(co_await http::GetBody(std::move(*request.body)));
    if (auto it = query.find("root"); it != query.end()) {
      co_return Auth::AuthToken{.root = std::move(it->second)};
    } else {
      co_return http::Response<>{.status = 400};
    }
  } else {
    co_return http::Response<>{.status = 400};
  }
}

auto LocalFileSystem::GetRoot(stdx::stop_token) const -> Task<Directory> {
  Directory d{};
  d.id = auth_token_.root;
  d.name = "root";
  co_return d;
}

auto LocalFileSystem::ListDirectoryPage(Directory directory,
                                        std::optional<std::string>,
                                        stdx::stop_token stop_token) const
    -> Task<PageData> {
  co_return co_await thread_pool_->Do(std::move(stop_token), [&] {
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

auto LocalFileSystem::GetGeneralData(stdx::stop_token stop_token) const
    -> Task<GeneralData> {
  co_return co_await thread_pool_->Do(std::move(stop_token), [&] {
    auto space = std::filesystem::space(auth_token_.root);
    return GeneralData{
        .username = auth_token_.root,
        .space_used = static_cast<int64_t>(space.capacity - space.free),
        .space_total = static_cast<int64_t>(space.capacity)};
  });
}

Generator<std::string> LocalFileSystem::GetFileContent(
    File file, http::Range range, stdx::stop_token stop_token) const {
  std::ifstream stream = co_await thread_pool_->Do(stop_token, [&] {
    return std::ifstream(file.id, std::ifstream::binary);
  });
  if (!range.end) {
    range.end = file.size - 1;
  }
  std::string buffer(kBufferSize, 0);
  co_await thread_pool_->Do(stop_token, [&] { stream.seekg(range.start); });
  int64_t bytes_read = 0;
  int64_t size = *range.end - range.start + 1;
  while (bytes_read < size) {
    if (stop_token.stop_requested()) {
      throw InterruptedException();
    }
    bool read_status = co_await thread_pool_->Do(stop_token, [&] {
      return bool(stream.read(
          buffer.data(), std::min<int64_t>(size - bytes_read, kBufferSize)));
    });
    if (!read_status) {
      throw RuntimeError("couldn't read file");
    }
    co_yield std::string(buffer.data(), stream.gcount());
    bytes_read += stream.gcount();
  }
}

template <typename ItemT>
Task<ItemT> LocalFileSystem::RenameItem(ItemT item, std::string new_name,
                                        stdx::stop_token stop_token) {
  throw RuntimeError("unimplemented");
}

auto LocalFileSystem::CreateDirectory(Directory parent, std::string name,
                                      stdx::stop_token stop_token)
    -> Task<Directory> {
  throw RuntimeError("unimplemented");
}

Task<> LocalFileSystem::RemoveItem(Item item, stdx::stop_token stop_token) {
  throw RuntimeError("unimplemented");
}

template <typename ItemT>
Task<ItemT> LocalFileSystem::MoveItem(ItemT source, Directory destination,
                                      stdx::stop_token stop_token) {
  throw RuntimeError("unimplemented");
}

auto LocalFileSystem::CreateFile(Directory parent, std::string_view name,
                                 FileContent content,
                                 stdx::stop_token stop_token) -> Task<File> {
  throw RuntimeError("unimplemented");
}

namespace util {

template <>
auto AbstractCloudProvider::Create<LocalFileSystem>(LocalFileSystem p)
    -> std::unique_ptr<AbstractCloudProvider> {
  return CreateAbstractCloudProvider(std::move(p));
}

}  // namespace util

template auto LocalFileSystem::RenameItem(File item, std::string new_name,
                                          stdx::stop_token stop_token)
    -> Task<File>;

template auto LocalFileSystem::RenameItem(Directory item, std::string new_name,
                                          stdx::stop_token stop_token)
    -> Task<Directory>;

template auto LocalFileSystem::MoveItem(File, Directory, stdx::stop_token)
    -> Task<File>;

template auto LocalFileSystem::MoveItem(Directory, Directory, stdx::stop_token)
    -> Task<Directory>;

}  // namespace coro::cloudstorage