#ifndef CORO_CLOUDSTORAGE_UTIL_ITEM_URL_PROVIDER_H
#define CORO_CLOUDSTORAGE_UTIL_ITEM_URL_PROVIDER_H

#include <functional>
#include <string>

namespace coro::cloudstorage::util {

class ItemUrlProvider {
 public:
  explicit ItemUrlProvider(
      std::function<std::string(std::string_view item_id)> impl)
      : impl_(std::move(impl)) {}

  std::string operator()(std::string_view item_id) const {
    return impl_(item_id);
  }

 private:
  std::function<std::string(std::string_view item_id)> impl_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_ITEM_URL_PROVIDER_H