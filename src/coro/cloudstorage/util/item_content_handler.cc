#include "coro/cloudstorage/util/item_content_handler.h"

#include "coro/cloudstorage/util/cloud_provider_utils.h"
#include "coro/cloudstorage/util/handler_utils.h"
#include "coro/util/regex.h"

namespace coro::cloudstorage::util {

namespace re = coro::util::re;

Task<http::Response<>> ItemContentHandler::operator()(
    http::Request<> request, stdx::stop_token stop_token) const {
  auto uri = http::ParseUri(request.url);
  re::smatch results;
  if (!re::regex_match(uri.path.value(), results,
                       re::regex(R"(\/content\/[^\/]+\/[^\/]+\/(.*)$)"))) {
    co_return http::Response<>{.status = 400};
  }
  std::string item_id = http::DecodeUri(
      http::DecodeUri(ToStringView(results[1].begin(), results[1].end())));
  auto item = co_await GetItemById(provider_, cache_manager_, clock_->Now(),
                                   item_id, stop_token);
  auto* file = std::get_if<AbstractCloudProvider::File>(&item.item);
  if (!file) {
    co_return http::Response<>{.status = 400};
  }
  co_return co_await GetFileContentResponse(
      provider_, std::move(*file),
      [&]() -> std::optional<http::Range> {
        if (auto header = http::GetHeader(request.headers, "Range")) {
          return http::ParseRange(std::move(*header));
        } else {
          return std::nullopt;
        }
      }(),
      std::move(stop_token));
}

}  // namespace coro::cloudstorage::util