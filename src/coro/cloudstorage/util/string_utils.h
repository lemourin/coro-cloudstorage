#ifndef CORO_CLOUDSTORAGE_UTIL_STRING_UTILS_H
#define CORO_CLOUDSTORAGE_UTIL_STRING_UTILS_H

#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "coro/http/http.h"

namespace coro::cloudstorage::util {

template <typename T>
struct FromStringT;

template <typename T>
T FromString(std::string sv) {
  return FromStringT<T>{}(std::move(sv));
}

template <>
inline std::string FromString(std::string sv) {
  return sv;
}

template <>
inline uint64_t FromString<uint64_t>(std::string sv) {
  return std::stoull(sv);
}

template <>
inline int64_t FromString<int64_t>(std::string sv) {
  return std::stoll(sv);
}

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

template <typename T1, typename T2>
std::string_view ToStringView(T1 begin, T2 end) {
  if (begin == end) {
    return std::string_view();
  } else {
    return std::string_view(&*begin, end - begin);
  }
}

std::string Trim(std::string input, http::Range range);

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_STRING_UTILS_H
