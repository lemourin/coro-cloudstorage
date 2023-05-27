#ifndef CORO_CLOUDSTORAGE_UTIL_ITEM_THUMBNAIL_HANDLER_H
#define CORO_CLOUDSTORAGE_UTIL_ITEM_THUMBNAIL_HANDLER_H

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/cloudstorage/util/cache_manager.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"
#include "coro/http/http.h"
#include "coro/stdx/stop_token.h"
#include "coro/task.h"

namespace coro::cloudstorage::util {

class ItemThumbnailHandler {
 public:
  ItemThumbnailHandler(AbstractCloudProvider* provider,
                       const ThumbnailGenerator* thumbnail_generator,
                       CloudProviderCacheManager cache_manager)
      : provider_(provider),
        thumbnail_generator_(thumbnail_generator),
        cache_manager_(std::move(cache_manager)) {}

  Task<http::Response<>> operator()(http::Request<> request,
                                    stdx::stop_token stop_token) const;

 private:
  AbstractCloudProvider* provider_;
  const ThumbnailGenerator* thumbnail_generator_;
  CloudProviderCacheManager cache_manager_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_ITEM_THUMBNAIL_HANDLER_H