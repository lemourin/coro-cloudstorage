#include "coro/cloudstorage/util/string_utils.h"

#include <cstring>

namespace coro::cloudstorage::util {

std::string ErrorToString(int error_code) {
  size_t length = 1;
  while (true) {
    std::string buffer(length, 0);
    int error = [&] {
#if defined(HAVE_STRERROR_R) && defined(_GNU_SOURCE)
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

}  // namespace coro::cloudstorage::util
