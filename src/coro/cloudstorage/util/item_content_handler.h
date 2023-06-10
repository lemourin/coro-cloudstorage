#ifndef CORO_CLOUDSTORAGE_UTIL_ITEM_CONTENT_HANDLER_H
#define CORO_CLOUDSTORAGE_UTIL_ITEM_CONTENT_HANDLER_H

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/cloudstorage/util/clock.h"
#include "coro/cloudstorage/util/cloud_provider_account.h"
#include "coro/http/http.h"
#include "coro/stdx/stop_token.h"

namespace coro::cloudstorage::util {

class ItemContentHandler {
 public:
  explicit ItemContentHandler(CloudProviderAccount account)
      : account_(std::move(account)) {}

  Task<http::Response<>> operator()(http::Request<> request,
                                    stdx::stop_token stop_token) const;

 private:
  CloudProviderAccount account_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_ITEM_CONTENT_HANDLER_H