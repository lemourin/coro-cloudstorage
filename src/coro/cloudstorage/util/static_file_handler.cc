#include "coro/cloudstorage/util/static_file_handler.h"

#include <fmt/format.h>

#include "coro/cloudstorage/util/assets.h"

namespace coro::cloudstorage::util {

namespace {

using Response = http::Response<>;

Response Resolve(Theme theme, std::string_view url) {
  auto it = url.find_last_of('.');
  return Response{
      .status = 302,
      .headers = {{"Location", util::StrCat(url.substr(0, it), "-",
                                            ToString(theme), url.substr(it))},
                  {"Vary", "Cookie"}}};
}

}  // namespace

Task<Response> StaticFileHandler::operator()(Request request,
                                             stdx::stop_token) const {
  std::optional<std::string_view> content;
  std::string mime_type;
  Theme theme = GetTheme(request.headers);

  for (auto type : factory_->GetSupportedCloudProviders()) {
    const auto& auth = factory_->GetAuth(type);
    if (request.url == util::StrCat("/static/", auth.GetId(), ".png")) {
      content = auth.GetIcon();
    }
  }

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
             request.url == "/static/folder.svg" ||
             request.url == "/static/configure-settings.svg" ||
             request.url == "/static/go-previous.svg") {
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
  } else if (request.url == "/static/configure-settings-light.svg") {
    content = util::kAssetsIconsActions32SettingsConfigureSvg;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/configure-settings-dark.svg") {
    content = util::kAssetsIconsDarkActions32SettingsConfigureSvg;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/go-previous-light.svg") {
    content = util::kAssetsIconsActions32GoPreviousSvg;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/go-previous-dark.svg") {
    content = util::kAssetsIconsDarkActions32GoPreviousSvg;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/account_list_main.js") {
    content = util::kAssetsJsAccountListMainJs;
    mime_type = "text/javascript;charset=UTF-8";
  } else if (request.url == "/static/settings_main.js") {
    content = util::kAssetsJsSettingsMainJs;
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

}  // namespace coro::cloudstorage::util