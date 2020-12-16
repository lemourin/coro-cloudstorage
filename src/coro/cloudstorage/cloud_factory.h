#ifndef CORO_CLOUDSTORAGE_CLOUD_FACTORY_H
#define CORO_CLOUDSTORAGE_CLOUD_FACTORY_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/providers/google_drive.h>
#include <coro/cloudstorage/providers/mega.h>
#include <coro/cloudstorage/providers/one_drive.h>
#include <coro/cloudstorage/util/auth_handler.h>
#include <coro/http/http.h>
#include <coro/util/type_list.h>

namespace coro::cloudstorage {

template <typename T>
concept HasGetAuthorizationUrl = requires(typename T::Auth v) {
  { v.GetAuthorizationUrl({}) }
  ->std::convertible_to<std::string>;
};

namespace internal {
template <typename CloudProvider>
struct CreateCloudProvider {
  template <typename CloudFactory, typename OnTokenUpdated = void (*)(
                                       typename CloudProvider::Auth::AuthToken)>
  auto operator()(
      const CloudFactory& factory,
      typename CloudProvider::Auth::AuthToken auth_token,
      OnTokenUpdated on_token_updated =
          [](typename CloudProvider::Auth::AuthToken) {}) const {
    return MakeCloudProvider<CloudProvider>(
        factory.http_, std::move(auth_token),
        typename CloudFactory::template AuthData<CloudProvider>{}(),
        std::move(on_token_updated));
  }
};

template <>
struct CreateCloudProvider<Mega> {
  template <typename CloudFactory, typename... Args>
  auto operator()(const CloudFactory& factory, Mega::Auth::AuthToken auth_token,
                  Args&&...) const {
    return MakeCloudProvider<Mega>(
        Mega(factory.event_loop_, factory.http_, std::move(auth_token),
             typename CloudFactory::template AuthData<Mega>{}()));
  }
};
}  // namespace internal

template <template <typename> typename AuthDataT, coro::http::HttpClient Http>
class CloudFactory {
 public:
  template <typename T>
  using AuthData = AuthDataT<T>;

  CloudFactory(event_base* event_loop, Http& http)
      : event_loop_(event_loop), http_(http) {}

  template <typename CloudProvider, typename... Args>
  auto Create(typename CloudProvider::Auth::AuthToken auth_token,
              Args&&... args) const {
    return internal::CreateCloudProvider<CloudProvider>{}(
        *this, std::move(auth_token), std::forward<Args>(args)...);
  }

  template <typename CloudProvider, typename OnAuthTokenCreated>
  auto CreateAuthHandler(OnAuthTokenCreated on_auth_token_created) const {
    return util::MakeAuthHandler<AuthData, CloudProvider>(
        event_loop_, http_, std::move(on_auth_token_created));
  }

  template <typename CloudProvider>
  std::optional<std::string> GetAuthorizationUrl() const {
    if constexpr (HasGetAuthorizationUrl<CloudProvider>) {
      return CloudProvider::Auth::GetAuthorizationUrl(
          AuthData<CloudProvider>{}());
    } else {
      return std::nullopt;
    }
  }

 private:
  template <typename>
  friend struct internal::CreateCloudProvider;

  event_base* event_loop_;
  Http& http_;
};

template <typename CloudProvider>
constexpr std::string_view GetCloudProviderId() = delete;

template <>
constexpr std::string_view GetCloudProviderId<Mega>() {
  return "mega";
}

template <>
constexpr std::string_view GetCloudProviderId<GoogleDrive>() {
  return "google";
}

template <>
constexpr std::string_view GetCloudProviderId<OneDrive>() {
  return "onedrive";
}

using CloudProviders = ::coro::util::TypeList<GoogleDrive, Mega, OneDrive>;

template <template <typename> typename AuthData, http::HttpClient Http>
auto MakeCloudFactory(event_base* event_loop, Http& http) {
  return CloudFactory<AuthData, Http>(event_loop, http);
}

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_FACTORY_H
