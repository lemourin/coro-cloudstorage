#ifndef CORO_CLOUDSTORAGE_GENERATOR_UTILS_H
#define CORO_CLOUDSTORAGE_GENERATOR_UTILS_H

#include <string>

#include "coro/generator.h"

namespace coro::cloudstorage::util {

Generator<std::string> Take(Generator<std::string>& generator,
                            Generator<std::string>::iterator& iterator,
                            size_t at_most);

}  // namespace coro::cloudstorage::util

#endif