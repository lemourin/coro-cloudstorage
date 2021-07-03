#ifndef CORO_CLOUDSTORAGE_CLOUD_FACTORY_H
#define CORO_CLOUDSTORAGE_CLOUD_FACTORY_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/auth_data.h>
#include <coro/cloudstorage/util/auth_handler.h>
#include <coro/http/http.h>
#include <coro/util/type_list.h>

namespace coro::cloudstorage {

template <typename T>
concept HasGetAuthorizationUrl = requires(typename T::Auth v) {
  { v.GetAuthorizationUrl({}) } -> stdx::convertible_to<std::string>;
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
    return CreateCloudProvider<CloudProvider>{}(*this, std::move(auth_token),
                                                std::move(on_token_updated));
  }

  template <typename CloudProvider>
  auto CreateAuthHandler() const {
    return ::coro::cloudstorage::util::CreateAuthHandler<CloudProvider>{}(
        *this, auth_data_.template operator()<CloudProvider>());
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

  template <typename CloudProvider, typename OnTokenUpdated>
  using AuthManagerT =
      util::AuthManager<Http, typename CloudProvider::Auth, OnTokenUpdated>;

  template <typename CloudProvider, typename OnTokenUpdated,
            typename Auth = typename CloudProvider::Auth>
  auto CreateAuthManager(typename Auth::AuthToken auth_token,
                         OnTokenUpdated on_token_updated) const {
    return AuthManagerT<CloudProvider, OnTokenUpdated>(
        *http_, std::move(auth_token), std::move(on_token_updated),
        util::RefreshToken<HttpT, Auth>{
            .http = http_, .auth_data = GetAuthData<CloudProvider>()},
        util::AuthorizeRequest{});
  }

  template <typename CloudProvider>
  auto GetAuthData() const {
    return auth_data_.template operator()<CloudProvider>();
  }

 private:
  template <typename>
  friend struct CreateCloudProvider;

  template <typename>
  friend struct ::coro::cloudstorage::util::CreateAuthHandler;

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
