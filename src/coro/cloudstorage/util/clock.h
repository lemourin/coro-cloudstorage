#ifndef CORO_CLOUDSTORAGE_UTIL_CLOCK_H
#define CORO_CLOUDSTORAGE_UTIL_CLOCK_H

#include <cstdint>

namespace coro::cloudstorage::util {

class Clock {
 public:
  int64_t Now() const;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_CLOCK_H