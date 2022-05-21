#ifndef CORO_CLOUDSTORAGE_UTIL_GENERATE_THUMBNAIL_H
#define CORO_CLOUDSTORAGE_UTIL_GENERATE_THUMBNAIL_H

#include <future>

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/cloudstorage/util/avio_context.h"
#include "coro/cloudstorage/util/generator_utils.h"
#include "coro/cloudstorage/util/thumbnail_options.h"
#include "coro/task.h"
#include "coro/util/thread_pool.h"

extern "C" {
#include <libavformat/avformat.h>
};

namespace coro::cloudstorage::util {

std::string GenerateThumbnail(AVIOContext* io_context,
                              ThumbnailOptions options);

class ThumbnailGenerator {
 public:
  ThumbnailGenerator(coro::util::ThreadPool* thread_pool,
                     coro::util::EventLoop* event_loop)
      : thread_pool_(thread_pool), event_loop_(event_loop) {}

  template <typename CloudProvider, IsFile<CloudProvider> File>
  Task<std::string> operator()(CloudProvider* provider, File file,
                               ThumbnailOptions options,
                               stdx::stop_token stop_token) const {
    decltype(CreateIOContext(event_loop_, provider, file,
                             stop_token)) io_context;
    co_return co_await thread_pool_->Do([&] {
      io_context = CreateIOContext(event_loop_, provider, std::move(file),
                                   std::move(stop_token));
      return GenerateThumbnail(io_context.get(), options);
    });
  }

 private:
  coro::util::ThreadPool* thread_pool_;
  coro::util::EventLoop* event_loop_;
};

}  // namespace coro::cloudstorage::util

#endif  //  CORO_CLOUDSTORAGE_UTIL_GENERATE_THUMBNAIL_H