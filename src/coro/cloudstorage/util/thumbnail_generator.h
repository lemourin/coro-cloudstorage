#ifndef CORO_CLOUDSTORAGE_UTIL_GENERATE_THUMBNAIL_H
#define CORO_CLOUDSTORAGE_UTIL_GENERATE_THUMBNAIL_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/abstract_cloud_provider.h>
#include <coro/cloudstorage/util/avio_context.h>
#include <coro/cloudstorage/util/generator_utils.h>
#include <coro/cloudstorage/util/thumbnail_options.h>
#include <coro/task.h>
#include <coro/util/thread_pool.h>

#include <future>

extern "C" {
#include <libavformat/avformat.h>
};

namespace coro::cloudstorage::util {

std::string GenerateThumbnail(AVIOContext* io_context,
                              ThumbnailOptions options);

template <typename ThreadPool, typename EventLoop>
class ThumbnailGenerator {
 public:
  ThumbnailGenerator(ThreadPool* thread_pool, EventLoop* event_loop)
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
  ThreadPool* thread_pool_;
  EventLoop* event_loop_;
};

class ThumbnailGeneratorF
    : public std::function<Task<std::string>(
          AbstractCloudProvider::CloudProvider*, AbstractCloudProvider::File,
          ThumbnailOptions, stdx::stop_token)> {
 public:
  template <typename F>
  explicit ThumbnailGeneratorF(const F* f)
      : std::function<Task<std::string>(AbstractCloudProvider::CloudProvider*,
                                        AbstractCloudProvider::File,
                                        ThumbnailOptions, stdx::stop_token)>(
            [f](AbstractCloudProvider::CloudProvider* p,
                AbstractCloudProvider::File file, ThumbnailOptions options,
                stdx::stop_token stop_token) {
              return (*f)(p, std::move(file), options, std::move(stop_token));
            }) {}
};

}  // namespace coro::cloudstorage::util

#endif  //  CORO_CLOUDSTORAGE_UTIL_GENERATE_THUMBNAIL_H