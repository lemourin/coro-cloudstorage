#include "coro/cloudstorage/util/handler_utils.h"

#include <string>

#include "coro/cloudstorage/cloud_exception.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http.h"

namespace coro::cloudstorage::util {

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
  if (components.empty()) {
    throw CloudException("invalid path");
  }
  components.erase(components.begin());
  return components;
}

}  // namespace coro::cloudstorage::util
