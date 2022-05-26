#ifndef CORO_CLOUDSTORAGE_WEBDAV_HANDLER_H
#define CORO_CLOUDSTORAGE_WEBDAV_HANDLER_H

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/cloudstorage/util/file_utils.h"
#include "coro/cloudstorage/util/handler_utils.h"
#include "coro/cloudstorage/util/webdav_utils.h"
#include "coro/http/http.h"

namespace coro::cloudstorage::util {

class WebDAVHandler {
 public:
  using Request = http::Request<>;
  using Response = http::Response<>;
  using CloudProvider = AbstractCloudProvider::CloudProvider;

  explicit WebDAVHandler(CloudProvider* provider) : provider_(provider) {}

  Task<Response> operator()(Request request, stdx::stop_token stop_token) const;

 private:
  CloudProvider* provider_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_WEBDAV_HANDLER_H
