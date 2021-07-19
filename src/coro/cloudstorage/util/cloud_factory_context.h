#ifndef CORO_CLOUDSTORAGE_UTIL_CLOUD_FACTORY_CONTEXT_H
#define CORO_CLOUDSTORAGE_UTIL_CLOUD_FACTORY_CONTEXT_H

#include <coro/cloudstorage/util/muxer.h>
#include <coro/cloudstorage/util/random_number_generator.h>
#include <coro/cloudstorage/util/thumbnail_generator.h>
#include <coro/http/cache_http.h>
#include <coro/http/curl_http.h>
#include <coro/util/event_loop.h>
#include <coro/util/thread_pool.h>

namespace coro::cloudstorage::util {

template <typename AuthData = coro::cloudstorage::util::AuthData>
class CloudFactoryContext {
 public:
  using HttpT = http::CacheHttp<http::CurlHttp>;
  using EventLoopT = coro::util::EventLoop;
  using ThreadPoolT = coro::util::ThreadPool<EventLoopT>;
  using ThumbnailGeneratorT = util::ThumbnailGenerator<ThreadPoolT, EventLoopT>;
  using MuxerT = util::Muxer<EventLoopT, ThreadPoolT>;
  using RandomNumberGeneratorT =
      util::RandomNumberGenerator<std::default_random_engine>;
  using CloudFactoryT = CloudFactory<EventLoopT, HttpT, ThumbnailGeneratorT,
                                     MuxerT, RandomNumberGeneratorT, AuthData>;

  CloudFactoryContext(event_base* event_base)
      : event_loop_(event_base),
        thread_pool_(&event_loop_),
        http_(coro::http::CacheHttpConfig{}, event_base),
        thumbnail_generator_(&thread_pool_, &event_loop_),
        muxer_(&event_loop_, &thread_pool_),
        random_engine_(std::random_device()()),
        random_number_generator_(&random_engine_),
        factory_(&event_loop_, &http_, &thumbnail_generator_, &muxer_,
                 &random_number_generator_) {}

  CloudFactoryContext(CloudFactoryContext&&) = delete;
  CloudFactoryContext& operator=(CloudFactoryContext&&) = delete;

  CloudFactoryT* factory() { return &factory_; }
  EventLoopT* event_loop() { return &event_loop_; }
  ThreadPoolT* thread_pool() { return &thread_pool_; }
  ThumbnailGeneratorT* thumbnail_generator() { return &thumbnail_generator_; }

 private:
  EventLoopT event_loop_;
  ThreadPoolT thread_pool_;
  HttpT http_;
  ThumbnailGeneratorT thumbnail_generator_;
  MuxerT muxer_;
  std::default_random_engine random_engine_;
  RandomNumberGeneratorT random_number_generator_;
  CloudFactoryT factory_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_CLOUD_FACTORY_CONTEXT_H