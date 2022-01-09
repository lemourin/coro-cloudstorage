#ifndef CORO_CLOUDSTORAGE_SETTINGS_HANDLER_H
#define CORO_CLOUDSTORAGE_SETTINGS_HANDLER_H

#include <fmt/format.h>

#include "coro/http/http.h"

namespace coro::cloudstorage::util {

namespace internal {

Task<http::Response<>> GetSettingsHandlerResponse(http::Request<> request,
                                                  stdx::stop_token);

}

template <typename SettingsManagerT>
struct SettingsHandler {
  using Request = http::Request<>;
  using Response = http::Response<>;

  Task<Response> operator()(Request request,
                            stdx::stop_token stop_token) const {
    return internal::GetSettingsHandlerResponse(std::move(request),
                                                std::move(stop_token));
  }

  SettingsManagerT* settings_manager;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_SETTINGS_HANDLER_H
