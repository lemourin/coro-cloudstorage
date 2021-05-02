#include "serialize_utils.h"

#include <iomanip>
#include <sstream>

namespace coro::cloudstorage::util {

std::string TimeStampToString(std::optional<int64_t> size) {
  if (!size) {
    return "";
  }
  std::tm tm = http::gmtime(static_cast<time_t>(*size));
  std::stringstream ss;
  ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return std::move(ss).str();
}

std::string SizeToString(std::optional<int64_t> size) {
  if (!size) {
    return "";
  }
  std::stringstream stream;
  stream << std::setprecision(2) << std::fixed;
  if (*size < 1'000) {
    stream << *size << "B";
  } else if (*size < 1'000'000) {
    stream << *size * 1e-3 << "KB";
  } else if (*size < 1'000'000'000) {
    stream << *size * 1e-6 << "MB";
  } else {
    stream << *size * 1e-9 << "GB";
  }
  return std::move(stream).str();
}

}  // namespace coro::cloudstorage::util