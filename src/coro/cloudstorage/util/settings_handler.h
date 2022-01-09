#ifndef CORO_CLOUDSTORAGE_SETTINGS_HANDLER_H
#define CORO_CLOUDSTORAGE_SETTINGS_HANDLER_H

#include <string>

#include "coro/http/http.h"
#include "coro/stdx/stop_token.h"

namespace coro::cloudstorage::util {

namespace internal {

struct SettingsHandlerData {
  std::string_view path;
  std::span<const std::pair<std::string, std::string>> headers;
  std::optional<Generator<std::string>> request_body;
  bool public_network;
  bool effective_public_network;
  stdx::stop_token stop_token;
};

Task<http::Response<>> GetSettingsHandlerResponse(SettingsHandlerData);

}  // namespace internal

template <typename SettingsManagerT>
class SettingsHandler {
 public:
  using Request = http::Request<>;
  using Response = http::Response<>;

  explicit SettingsHandler(SettingsManagerT* settings_manager)
      : settings_manager_(settings_manager) {}

  Task<Response> operator()(Request request,
                            stdx::stop_token stop_token) const {
    auto uri = http::ParseUri(request.url);
    if (!uri.path) {
      co_return Response{.status = 400};
    }
    if (request.method == http::Method::kPost) {
      if (*uri.path == "/settings/public-network") {
        auto body = co_await http::GetBody(std::move(request.body).value());
        auto query = http::ParseQuery(body);
        if (auto it = query.find("value"); it != query.end()) {
          settings_manager_->SetEnablePublicNetwork(it->second == "true");
          co_return Response{.status = 200};
        } else {
          co_return Response{.status = 400};
        }
      }
    }
    co_return co_await internal::GetSettingsHandlerResponse(
        internal::SettingsHandlerData{
            .path = *uri.path,
            .headers = request.headers,
            .request_body = std::move(request.body),
            .public_network = settings_manager_->IsPublicNetworkEnabled(),
            .effective_public_network =
                settings_manager_->EffectiveIsPublicNetworkEnabled(),
            .stop_token = std::move(stop_token)});
  }

 private:
  SettingsManagerT* settings_manager_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_SETTINGS_HANDLER_H
