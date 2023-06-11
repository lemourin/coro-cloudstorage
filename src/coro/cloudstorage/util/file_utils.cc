#include "coro/cloudstorage/util/file_utils.h"

#include <cstdlib>
#include <memory>

#include "coro/cloudstorage/util/string_utils.h"

#ifdef WINRT
#include <winrt/Windows.Storage.h>
#endif

#ifdef WIN32
#include <direct.h>
#include <shlobj.h>
#include <windows.h>

#undef CreateDirectory
#undef RemoveDirectory

#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace coro::cloudstorage::util {

namespace {

#ifdef WIN32
#define mkdir _mkdir
#define rmdir _rmdir
#else

namespace {
int mkdir(const char* path) { return ::mkdir(path, 0777); }
}  // namespace

#endif
#ifdef __ANDROID__
std::string gAndroidTempDirectory;  // NOLINT
#endif

}  // namespace

#ifdef WIN32
const char kPathSeparator = '\\';
#else
const char kPathSeparator = '/';
#endif

bool IsPathSeparator(char c) { return c == '/' || c == '\\'; }

std::string GetConfigDirectory() {
#if defined(WINRT)
  auto directory =
      winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
  return winrt::to_string(directory.Path());
#elif defined(WIN32)
  PWSTR path;
  if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path) != S_OK) {
    throw RuntimeError("cannot fetch configuration path");
  }
  int size =
      WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
  std::string path_utf8(static_cast<size_t>(size - 1L), 0);
  WideCharToMultiByte(CP_UTF8, 0, path, -1, path_utf8.data(), size, nullptr,
                      nullptr);
  return path_utf8;
#else
  std::string path;
  if (const char* xdg_config = getenv("XDG_CONFIG_HOME")) {  // NOLINT
    path = xdg_config;
  } else if (const char* home = getenv("HOME")) {            // NOLINT
    path = std::string(home) + "/.config/";
  }
  return path;
#endif
}

std::string GetCacheDirectory() {
#if defined(WINRT)
  auto directory =
      winrt::Windows::Storage::ApplicationData::Current().LocalCacheFolder();
  return winrt::to_string(directory.Path());
#elif defined(WIN32)
  return GetConfigDirectory();
#else
  std::string path;
  if (const char* xdg_config = getenv("XDG_CACHE_HOME")) {  // NOLINT
    path = xdg_config;
  } else if (const char* home = getenv("HOME")) {           // NOLINT
#ifdef __APPLE__
    path = std::string(home) + "/Library/Caches/";
#else
    path = std::string(home) + "/.cache/";
#endif
  }
  return path;
#endif
}

#ifdef __ANDROID__
void SetAndroidTempDirectory(std::string path) {
  gAndroidTempDirectory = std::move(path);
}
#endif

int64_t Ftell(std::FILE* file) {
#ifdef WIN32
  return _ftelli64(file);
#else
  return ftello(file);
#endif
}

int Fseek(std::FILE* file, int64_t offset, int origin) {
#ifdef WIN32
  return _fseeki64(file, offset, origin);
#else
  return fseeko(file, offset, origin);
#endif
}

std::unique_ptr<std::FILE, FileDeleter> CreateTmpFile() {
  return std::unique_ptr<std::FILE, FileDeleter>([] {
#if defined(__ANDROID__)
    std::string name = gAndroidTempDirectory + "/tmp.XXXXXX";
    int fno = mkstemp(name.data());
    if (name.empty()) {
      throw RuntimeError("couldn't create tmpfile");
    }
    std::remove(name.c_str());
    return fdopen(fno, "w+");
#elif defined(_MSC_VER)
    std::FILE* file;
    if (tmpfile_s(&file) != 0) {
      throw RuntimeError("couldn't create tmpfile");
    }
    return file;
#else
    std::FILE* file = std::tmpfile();
    if (!file) {
      throw RuntimeError("couldn't create tmpfile");
    }
    return file;
#endif
  }());
}

std::string GetFileName(std::string path) {
  while (!path.empty() && IsPathSeparator(path.back())) {
    path.pop_back();
  }
  auto it = [&] {
    for (size_t it = path.size(); it-- > 0;) {
      if (IsPathSeparator(path[it])) {
        return it;
      }
    }
    return std::string_view::npos;
  }();
  return path.substr(it == std::string::npos ? 0 : it + 1);
}

std::string GetDirectoryPath(std::string path) {
  while (!path.empty() && IsPathSeparator(path.back())) {
    path.pop_back();
  }
  auto it = [&] {
    for (size_t it = path.size(); it-- > 0;) {
      if (IsPathSeparator(path[it])) {
        return it;
      }
    }
    throw RuntimeError("root has no parent");
  }();
  return path.substr(0, it + 1);
}

std::span<const std::string> GetDirectoryPath(
    std::span<const std::string> path) {
  if (path.empty()) {
    throw RuntimeError("root has no parent");
  }
  return path.subspan(0, path.size() - 1);
}

void CreateDirectory(std::string_view path) {
#ifdef WINRT
  auto it = path.begin();
  int status = 0;
  while (it != path.end()) {
    it = std::find_if(it + 1, path.end(), IsPathSeparator);
    std::string directory(path.begin(), it);
    status = mkdir(directory.c_str());
  }
  if (status != 0 && errno != EEXIST) {
    throw RuntimeError(StrCat("cannot create directory, errno=", errno, " ",
                              ErrorToString(errno), ", path=", path));
  }
#else
  auto it = path.begin();
  while (it != path.end()) {
    it = std::find_if(it + 1, path.end(), IsPathSeparator);
    std::string directory(path.begin(), it);
    if (mkdir(directory.c_str()) != 0) {
      if (errno != EEXIST) {
        throw RuntimeError(StrCat("cannot create parent directory=", directory,
                                  ", errno=", errno, " ", ErrorToString(errno),
                                  ", path=", path));
      }
    }
  }
#endif
}

void RemoveDirectory(std::string_view path) {
  if (int status = rmdir(std::string(path).c_str())) {
    throw RuntimeError(
        StrCat("can't remove directory ", path, ": ", ErrorToString(status)));
  }
}

Task<int64_t> GetFileSize(coro::util::ThreadPool* thread_pool,
                          std::FILE* file) {
  co_return co_await thread_pool->Do([=] {
    if (Fseek(file, 0, SEEK_END) != 0) {
      throw RuntimeError("fseek failed");
    }
    return Ftell(file);
  });
}

Task<> WriteFile(coro::util::ThreadPool* thread_pool, std::FILE* file,
                 int64_t offset, std::string_view data) {
  co_return co_await thread_pool->Do([=] {
    if (Fseek(file, offset, SEEK_SET) != 0) {
      throw RuntimeError("fseek failed " + std::to_string(offset) + " " +
                         std::to_string(errno));
    }
    if (fwrite(data.data(), 1, data.size(), file) != data.size()) {
      throw RuntimeError("fwrite failed");
    }
  });
}

Generator<std::string> ReadFile(coro::util::ThreadPool* thread_pool,
                                std::FILE* file) {
  const int kBufferSize = 4096;
  std::array<char, kBufferSize> buffer;
  if (co_await thread_pool->Do(Fseek, file, 0, SEEK_SET) != 0) {
    throw std::runtime_error("fseek failed");
  }
  while (feof(file) == 0) {
    size_t size =
        co_await thread_pool->Do(fread, buffer.data(), 1, kBufferSize, file);
    if (ferror(file) != 0) {
      throw std::runtime_error("read error");
    }
    co_yield std::string(buffer.data(), size);
  }
}

Task<std::string> ReadFile(coro::util::ThreadPool* thread_pool, std::FILE* file,
                           int64_t offset, size_t size) {
  co_return co_await thread_pool->Do([=] {
    if (Fseek(file, offset, SEEK_SET) != 0) {
      throw std::runtime_error("fseek failed " + std::to_string(offset));
    }
    std::string buffer(size, 0);
    if (fread(buffer.data(), 1, size, file) != size) {
      throw std::runtime_error("fread failed");
    }
    return buffer;
  });
}

}  // namespace coro::cloudstorage::util
