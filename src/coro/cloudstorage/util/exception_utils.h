#ifndef CORO_CLOUDSTORAGE_EXCEPTION_UTILS
#define CORO_CLOUDSTORAGE_EXCEPTION_UTILS

#include <optional>
#include <string>

#include "coro/stdx/source_location.h"
#include "coro/stdx/stacktrace.h"

namespace coro::cloudstorage::util {

struct ErrorMetadata {
  std::optional<int> status;
  std::string what;
  std::optional<stdx::source_location> source_location;
  std::optional<stdx::stacktrace> stacktrace;
};

ErrorMetadata GetErrorMetadata(
    const std::exception_ptr& = std::current_exception());

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_EXCEPTION_UTILS