#ifndef CORO_CLOUDSTORAGE_WEBDAV_UTILS_H
#define CORO_CLOUDSTORAGE_WEBDAV_UTILS_H

#include <optional>
#include <span>
#include <string>

#include "coro/generator.h"

namespace coro::cloudstorage::util {

struct ElementData {
  std::string path;
  std::string name;
  bool is_directory;
  std::optional<int64_t> size;
  std::optional<std::string> mime_type;
  std::optional<int64_t> timestamp;
};

std::string GetMultiStatusResponse(std::span<const std::string> responses);
std::string GetElement(const ElementData&);

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_WEBDAV_UTILS_H
