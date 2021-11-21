#include "coro/cloudstorage/util/string_utils.h"

#include <cstring>

namespace coro::cloudstorage::util {

std::string ErrorToString(int error_code) {
  size_t length = 1;
  while (true) {
    std::string buffer(length, 0);
    int error = [&] {
#if defined(HAVE_GLIBC_STRERROR_R)
      errno = 0;
      if (const char* ret = strerror_r(error_code, buffer.data(), length);
          ret && errno == 0) {
        buffer = ret;
        return 0;
      } else {
        return errno;
      }
#elif defined(HAVE_STRERROR_R)
      int err = strerror_r(error_code, buffer.data(), length);
      if (err == -1) {
        return errno;
      } else {
        return err;
      }
#elif defined(HAVE_STRERROR_S)
      return strerror_s(buffer.data(), length, error_code);
#else
#error "threadsafe strerror is missing"
#endif
    }();
    if (error == 0) {
      buffer.resize(strlen(buffer.c_str()));
      return buffer;
    } else if (error == ERANGE) {
      length *= 2;
    } else {
      return util::StrCat("error ", error_code);
    }
  }
}

std::string_view TrimWhitespace(std::string_view input) {
  int it1 = 0;
  int it2 = input.size() - 1;

  while (it1 < input.size() && std::isspace(input[it1])) {
    it1++;
  }
  while (it2 >= it1 && std::isspace(input[it2])) {
    it2--;
  }
  return input.substr(it1, it2 - it1 + 1);
}

}  // namespace coro::cloudstorage::util
