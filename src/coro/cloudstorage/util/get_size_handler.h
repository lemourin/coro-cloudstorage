#ifndef CORO_CLOUDSTORAGE_UTIL_GET_SIZE_HANDLER_H
#define CORO_CLOUDSTORAGE_UTIL_GET_SIZE_HANDLER_H

#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/stdx/stop_token.h>

#include <list>

namespace coro::cloudstorage::util {

template <typename CloudProviderAccount>
struct GetSizeHandler {
  using Request = http::Request<>;
  using Response = http::Response<>;

  struct VolumeData {
    std::optional<int64_t> space_used;
    std::optional<int64_t> space_total;
  };

  template <typename CloudProvider>
  static Task<VolumeData> GetVolumeData(CloudProvider* provider,
                                        stdx::stop_token stop_token) {
    using CloudProviderT = typename CloudProvider::Type;
    if constexpr (HasUsageData<typename CloudProviderT::GeneralData>) {
      try {
        auto data = co_await provider->GetGeneralData(stop_token);
        co_return VolumeData{.space_used = data.space_used,
                             .space_total = data.space_total};
      } catch (...) {
        co_return VolumeData{};
      }
    } else {
      co_return VolumeData{};
    }
  }

  Task<Response> operator()(Request request,
                            stdx::stop_token stop_token) const {
    auto query = http::ParseQuery(http::ParseUri(request.url).query.value());
    auto account_id = query.find("account_id");
    if (account_id == query.end()) {
      co_return Response{.status = 400};
    }
    for (CloudProviderAccount& account : *accounts) {
      if (account.GetId() == account_id->second) {
        VolumeData volume_data = co_await std::visit(
            [&](auto& provider) {
              return GetVolumeData(&provider, stop_token);
            },
            account.provider());
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
  std::list<CloudProviderAccount>* accounts;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_GET_SIZE_HANDLER_H