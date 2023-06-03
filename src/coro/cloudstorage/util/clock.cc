#include "coro/cloudstorage/util/clock.h"

#include <chrono>

namespace coro::cloudstorage::util {

int64_t Clock::Now() const {
  return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

}  // namespace coro::cloudstorage::util