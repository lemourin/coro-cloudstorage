#include "coro/cloudstorage/util/settings_utils.h"

#include <stdexcept>

#include "coro/cloudstorage/util/file_utils.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/exception.h"

namespace coro::cloudstorage::util {

namespace {

std::string Append(std::string path, std::string_view app_name,
                   std::string_view file_name) {
  if (!path.empty() && !IsPathSeparator(path.back())) {
    path += kPathSeparator;
  }
  path += app_name;
  path += kPathSeparator;
  path += file_name;
  return path;
}

}  // namespace

std::string GetConfigFilePath(std::string_view app_name,
                              std::string_view file_name) {
  return Append(GetConfigDirectory(), app_name, file_name);
}

std::string GetCacheFilePath(std::string_view app_name,
                             std::string_view file_name) {
  return Append(GetCacheDirectory(), app_name, file_name);
}

}  // namespace coro::cloudstorage::util