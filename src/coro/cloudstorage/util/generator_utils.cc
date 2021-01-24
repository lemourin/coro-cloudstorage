#include "generator_utils.h"

namespace coro::cloudstorage::util {

Generator<std::string> Take(Generator<std::string>::iterator& iterator,
                            int chunk_size) {
  while (chunk_size > 0) {
    if ((*iterator).empty()) {
      co_await ++iterator;
    }
    int size = std::min<int>((*iterator).size(), chunk_size);
    co_yield std::string((*iterator).begin(), (*iterator).begin() + size);
    chunk_size -= size;
    (*iterator).erase((*iterator).begin(), (*iterator).begin() + size);
  }
}

}  // namespace coro::cloudstorage::util