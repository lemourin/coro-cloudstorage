#ifndef CORO_CLOUDSTORAGE_CLOUD_FACTORY_H
#define CORO_CLOUDSTORAGE_CLOUD_FACTORY_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/auth_handler.h>
#include <coro/http/http.h>
#include <coro/util/type_list.h>

namespace coro::cloudstorage {

template <typename T>
concept HasGetAuthorizationUrl = requires(typename T::Auth v) {
  { v.GetAuthorizationUrl({}) }
  ->stdx::convertible_to<std::string>;
};

template <coro::http::HttpClient Http, typename AuthData>
class CloudFactory {
 public:
  CloudFactory(event_base* event_loop, const Http& http, AuthData auth_data)
      : event_loop_(event_loop),
        http_(http),
        auth_data_(std::move(auth_data)) {}

  template <typename CloudProvider, typename... Args>
  auto Create(typename CloudProvider::Auth::AuthToken auth_token,
              Args&&... args) const {
    return CreateCloudProvider<CloudProvider>{}(*this, std::move(auth_token),
                                                std::forward<Args>(args)...);
  }

  template <typename CloudProvider, typename OnAuthTokenCreated>
  auto CreateAuthHandler(OnAuthTokenCreated on_auth_token_created) const {
    return util::MakeAuthHandler<CloudProvider>(
        event_loop_, http_, auth_data_.template operator()<CloudProvider>(),
        std::move(on_auth_token_created));
  }

  template <typename CloudProvider>
  std::optional<std::string> GetAuthorizationUrl() const {
    if constexpr (HasGetAuthorizationUrl<CloudProvider>) {
      return CloudProvider::Auth::GetAuthorizationUrl(
          auth_data_.template operator()<CloudProvider>());
    } else {
      return std::nullopt;
    }
  }

 private:
  template <typename>
  friend struct CreateCloudProvider;

  event_base* event_loop_;
  const Http& http_;
  AuthData auth_data_;
};

template <typename CloudProvider>
constexpr std::string_view GetCloudProviderId() {
  return CloudProvider::kId;
}

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_FACTORY_H
