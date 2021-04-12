#ifndef CORO_CLOUDSTORAGE_UTIL_GENERATE_THUMBNAIL_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/task.h>

namespace coro::cloudstorage::util {

template <typename CloudProvider, IsFile<CloudProvider> File>
Task<std::string> GenerateThumbnail(const CloudProvider& provider, File file,
                                    stdx::stop_token stop_token) {
  throw std::runtime_error("unimplemented");
}

}  // namespace coro::cloudstorage::util

#endif  //  CORO_CLOUDSTORAGE_UTIL_GENERATE_THUMBNAIL_H