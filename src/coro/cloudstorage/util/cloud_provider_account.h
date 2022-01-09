#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_ACCOUNT_H
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_ACCOUNT_H

#include <optional>
#include <string>
#include <string_view>

#include "coro/cloudstorage/util/settings_manager.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/util/type_list.h"

namespace coro::cloudstorage::util {

template <typename CloudProvider>
static std::string GetAccountId(std::string_view username) {
  return StrCat("[", CloudProvider::kId, "] ", username);
}

namespace internal {
template <typename AuthTokenManagerT, typename CloudProvider>
struct OnAuthTokenChanged {
  void operator()(typename CloudProvider::Auth::AuthToken auth_token) {
    if (*account_id) {
      d->template SaveToken<CloudProvider>(std::move(auth_token), **account_id);
    }
  }
  AuthTokenManagerT* d;
  std::shared_ptr<std::optional<std::string>> account_id;
};
}  // namespace internal

template <typename CloudProviderTypeList, typename CloudFactory,
          typename AuthTokenManagerT = SettingsManager>
class CloudProviderAccount;

template <typename... CloudProviders, typename CloudFactory,
          typename AuthTokenManagerT>
class CloudProviderAccount<coro::util::TypeList<CloudProviders...>,
                           CloudFactory, AuthTokenManagerT> {
 public:
  template <typename T>
  using CloudProviderT =
      decltype(std::declval<CloudFactory>().template Create<T>(
          std::declval<typename T::Auth::AuthToken>(),
          std::declval<internal::OnAuthTokenChanged<AuthTokenManagerT, T>>()));

  using Ts = coro::util::TypeList<CloudProviderT<CloudProviders>...>;

  template <typename CloudProvider>
  explicit CloudProviderAccount(std::string username, int64_t version,
                                CloudProvider account)
      : username_(std::move(username)),
        version_(version),
        provider_(std::move(account)) {}

  std::string GetId() const {
    return std::visit(
        [&]<typename CloudProvider>(const CloudProvider&) {
          return GetAccountId<typename CloudProvider::Type>(username_);
        },
        provider_);
  }

  std::string_view username() const { return username_; }
  auto& provider() { return provider_; }
  const auto& provider() const { return provider_; }
  stdx::stop_token stop_token() const { return stop_source_.get_token(); }

 private:
  template <typename, typename, typename, typename, typename>
  friend class AccountManagerHandler;

  std::string username_;
  int64_t version_;
  std::variant<CloudProviderT<CloudProviders>...> provider_;
  stdx::stop_source stop_source_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_ACCOUNT_H