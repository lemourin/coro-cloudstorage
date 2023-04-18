#include "coro/cloudstorage/util/handler_utils.h"

#include <string>

#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http.h"

namespace coro::cloudstorage::util {

namespace internal {

Generator<std::string> GetFileContentResponseBody(
    Generator<std::string> content, Generator<std::string>::iterator it) {
  while (it != content.end()) {
    co_yield std::move(*it);
    co_await ++it;
  }
}

}  // namespace internal

std::vector<std::string> GetEffectivePath(std::string_view uri_path) {
  std::vector<std::string> components;
  for (std::string_view component : SplitString(std::string(uri_path), '/')) {
    if (component.empty() || component == ".") {
      continue;
    } else if (component == "..") {
      if (components.empty()) {
        throw CloudException("invalid path");
      } else {
        components.pop_back();
      }
    } else {
      components.emplace_back(component);
    }
  }
  for (auto& d : components) {
    d = http::DecodeUri(d);
  }
  if (components.size() < 3) {
    throw CloudException("invalid path");
  }
  components.erase(components.begin(), components.begin() + 3);
  return components;
}

}  // namespace coro::cloudstorage::util
