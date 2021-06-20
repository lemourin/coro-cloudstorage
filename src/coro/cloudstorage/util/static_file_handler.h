#ifndef CORO_CLOUDSTORAGE_UTIL_STATIC_FILE_HANDLER_H
#define CORO_CLOUDSTORAGE_UTIL_STATIC_FILE_HANDLER_H

#include <coro/http/http.h>

#include <optional>
#include <string_view>

namespace coro::cloudstorage::util {

template <typename... CloudProviders>
struct StaticFileHandler {
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;

  template <typename CloudProvider>
  static bool GetIcon(std::string_view name,
                      std::optional<std::string_view>& output) {
    if (name == StrCat("/static/", CloudProvider::kId, ".png")) {
      output = CloudProvider::kIcon;
      return true;
    } else {
      return false;
    }
  }

  Task<Response> operator()(Request request, stdx::stop_token) const {
    std::optional<std::string_view> content;
    std::string mime_type;
    (GetIcon<CloudProviders>(request.url, content) || ...);
    if (content) {
      mime_type = "image/png";
    } else if (request.url == "/static/default.css") {
      content = util::kAssetsStylesDefaultCss;
      mime_type = "text/css";
    } else if (request.url == "/static/user-trash.svg") {
      content = util::kAssetsIconsUserTrashSvg;
      mime_type = "image/svg+xml";
    } else if (request.url == "/static/account_list_main.js") {
      content = util::kAssetsJsAccountListMainJs;
      mime_type = "text/javascript;charset=UTF-8";
    }
    if (!content) {
      co_return Response{.status = 404};
    }
    co_return Response{
        .status = 200,
        .headers = {{"Content-Type", std::move(mime_type)},
                    {"Content-Length", std::to_string(content->size())},
                    {"Cache-Control", "public"},
                    {"Cache-Control", "max-age=604800"}},
        .body = http::CreateBody(std::string(*content))};
  }
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_STATIC_FILE_HANDLER_H