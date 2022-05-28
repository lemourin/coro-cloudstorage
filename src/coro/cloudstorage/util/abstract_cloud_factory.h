#ifndef CORO_CLOUDSTORAGE_ABSTRACT_CLOUD_FACTORY_H
#define CORO_CLOUDSTORAGE_ABSTRACT_CLOUD_FACTORY_H

#include "coro/cloudstorage/util/abstract_cloud_provider.h"

namespace coro::cloudstorage::util {

class AbstractCloudFactory {
 public:
  virtual ~AbstractCloudFactory() = default;

  virtual std::unique_ptr<AbstractCloudProvider::CloudProvider> Create(
      AbstractCloudProvider::Auth::AuthToken auth_token,
      std::function<void(const AbstractCloudProvider::Auth::AuthToken&)>
          on_token_updated) const = 0;

  virtual std::unique_ptr<AbstractCloudProvider::Auth> CreateAuth(
      AbstractCloudProvider::Type) const = 0;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_ABSTRACT_CLOUD_FACTORY_H
