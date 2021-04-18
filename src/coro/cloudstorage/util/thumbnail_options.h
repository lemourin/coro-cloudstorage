#ifndef CORO_CLOUDSTORAGE_UTIL_THUMBNAIL_OPTIONS
#define CORO_CLOUDSTORAGE_UTIL_THUMBNAIL_OPTIONS

namespace coro::cloudstorage::util {

struct ThumbnailOptions {
  int size = 256;
  enum class Codec { PNG, JPEG } codec;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_THUMBNAIL_OPTIONS