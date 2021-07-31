#include "coro/cloudstorage/providers/mega.h"

#include <cryptopp/aes.h>
#include <cryptopp/cryptlib.h>
#include <cryptopp/modes.h>
#include <cryptopp/pwdbased.h>
#include <cryptopp/sha.h>

#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace coro::cloudstorage {

namespace {

enum ItemType { kFile = 0, kFolder, kRoot, kInbox, kTrash };

template <typename T1, typename T2>
T2 GetPaddingSize(T1 size, T2 padding) {
  return size % padding == 0 ? 0 : padding - (size % padding);
}

std::vector<uint8_t> PadNull(std::string_view data, int q) {
  std::vector<uint8_t> result(data.size() + GetPaddingSize(data.size(), q));
  for (size_t i = 0; i < data.size(); i++) {
    result[i] = data[i];
  }
  return result;
}

std::span<const uint8_t> ReadNumber(std::span<const uint8_t> data) {
  int length = (data[0] * 256 + data[1] + 7) >> 3;
  return data.subspan(2, length);
}

std::tuple<std::span<const uint8_t>, std::span<const uint8_t>,
           std::span<const uint8_t>>
GetRSAKey(std::span<const uint8_t> decrypted_pkey) {
  auto p = ReadNumber(decrypted_pkey);
  auto q = ReadNumber(std::span(decrypted_pkey).subspan(p.size() + 2));
  auto d = ReadNumber(
      std::span(decrypted_pkey).subspan(p.size() + 2 + q.size() + 2));
  return std::make_tuple(p, q, d);
}

std::vector<uint8_t> DecryptRSA(std::span<const uint8_t> m_bytes,
                                std::span<const uint8_t> p_bytes,
                                std::span<const uint8_t> q_bytes,
                                std::span<const uint8_t> d_bytes) {
  CryptoPP::Integer m(m_bytes.data(), m_bytes.size());
  CryptoPP::Integer p(p_bytes.data(), p_bytes.size());
  CryptoPP::Integer q(q_bytes.data(), q_bytes.size());
  CryptoPP::Integer d(d_bytes.data(), d_bytes.size());
  CryptoPP::Integer n = p * q;
  auto r = a_exp_b_mod_c(m, d, p * q);
  std::vector<uint8_t> output(r.ByteCount());
  r.Encode(output.data(), output.size());
  return output;
}

template <typename Cipher>
std::vector<uint8_t> BlockTransform(Cipher& cipher, std::string_view message) {
  if (message.size() % 16 != 0) {
    throw CloudException(
        util::StrCat("invalid message length ", message.size()));
  }
  std::vector<uint8_t> decrypted(message.size());
  for (int i = 0; i < message.size(); i += 16) {
    cipher.ProcessData(reinterpret_cast<uint8_t*>(decrypted.data() + i),
                       reinterpret_cast<const uint8_t*>(message.data() + i),
                       16);
  }
  return decrypted;
}

template <size_t Size>
auto ToA32(std::span<const uint8_t, Size> bytes) {
  if constexpr (Size == std::dynamic_extent) {
    std::vector<uint32_t> result(bytes.size() / 4);
    for (size_t i = 0; 4 * i + 3 < bytes.size(); i++) {
      result[i] = (bytes[4 * i] << 24) | (bytes[4 * i + 1] << 16) |
                  (bytes[4 * i + 2] << 8) | bytes[4 * i + 3];
    }
    return result;
  } else {
    static_assert(Size % 4 == 0);
    std::array<uint32_t, Size / 4> result;
    for (size_t i = 0; 4 * i + 3 < bytes.size(); i++) {
      result[i] = (bytes[4 * i] << 24) | (bytes[4 * i + 1] << 16) |
                  (bytes[4 * i + 2] << 8) | bytes[4 * i + 3];
    }
    return result;
  }
}

auto ToIV(std::span<const uint8_t, 32> compkey) {
  auto a32 = ToA32(compkey);
  return Mega::ToBytes(
      Mega::MakeConstSpan(std::array<uint32_t, 4>{a32[4], a32[5], 0, 0}));
}

auto ToMAC(std::span<const uint8_t, 32> compkey) {
  auto a32 = ToA32(compkey);
  return Mega::ToBytes(
      Mega::MakeConstSpan(std::array<uint32_t, 2>{a32[6], a32[7]}));
}

template <typename T>
T ToItemImpl(std::span<const uint8_t> master_key, const nlohmann::json& json) {
  CryptoPP::ECB_Mode<CryptoPP::AES>::Decryption cipher;
  cipher.SetKey(master_key.data(), master_key.size());
  T result = {};
  if constexpr (std::is_same_v<T, Mega::File> ||
                std::is_same_v<T, Mega::Directory>) {
    auto args = util::SplitString(std::string(json["k"]), ':');
    if (args.size() < 2) {
      throw CloudException("invalid item");
    }
    std::string_view item_user = args[0];
    std::string_view item_key = args[1];
    auto item_key_parts = util::SplitString(args[1], '/');
    if (item_key_parts.size() >= 2) {
      item_key = item_key_parts[0];
    }
    result.user = item_user;
    if (item_user == std::string(json["u"])) {
      result.compkey = [&] {
        std::vector<uint8_t> decoded =
            BlockTransform(cipher, Mega::FromBase64(item_key));
        constexpr size_t key_length = std::is_same_v<T, Mega::File> ? 32 : 16;
        if (decoded.size() != key_length) {
          throw CloudException(
              util::StrCat("invalid key length ", decoded.size()));
        }
        std::array<uint8_t, key_length> result;
        memcpy(result.data(), decoded.data(), key_length);
        return result;
      }();
      try {
        result.attr = Mega::DecryptAttribute(
            Mega::GetItemKey(result), Mega::FromBase64(std::string(json["a"])));
        result.name = result.attr.at("n");
      } catch (const nlohmann::json::exception&) {
        result.name = "MALFORMED ATTRIBUTES";
      } catch (const CloudException&) {
        result.name = "MALFORMED ATTRIBUTES";
      }
    }
  }
  if constexpr (std::is_same_v<T, Mega::File>) {
    result.size = json["s"];
    if (json.contains("fa")) {
      std::string fa = json["fa"];
      if (auto thumbnail = Mega::GetAttribute(fa, 0)) {
        result.thumbnail_id = Mega::DecodeHandle(*thumbnail);
      }
    }
  }
  result.timestamp = json["ts"];
  if constexpr (std::is_same_v<T, Mega::File> ||
                std::is_same_v<T, Mega::Directory>) {
    result.parent = Mega::DecodeHandle(std::string(json["p"]));
  }
  result.id = Mega::DecodeHandle(std::string(json["h"]));
  return result;
}

std::array<uint32_t, 4> XorBlocks(std::span<const uint32_t, 4> block,
                                  std::span<const uint32_t, 4> cbc_mac) {
  return {cbc_mac[0] ^ block[0], cbc_mac[1] ^ block[1], cbc_mac[2] ^ block[2],
          cbc_mac[3] ^ block[3]};
}

std::string EncodeChunk(std::span<const uint8_t, 16> key,
                        std::span<const uint8_t, 32> compkey, int64_t position,
                        std::string_view input) {
  auto civ = [&] {
    auto iv = ToA32(Mega::MakeConstSpan(ToIV(compkey)));
    iv[2] = uint32_t(uint64_t(position) / 0x1000000000);
    iv[3] = uint32_t(uint64_t(position) / 0x10);
    return Mega::ToBytes(std::span<const uint32_t, 4>(iv));
  }();
  CryptoPP::CTR_Mode<CryptoPP::AES>::Encryption cipher;
  cipher.SetKeyWithIV(key.data(), key.size(), civ.data(), civ.size());
  std::string padded_input = util::StrCat(std::string(position % 16, 0), input);
  std::string encrypted(padded_input.size(), 0);
  cipher.ProcessData(reinterpret_cast<uint8_t*>(encrypted.data()),
                     reinterpret_cast<const uint8_t*>(padded_input.data()),
                     padded_input.size());
  return encrypted.substr(position % 16);
}

}  // namespace

std::array<uint8_t, 16> Mega::Auth::GetPasswordKey(std::string_view password) {
  auto d = ToA32(std::span<const uint8_t>(PadNull(password, 4)));
  auto pkey = ToBytes(std::span<const uint32_t, 4>(
      std::array<uint32_t, 4>{0x93C467E3, 0x7DB0C7A4, 0xD1BE3F81, 0x0152CB56}));

  size_t n = (d.size() + 3) / 4;
  std::array<uint32_t, 4> key{};
  for (size_t i = 0; i < 65536; i++) {
    for (size_t j = 0; j < n; j++) {
      memcpy(key.data(), reinterpret_cast<const char*>(d.data()) + 4 * j,
             (std::min)(static_cast<size_t>(4), d.size() - 4 * j) *
                 sizeof(uint32_t));
      auto bkey = ToBytes(std::span<const uint32_t, 4>(key));
      CryptoPP::ECB_Mode<CryptoPP::AES>::Encryption cipher;
      cipher.SetKey(bkey.data(), bkey.size());
      cipher.ProcessData(pkey.data(), pkey.data(), pkey.size());
    }
  }
  return pkey;
}

std::string Mega::Auth::GetHash(std::string_view text,
                                std::span<const uint8_t, 16> key) {
  auto d = ToA32(std::span<const uint8_t>(PadNull(text, 4)));
  std::array<uint32_t, 4> h{};
  for (size_t i = 0; i < d.size(); i++) {
    h[i % 4] ^= d[i];
  }
  auto hb = ToBytes(std::span<const uint32_t, 4>(h));
  CryptoPP::ECB_Mode<CryptoPP::AES>::Encryption cipher;
  cipher.SetKey(key.data(), key.size());
  for (int i = 0; i < 16384; i++) {
    cipher.ProcessData(hb.data(), hb.data(), hb.size());
  }
  auto ha = ToA32(std::span<const uint8_t, 16>(hb));
  return http::ToBase64(ToStringView(ToBytes(
      std::span<const uint32_t, 2>(std::array<uint32_t, 2>{ha[0], ha[2]}))));
}

auto Mega::Auth::DecryptSessionId(std::span<const uint8_t, 16> passkey,
                                  std::string_view key, std::string_view privk,
                                  std::string_view csid) -> SessionData {
  CryptoPP::ECB_Mode<CryptoPP::AES>::Decryption cipher;
  cipher.SetKey(passkey.data(), passkey.size());
  if (key.size() != 16) {
    throw CloudException(util::StrCat("invalid key length ", key.size()));
  }
  std::array<uint8_t, 16> decrypted_key;
  memcpy(decrypted_key.data(), key.data(), key.size());
  cipher.ProcessData(decrypted_key.data(),
                     reinterpret_cast<const uint8_t*>(key.data()), key.size());
  cipher.SetKey(decrypted_key.data(), decrypted_key.size());
  std::vector<uint8_t> decrypted_pkey = BlockTransform(cipher, privk);
  auto m = ReadNumber(ToBytes(csid));
  auto [p, q, d] = GetRSAKey(decrypted_pkey);
  return {
      .pkey = decrypted_key,
      .session_id = ToBase64(ToStringView(
          std::span<const uint8_t>(DecryptRSA(m, p, q, d)).subspan(0, 43)))};
}

auto Mega::Auth::GetLoginWithSaltData(std::string_view password,
                                      std::string_view salt)
    -> LoginWithSaltData {
  CryptoPP::PKCS5_PBKDF2_HMAC<CryptoPP::SHA512> pbkdf;
  std::array<uint8_t, 32> output = {};
  pbkdf.DeriveKey(
      output.data(), output.size(), 0,
      reinterpret_cast<const uint8_t*>(password.data()), password.size(),
      reinterpret_cast<const uint8_t*>(salt.data()), salt.size(), 100000);
  LoginWithSaltData data;
  memcpy(data.password_key.data(), output.data(), 16);
  memcpy(data.handle.data(), output.data() + 16, 16);
  return data;
}

std::optional<std::string_view> Mega::GetAttribute(std::string_view attr,
                                                   int index) {
  auto search = util::StrCat(":", index, "*");
  if (auto it = attr.find(search); it != std::string_view::npos) {
    it += search.length();
    auto start = it;
    while (it < attr.length() && attr[it] != '/') {
      it++;
    }
    return attr.substr(start, it - start);
  }
  return std::nullopt;
}

std::string_view Mega::ToStringView(std::span<const uint8_t> d) {
  return std::string_view(reinterpret_cast<const char*>(d.data()), d.size());
}

std::span<const uint8_t> Mega::ToBytes(std::string_view d) {
  return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(d.data()),
                                  d.size());
}

nlohmann::json Mega::DecryptAttribute(std::span<const uint8_t, 16> key,
                                      std::string_view input) {
  auto decrypted = DecodeAttributeContent(key, input);
  if (!decrypted.starts_with("MEGA")) {
    throw CloudException("attribute decryption error");
  }
  return nlohmann::json::parse(decrypted.substr(4).c_str());
}

std::string Mega::EncryptAttribute(std::span<const uint8_t, 16> key,
                                   const nlohmann::json& json) {
  return EncodeAttributeContent(key, util::StrCat("MEGA", json));
}

std::string Mega::ToBase64(std::string_view input) {
  auto output = http::ToBase64(input);
  for (char& c : output) {
    if (c == '+') {
      c = '-';
    } else if (c == '/') {
      c = '_';
    }
  }
  while (!output.empty() && output.back() == '=') {
    output.pop_back();
  }
  return output;
}

std::string Mega::FromBase64(std::string_view input_sv) {
  std::string input(input_sv);
  for (char& c : input) {
    if (c == '-') {
      c = '+';
    } else if (c == '_') {
      c = '/';
    }
  }
  return http::FromBase64(input);
}

uint64_t Mega::DecodeHandle(std::string_view b64) {
  auto d = ToA32(std::span<const uint8_t>(PadNull(FromBase64(b64), 8)));
  if (d.size() != 2) {
    throw CloudException("invalid handle");
  }
  return d[0] | (static_cast<uint64_t>(d[1]) << 32);
}

std::string Mega::ToHandle(uint64_t id) {
  std::array<uint8_t, 6> output{
      static_cast<uint8_t>((id & ((1ULL << 32) - 1)) >> 24),
      static_cast<uint8_t>((id & ((1ULL << 24) - 1)) >> 16),
      static_cast<uint8_t>((id & ((1ULL << 16) - 1)) >> 8),
      static_cast<uint8_t>(id & ((1ULL << 8) - 1)),
      static_cast<uint8_t>(id >> 56),
      static_cast<uint8_t>((id & ((1ULL << 56) - 1)) >> 48)};
  return ToBase64(std::string_view(reinterpret_cast<const char*>(output.data()),
                                   output.size()));
}

std::string Mega::ToAttributeHandle(uint64_t id) {
  std::array<uint8_t, 8> output{
      static_cast<uint8_t>((id & ((1ULL << 32) - 1)) >> 24),
      static_cast<uint8_t>((id & ((1ULL << 24) - 1)) >> 16),
      static_cast<uint8_t>((id & ((1ULL << 16) - 1)) >> 8),
      static_cast<uint8_t>(id & ((1ULL << 8) - 1)),
      static_cast<uint8_t>(id >> 56),
      static_cast<uint8_t>((id & ((1ULL << 56) - 1)) >> 48),
      static_cast<uint8_t>((id & ((1ULL << 48) - 1)) >> 40),
      static_cast<uint8_t>((id & ((1ULL << 40) - 1)) >> 32)};
  return ToBase64(std::string_view(reinterpret_cast<const char*>(output.data()),
                                   output.size()));
}

std::string Mega::DecodeChunk(std::span<const uint8_t, 16> key,
                              std::span<const uint8_t, 32> compkey,
                              int64_t position, std::string_view input) {
  auto civ = [&] {
    auto iv = ToA32(MakeConstSpan(ToIV(compkey)));
    iv[2] = uint32_t(uint64_t(position) / 0x1000000000);
    iv[3] = uint32_t(uint64_t(position) / 0x10);
    return ToBytes(MakeConstSpan(iv));
  }();
  CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption cipher;
  cipher.SetKeyWithIV(key.data(), key.size(), civ.data(), civ.size());
  std::string padded_input = util::StrCat(std::string(position % 16, 0), input);
  std::string decrypted(padded_input.size(), 0);
  cipher.ProcessData(reinterpret_cast<uint8_t*>(decrypted.data()),
                     reinterpret_cast<const uint8_t*>(padded_input.data()),
                     padded_input.size());
  return decrypted.substr(position % 16);
}

std::string Mega::DecodeAttributeContent(std::span<const uint8_t, 16> key,
                                         std::string_view encoded) {
  std::array<uint8_t, 16> iv = {};
  CryptoPP::CBC_Mode<CryptoPP::AES>::Decryption cipher;
  cipher.SetKeyWithIV(key.data(), key.size(), iv.data(), iv.size());
  std::string decrypted(encoded.size(), 0);
  cipher.ProcessData(reinterpret_cast<uint8_t*>(decrypted.data()),
                     reinterpret_cast<const uint8_t*>(encoded.data()),
                     encoded.size());
  return decrypted;
}

std::array<uint8_t, 16> Mega::ToFileKey(std::span<const uint8_t, 32> compkey) {
  auto a32 = coro::cloudstorage::ToA32(compkey);
  return ToBytes(MakeConstSpan(XorBlocks(std::span(a32).subspan<0, 4>(),
                                         std::span(a32).subspan<4, 4>())));
}

std::string Mega::EncodeAttributeContent(std::span<const uint8_t> key,
                                         std::string_view content) {
  std::array<uint8_t, 16> iv = {};
  CryptoPP::CBC_Mode<CryptoPP::AES>::Encryption cipher;
  cipher.SetKeyWithIV(key.data(), key.size(), iv.data(), iv.size());
  std::string padded_input =
      util::StrCat(content, std::string(GetPaddingSize(content.size(), 16), 0));
  std::string encrypted(padded_input.size(), 0);
  cipher.ProcessData(reinterpret_cast<uint8_t*>(encrypted.data()),
                     reinterpret_cast<const uint8_t*>(padded_input.data()),
                     padded_input.size());
  return encrypted;
}

std::string Mega::BlockEncrypt(std::span<const uint8_t> key,
                               std::string_view message) {
  CryptoPP::ECB_Mode<CryptoPP::AES>::Encryption cipher;
  cipher.SetKey(key.data(), key.size());
  return std::string(ToStringView(BlockTransform(cipher, message)));
}

Generator<std::string> Mega::GetEncodedStream(
    std::span<const uint8_t, 16> key, std::span<const uint8_t, 32> compkey,
    Generator<std::string> decoded, std::array<uint32_t, 4>& cbc_mac_a32) {
  std::array<uint8_t, 16> iv = {};
  CryptoPP::CBC_Mode<CryptoPP::AES>::Encryption cipher;
  cipher.SetKeyWithIV(key.data(), key.size(), iv.data(), iv.size());
  int64_t position = 0;
  std::string carry_over;
  auto cbc_mac = ToBytes(MakeConstSpan(cbc_mac_a32));
  FOR_CO_AWAIT(std::string & chunk, decoded) {
    co_yield EncodeChunk(key, compkey, position, chunk);
    position += static_cast<int64_t>(chunk.size());
    chunk = util::StrCat(carry_over, chunk);
    for (int i = 0; i + 16 < chunk.size(); i += 16) {
      cipher.ProcessData(cbc_mac.data(),
                         reinterpret_cast<const uint8_t*>(chunk.data() + i),
                         16);
    }
    carry_over = chunk.substr(chunk.size() - (chunk.size() % 16));
  }
  if (!carry_over.empty()) {
    carry_over.resize(16);
    cipher.ProcessData(cbc_mac.data(),
                       reinterpret_cast<const uint8_t*>(carry_over.data()), 16);
  }
  cipher.ProcessData(cbc_mac.data(), cbc_mac.data(), 16);
}

auto Mega::ToItem(const nlohmann::json& json,
                  std::span<const uint8_t> master_key) -> Item {
  switch (static_cast<int>(json["t"])) {
    case kFile:
      return ToItemImpl<File>(master_key, json);
    case kFolder:
      return ToItemImpl<Directory>(master_key, json);
    case kInbox:
      return ToItemImpl<Inbox>(master_key, json);
    case kRoot:
      return ToItemImpl<Root>(master_key, json);
    case kTrash:
      return ToItemImpl<Trash>(master_key, json);
    default:
      throw CloudException("unknown file type");
  }
}

}  // namespace coro::cloudstorage
