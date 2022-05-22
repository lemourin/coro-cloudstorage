#ifndef CORO_CLOUDSTORAGE_CLOUD_FACTORY_H
#define CORO_CLOUDSTORAGE_CLOUD_FACTORY_H

#include <boost/di.hpp>
#include <random>

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/auth_handler.h"
#include "coro/cloudstorage/util/muxer.h"
#include "coro/cloudstorage/util/random_number_generator.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"
#include "coro/http/http.h"
#include "coro/util/event_loop.h"
#include "coro/util/type_list.h"

namespace coro::cloudstorage {

template <typename T>
concept HasGetAuthorizationUrl = requires(typename T::Auth v) {
                                   {
                                     v.GetAuthorizationUrl({})
                                     } -> stdx::convertible_to<std::string>;
                                 };

template <typename T>
concept HasAuthHandlerT =
    requires { typename T::Auth::template AuthHandler<>; };

template <typename T>
concept HasAuthHandler = requires { typename T::Auth::AuthHandler; };

template <typename T>
concept HasCloudProviderT = requires { typename T::template CloudProvider<>; };

class CloudFactory {
 public:
  template <typename CloudProvider, typename OnTokenUpdated>
  using AuthManagerT =
      util::AuthManager<typename CloudProvider::Auth, OnTokenUpdated>;

  CloudFactory(const coro::util::EventLoop* event_loop,
               coro::util::ThreadPool* thread_pool,
               const coro::http::Http* http,
               const util::ThumbnailGenerator* thumbnail_generator,
               const util::Muxer* muxer,
               util::RandomNumberGenerator* random_number_generator,
               util::AuthData auth_data = util::AuthData{})
      : event_loop_(event_loop),
        thread_pool_(thread_pool),
        http_(http),
        thumbnail_generator_(thumbnail_generator),
        muxer_(muxer),
        random_number_generator_(random_number_generator),
        auth_data_(auth_data) {}

  template <typename CloudProvider, typename OnTokenUpdated>
  auto Create(typename CloudProvider::Auth::AuthToken auth_token,
              OnTokenUpdated on_token_updated) const {
    namespace di = boost::di;

    auto injector = [&] {
      auto base_injector = di::make_injector(
          di::bind<typename CloudProvider::Auth::AuthToken>().to(auth_token),
          GetConfig<CloudProvider>());
      if constexpr (HasAuthData<typename CloudProvider::Auth>) {
        return di::make_injector(
            di::bind<
                OnAuthTokenUpdated<typename CloudProvider::Auth::AuthToken>>()
                .to([&](const auto&) {
                  return OnAuthTokenUpdated<
                      typename CloudProvider::Auth::AuthToken>(
                      on_token_updated);
                }),
            di::bind<coro::cloudstorage::util::AuthManager3<
                typename CloudProvider::Auth>>()
                .to([](const auto& injector) {
                  return coro::cloudstorage::util::AuthManager3<
                      typename CloudProvider::Auth>(
                      injector.template create<
                          AuthManagerT<CloudProvider, OnTokenUpdated>>());
                }),
            std::move(base_injector));
      } else {
        return base_injector;
      }
    }();

    if constexpr (HasCloudProviderT<CloudProvider>) {
      return injector.template create<CloudProvider::template CloudProvider>();
    } else {
      return injector.template create<typename CloudProvider::CloudProvider>();
    }
  }

  template <typename CloudProvider>
  auto CreateAuthHandler() const {
    auto injector = GetConfig<CloudProvider>();
    if constexpr (HasAuthHandler<CloudProvider>) {
      return injector
          .template create<typename CloudProvider::Auth::AuthHandler>();
    } else if constexpr (HasAuthHandlerT<CloudProvider>) {
      return injector
          .template create<CloudProvider::Auth::template AuthHandler>();
    } else {
      return injector.template create<util::AuthHandler>();
    }
  }

  template <typename CloudProvider>
  std::optional<std::string> GetAuthorizationUrl() const {
    if constexpr (HasGetAuthorizationUrl<CloudProvider>) {
      return CloudProvider::Auth::GetAuthorizationUrl(
          GetAuthData<CloudProvider>());
    } else {
      return std::nullopt;
    }
  }

  template <typename CloudProvider>
  auto GetAuthData() const {
    return auth_data_.template operator()<CloudProvider>();
  }

 private:
  template <typename CloudProvider>
  auto GetConfig() const {
    namespace di = boost::di;

    auto injector = di::make_injector(
        di::bind<class coro::cloudstorage::CloudProviderT>.template to<CloudProvider>(),
        di::bind<coro::http::Http>.to(http_),
        di::bind<coro::util::EventLoop>().to(event_loop_),
        di::bind<coro::util::ThreadPool>().to(thread_pool_),
        di::bind<coro::cloudstorage::util::RandomNumberGenerator>().to(
            random_number_generator_));

    if constexpr (HasAuthData<typename CloudProvider::Auth>) {
      return di::make_injector(
          std::move(injector),
          di::bind<typename CloudProvider::Auth::AuthData>().to(
              GetAuthData<CloudProvider>()));
    } else {
      return injector;
    }
  }

  const coro::util::EventLoop* event_loop_;
  coro::util::ThreadPool* thread_pool_;
  const coro::http::Http* http_;
  const util::ThumbnailGenerator* thumbnail_generator_;
  const util::Muxer* muxer_;
  util::RandomNumberGenerator* random_number_generator_;
  util::AuthData auth_data_;
};

template <typename CloudProvider>
constexpr std::string_view GetCloudProviderId() {
  return CloudProvider::kId;
}

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_FACTORY_H
