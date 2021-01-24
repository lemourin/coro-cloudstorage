#ifndef CORO_CLOUDSTORAGE_GENERATOR_UTILS_H
#define CORO_CLOUDSTORAGE_GENERATOR_UTILS_H

#include <coro/generator.h>

#include <string>

namespace coro::cloudstorage::util {

Generator<std::string> Take(Generator<std::string>::iterator& iterator,
                            int64_t chunk_size);

}  // namespace coro::cloudstorage::util

#endif