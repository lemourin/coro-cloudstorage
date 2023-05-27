#ifndef CORO_CLOUDSTORAGE_UTIL_ITEM_CONTENT_HANDLER_H
#define CORO_CLOUDSTORAGE_UTIL_ITEM_CONTENT_HANDLER_H

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/http/http.h"
#include "coro/stdx/stop_token.h"

namespace coro::cloudstorage::util {

class ItemContentHandler {
 public:
  explicit ItemContentHandler(AbstractCloudProvider* provider)
      : provider_(provider) {}

  Task<http::Response<>> operator()(http::Request<> request,
                                    stdx::stop_token stop_token) const;

 private:
  AbstractCloudProvider* provider_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_ITEM_CONTENT_HANDLER_H