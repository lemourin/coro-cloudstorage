#include "coro/cloudstorage/util/static_file_handler.h"

#include <fmt/format.h>

#include "coro/cloudstorage/util/assets.h"

namespace coro::cloudstorage::util {

namespace {

using Response = http::Response<>;

Response Resolve(Theme theme, std::string_view url) {
  auto it = url.find_last_of('.');
  return Response{
      .status = 301,
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
    content = util::kLayoutCss;
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
    content = util::kColorsLightCss;
    mime_type = "text/css";
  } else if (request.url == "/static/colors-dark.css") {
    content = util::kColorsDarkCss;
    mime_type = "text/css";
  } else if (request.url == "/static/user-trash-light.svg") {
    content = util::kTrashIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/user-trash-dark.svg") {
    content = util::kDarkTrashIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/audio-x-generic-light.svg") {
    content = util::kAudioIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/audio-x-generic-dark.svg") {
    content = util::kDarkAudioIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/image-x-generic-light.svg") {
    content = util::kImageIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/image-x-generic-dark.svg") {
    content = util::kDarkImageIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/unknown-light.svg") {
    content = util::kUnknownIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/unknown-dark.svg") {
    content = util::kDarkUnknownIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/video-x-generic-light.svg") {
    content = util::kVideoIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/video-x-generic-dark.svg") {
    content = util::kDarkVideoIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/folder-light.svg") {
    content = util::kFolderIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/folder-dark.svg") {
    content = util::kDarkFolderIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/configure-settings-light.svg") {
    content = util::kSettingsIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/configure-settings-dark.svg") {
    content = util::kDarkSettingsIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/go-previous-light.svg") {
    content = util::kGoBackIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/go-previous-dark.svg") {
    content = util::kDarkGoBackIcon;
    mime_type = "image/svg+xml";
  } else if (request.url == "/static/account_list_main.js") {
    content = util::kAccountListMainJs;
    mime_type = "text/javascript;charset=UTF-8";
  } else if (request.url == "/static/settings_main.js") {
    content = util::kSettingsMainJs;
    mime_type = "text/javascript;charset=UTF-8";
  } else if (request.url == "/static/favicon.ico") {
    content = util::kFavIcon;
    mime_type = "image/x-icon";
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