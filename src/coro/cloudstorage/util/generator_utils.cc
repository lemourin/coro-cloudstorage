#include "generator_utils.h"

namespace coro::cloudstorage::util {

Generator<std::string> Take(Generator<std::string>& generator,
                            Generator<std::string>::iterator& iterator,
                            size_t at_most) {
  if (iterator == generator.end()) {
    co_return;
  }
  while (at_most > 0) {
    if ((*iterator).empty()) {
      try {
        co_await ++iterator;
      } catch (...) {
        throw;
      }
    }
    if (iterator == generator.end()) {
      break;
    }
    auto size = std::min<size_t>((*iterator).size(), at_most);
    co_yield std::string((*iterator).begin(), (*iterator).begin() + size);
    at_most -= size;
    (*iterator).erase((*iterator).begin(), (*iterator).begin() + size);
  }
}

}  // namespace coro::cloudstorage::util