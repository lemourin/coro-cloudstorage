#ifndef CORO_CLOUDSTORAGE_CLOUD_FACTORY_H
#define CORO_CLOUDSTORAGE_CLOUD_FACTORY_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/providers/mega.h>
#include <coro/http/http.h>

namespace coro::cloudstorage {

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

 private:
  template <typename>
  friend struct internal::CreateCloudProvider;

  event_base* event_loop_;
  Http& http_;
};

template <template <typename> typename AuthData, http::HttpClient Http>
auto MakeCloudFactory(event_base* event_loop, Http& http) {
  return CloudFactory<AuthData, Http>(event_loop, http);
}

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_FACTORY_H
