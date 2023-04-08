#ifndef CORO_CLOUDSTORAGE_FUSE_AUTH_DATA_H
#define CORO_CLOUDSTORAGE_FUSE_AUTH_DATA_H

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "coro/cloudstorage/util/string_utils.h"
#include "coro/stdx/concepts.h"

namespace coro::cloudstorage::util {

template <typename CloudProvider>
typename CloudProvider::Auth::AuthData GetAuthData(const nlohmann::json&) =
    delete;

template <typename T>
concept HasRedirectUri = requires(T v) {
  { v.redirect_uri } -> stdx::convertible_to<std::string>;
};

class AuthData {
 public:
  AuthData(std::string redirect_uri, nlohmann::json auth_data)
      : redirect_uri_(std::move(redirect_uri)),
        auth_data_(std::move(auth_data)) {}

  template <typename CloudProvider>
  auto operator()() const {
    nlohmann::json json = auth_data_.at(CloudProvider::kId);
    auto auth_data = GetAuthData<CloudProvider>(json);
    if constexpr (HasRedirectUri<decltype(auth_data)>) {
      if (auto it = json.find("redirect_uri");
          it != json.end() && it->is_string()) {
        auth_data.redirect_uri = *it;
      } else {
        auth_data.redirect_uri =
            StrCat(redirect_uri_, "/auth/", CloudProvider::kId);
      }
    }
    return auth_data;
  }

  std::string_view redirect_uri() const { return redirect_uri_; }

 private:
  std::string redirect_uri_;
  nlohmann::json auth_data_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_AUTH_DATA_H
