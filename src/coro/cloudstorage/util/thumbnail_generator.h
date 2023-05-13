#ifndef CORO_CLOUDSTORAGE_UTIL_GENERATE_THUMBNAIL_H
#define CORO_CLOUDSTORAGE_UTIL_GENERATE_THUMBNAIL_H

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/cloudstorage/util/thumbnail_options.h"
#include "coro/task.h"
#include "coro/util/thread_pool.h"

namespace coro::cloudstorage::util {

class ThumbnailGeneratorException : public std::exception {
 public:
  using std::exception::exception;
};

class ThumbnailGenerator {
 public:
  ThumbnailGenerator(coro::util::ThreadPool* thread_pool,
                     const coro::util::EventLoop* event_loop)
      : thread_pool_(thread_pool), event_loop_(event_loop) {}

  Task<std::string> operator()(AbstractCloudProvider* provider,
                               AbstractCloudProvider::File file,
                               ThumbnailOptions options,
                               stdx::stop_token stop_token) const;

 private:
  coro::util::ThreadPool* thread_pool_;
  const coro::util::EventLoop* event_loop_;
};

}  // namespace coro::cloudstorage::util

#endif  //  CORO_CLOUDSTORAGE_UTIL_GENERATE_THUMBNAIL_H