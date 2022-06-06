#include "coro/cloudstorage/util/file_utils.h"

#include <cstdlib>
#include <memory>
#include <utility>

namespace coro::cloudstorage::util {

namespace {

#ifdef __ANDROID__
std::string gAndroidTempDirectory;  // NOLINT
#endif

bool IsPathSeparator(char c) { return c == '/' || c == '\\'; }

}  // namespace

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
      throw std::runtime_error("couldn't create tmpfile");
    }
    std::remove(name.c_str());
    return fdopen(fno, "w+");
#elif defined(_MSC_VER)
    std::FILE* file;
    if (tmpfile_s(&file) != 0) {
      throw std::runtime_error("couldn't create tmpfile");
    }
    return file;
#else
    std::FILE* file = std::tmpfile();
    if (!file) {
      throw std::runtime_error("couldn't create tmpfile");
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
    throw std::runtime_error("root has no parent");
  }();
  return path.substr(0, it + 1);
}

std::span<const std::string> GetDirectoryPath(
    std::span<const std::string> path) {
  if (path.empty()) {
    throw std::runtime_error("root has no parent");
  }
  return path.subspan(0, path.size() - 1);
}

}  // namespace coro::cloudstorage::util
