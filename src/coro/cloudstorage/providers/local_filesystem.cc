#include "coro/cloudstorage/providers/local_filesystem.h"

#ifdef WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include <fmt/format.h>

#include <cstdlib>

namespace coro::cloudstorage {

namespace {

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

}  // namespace

bool LocalFileSystem::IsFileHidden(const std::filesystem::directory_entry& e) {
#ifdef WIN32
  return GetFileAttributesW(e.path().c_str()) &
         (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
#else
  return e.path().filename().c_str()[0] == '.' ||
         e.path().filename() == "lost+found";
#endif
}

int64_t LocalFileSystem::GetTimestamp(
    const std::filesystem::directory_entry& e) {
#if defined(WIN32)
  struct _stat64 file_info;
  if (_wstati64(e.path().wstring().c_str(), &file_info) != 0) {
    throw std::runtime_error("failed to get last write time");
  }
  return file_info.st_mtime;
#elif defined(__APPLE__)
  struct stat file_info;
  if (stat(e.path().c_str(), &file_info) != 0) {
    throw std::runtime_error("failed to get last write time");
  }
  return file_info.st_mtimespec.tv_sec;
#else
  struct stat64 file_info;
  if (stat64(e.path().c_str(), &file_info) != 0) {
    throw std::runtime_error("failed to get last write time");
  }
  return file_info.st_mtim.tv_sec;
#endif
}

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
}  // namespace coro::cloudstorage