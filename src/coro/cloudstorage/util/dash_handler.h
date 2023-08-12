#ifndef CORO_CLOUDSTORAGE_UTIL_DASH_HANDLER_H
#define CORO_CLOUDSTORAGE_UTIL_DASH_HANDLER_H

#include "coro/http/http.h"
#include "coro/stdx/any_invocable.h"

namespace coro::cloudstorage::util {

class DashHandler {
 public:
  DashHandler(stdx::any_invocable<std::string(std::string_view item_id) const>
                  content_url_generator,
              stdx::any_invocable<std::string(std::string_view item_id) const>
                  thumbnail_url_generator)
      : content_url_generator_(std::move(content_url_generator)),
        thumbnail_url_generator_(std::move(thumbnail_url_generator)) {}

  Task<http::Response<>> operator()(http::Request<> request,
                                    stdx::stop_token stop_token) const;

 private:
  stdx::any_invocable<std::string(std::string_view item_id) const>
      content_url_generator_;
  stdx::any_invocable<std::string(std::string_view item_id) const>
      thumbnail_url_generator_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_DASH_HANDLER_H