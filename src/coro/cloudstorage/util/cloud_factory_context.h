#ifndef CORO_CLOUDSTORAGE_UTIL_CLOUD_FACTORY_CONTEXT_H
#define CORO_CLOUDSTORAGE_UTIL_CLOUD_FACTORY_CONTEXT_H

#include "coro/cloudstorage/cloud_factory.h"
#include "coro/cloudstorage/util/account_manager_handler.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/cache_manager.h"
#include "coro/cloudstorage/util/muxer.h"
#include "coro/cloudstorage/util/random_number_generator.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"
#include "coro/http/cache_http.h"
#include "coro/http/curl_http.h"
#include "coro/http/http_server.h"
#include "coro/util/event_loop.h"
#include "coro/util/thread_pool.h"

namespace coro::cloudstorage::util {

class CloudFactoryContext {
 public:
  CloudFactoryContext(const coro::util::EventLoop* event_loop,
                      CloudFactoryConfig config);

  CloudFactoryContext(CloudFactoryContext&&) = delete;
  CloudFactoryContext& operator=(CloudFactoryContext&&) = delete;

  auto* factory() { return &factory_; }
  auto* thread_pool() { return &thread_pool_; }
  auto* cache() { return &cache_; }

  AccountManagerHandler CreateAccountManagerHandler(AccountListener listener);

  template <typename HandlerTypeT>
  http::HttpServer<HandlerTypeT> CreateHttpServer(HandlerTypeT handler) {
    return http::HttpServer<HandlerTypeT>(
        event_loop_, settings_manager_.GetHttpServerConfig(),
        std::move(handler));
  }

  http::HttpServer<AccountManagerHandler> CreateHttpServer(
      AccountListener listener);

 private:
  const coro::util::EventLoop* event_loop_;
  coro::util::ThreadPool thread_pool_;
  http::HttpImpl<http::CurlHttp> curl_http_;
  http::HttpImpl<http::CacheHttp> http_;
  coro::util::ThreadPool thumbnail_thread_pool_;
  util::ThumbnailGenerator thumbnail_generator_;
  util::Muxer muxer_;
  std::default_random_engine random_engine_;
  util::RandomNumberGenerator random_number_generator_;
  util::CacheManager cache_;
  CloudFactory factory_;
  util::SettingsManager settings_manager_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_CLOUD_FACTORY_CONTEXT_H
