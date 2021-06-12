#include "crypto_utils.h"

#include <cryptopp/cryptlib.h>
#include <cryptopp/hex.h>
#include <cryptopp/hmac.h>
#include <cryptopp/sha.h>

namespace coro::cloudstorage::util {

std::string GetSHA256(std::string_view message) {
  ::CryptoPP::SHA256 hash;
  std::string result;
  ::CryptoPP::StringSource(
      std::string(message), true,
      new ::CryptoPP::HashFilter(hash, new ::CryptoPP::StringSink(result)));
  return result;
}

std::string ToHex(std::string_view message) {
  std::string result;
  ::CryptoPP::StringSource(
      std::string(message), true,
      new ::CryptoPP::HexEncoder(new ::CryptoPP::StringSink(result), false));
  return result;
}

std::string GetHMACSHA256(std::string_view key, std::string_view message) {
  std::string mac;
  ::CryptoPP::HMAC<::CryptoPP::SHA256> hmac(
      reinterpret_cast<const uint8_t*>(key.data()), key.length());
  ::CryptoPP::StringSource(
      std::string(message), true,
      new ::CryptoPP::HashFilter(hmac, new ::CryptoPP::StringSink(mac)));
  std::string result;
  ::CryptoPP::StringSource(mac, true, new ::CryptoPP::StringSink(result));
  return result;
}

std::string GetHMACSHA1(std::string_view key, std::string_view message) {
  std::string mac;
  ::CryptoPP::HMAC<::CryptoPP::SHA1> hmac(
      reinterpret_cast<const uint8_t*>(key.data()), key.length());
  ::CryptoPP::StringSource(
      std::string(message), true,
      new ::CryptoPP::HashFilter(hmac, new ::CryptoPP::StringSink(mac)));
  std::string result;
  ::CryptoPP::StringSource(mac, true, new ::CryptoPP::StringSink(result));
  return result;
}

}  // namespace coro::cloudstorage::util