#ifndef CORO_CLOUDSTORAGE_UTIL_SETTINGS_UTILS_H
#define CORO_CLOUDSTORAGE_UTIL_SETTINGS_UTILS_H

#include <string>
#include <string_view>

namespace coro::cloudstorage::util {

std::string GetConfigFilePath(std::string_view app_name = "coro-cloudstorage",
                              std::string_view file_name = "config.sqlite");

std::string GetCacheFilePath(std::string_view app_name = "coro-cloudstorage",
                             std::string_view file_name = "cache.sqlite");

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_SETTINGS_UTILS_H