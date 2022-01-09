#include "coro/cloudstorage/util/auth_token_manager.h"

#include <utility>

namespace coro::cloudstorage::util {

void AuthTokenManager::RemoveToken(std::string_view id,
                                   std::string_view provider_id) const {
  auto token_file = path_;
  nlohmann::json json;
  {
    std::ifstream input_token_file{token_file};
    if (input_token_file) {
      input_token_file >> json;
    }
  }
  nlohmann::json result;
  for (auto token : json["auth_token"]) {
    if (token["type"] != std::string(provider_id) ||
        token["id"] != std::string(id)) {
      result["auth_token"].emplace_back(std::move(token));
    }
  }
  if (result.is_null()) {
    remove(token_file.c_str());
    RemoveDirectory(GetDirectoryPath(token_file));
  } else {
    std::ofstream stream{token_file};
    stream.exceptions(std::ofstream::failbit | std::ofstream::badbit);
    stream << result.dump(2);
  }
}

}  // namespace coro::cloudstorage::util
