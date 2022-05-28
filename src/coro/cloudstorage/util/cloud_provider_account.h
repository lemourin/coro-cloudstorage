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
std::string GetAccountId(std::string_view username) {
  return StrCat("[", CloudProvider::kId, "] ", username);
}

namespace internal {

template <typename CloudProvider>
struct OnAuthTokenChanged {
  void operator()(typename CloudProvider::Auth::AuthToken auth_token) {
    if (*account_id) {
      d->template SaveToken<CloudProvider>(std::move(auth_token), **account_id);
    }
  }
  SettingsManager* d;
  std::shared_ptr<std::optional<std::string>> account_id;
};

}  // namespace internal

class CloudProviderAccount {
 public:
  template <typename CloudProvider>
  explicit CloudProviderAccount(std::string username, int64_t version,
                                CloudProvider account)
      : username_(std::move(username)),
        version_(version),
        type_(CloudProvider::Type::kId),
        id_(GetAccountId<typename CloudProvider::Type>(username_)),
        provider_(AbstractCloudProvider::Create(std::move(account))) {}

  CloudProviderAccount(const CloudProviderAccount&) = delete;
  CloudProviderAccount(CloudProviderAccount&&) = delete;
  CloudProviderAccount& operator=(const CloudProviderAccount&) = delete;
  CloudProviderAccount& operator=(CloudProviderAccount&&) = delete;

  std::string_view type() const { return type_; }
  std::string_view id() const { return id_; }
  std::string_view username() const { return username_; }
  auto& provider() { return *provider_; }
  const auto& provider() const { return *provider_; }
  stdx::stop_token stop_token() const { return stop_source_.get_token(); }

 private:
  friend class AccountManagerHandler;

  std::string username_;
  int64_t version_;
  std::string type_;
  std::string id_;
  std::unique_ptr<AbstractCloudProvider::CloudProvider> provider_;
  stdx::stop_source stop_source_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_ACCOUNT_H