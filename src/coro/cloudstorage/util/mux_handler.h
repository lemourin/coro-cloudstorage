#ifndef CORO_CLOUDSTORAGE_MUX_HANDLER_H
#define CORO_CLOUDSTORAGE_MUX_HANDLER_H

#include "coro/cloudstorage/util/cloud_provider_account.h"
#include "coro/cloudstorage/util/muxer.h"
#include "coro/http/http.h"
#include "coro/stdx/stop_token.h"

namespace coro::cloudstorage::util {

class MuxHandler {
 public:
  MuxHandler(const Muxer* muxer,
             std::span<const std::shared_ptr<CloudProviderAccount>> accounts)
      : muxer_(muxer), accounts_(accounts) {}

  Task<http::Response<>> operator()(http::Request<> request,
                                    stdx::stop_token stop_token) const;

 private:
  const Muxer* muxer_;
  std::span<const std::shared_ptr<CloudProviderAccount>> accounts_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_MUX_HANDLER_H
