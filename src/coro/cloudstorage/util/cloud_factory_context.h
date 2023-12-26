#ifndef CORO_CLOUDSTORAGE_UTIL_CLOUD_FACTORY_CONTEXT_H
#define CORO_CLOUDSTORAGE_UTIL_CLOUD_FACTORY_CONTEXT_H

#include "coro/cloudstorage/cloud_factory.h"
#include "coro/cloudstorage/util/account_manager_handler.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/cache_manager.h"
#include "coro/cloudstorage/util/clock.h"
#include "coro/cloudstorage/util/muxer.h"
#include "coro/cloudstorage/util/random_number_generator.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"
#include "coro/http/cache_http.h"
#include "coro/http/curl_http.h"
#include "coro/http/http_server.h"
#include "coro/util/event_loop.h"
#include "coro/util/thread_pool.h"

namespace coro::cloudstorage::util {

class CloudFactoryServer {
 public:
  CloudFactoryServer(AccountManagerHandler,
                     const coro::util::EventLoop* event_loop,
                     const coro::util::TcpServer::Config& config);

  Task<> Quit();

 private:
  AccountManagerHandler account_manager_;
  coro::util::TcpServer http_server_;
};

class CloudFactoryContext {
 public:
  CloudFactoryContext(const coro::util::EventLoop* event_loop,
                      CloudFactoryConfig config);

  CloudFactoryContext(CloudFactoryContext&&) = delete;
  CloudFactoryContext& operator=(CloudFactoryContext&&) = delete;

  auto* factory() { return &factory_; }
  auto* thread_pool() { return &thread_pool_; }
  auto* cache() { return &cache_; }
  auto* clock() { return &clock_; }

  AccountManagerHandler CreateAccountManagerHandler(AccountListener listener);
  coro::util::TcpServer CreateHttpServer(coro::http::HttpHandler handler);
  CloudFactoryServer CreateHttpServer(AccountListener listener);

 private:
  const coro::util::EventLoop* event_loop_;
  std::unique_ptr<CacheDatabase, CacheDatabaseDeleter> cache_db_;
  coro::util::ThreadPool thread_pool_;
  http::Http curl_http_;
  http::Http http_;
  coro::util::ThreadPool thumbnail_thread_pool_;
  util::ThumbnailGenerator thumbnail_generator_;
  util::Muxer muxer_;
  std::default_random_engine random_engine_;
  util::RandomNumberGenerator random_number_generator_;
  util::CacheManager cache_;
  CloudFactory factory_;
  util::SettingsManager settings_manager_;
  util::Clock clock_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_CLOUD_FACTORY_CONTEXT_H
