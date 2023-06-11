#ifndef CORO_CLOUDSTORAGE_CRYPTO_UTILS_H
#define CORO_CLOUDSTORAGE_CRYPTO_UTILS_H

#include <string>
#include <string_view>

namespace coro::cloudstorage::util {

std::string GetSHA256(std::string_view message);
std::string ToHex(std::string_view message);
std::string GetHMACSHA256(std::string_view key, std::string_view message);

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_CRYPTO_UTILS_H