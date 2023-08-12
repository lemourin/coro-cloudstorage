#ifndef CORO_CLOUDSTORAGE_ABSTRACT_CLOUD_FACTORY_H
#define CORO_CLOUDSTORAGE_ABSTRACT_CLOUD_FACTORY_H

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/cloudstorage/util/item_url_provider.h"
#include "coro/cloudstorage/util/on_auth_token_updated.h"

namespace coro::cloudstorage::util {

class AbstractCloudFactory {
 public:
  virtual ~AbstractCloudFactory() = default;

  virtual std::unique_ptr<AbstractCloudProvider> Create(
      AbstractCloudProvider::Auth::AuthToken auth_token,
      OnAuthTokenUpdated<AbstractCloudProvider::Auth::AuthToken>
          on_token_updated,
      ItemUrlProvider item_url_provider) const = 0;

  virtual const AbstractCloudProvider::Auth& GetAuth(
      AbstractCloudProvider::Type) const = 0;

  virtual std::span<const AbstractCloudProvider::Type>
  GetSupportedCloudProviders() const = 0;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_ABSTRACT_CLOUD_FACTORY_H
