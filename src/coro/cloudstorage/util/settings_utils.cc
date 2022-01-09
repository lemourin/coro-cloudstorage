#include "coro/cloudstorage/util/settings_utils.h"

#include <stdexcept>

#include "coro/cloudstorage/util/string_utils.h"

#ifdef WIN32
#include <direct.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace coro::cloudstorage::util {

#ifdef WIN32
constexpr char kDelimiter = '\\';
#else
constexpr char kDelimiter = '/';
#endif

#ifdef WIN32
#define mkdir _mkdir
#define rmdir _rmdir
#else

namespace {
int mkdir(const char* path) { return ::mkdir(path, 0777); }
}  // namespace

#endif

std::string GetConfigFilePath(std::string_view app_name,
                              std::string_view file_name) {
#ifdef WIN32
  PWSTR path;
  if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path) != S_OK) {
    throw std::runtime_error("cannot fetch configuration path");
  }
  int size =
      WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
  std::string path_utf8(static_cast<size_t>(size - 1L), 0);
  WideCharToMultiByte(CP_UTF8, 0, path, -1, path_utf8.data(), size, nullptr,
                      nullptr);
  path_utf8 += "\\";
  path_utf8 += app_name;
  path_utf8 += "\\";
  path_utf8 += file_name;
  return path_utf8;
#else
  std::string path;
  if (const char* xdg_config = getenv("XDG_CONFIG_HOME")) {  // NOLINT
    path = xdg_config;
  } else if (const char* home = getenv("HOME")) {  // NOLINT
    path = std::string(home) + "/.config/";
  }
  path += app_name;
  path += "/";
  path += file_name;
  return path;
#endif
}

std::string GetDirectoryPath(std::string_view path) {
  auto it = path.find_last_of(kDelimiter);
  if (it == std::string::npos) {
    throw std::invalid_argument("not a directory");
  }
  return std::string(path.begin(), path.begin() + it);
}

void CreateDirectory(std::string_view path) {
  size_t it = 0;
  while (it != std::string::npos) {
    it = path.find_first_of(kDelimiter, it + 1);
    if (mkdir(std::string(path.begin(), it == std::string::npos
                                            ? path.end()
                                            : path.begin() + it)
                  .c_str()) != 0) {
      if (errno != EEXIST) {
        throw std::runtime_error(StrCat("cannot create directory ", path, ": ",
                                        ErrorToString(errno)));
      }
    }
  }
}

void RemoveDirectory(std::string_view path) {
  if (int status = rmdir(std::string(path).c_str())) {
    throw std::runtime_error(
        StrCat("can't remove directory ", path, ": ", ErrorToString(status)));
  }
}

nlohmann::json ReadSettings(std::string_view path) {
  std::ifstream file{std::string(path)};
  if (!file) {
    return {};
  }
  nlohmann::json json;
  file >> json;
  return json;
}

}  // namespace coro::cloudstorage::util