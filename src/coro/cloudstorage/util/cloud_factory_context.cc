#include "coro/cloudstorage/util/cloud_factory_context.h"

#include "coro/cloudstorage/util/settings_utils.h"

namespace coro::cloudstorage::util {

CloudFactoryContext::CloudFactoryContext(
    const coro::util::EventLoop* event_loop, std::string config_path)
    : event_loop_(event_loop),
      thread_pool_(event_loop_),
      curl_http_(event_loop_, GetDirectoryPath(GetConfigFilePath())),
      http_(coro::http::CacheHttpConfig{}, &curl_http_),
      thumbnail_generator_(&thread_pool_, event_loop_),
      muxer_(event_loop_, &thread_pool_),
      random_engine_(std::random_device()()),
      random_number_generator_(&random_engine_),
      factory_(event_loop_, &thread_pool_, &http_, &thumbnail_generator_,
               &muxer_, &random_number_generator_),
      settings_manager_([&] {
        AuthTokenManager auth_token_manager(&factory_, config_path);
        return SettingsManager(std::move(auth_token_manager),
                               std::move(config_path));
      }()) {}

AccountManagerHandler CloudFactoryContext::CreateAccountManagerHandler(
    AccountListener listener) {
  return AccountManagerHandler(&factory_, &thumbnail_generator_,
                               std::move(listener), settings_manager_);
}

http::HttpServer<AccountManagerHandler> CloudFactoryContext::CreateHttpServer(
    AccountListener listener) {
  return CreateHttpServer(CreateAccountManagerHandler(std::move(listener)));
}

}  // namespace coro::cloudstorage::util