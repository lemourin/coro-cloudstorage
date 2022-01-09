#ifndef CORO_CLOUDSTORAGE_UTIL_STATIC_FILE_HANDLER_H
#define CORO_CLOUDSTORAGE_UTIL_STATIC_FILE_HANDLER_H

#include <fmt/format.h>

#include <optional>
#include <string_view>

#include "coro/cloudstorage/util/theme_handler.h"
#include "coro/http/http.h"

namespace coro::cloudstorage::util {

template <typename... CloudProviders>
class StaticFileHandler {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;

  Task<Response> operator()(Request request, stdx::stop_token) const {
    std::optional<std::string_view> content;
    std::string mime_type;
    Theme theme = GetTheme(request.headers);
    (GetIcon<CloudProviders>(request.url, content) || ...);
    if (content) {
      mime_type = "image/png";
    } else if (request.url == "/static/layout.css") {
      content = util::kAssetsStylesLayoutCss;
      mime_type = "text/css";
    } else if (request.url == "/static/colors.css" ||
               request.url == "/static/user-trash.svg" ||
               request.url == "/static/audio-x-generic.svg" ||
               request.url == "/static/image-x-generic.svg" ||
               request.url == "/static/unknown.svg" ||
               request.url == "/static/video-x-generic.svg" ||
               request.url == "/static/folder.svg") {
      co_return Resolve(theme, request.url);
    } else if (request.url == "/static/colors-light.css") {
      content = util::kAssetsStylesColorsLightCss;
      mime_type = "text/css";
    } else if (request.url == "/static/colors-dark.css") {
      content = util::kAssetsStylesColorsDarkCss;
      mime_type = "text/css";
    } else if (request.url == "/static/user-trash-light.svg") {
      content = util::kAssetsIconsPlaces64UserTrashSvg;
      mime_type = "image/svg+xml";
    } else if (request.url == "/static/user-trash-dark.svg") {
      content = util::kAssetsIconsDarkPlaces64UserTrashSvg;
      mime_type = "image/svg+xml";
    } else if (request.url == "/static/audio-x-generic-light.svg") {
      content = util::kAssetsIconsMimetypes64AudioXGenericSvg;
      mime_type = "image/svg+xml";
    } else if (request.url == "/static/audio-x-generic-dark.svg") {
      content = util::kAssetsIconsDarkMimetypes64AudioXGenericSvg;
      mime_type = "image/svg+xml";
    } else if (request.url == "/static/image-x-generic-light.svg") {
      content = util::kAssetsIconsMimetypes64ImageXGenericSvg;
      mime_type = "image/svg+xml";
    } else if (request.url == "/static/image-x-generic-dark.svg") {
      content = util::kAssetsIconsDarkMimetypes64ImageXGenericSvg;
      mime_type = "image/svg+xml";
    } else if (request.url == "/static/unknown-light.svg") {
      content = util::kAssetsIconsMimetypes64UnknownSvg;
      mime_type = "image/svg+xml";
    } else if (request.url == "/static/unknown-dark.svg") {
      content = util::kAssetsIconsDarkMimetypes64UnknownSvg;
      mime_type = "image/svg+xml";
    } else if (request.url == "/static/video-x-generic-light.svg") {
      content = util::kAssetsIconsMimetypes64VideoXGenericSvg;
      mime_type = "image/svg+xml";
    } else if (request.url == "/static/video-x-generic-dark.svg") {
      content = util::kAssetsIconsDarkMimetypes64VideoXGenericSvg;
      mime_type = "image/svg+xml";
    } else if (request.url == "/static/folder-light.svg") {
      content = util::kAssetsIconsPlaces64FolderSvg;
      mime_type = "image/svg+xml";
    } else if (request.url == "/static/folder-dark.svg") {
      content = util::kAssetsIconsDarkPlaces64FolderSvg;
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

 private:
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

  static Response Resolve(Theme theme, std::string_view url) {
    auto it = url.find_last_of('.');
    return Response{
        .status = 302,
        .headers = {{"Location", util::StrCat(url.substr(0, it), "-",
                                              ToString(theme), url.substr(it))},
                    {"Vary", "Cookie"}}};
  }
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_STATIC_FILE_HANDLER_H