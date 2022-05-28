#ifndef CORO_CLOUDSTORAGE_CLOUD_FACTORY_H
#define CORO_CLOUDSTORAGE_CLOUD_FACTORY_H

#include <boost/di.hpp>
#include <random>

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/abstract_cloud_factory.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/auth_handler.h"
#include "coro/cloudstorage/util/auth_manager.h"
#include "coro/cloudstorage/util/muxer.h"
#include "coro/cloudstorage/util/random_number_generator.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"
#include "coro/http/http.h"
#include "coro/util/event_loop.h"
#include "coro/util/type_list.h"

namespace coro::cloudstorage {

class CloudFactory : public util::AbstractCloudFactory {
 public:
  CloudFactory(const coro::util::EventLoop* event_loop,
               coro::util::ThreadPool* thread_pool,
               const coro::http::Http* http,
               const util::ThumbnailGenerator* thumbnail_generator,
               const util::Muxer* muxer,
               util::RandomNumberGenerator* random_number_generator,
               util::AuthData auth_data = util::AuthData{})
      : event_loop_(event_loop),
        thread_pool_(thread_pool),
        http_(http),
        thumbnail_generator_(thumbnail_generator),
        muxer_(muxer),
        random_number_generator_(random_number_generator),
        auth_data_(auth_data) {}

  std::unique_ptr<util::AbstractCloudProvider::CloudProvider> Create(
      util::AbstractCloudProvider::Auth::AuthToken auth_token,
      std::function<void(const util::AbstractCloudProvider::Auth::AuthToken&)>
          on_token_updated) const override;

  std::unique_ptr<util::AbstractCloudProvider::Auth> CreateAuth(
      util::AbstractCloudProvider::Type) const override;

  std::vector<util::AbstractCloudProvider::Type> GetSupportedCloudProviders()
      const override;

 private:
  std::unique_ptr<util::AbstractCloudFactory> CreateCloudFactory(
      util::AbstractCloudProvider::Type type) const;

  const coro::util::EventLoop* event_loop_;
  coro::util::ThreadPool* thread_pool_;
  const coro::http::Http* http_;
  const util::ThumbnailGenerator* thumbnail_generator_;
  const util::Muxer* muxer_;
  util::RandomNumberGenerator* random_number_generator_;
  util::AuthData auth_data_;
};

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_FACTORY_H
