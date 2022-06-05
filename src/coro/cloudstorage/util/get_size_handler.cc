#include "coro/cloudstorage/util/get_size_handler.h"

#include "coro/util/stop_token_or.h"

namespace coro::cloudstorage::util {

auto GetSizeHandler::operator()(Request request,
                                stdx::stop_token stop_token) const
    -> Task<Response> {
  auto query = http::ParseQuery(http::ParseUri(request.url).query.value());
  auto account_id = query.find("account_id");
  if (account_id == query.end()) {
    co_return Response{.status = 400};
  }
  for (CloudProviderAccount& account : *accounts) {
    if (account.id() == account_id->second) {
      coro::util::StopTokenOr stop_token_or(std::move(stop_token),
                                            account.stop_token());
      auto volume_data =
          co_await account.provider().GetGeneralData(stop_token_or.GetToken());
      nlohmann::json json;
      if (volume_data.space_total) {
        json["space_total"] = *volume_data.space_total;
      }
      if (volume_data.space_used) {
        json["space_used"] = *volume_data.space_used;
      }
      co_return Response{.status = 200,
                         .headers = {{"Content-Type", "application/json"}},
                         .body = http::CreateBody(json.dump())};
    }
  }
  co_return Response{.status = 404};
}

}  // namespace coro::cloudstorage::util