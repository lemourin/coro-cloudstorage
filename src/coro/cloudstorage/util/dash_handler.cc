#include "coro/cloudstorage/util/dash_handler.h"

#include <fmt/format.h>

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/util/regex.h"

namespace coro::cloudstorage::util {

namespace {

namespace re = coro::util::re;

Generator<std::string> GetDashPlayer(std::string path,
                                     std::string thumbnail_url) {
  std::stringstream stream;
  stream << "<source src='" << path << "'>";
  std::string content = fmt::format(
      fmt::runtime(kDashPlayerHtml), fmt::arg("poster", thumbnail_url),
      fmt::arg("source", std::move(stream).str()));
  co_yield std::move(content);
}

}  // namespace

Task<http::Response<>> DashHandler::operator()(
    http::Request<> request, stdx::stop_token stop_token) const {
  auto uri = http::ParseUri(request.url);
  re::smatch results;
  if (!re::regex_match(uri.path.value(), results,
                       re::regex(R"(\/dash\/[^\/]+\/[^\/]+\/(.*)$)"))) {
    co_return http::Response<>{.status = 400};
  }
  std::string item_id = http::DecodeUri(
      http::DecodeUri(ToStringView(results[1].begin(), results[1].end())));

  co_return http::Response<>{
      .status = 200,
      .body = GetDashPlayer(content_url_generator_(item_id),
                            thumbnail_url_generator_(item_id))};
}

}  // namespace coro::cloudstorage::util