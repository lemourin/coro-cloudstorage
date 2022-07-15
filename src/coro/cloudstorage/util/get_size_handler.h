#ifndef CORO_CLOUDSTORAGE_UTIL_GET_SIZE_HANDLER_H
#define CORO_CLOUDSTORAGE_UTIL_GET_SIZE_HANDLER_H

#include <list>

#include "coro/cloudstorage/util/cloud_provider_account.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"
#include "coro/stdx/stop_token.h"

namespace coro::cloudstorage::util {

struct GetSizeHandler {
  using Request = http::Request<>;
  using Response = http::Response<>;

  Task<Response> operator()(Request request, stdx::stop_token stop_token) const;

  std::span<std::shared_ptr<CloudProviderAccount>> accounts;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_GET_SIZE_HANDLER_H