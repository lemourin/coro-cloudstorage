#ifndef CORO_CLOUDSTORAGE_GENERATOR_UTILS_H
#define CORO_CLOUDSTORAGE_GENERATOR_UTILS_H

#include <coro/generator.h>

#include <string>

namespace coro::cloudstorage::util {

Generator<std::string> Take(Generator<std::string>::iterator& iterator,
                            size_t chunk_size);

Generator<std::string> Take(Generator<std::string>& generator,
                            Generator<std::string>::iterator& iterator,
                            size_t at_most);

}  // namespace coro::cloudstorage::util

#endif