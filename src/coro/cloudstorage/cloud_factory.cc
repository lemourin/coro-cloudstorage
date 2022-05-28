#include "coro/cloudstorage/cloud_factory.h"

#include "coro/cloudstorage/providers/amazon_s3.h"
#include "coro/cloudstorage/providers/box.h"
#include "coro/cloudstorage/providers/dropbox.h"
#include "coro/cloudstorage/providers/google_drive.h"
#include "coro/cloudstorage/providers/hubic.h"
#include "coro/cloudstorage/providers/local_filesystem.h"
#include "coro/cloudstorage/providers/mega.h"
#include "coro/cloudstorage/providers/one_drive.h"
#include "coro/cloudstorage/providers/pcloud.h"
#include "coro/cloudstorage/providers/webdav.h"
#include "coro/cloudstorage/providers/yandex_disk.h"

namespace coro::cloudstorage {

namespace {

namespace di = boost::di;

using ::coro::cloudstorage::util::AbstractCloudFactory;
using ::coro::cloudstorage::util::AbstractCloudProvider;
using ::coro::cloudstorage::util::AuthManager;
using ::coro::cloudstorage::util::AuthManager3;

template <typename CloudProvider, typename Impl>
class AuthHandlerImpl : public AbstractCloudProvider::Auth::AuthHandler {
 public:
  using AuthToken = typename CloudProvider::Auth::AuthToken;

  AuthHandlerImpl(AbstractCloudProvider::Type type, Impl impl)
      : type_(type), impl_(std::move(impl)) {}

  Task<std::variant<http::Response<>, AbstractCloudProvider::Auth::AuthToken>>
  OnRequest(http::Request<> request, stdx::stop_token stop_token) override {
    auto result = co_await impl_(std::move(request), std::move(stop_token));
    if constexpr (std::is_same_v<decltype(result), AuthToken>) {
      co_return AbstractCloudProvider::Auth::AuthToken{
          .type = type_, .impl = std::move(result)};
    } else if (std::holds_alternative<http::Response<>>(result)) {
      co_return std::move(std::get<http::Response<>>(result));
    } else {
      co_return AbstractCloudProvider::Auth::AuthToken{
          .type = type_, .impl = std::get<AuthToken>(std::move(result))};
    }
  }

 private:
  AbstractCloudProvider::Type type_;
  Impl impl_;
};

template <typename CloudProvider, typename Impl>
auto CreateAuthHandlerImpl(AbstractCloudProvider::Type type, Impl impl) {
  return std::make_unique<AuthHandlerImpl<CloudProvider, Impl>>(
      type, std::move(impl));
}

template <typename CloudProvider, typename Impl>
class AuthImpl : public AbstractCloudProvider::Auth {
 public:
  AuthImpl(AbstractCloudProvider::Type type, const util::AuthData* auth_data,
           Impl impl)
      : type_(type), auth_data_(auth_data), impl_(std::move(impl)) {}

  std::optional<std::string> GetAuthorizationUrl() const override {
    if constexpr (HasGetAuthorizationUrl<CloudProvider>) {
      return CloudProvider::Auth::GetAuthorizationUrl(
          auth_data_->template operator()<CloudProvider>());
    } else {
      return std::nullopt;
    }
  }

  std::unique_ptr<AuthHandler> CreateAuthHandler() const override {
    return CreateAuthHandlerImpl<CloudProvider>(type_, impl_());
  }

  std::string_view GetId() const override { return CloudProvider::kId; }

  std::string_view GetIcon() const override { return CloudProvider::kIcon; }

  nlohmann::json ToJson(const AuthToken& auth_token) const override {
    return util::ToJson(
        std::any_cast<const typename CloudProvider::Auth::AuthToken&>(
            auth_token.impl));
  }

  AuthToken ToAuthToken(const nlohmann::json& json) const override {
    return AuthToken{
        .type = type_,
        .impl =
            util::ToAuthToken<typename CloudProvider::Auth::AuthToken>(json)};
  }

 private:
  AbstractCloudProvider::Type type_;
  const util::AuthData* auth_data_;
  Impl impl_;
};

template <typename CloudProvider, typename Impl>
auto CreateAuthImpl(AbstractCloudProvider::Type type,
                    const util::AuthData* auth_data, Impl impl) {
  return std::make_unique<AuthImpl<CloudProvider, Impl>>(type, auth_data,
                                                         std::move(impl));
}

template <typename CloudProvider>
class CloudFactoryImpl : public AbstractCloudFactory {
 public:
  using AuthToken = typename CloudProvider::Auth::AuthToken;
  using Auth = typename CloudProvider::Auth;

  CloudFactoryImpl(AbstractCloudProvider::Type type,
                   const coro::util::EventLoop* event_loop,
                   coro::util::ThreadPool* thread_pool,
                   const coro::http::Http* http,
                   const util::ThumbnailGenerator* thumbnail_generator,
                   const util::Muxer* muxer,
                   util::RandomNumberGenerator* random_number_generator,
                   const util::AuthData* auth_data)
      : type_(type),
        event_loop_(event_loop),
        thread_pool_(thread_pool),
        http_(http),
        thumbnail_generator_(thumbnail_generator),
        muxer_(muxer),
        random_number_generator_(random_number_generator),
        auth_data_(auth_data) {}

  std::unique_ptr<AbstractCloudProvider::CloudProvider> Create(
      AbstractCloudProvider::Auth::AuthToken auth_token,
      std::function<void(const AbstractCloudProvider::Auth::AuthToken&)>
          on_token_updated) const override {
    auto provider = Create(
        std::any_cast<typename CloudProvider::Auth::AuthToken&&>(
            std::move(auth_token.impl)),
        [type = auth_token.type,
         on_token_updated = std::move(on_token_updated)](
            const typename CloudProvider::Auth::AuthToken& auth_token) mutable {
          on_token_updated(AbstractCloudProvider::Auth::AuthToken{
              .type = type, .impl = auth_token});
        });
    return AbstractCloudProvider::Create(std::move(provider));
  }

  std::unique_ptr<AbstractCloudProvider::Auth> CreateAuth(
      AbstractCloudProvider::Type) const override {
    return CreateAuthImpl<CloudProvider>(
        type_, auth_data_, [&] { return CreateAuthHandlerImpl(); });
  }

  std::vector<AbstractCloudProvider::Type> GetSupportedCloudProviders()
      const override {
    return {type_};
  }

 private:
  template <typename OnTokenUpdated>
  auto Create(AuthToken auth_token, OnTokenUpdated on_token_updated) const {
    namespace di = boost::di;

    auto injector = [&] {
      auto base_injector =
          di::make_injector(di::bind<AuthToken>().to(auth_token), GetConfig());
      if constexpr (HasAuthData<typename CloudProvider::Auth>) {
        return di::make_injector(
            di::bind<OnAuthTokenUpdated<AuthToken>>().to([&](const auto&) {
              return OnAuthTokenUpdated<AuthToken>(on_token_updated);
            }),
            di::bind<AuthManager3<Auth>>().to([&](const auto& injector) {
              return AuthManager3<Auth>(
                  injector.template create<AuthManager<Auth>>());
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

  auto GetAuthData() const {
    return auth_data_->template operator()<CloudProvider>();
  }

  auto GetConfig() const {
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
          di::bind<typename CloudProvider::Auth::AuthData>().to(GetAuthData()));
    } else {
      return injector;
    }
  }

  auto CreateAuthHandlerImpl() const {
    auto injector = GetConfig();
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

  AbstractCloudProvider::Type type_;
  const coro::util::EventLoop* event_loop_;
  coro::util::ThreadPool* thread_pool_;
  const coro::http::Http* http_;
  const util::ThumbnailGenerator* thumbnail_generator_;
  const util::Muxer* muxer_;
  util::RandomNumberGenerator* random_number_generator_;
  const util::AuthData* auth_data_;
};

}  // namespace

std::unique_ptr<AbstractCloudFactory> CloudFactory2::CreateCloudFactory(
    AbstractCloudProvider::Type type) const {
  auto create = [&]<typename T>() {
    return std::make_unique<CloudFactoryImpl<T>>(
        type, event_loop_, thread_pool_, http_, thumbnail_generator_, muxer_,
        random_number_generator_, &auth_data_);
  };

  switch (type) {
    case AbstractCloudProvider::Type::kAmazonS3:
      return create.operator()<AmazonS3>();
    case AbstractCloudProvider::Type::kBox:
      return create.operator()<Box>();
    case AbstractCloudProvider::Type::kDropbox:
      return create.operator()<Dropbox>();
    case AbstractCloudProvider::Type::kGoogleDrive:
      return create.operator()<GoogleDrive>();
    case AbstractCloudProvider::Type::kLocalFileSystem:
      return create.operator()<LocalFileSystem>();
    case AbstractCloudProvider::Type::kHubiC:
      return create.operator()<HubiC>();
    case AbstractCloudProvider::Type::kMega:
      return create.operator()<Mega>();
    case AbstractCloudProvider::Type::kOneDrive:
      return create.operator()<OneDrive>();
    case AbstractCloudProvider::Type::kPCloud:
      return create.operator()<PCloud>();
    case AbstractCloudProvider::Type::kWebDAV:
      return create.operator()<WebDAV>();
    case AbstractCloudProvider::Type::kYandexDisk:
      return create.operator()<YandexDisk>();
  }
}

std::unique_ptr<AbstractCloudProvider::CloudProvider> CloudFactory2::Create(
    AbstractCloudProvider::Auth::AuthToken auth_token,
    std::function<void(const AbstractCloudProvider::Auth::AuthToken&)>
        on_token_updated) const {
  return CreateCloudFactory(auth_token.type)
      ->Create(std::move(auth_token), std::move(on_token_updated));
}

std::unique_ptr<AbstractCloudProvider::Auth> CloudFactory2::CreateAuth(
    AbstractCloudProvider::Type type) const {
  return CreateCloudFactory(type)->CreateAuth(type);
}

std::vector<AbstractCloudProvider::Type>
CloudFactory2::GetSupportedCloudProviders() const {
  using Type = AbstractCloudProvider::Type;

  return {Type::kAmazonS3,
          Type::kBox,
          Type::kDropbox,
          Type::kGoogleDrive,
          Type::kLocalFileSystem,
          Type::kHubiC,
          Type::kMega,
          Type::kOneDrive,
          Type::kPCloud,
          Type::kWebDAV,
          Type::kYandexDisk};
}

}  // namespace coro::cloudstorage