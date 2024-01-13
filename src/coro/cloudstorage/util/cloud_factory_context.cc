#include "coro/cloudstorage/util/cloud_factory_context.h"

namespace coro::cloudstorage::util {

using ::coro::http::CurlHttpConfig;

CloudFactoryServer::CloudFactoryServer(
    AccountManagerHandler account_manager,
    const coro::util::EventLoop* event_loop,
    const coro::util::TcpServer::Config& config)
    : account_manager_(std::move(account_manager)),
      http_server_(coro::http::CreateHttpServer(
          [&]<typename... Args>(Args... args) {
            return account_manager_(std::forward<Args>(args)...);
          },
          event_loop, config)) {}

Task<> CloudFactoryServer::Quit() {
  account_manager_.Quit();
  return http_server_.Quit();
}

CloudFactoryContext::CloudFactoryContext(CloudFactoryConfig config)
    : event_loop_(config.event_loop),
      cache_db_(CreateCacheDatabase(config.cache_path)),
      thread_pool_(event_loop_, (std::thread::hardware_concurrency() + 1) / 2,
                   "coro-tpool"),
      http_(std::move(config.http)),
      cached_http_(http::CacheHttp(config.http_cache_config, &http_)),
      thumbnail_thread_pool_(
          event_loop_, std::thread::hardware_concurrency() / 2, "coro-thumb"),
      thumbnail_generator_(&thumbnail_thread_pool_, event_loop_),
      muxer_(event_loop_, &thumbnail_thread_pool_),
      random_number_generator_(std::move(config.random_number_generator)),
      cache_(cache_db_.get(), event_loop_),
      factory_(event_loop_, &thread_pool_, &cached_http_, &thumbnail_generator_,
               &muxer_, &random_number_generator_, config.auth_data),
      settings_manager_(&factory_, std::move(config)) {}

AccountManagerHandler CloudFactoryContext::CreateAccountManagerHandler(
    AccountListener listener) {
  return {&factory_,           &thumbnail_generator_, &muxer_, &clock_,
          std::move(listener), &settings_manager_,    &cache_};
}

coro::util::TcpServer CloudFactoryContext::CreateHttpServer(
    coro::http::HttpHandler handler) {
  return coro::http::CreateHttpServer(std::move(handler), event_loop_,
                                      settings_manager_.GetHttpServerConfig());
}

CloudFactoryServer CloudFactoryContext::CreateHttpServer(
    AccountListener listener) {
  return {CreateAccountManagerHandler(std::move(listener)), event_loop_,
          settings_manager_.GetHttpServerConfig()};
}

}  // namespace coro::cloudstorage::util