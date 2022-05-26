#ifndef CORO_CLOUDSTORAGE_SETTINGS_HANDLER_H
#define CORO_CLOUDSTORAGE_SETTINGS_HANDLER_H

#include <string>

#include "coro/cloudstorage/util/settings_manager.h"
#include "coro/http/http.h"
#include "coro/stdx/stop_token.h"

namespace coro::cloudstorage::util {

class SettingsHandler {
 public:
  using Request = http::Request<>;
  using Response = http::Response<>;

  explicit SettingsHandler(SettingsManager* settings_manager)
      : settings_manager_(settings_manager) {}

  Task<Response> operator()(Request request, stdx::stop_token stop_token) const;

 private:
  SettingsManager* settings_manager_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_SETTINGS_HANDLER_H
