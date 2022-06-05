#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_UTILS_H_
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_UTILS_H_

#include <string_view>

namespace coro::cloudstorage::util {

enum class FileType { kUnknown, kVideo, kAudio, kImage };

FileType GetFileType(std::string_view mime_type);

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_UTILS_H_