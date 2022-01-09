#ifndef CORO_CLOUDSTORAGE_UTIL_SETTINGS_UTILS_H
#define CORO_CLOUDSTORAGE_UTIL_SETTINGS_UTILS_H

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace coro::cloudstorage::util {

std::string GetConfigFilePath(std::string_view app_name = "coro-cloudstorage",
                              std::string_view file_name = "config.json");

std::string GetDirectoryPath(std::string_view path);

void CreateDirectory(std::string_view path);
void RemoveDirectory(std::string_view path);

nlohmann::json ReadSettings(std::string_view path);

template <typename F>
void EditSettings(std::string_view path, const F& edit) {
  nlohmann::json json = ReadSettings(path);
  json = edit(std::move(json));
  if (json.is_null()) {
    remove(std::string(path).c_str());
    RemoveDirectory(GetDirectoryPath(path));
  } else {
    CreateDirectory(GetDirectoryPath(path));
    std::ofstream stream{std::string(path)};
    stream.exceptions(std::ofstream::failbit | std::ofstream::badbit);
    stream << json.dump(2);
  }
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_SETTINGS_UTILS_H