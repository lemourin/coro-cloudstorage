#ifndef CORO_CLOUDSTORAGE_UTIL_CLOUD_FACTORY_CONTEXT_H
#define CORO_CLOUDSTORAGE_UTIL_CLOUD_FACTORY_CONTEXT_H

#include "coro/cloudstorage/cloud_factory.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/muxer.h"
#include "coro/cloudstorage/util/random_number_generator.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"
#include "coro/http/cache_http.h"
#include "coro/http/curl_http.h"
#include "coro/util/event_loop.h"
#include "coro/util/thread_pool.h"

namespace coro::cloudstorage::util {

class CloudFactoryContext {
 public:
  explicit CloudFactoryContext(const coro::util::EventLoop* event_loop);

  CloudFactoryContext(CloudFactoryContext&&) = delete;
  CloudFactoryContext& operator=(CloudFactoryContext&&) = delete;

  auto* factory() { return &factory_; }
  auto* event_loop() { return event_loop_; }
  auto* thread_pool() { return &thread_pool_; }
  auto* thumbnail_generator() { return &thumbnail_generator_; }

 private:
  const coro::util::EventLoop* event_loop_;
  coro::util::ThreadPool thread_pool_;
  http::HttpImpl<http::CurlHttp> curl_http_;
  http::HttpImpl<http::CacheHttp> http_;
  util::ThumbnailGenerator thumbnail_generator_;
  util::Muxer muxer_;
  std::default_random_engine random_engine_;
  util::RandomNumberGenerator random_number_generator_;
  CloudFactory factory_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_CLOUD_FACTORY_CONTEXT_H
