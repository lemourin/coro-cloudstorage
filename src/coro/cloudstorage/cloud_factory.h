#ifndef CORO_CLOUDSTORAGE_CLOUD_FACTORY_H
#define CORO_CLOUDSTORAGE_CLOUD_FACTORY_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/auth_data.h>
#include <coro/cloudstorage/util/auth_handler.h>
#include <coro/cloudstorage/util/thumbnail_generator.h>
#include <coro/http/http.h>
#include <coro/util/event_loop.h>
#include <coro/util/type_list.h>

#include <boost/di.hpp>
#include <iostream>

namespace coro::cloudstorage {

template <typename T>
concept HasGetAuthorizationUrl = requires(typename T::Auth v) {
  { v.GetAuthorizationUrl({}) } -> stdx::convertible_to<std::string>;
};

template <typename T>
concept HasAuthHandlerT = requires {
  typename T::Auth::template AuthHandler<>;
};

template <typename T>
concept HasAuthHandler = requires {
  typename T::Auth::AuthHandler;
};

template <typename T>
concept HasCloudProviderT = requires {
  typename T::template CloudProvider<>;
};

template <typename EventLoopT, coro::http::HttpClient HttpT,
          typename ThumbnailGeneratorT, typename MuxerT,
          typename AuthDataT = coro::cloudstorage::util::AuthData>
class CloudFactory {
 public:
  using EventLoop = EventLoopT;
  using Http = HttpT;
  using AuthData = AuthDataT;
  using ThumbnailGenerator = ThumbnailGeneratorT;
  using Muxer = MuxerT;

  template <typename CloudProvider, typename OnTokenUpdated>
  using AuthManagerT =
      util::AuthManager<Http, typename CloudProvider::Auth, OnTokenUpdated>;

  CloudFactory(const EventLoop& event_loop, const Http& http,
               const ThumbnailGenerator& thumbnail_generator,
               const Muxer& muxer, AuthData auth_data = AuthData{})
      : event_loop_(&event_loop),
        http_(&http),
        thumbnail_generator_(&thumbnail_generator),
        muxer_(&muxer),
        auth_data_(std::move(auth_data)) {}

  template <typename CloudProvider, typename OnTokenUpdated>
  auto Create(typename CloudProvider::Auth::AuthToken auth_token,
              OnTokenUpdated on_token_updated) const {
    namespace di = boost::di;

    auto injector = di::make_injector(
        di::bind<class OnAuthTokenUpdatedT>().template to<OnTokenUpdated>(
            on_token_updated),
        di::bind<typename CloudProvider::Auth::AuthToken>().to(auth_token),
        di::bind<class coro::cloudstorage::AuthManagerT>()
            .template to<AuthManagerT<CloudProvider, OnTokenUpdated>>(),
        GetConfig<CloudProvider>());

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

    return di::make_injector(
        di::bind<class coro::cloudstorage::CloudProviderT>()
            .template to<CloudProvider>(),
        di::bind<typename CloudProvider::Auth::AuthData>().to(
            GetAuthData<CloudProvider>()),
        di::bind<http::FetchF>().to(http::FetchF(http_)),
        di::bind<class coro::cloudstorage::HttpT>().template to<HttpT>(http_),
        di::bind<coro::util::WaitF>().to(coro::util::WaitF(event_loop_)),
        di::bind<class coro::cloudstorage::EventLoopT>()
            .template to<EventLoopT>(event_loop_),
        di::bind<util::ThumbnailGeneratorF>().to(
            util::ThumbnailGeneratorF(thumbnail_generator_)),
        di::bind<class coro::cloudstorage::ThumbnailGeneratorT>()
            .template to<ThumbnailGeneratorT>(thumbnail_generator_),
        di::bind<class coro::cloudstorage::MuxerT>().template to<MuxerT>(
            muxer_));
  }

  const EventLoop* event_loop_;
  const Http* http_;
  const ThumbnailGenerator* thumbnail_generator_;
  const Muxer* muxer_;
  AuthData auth_data_;
};

template <typename CloudProvider>
constexpr std::string_view GetCloudProviderId() {
  return CloudProvider::kId;
}

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_FACTORY_H
