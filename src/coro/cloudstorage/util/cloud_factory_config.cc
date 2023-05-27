#include "coro/cloudstorage/util/cloud_factory_config.h"

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http_parse.h"

namespace coro::cloudstorage::util {

std::string CloudFactoryConfig::GetDefaultPostAuthRedirectUri(
    std::string_view account_type, std::string_view username) {
  return StrCat("/list/", account_type, '/', http::EncodeUri(username), '/');
};

AuthData CloudFactoryConfig::GetDefaultAuthData() {
  return {"http://localhost:12345", nlohmann::json::parse(kAuthDataJson)};
}

}  // namespace coro::cloudstorage::util
