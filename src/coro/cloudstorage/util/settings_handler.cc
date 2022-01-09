#include "coro/cloudstorage/util/settings_handler.h"

#include "coro/cloudstorage/util/assets.h"
#include "coro/http/http.h"

namespace coro::cloudstorage::util {

namespace {

std::string GetHostSelector() {
  return "<input id='host' type='text'></input>";
}

}  // namespace

auto SettingsHandler::operator()(Request request, stdx::stop_token) const
    -> Task<Response> {
  co_return Response{.status = 200,
                     .body = http::CreateBody(fmt::format(
                         fmt::runtime(util::kAssetsHtmlSettingsPageHtml),
                         fmt::arg("host_selector", GetHostSelector())))};
}

}  // namespace coro::cloudstorage::util