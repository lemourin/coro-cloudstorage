#ifndef CORO_CLOUDSTORAGE_UTIL_ITEM_CONTENT_HANDLER_H
#define CORO_CLOUDSTORAGE_UTIL_ITEM_CONTENT_HANDLER_H

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/cloudstorage/util/cache_manager.h"
#include "coro/cloudstorage/util/clock.h"
#include "coro/http/http.h"
#include "coro/stdx/stop_token.h"

namespace coro::cloudstorage::util {

class ItemContentHandler {
 public:
  ItemContentHandler(AbstractCloudProvider* provider, const Clock* clock,
                     CloudProviderCacheManager cache_manager)
      : provider_(provider),
        clock_(clock),
        cache_manager_(std::move(cache_manager)) {}

  Task<http::Response<>> operator()(http::Request<> request,
                                    stdx::stop_token stop_token) const;

 private:
  AbstractCloudProvider* provider_;
  const Clock* clock_;
  CloudProviderCacheManager cache_manager_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_ITEM_CONTENT_HANDLER_H