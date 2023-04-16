#ifndef CORO_CLOUDSTORAGE_GENERATOR_UTILS_H
#define CORO_CLOUDSTORAGE_GENERATOR_UTILS_H

#include <string>

#include "coro/generator.h"

namespace coro::cloudstorage::util {

inline Generator<std::string> ToGenerator(std::string chunk) {
  co_yield std::move(chunk);
}

template <typename... Args>
Generator<std::string> Forward(Generator<std::string> body, Args...) {
  FOR_CO_AWAIT(std::string & chunk, body) { co_yield std::move(chunk); }
}

Generator<std::string> Take(Generator<std::string>& generator,
                            Generator<std::string>::iterator& iterator,
                            size_t at_most);

}  // namespace coro::cloudstorage::util

#endif