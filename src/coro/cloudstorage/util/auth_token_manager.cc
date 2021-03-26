#include "auth_token_manager.h"

#ifdef WIN32
#include <direct.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace coro::cloudstorage::util {

std::string GetDirectoryPath(std::string_view path, char delimiter) {
  auto it = path.find_last_of(delimiter);
  if (it == std::string::npos) {
    throw std::invalid_argument("not a directory");
  }
  return std::string(path.begin(), path.begin() + it);
}

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
  if (_mkdir(path_utf8.c_str()) != 0) {
    if (errno != EEXIST) {
      throw std::runtime_error("cannot initialize config file");
    }
  }
  path_utf8 += "\\";
  path_utf8 += file_name;
  return path_utf8;
#else
  std::string path;
  if (const char* xdg_config = getenv("XDG_CONFIG_HOME")) {
    path = xdg_config;
  } else if (const char* home = getenv("HOME")) {
    path = std::string(home) + "/.config/";
  }
  path += app_name;
  if (mkdir(path.c_str(), 0777) != 0) {
    if (errno != EEXIST) {
      throw std::runtime_error("cannot initialize config file");
    }
  }
  path += "/";
  path += file_name;
  return path;
#endif
}

void AuthTokenManager::RemoveToken(std::string_view id,
                                   std::string_view provider_id) const {
  auto token_file = path_;
  nlohmann::json json;
  {
    std::ifstream input_token_file{token_file};
    if (input_token_file) {
      input_token_file >> json;
    }
  }
  nlohmann::json result;
  for (auto token : json["auth_token"]) {
    if (token["type"] != std::string(provider_id) ||
        token["id"] != std::string(id)) {
      result["auth_token"].emplace_back(std::move(token));
    }
  }
  if (result.is_null()) {
    remove(token_file.c_str());
#ifdef WIN32
    _rmdir(GetDirectoryPath(token_file, '\\').c_str());
#else
    rmdir(GetDirectoryPath(token_file, '/').c_str());
#endif
  } else {
    std::ofstream{token_file} << result;
  }
}

}  // namespace coro::cloudstorage::util