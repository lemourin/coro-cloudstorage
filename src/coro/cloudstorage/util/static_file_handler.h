#ifndef CORO_CLOUDSTORAGE_UTIL_STATIC_FILE_HANDLER_H
#define CORO_CLOUDSTORAGE_UTIL_STATIC_FILE_HANDLER_H

#include <optional>
#include <string_view>

#include "coro/cloudstorage/util/abstract_cloud_factory.h"
#include "coro/cloudstorage/util/theme_handler.h"
#include "coro/http/http.h"

namespace coro::cloudstorage::util {

class StaticFileHandler {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;

  explicit StaticFileHandler(const AbstractCloudFactory* factory)
      : factory_(factory) {}

  Task<Response> operator()(Request request, stdx::stop_token) const;

 private:
  const AbstractCloudFactory* factory_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_STATIC_FILE_HANDLER_H