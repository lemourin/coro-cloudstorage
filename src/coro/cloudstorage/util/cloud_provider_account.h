#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_ACCOUNT_H
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_ACCOUNT_H

#include <optional>
#include <string>
#include <string_view>

#include "coro/cloudstorage/util/settings_manager.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/util/type_list.h"

namespace coro::cloudstorage::util {

inline std::string GetAccountId(std::string_view id,
                                std::string_view username) {
  return StrCat("[", id, "] ", username);
}

class CloudProviderAccount {
 public:
  struct Id {
    std::string type;
    std::string username;

    friend bool operator==(const Id& a, const Id& b) {
      return std::tie(a.type, a.username) == std::tie(b.type, b.username);
    }
  };

  CloudProviderAccount(std::string username, int64_t version,
                       std::unique_ptr<AbstractCloudProvider> account)
      : username_(std::move(username)),
        version_(version),
        type_(account->GetId()),
        provider_(std::move(account)) {}

  std::string_view type() const { return type_; }
  Id id() const { return {type_, username_}; }
  std::string_view username() const { return username_; }
  auto& provider() { return *provider_; }
  const auto& provider() const { return *provider_; }
  stdx::stop_token stop_token() const { return stop_source_.get_token(); }

 private:
  friend class AccountManagerHandler;

  std::string username_;
  int64_t version_;
  std::string type_;
  std::unique_ptr<AbstractCloudProvider> provider_;
  stdx::stop_source stop_source_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_ACCOUNT_H