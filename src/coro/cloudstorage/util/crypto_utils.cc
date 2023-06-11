#include "coro/cloudstorage/util/crypto_utils.h"

#include <cryptopp/hex.h>
#include <cryptopp/hmac.h>
#include <cryptopp/sha.h>

namespace coro::cloudstorage::util {

std::string GetSHA256(std::string_view message) {
  ::CryptoPP::SHA256 hash;
  std::string result(hash.DigestSize(), 0);
  hash.CalculateDigest(reinterpret_cast<uint8_t*>(result.data()),
                       reinterpret_cast<const uint8_t*>(message.data()),
                       message.size());
  return result;
}

std::string ToHex(std::string_view message) {
  ::CryptoPP::HexEncoder hex_encoder(/*attachment=*/nullptr,
                                     /*upperCase=*/false);
  hex_encoder.Put(reinterpret_cast<const uint8_t*>(message.data()),
                  message.size());
  std::string result(2 * message.size(), 0);
  hex_encoder.Get(reinterpret_cast<uint8_t*>(result.data()), result.size());
  return result;
}

std::string GetHMACSHA256(std::string_view key, std::string_view message) {
  ::CryptoPP::HMAC<::CryptoPP::SHA256> hmac(
      reinterpret_cast<const uint8_t*>(key.data()), key.length());
  std::string result(hmac.DigestSize(), 0);
  hmac.CalculateDigest(reinterpret_cast<uint8_t*>(result.data()),
                       reinterpret_cast<const uint8_t*>(message.data()),
                       message.size());
  return result;
}

}  // namespace coro::cloudstorage::util
