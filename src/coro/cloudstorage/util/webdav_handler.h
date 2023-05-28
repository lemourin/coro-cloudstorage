#ifndef CORO_CLOUDSTORAGE_WEBDAV_HANDLER_H
#define CORO_CLOUDSTORAGE_WEBDAV_HANDLER_H

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/cloudstorage/util/file_utils.h"
#include "coro/cloudstorage/util/handler_utils.h"
#include "coro/cloudstorage/util/webdav_utils.h"
#include "coro/http/http.h"

namespace coro::cloudstorage::util {

class WebDAVHandler {
 public:
  explicit WebDAVHandler(AbstractCloudProvider* provider)
      : provider_(provider) {}

  Task<http::Response<>> operator()(http::Request<> request,
                                    stdx::stop_token stop_token) const;

 private:
  AbstractCloudProvider* provider_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_WEBDAV_HANDLER_H
