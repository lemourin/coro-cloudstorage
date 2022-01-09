#ifndef CORO_CLOUDSTORAGE_UTIL_SETTINGS_UTILS_H
#define CORO_CLOUDSTORAGE_UTIL_SETTINGS_UTILS_H

#include <string>
#include <string_view>

namespace coro::cloudstorage::util {

std::string GetConfigFilePath(std::string_view app_name = "coro-cloudstorage",
                              std::string_view file_name = "config.json");

std::string GetDirectoryPath(std::string_view path);

void CreateDirectory(std::string_view path);
void RemoveDirectory(std::string_view path);

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_SETTINGS_UTILS_H