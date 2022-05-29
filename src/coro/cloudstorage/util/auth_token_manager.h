#ifndef CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H
#define CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "coro/cloudstorage/cloud_factory.h"
#include "coro/cloudstorage/util/serialize_utils.h"
#include "coro/cloudstorage/util/settings_utils.h"
#include "coro/util/type_list.h"

namespace coro::cloudstorage::util {

struct AuthToken : AbstractCloudProvider::Auth::AuthToken {
  std::string id;
};

class AuthTokenManager {
 public:
  AuthTokenManager(AbstractCloudFactory* factory,
                   std::string path = GetConfigFilePath())
      : factory_(factory), path_(std::move(path)) {}

  std::vector<AuthToken> LoadTokenData() const;

  void SaveToken(AbstractCloudProvider::Auth::AuthToken token,
                 std::string_view id) const;

  void SaveToken(nlohmann::json token, std::string_view id,
                 std::string_view provider_id) const;

  void RemoveToken(std::string_view id, std::string_view provider_id) const;

 private:
  AbstractCloudFactory* factory_;
  std::string path_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_AUTH_TOKEN_MANAGER_H
