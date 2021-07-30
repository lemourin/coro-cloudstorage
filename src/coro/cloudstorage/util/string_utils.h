#ifndef CORO_CLOUDSTORAGE_UTIL_STRING_UTILS_H
#define CORO_CLOUDSTORAGE_UTIL_STRING_UTILS_H

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace coro::cloudstorage::util {

template <typename T>
std::string ToString(T d) {
  std::stringstream stream;
  stream << std::move(d);
  return std::move(stream).str();
}

inline std::string ToString(std::string_view sv) { return std::string(sv); }

inline std::string StrCat() { return ""; }

template <typename Head, typename... Tail>
std::string StrCat(Head&& head, Tail&&... tail) {
  std::string result = ToString(std::forward<Head>(head));
  result += StrCat(std::forward<Tail>(tail)...);
  return result;
}

template <typename T, typename C>
std::vector<T> SplitString(const T& string, C delim) {
  std::vector<T> result;
  T current;
  for (auto c : string) {
    if (c == delim) {
      if (!current.empty()) {
        result.emplace_back(std::move(current));
      }
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) {
    result.emplace_back(std::move(current));
  }
  return result;
}

std::string ErrorToString(int error_code);

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_STRING_UTILS_H
