#ifndef CORO_CLOUDSTORAGE_FUSE_AUTH_DATA_H
#define CORO_CLOUDSTORAGE_FUSE_AUTH_DATA_H

#include <coro/stdx/concepts.h>

#include <string_view>

namespace coro::cloudstorage::util {

template <typename CloudProvider>
typename CloudProvider::Auth::AuthData GetAuthData() = delete;

template <typename T>
concept HasRedirectUri = requires(T v) {
  { v.redirect_uri }
  ->stdx::convertible_to<std::string>;
};

struct AuthData {
  static constexpr std::string_view kHostname = "http://localhost:12345";

  template <typename CloudProvider>
  auto operator()() const {
    auto auth_data = GetAuthData<CloudProvider>();
    if constexpr (HasRedirectUri<decltype(auth_data)>) {
      auth_data.redirect_uri =
          std::string(kHostname) + "/auth/" + std::string(CloudProvider::kId);
    }
    return auth_data;
  }
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_AUTH_DATA_H
