#include "coro/cloudstorage/util/cloud_factory_config.h"

#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http_parse.h"

namespace coro::cloudstorage::util {

std::string CloudFactoryConfig::GetDefaultPostAuthRedirectUri(
    std::string_view account_type, std::string_view username) {
  return StrCat("/", account_type, "/", http::EncodeUri(username));
};

}  // namespace coro::cloudstorage::util