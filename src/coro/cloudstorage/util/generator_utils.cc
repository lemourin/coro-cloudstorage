#include "coro/cloudstorage/util/generator_utils.h"

#include <algorithm>

namespace coro::cloudstorage::util {

Generator<std::string> Take(Generator<std::string>& generator,
                            Generator<std::string>::iterator& iterator,
                            size_t at_most) {
  if (iterator == generator.end()) {
    co_return;
  }
  try {
    while (at_most > 0) {
      if ((*iterator).empty()) {
        co_await ++iterator;
      }
      if (iterator == generator.end()) {
        break;
      }
      auto size = std::min<size_t>((*iterator).size(), at_most);
      at_most -= size;
      if ((*iterator).size() == size) {
        co_yield std::move(*iterator);
        (*iterator).clear();
      } else {
        co_yield std::string(
            (*iterator).begin(),
            (*iterator).begin() +
                static_cast<std::string::difference_type>(size));
        (*iterator).erase((*iterator).begin(),
                          (*iterator).begin() +
                              static_cast<std::string::difference_type>(size));
      }
    }
  } catch (...) {
    throw;
  }
}

}  // namespace coro::cloudstorage::util
