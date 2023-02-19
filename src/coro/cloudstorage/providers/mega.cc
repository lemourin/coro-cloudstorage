#include "coro/cloudstorage/providers/mega.h"

#include <cryptopp/aes.h>
#include <cryptopp/cryptlib.h>
#include <cryptopp/modes.h>
#include <cryptopp/pwdbased.h>
#include <cryptopp/sha.h>

#include <iostream>
#include <span>
#include <string>
#include <tuple>
#include <vector>

#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"
#include "coro/cloudstorage/util/cloud_provider_utils.h"
#include "coro/cloudstorage/util/string_utils.h"

namespace coro::cloudstorage {

namespace {

constexpr std::string_view kApiEndpoint = "https://g.api.mega.co.nz";
constexpr int kRetryCount = 7;

using ::coro::cloudstorage::util::CreateAbstractCloudProviderImpl;
using ::coro::cloudstorage::util::FileType;
using ::coro::cloudstorage::util::GetFileType;
using ::coro::cloudstorage::util::ThumbnailOptions;

enum ItemType { kFile = 0, kFolder, kRoot, kInbox, kTrash };

struct SessionData {
  std::array<uint8_t, 16> pkey;
  std::string session_id;
};

struct LoginWithSaltData {
  std::array<uint8_t, 16> handle;
  std::array<uint8_t, 16> password_key;
};

LoginWithSaltData GetLoginWithSaltData(std::string_view password,
                                       std::string_view salt) {
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

std::string ToBase64(std::string_view input) {
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

std::string FromBase64(std::string_view input_sv) {
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

std::string_view ToStringView(std::span<const uint8_t> d) {
  return std::string_view(reinterpret_cast<const char*>(d.data()), d.size());
}

std::span<const uint8_t> ToBytes(std::string_view d) {
  return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(d.data()),
                                  d.size());
}

template <size_t Size>
auto ToBytes(std::span<const uint32_t, Size> span) {
  if constexpr (Size == std::dynamic_extent) {
    std::vector<uint8_t> result(span.size() * 4, 0);
    for (size_t i = 0; i < span.size(); i++) {
      result[4 * i] = span[i] >> 24;
      result[4 * i + 1] = (span[i] & ((1u << 24) - 1)) >> 16;
      result[4 * i + 2] = (span[i] & ((1u << 16) - 1)) >> 8;
      result[4 * i + 3] = (span[i] & ((1u << 8) - 1));
    }
    return result;
  } else {
    std::array<uint8_t, Size * 4> result;
    for (size_t i = 0; i < span.size(); i++) {
      result[4 * i] = span[i] >> 24;
      result[4 * i + 1] = (span[i] & ((1u << 24) - 1)) >> 16;
      result[4 * i + 2] = (span[i] & ((1u << 16) - 1)) >> 8;
      result[4 * i + 3] = (span[i] & ((1u << 8) - 1));
    }
    return result;
  }
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

std::array<uint32_t, 4> XorBlocks(std::span<const uint32_t, 4> block,
                                  std::span<const uint32_t, 4> cbc_mac) {
  return {cbc_mac[0] ^ block[0], cbc_mac[1] ^ block[1], cbc_mac[2] ^ block[2],
          cbc_mac[3] ^ block[3]};
}

template <typename T, std::size_t Size>
std::span<const T, Size> MakeConstSpan(const std::array<T, Size>& array) {
  return std::span<const T, Size>(array);
}

std::array<uint8_t, 16> ToFileKey(std::span<const uint8_t, 32> compkey) {
  auto a32 = ToA32(compkey);
  return ToBytes(MakeConstSpan(XorBlocks(std::span(a32).subspan<0, 4>(),
                                         std::span(a32).subspan<4, 4>())));
}

template <typename Item>
decltype(auto) GetItemKey(const Item& item) {
  if constexpr (std::is_same_v<Item, Mega::File>) {
    return ToFileKey(item.compkey);
  } else {
    return static_cast<const decltype(item.compkey)&>(item.compkey);
  }
}

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

auto ToIV(std::span<const uint8_t, 32> compkey) {
  auto a32 = ToA32(compkey);
  return ToBytes(MakeConstSpan(std::array<uint32_t, 4>{a32[4], a32[5], 0, 0}));
}

auto ToMAC(std::span<const uint8_t, 32> compkey) {
  auto a32 = ToA32(compkey);
  return ToBytes(MakeConstSpan(std::array<uint32_t, 2>{a32[6], a32[7]}));
}

std::optional<std::string_view> GetAttribute(std::string_view attr, int index) {
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

uint64_t DecodeHandle(std::string_view b64) {
  auto d = ToA32(std::span<const uint8_t>(PadNull(FromBase64(b64), 8)));
  if (d.size() != 2) {
    throw CloudException("invalid handle");
  }
  return d[0] | (static_cast<uint64_t>(d[1]) << 32);
}

std::string DecodeAttributeContent(std::span<const uint8_t, 16> key,
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

nlohmann::json DecryptAttribute(std::span<const uint8_t, 16> key,
                                std::string_view input) {
  auto decrypted = DecodeAttributeContent(key, input);
  if (!decrypted.starts_with("MEGA")) {
    throw CloudException("attribute decryption error");
  }
  return nlohmann::json::parse(decrypted.substr(4).c_str());
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
            BlockTransform(cipher, FromBase64(item_key));
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
        result.attr = DecryptAttribute(GetItemKey(result),
                                       FromBase64(std::string(json["a"])));
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
      if (auto thumbnail = GetAttribute(fa, 0)) {
        result.thumbnail_id = DecodeHandle(*thumbnail);
      }
    }
  }
  result.timestamp = json["ts"];
  if constexpr (std::is_same_v<T, Mega::File> ||
                std::is_same_v<T, Mega::Directory>) {
    result.parent = DecodeHandle(std::string(json["p"]));
  }
  result.id = DecodeHandle(std::string(json["h"]));
  return result;
}

std::string EncodeChunk(std::span<const uint8_t, 16> key,
                        std::span<const uint8_t, 32> compkey, int64_t position,
                        std::string_view input) {
  auto civ = [&] {
    auto iv = ToA32(MakeConstSpan(ToIV(compkey)));
    iv[2] = uint32_t(uint64_t(position) / 0x1000000000);
    iv[3] = uint32_t(uint64_t(position) / 0x10);
    return ToBytes(std::span<const uint32_t, 4>(iv));
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

std::array<uint8_t, 16> GetPasswordKey(std::string_view password) {
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

std::string GetHash(std::string_view text, std::span<const uint8_t, 16> key) {
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

auto DecryptSessionId(std::span<const uint8_t, 16> passkey,
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

std::string EncodeAttributeContent(std::span<const uint8_t> key,
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

std::string EncryptAttribute(std::span<const uint8_t, 16> key,
                             const nlohmann::json& json) {
  return EncodeAttributeContent(key, util::StrCat("MEGA", json));
}

std::string ToHandle(uint64_t id) {
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

std::string ToAttributeHandle(uint64_t id) {
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

std::string DecodeChunk(std::span<const uint8_t, 16> key,
                        std::span<const uint8_t, 32> compkey, int64_t position,
                        std::string_view input) {
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

std::string BlockEncrypt(std::span<const uint8_t> key,
                         std::string_view message) {
  CryptoPP::ECB_Mode<CryptoPP::AES>::Encryption cipher;
  cipher.SetKey(key.data(), key.size());
  return std::string(ToStringView(BlockTransform(cipher, message)));
}

Generator<std::string> GetEncodedStream(std::span<const uint8_t, 16> key,
                                        std::span<const uint8_t, 32> compkey,
                                        Generator<std::string> decoded,
                                        std::array<uint32_t, 4>& cbc_mac_a32) {
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

auto ToItem(const nlohmann::json& json, std::span<const uint8_t> master_key)
    -> Mega::Item {
  switch (static_cast<int>(json["t"])) {
    case kFile:
      return ToItemImpl<Mega::File>(master_key, json);
    case kFolder:
      return ToItemImpl<Mega::Directory>(master_key, json);
    case kInbox:
      return ToItemImpl<Mega::Inbox>(master_key, json);
    case kRoot:
      return ToItemImpl<Mega::Root>(master_key, json);
    case kTrash:
      return ToItemImpl<Mega::Trash>(master_key, json);
    default:
      throw CloudException("unknown file type");
  }
}

CloudException ToException(
    int status,
    stdx::source_location location = stdx::source_location::current(),
    stdx::stacktrace stacktrace = stdx::stacktrace::current()) {
  if (status == -3) {
    return CloudException(CloudException::Type::kRetry, std::move(location),
                          std::move(stacktrace));
  } else {
    return CloudException(util::StrCat("mega error ", status),
                          std::move(location), std::move(stacktrace));
  }
}

}  // namespace

auto Mega::GetRoot(stdx::stop_token stop_token) -> Task<Root> {
  co_await LazyInit(std::move(stop_token));
  for (const auto& [key, value] : items_) {
    if (std::holds_alternative<Root>(value)) {
      co_return std::get<Root>(value);
    }
  }
  throw CloudException(CloudException::Type::kNotFound);
}

auto Mega::GetGeneralData(stdx::stop_token stop_token) -> Task<GeneralData> {
  nlohmann::json command;
  command["a"] = "uq";
  command["xfer"] = 1;
  command["strg"] = 1;
  auto response = co_await DoCommand(std::move(command), std::move(stop_token));
  co_return GeneralData{.username = auth_token_.email,
                        .space_used = response["cstrg"],
                        .space_total = response["mstrg"]};
}

template <typename DirectoryT, typename>
auto Mega::ListDirectoryPage(DirectoryT directory, std::optional<std::string>,
                             coro::stdx::stop_token stop_token)
    -> Task<PageData> {
  co_await LazyInit(std::move(stop_token));
  if (!items_.contains(directory.id)) {
    throw CloudException(CloudException::Type::kNotFound);
  }
  auto it = file_tree_.find(directory.id);
  if (it == file_tree_.end()) {
    co_return PageData{};
  }
  PageData page_data;
  for (uint64_t id : it->second) {
    page_data.items.emplace_back(items_[id]);
  }
  co_return page_data;
}

Generator<std::string> Mega::GetFileContent(File file, http::Range range,
                                            coro::stdx::stop_token stop_token) {
  if (range.start >= file.size || (range.end && *range.end >= file.size)) {
    throw http::HttpException(http::HttpException::kRangeNotSatisfiable);
  }
  int64_t position = range.start;
  int64_t size = range.end.value_or(file.size - 1) - range.start + 1;
  co_await LazyInit(stop_token);
  auto json = co_await NewDownload(file.id, stop_token);
  std::array<uint8_t, 16> key = ToFileKey(file.compkey);
  DecryptAttribute(key, FromBase64(std::string(json["at"])));
  std::string url = json["g"];
  auto chunk_url = util::StrCat(url, "/", position, "-", position + size - 1);
  auto chunk_response = co_await http_->Fetch(chunk_url, stop_token);
  if (chunk_response.status / 100 != 2) {
    throw http::HttpException(chunk_response.status);
  }
  FOR_CO_AWAIT(std::string_view chunk, chunk_response.body) {
    co_yield DecodeChunk(key, file.compkey, position, chunk);
    position += static_cast<int64_t>(chunk.size());
  }
}

template <typename ItemT, typename>
Task<ItemT> Mega::RenameItem(ItemT item, std::string new_name,
                             stdx::stop_token stop_token) {
  nlohmann::json command;
  command["a"] = "a";
  item.name = new_name;
  item.attr["n"] = new_name;
  command["attr"] = ToBase64(EncryptAttribute(GetItemKey(item), item.attr));
  command["n"] = ToHandle(item.id);
  command["key"] = GetEncryptedItemKey(item.compkey);
  co_await DoCommand(std::move(command), std::move(stop_token));
  co_return std::get<ItemT>(items_[item.id] = std::move(item));
}

template <typename ItemT, typename>
Task<> Mega::RemoveItem(ItemT item, stdx::stop_token stop_token) {
  nlohmann::json command;
  command["a"] = "d";
  command["n"] = ToHandle(item.id);
  co_await DoCommand(std::move(command), std::move(stop_token));
  HandleRemoveItemEvent(item.id);
}

template <typename ItemT, typename DirectoryT, typename>
Task<ItemT> Mega::MoveItem(ItemT source, DirectoryT destination,
                           stdx::stop_token stop_token) {
  nlohmann::json command;
  command["a"] = "m";
  command["n"] = ToHandle(source.id);
  command["t"] = ToHandle(destination.id);
  co_await DoCommand(std::move(command), std::move(stop_token));
  HandleRemoveItemEvent(source.id);
  source.parent = destination.id;
  AddItem(source);
  co_return source;
}

template <typename DirectoryT, typename>
auto Mega::CreateDirectory(DirectoryT parent, std::string name,
                           stdx::stop_token stop_token) -> Task<Directory> {
  Directory directory{};
  directory.compkey = GenerateKey<uint8_t, 16>();
  directory.parent = parent.id;
  nlohmann::json command;
  command["a"] = "p";
  command["t"] = ToHandle(parent.id);
  nlohmann::json entry;
  entry["h"] = "xxxxxxxx";
  entry["t"] = 1;
  entry["k"] = GetEncryptedItemKey(directory.compkey);
  nlohmann::json attr;
  attr["n"] = std::move(name);
  entry["a"] = ToBase64(EncryptAttribute(directory.compkey, attr));
  command["n"].emplace_back(std::move(entry));

  auto response = co_await DoCommand(std::move(command), std::move(stop_token));
  auto item = ToItem(response["f"].at(0), auth_token_.pkey);
  AddItem(item);
  co_return std::get<Directory>(item);
}

auto Mega::GetItemThumbnail(File item, http::Range range,
                            stdx::stop_token stop_token) -> Task<Thumbnail> {
  if (item.thumbnail_id) {
    co_return co_await GetItemThumbnailImpl(std::move(item), range,
                                            std::move(stop_token));
  }
  auto new_item = co_await TrySetThumbnail(std::move(item), stop_token);
  co_return co_await GetItemThumbnailImpl(std::move(new_item), range,
                                          std::move(stop_token));
}

template <typename DirectoryT, typename>
auto Mega::CreateFile(DirectoryT parent, std::string_view name,
                      FileContent content, stdx::stop_token stop_token)
    -> Task<File> {
  nlohmann::json upload_response =
      co_await CreateUpload(content.size, stop_token);
  std::string upload_url = upload_response["p"];
  std::array<uint32_t, 8> compkey = GenerateKey<uint32_t, 8>();
  std::span<const uint32_t, 4> key(
      std::span<const uint32_t>(compkey).subspan<0, 4>());
  std::array<uint32_t, 4> cbc_mac{};
  std::array<uint8_t, 32> compkey_bytes = ToBytes(MakeConstSpan(compkey));
  http::Request<> request{
      .url = util::StrCat(upload_url, "/0"),
      .method = http::Method::kPost,
      .headers = {{"Content-Length", std::to_string(content.size)}},
      .body = GetEncodedStream(ToBytes(key), compkey_bytes,
                               std::move(content.data), cbc_mac)};
  auto response = co_await http_->Fetch(std::move(request), stop_token);
  if (response.status / 100 != 2) {
    throw http::HttpException(response.status);
  }

  std::array<uint32_t, 2> meta_mac{cbc_mac[0] ^ cbc_mac[1],
                                   cbc_mac[2] ^ cbc_mac[3]};
  std::array<uint32_t, 16> item_key = {compkey[0] ^ compkey[4],
                                       compkey[1] ^ compkey[5],
                                       compkey[2] ^ meta_mac[0],
                                       compkey[3] ^ meta_mac[1],
                                       compkey[4],
                                       compkey[5],
                                       meta_mac[0],
                                       meta_mac[1]};

  auto item_key_bytes = ToBytes(MakeConstSpan(item_key));
  std::string encoded_key = util::StrCat(
      EncodeAttributeContent(
          auth_token_.pkey,
          ToStringView(MakeConstSpan(item_key_bytes).subspan<0, 16>())),
      EncodeAttributeContent(
          auth_token_.pkey,
          ToStringView(MakeConstSpan(item_key_bytes).subspan<16, 16>())));

  std::string completion_handle =
      co_await http::GetBody(std::move(response.body));
  nlohmann::json commit_command;
  commit_command["a"] = "p";
  commit_command["t"] = ToHandle(parent.id);
  nlohmann::json entry;
  entry["h"] = std::move(completion_handle);
  entry["t"] = 0;
  nlohmann::json attr;
  attr["n"] = name;
  entry["a"] = ToBase64(EncryptAttribute(ToBytes(key), attr));
  entry["k"] = ToBase64(encoded_key);
  commit_command["n"].emplace_back(std::move(entry));

  std::optional<File> previous_file = FindByName(parent.id, name);
  nlohmann::json commit_command_response =
      co_await DoCommand(std::move(commit_command), stop_token);
  auto new_item = ToItem(commit_command_response["f"][0], auth_token_.pkey);
  AddItem(new_item);
  if (previous_file) {
    co_await RemoveItem(std::move(*previous_file), stop_token);
  }
  co_return co_await TrySetThumbnail(std::get<File>(new_item),
                                     std::move(stop_token));
}

auto Mega::TrySetThumbnail(File file, stdx::stop_token stop_token)
    -> Task<File> {
  auto impl = CreateAbstractCloudProviderImpl(this);
  switch (GetFileType(impl.Convert(file).mime_type)) {
    case FileType::kImage:
    case FileType::kVideo: {
      try {
        auto thumbnail = co_await thumbnail_generator_(
            &impl, impl.Convert(file),
            ThumbnailOptions{.size = 120,
                             .codec = ThumbnailOptions::Codec::JPEG},
            stop_token);
        co_return co_await SetThumbnail(std::move(file), std::move(thumbnail),
                                        std::move(stop_token));
      } catch (const std::exception& e) {
        std::cerr << "FAILED TO SET THUMBNAIL: " << e.what() << '\n';
      }
      break;
    }
    default:
      break;
  }
  co_return file;
}

auto Mega::SetThumbnail(File file, std::string thumbnail,
                        stdx::stop_token stop_token) -> Task<File> {
  std::string encoded =
      EncodeAttributeContent(ToFileKey(file.compkey), thumbnail);
  nlohmann::json command;
  command["a"] = "ufa";
  command["s"] = encoded.size();
  command["h"] = ToHandle(file.id);
  nlohmann::json url_response =
      co_await DoCommand(std::move(command), stop_token);
  auto response = co_await http_->Fetch(
      http::Request<std::string>{.url = url_response["p"],
                                 .method = http::Method::kPost,
                                 .body = std::move(encoded)},
      stop_token);
  if (response.status / 100 != 2) {
    throw http::HttpException(response.status);
  }
  std::string thumbnail_id_bytes =
      co_await http::GetBody(std::move(response.body));
  uint64_t thumbnail_id = DecodeHandle(ToBase64(thumbnail_id_bytes));
  nlohmann::json update_attributes;
  update_attributes["a"] = "pfa";
  update_attributes["n"] = ToHandle(file.id);
  update_attributes["fa"] = util::StrCat("0*", ToAttributeHandle(thumbnail_id));
  nlohmann::json attribute =
      co_await DoCommand(update_attributes, std::move(stop_token));
  if (auto* new_item =
          HandleAttributeUpdateEvent(std::string(attribute), file.id)) {
    co_return std::get<File>(*new_item);
  } else {
    throw CloudException(CloudException::Type::kNotFound);
  }
}

auto Mega::GetSession(Auth::UserCredential credential,
                      stdx::stop_token stop_token) -> Task<Auth::AuthToken> {
  auto prelogin_data = co_await Prelogin(credential.email, stop_token);
  nlohmann::json command;
  command["a"] = "us";
  command["user"] = http::ToLowerCase(std::string(credential.email));
  if (credential.twofactor) {
    command["mfa"] = std::move(*credential.twofactor);
  }
  std::array<uint8_t, 16> password_key;
  if (prelogin_data.version == 1) {
    password_key = GetPasswordKey(credential.password);
    command["uh"] = GetHash(credential.email, password_key);
  } else if (prelogin_data.version == 2 && prelogin_data.salt) {
    auto data = GetLoginWithSaltData(credential.password, *prelogin_data.salt);
    password_key = std::move(data.password_key);
    command["uh"] = ToBase64(ToStringView(std::move(data.handle)));
    command["sek"] = ToBase64(ToStringView(GenerateKey<uint8_t, 16>()));
  } else {
    throw CloudException("not supported account version");
  }
  auto response = co_await DoCommand(std::move(command), stop_token);
  auto session_data =
      DecryptSessionId(password_key, FromBase64(std::string(response["k"])),
                       FromBase64(std::string(response["privk"])),
                       FromBase64(std::string(response["csid"])));
  co_return Auth::AuthToken{
      credential.email, std::move(session_data.session_id), session_data.pkey};
}

template <typename T, size_t Size>
std::array<T, Size> Mega::GenerateKey() const {
  std::array<T, Size> key{};
  for (T& c : key) {
    c = random_number_generator_->template Get<T>();
  }
  return key;
}

auto Mega::FindByName(uint64_t parent, std::string_view name) const
    -> std::optional<File> {
  auto nodes = file_tree_.find(parent);
  if (nodes == file_tree_.end()) {
    return std::nullopt;
  }
  for (uint64_t handle : nodes->second) {
    auto it = items_.find(handle);
    if (it != items_.end()) {
      const File* file = std::get_if<File>(&it->second);
      if (file && file->name == name) {
        return *file;
      }
    }
  }
  return std::nullopt;
}

std::string Mega::GetEncryptedItemKey(std::span<const uint8_t> key) const {
  return ToBase64(BlockEncrypt(auth_token_.pkey, ToStringView(key)));
}

auto Mega::GetItemThumbnailImpl(File item, http::Range range,
                                stdx::stop_token stop_token)
    -> Task<Thumbnail> {
  if (!item.thumbnail_id) {
    throw CloudException(CloudException::Type::kNotFound);
  }
  auto response = co_await GetAttribute(*item.thumbnail_id, stop_token);
  std::string input = FromBase64(ToAttributeHandle(*item.thumbnail_id));
  if (input.size() % 8 != 0) {
    input.resize(input.size() + 8 - input.size() % 8);
  }
  http::Request<> request = {
      .url = response["p"],
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/octet-stream"},
                  {"Content-Length", "8"}},
      .body = http::CreateBody(std::move(input))};
  auto thumbnail_response =
      co_await http_->Fetch(std::move(request), stop_token);
  auto content = co_await http::GetBody(std::move(thumbnail_response.body));
  content = content.substr(12);
  Thumbnail thumbnail{.size = static_cast<int64_t>(content.size())};
  auto decoded = DecodeAttributeContent(ToFileKey(item.compkey), content);
  int64_t end = range.end.value_or(decoded.size() - 1);
  if (end >= static_cast<int64_t>(decoded.size())) {
    throw http::HttpException(http::HttpException::kRangeNotSatisfiable);
  }
  std::string output(end - range.start + 1, 0);
  memcpy(output.data(), decoded.data() + range.start, end - range.start + 1);
  thumbnail.data = http::CreateBody(std::move(output));
  co_return thumbnail;
}

Task<> Mega::LazyInit(stdx::stop_token stop_token) {
  if (!init_) {
    init_.emplace(DoInit{this});
    co_await init_->Get(std::move(stop_token));
    co_return;
  }
  std::exception_ptr exception;
  try {
    co_await init_->Get(std::move(stop_token));
    co_return;
  } catch (const CloudException&) {
  } catch (const http::HttpException&) {
  }
  init_.emplace(DoInit{this});
  co_await init_->Get(std::move(stop_token));
}

auto Mega::Prelogin(std::string_view email, stdx::stop_token stop_token)
    -> Task<PreloginData> {
  nlohmann::json command;
  command["a"] = "us0";
  command["user"] = http::ToLowerCase(std::string(email));
  auto response = co_await DoCommand(std::move(command), std::move(stop_token));
  PreloginData data{.version = response.at("v")};
  if (response.contains("s")) {
    data.salt = FromBase64(std::string(response["s"]));
  }
  co_return data;
}

Task<nlohmann::json> Mega::DoCommand(nlohmann::json command,
                                     stdx::stop_token stop_token) {
  nlohmann::json body;
  body.emplace_back(std::move(command));
  nlohmann::json response = co_await FetchJsonWithBackoff(
      http::Request<std::string>{.url = util::StrCat(kApiEndpoint, "/cs"),
                                 .method = http::Method ::kPost,
                                 .body = body.dump()},
      kRetryCount, std::move(stop_token));
  co_return response.at(0);
}

template <typename Request>
Task<nlohmann::json> Mega::FetchJson(Request request,
                                     stdx::stop_token stop_token) {
  std::vector<std::pair<std::string, std::string>> params = {
      {"id", std::to_string(id_++)}};
  if (!auth_token_.session.empty()) {
    params.emplace_back("sid", auth_token_.session);
  }
  http::Uri uri = coro::http::ParseUri(request.url);
  uri.query = util::StrCat(uri.query ? util::StrCat(*uri.query, "&") : "",
                           http::FormDataToString(params));
  request.url =
      util::StrCat(*uri.scheme, "://", *uri.host, *uri.path, "?", *uri.query);
  nlohmann::json response = co_await util::FetchJson(*http_, std::move(request),
                                                     std::move(stop_token));
  if (response.is_number() && response != 0) {
    throw ToException(response);
  }
  if (response.is_array()) {
    for (const nlohmann::json& entry : response) {
      if (entry.is_number() && entry != 0) {
        throw ToException(entry);
      }
    }
  }
  co_return response;
}

template <typename TaskF>
auto Mega::DoWithBackoff(const TaskF& task, int retry_count,
                         stdx::stop_token stop_token)
    -> Task<typename decltype(task())::type> {
  int backoff_ms = 0;
  while (true) {
    try {
      if (backoff_ms > 0) {
        co_await event_loop_->Wait(backoff_ms, stop_token);
      }
      co_return co_await task();
    } catch (const CloudException& e) {
      if (e.type() == CloudException::Type::kRetry) {
        backoff_ms = std::max<int>(backoff_ms * 2, 100);
        retry_count--;
        if (retry_count == 0) {
          throw;
        }
      } else {
        throw;
      }
    } catch (const http::HttpException&) {
      backoff_ms = std::max<int>(backoff_ms * 2, 100);
      retry_count--;
      if (retry_count == 0) {
        throw;
      }
    }
  }
}

Task<nlohmann::json> Mega::FetchJsonWithBackoff(
    http::Request<std::string> request, int retry_count,
    stdx::stop_token stop_token) {
  co_return co_await DoWithBackoff(
      [&]() -> Task<nlohmann::json> {
        co_return co_await FetchJson(request, stop_token);
      },
      retry_count, stop_token);
}

Task<nlohmann::json> Mega::GetFileSystem(stdx::stop_token stop_token) {
  nlohmann::json command;
  command["a"] = "f";
  command["c"] = 1;
  co_return co_await DoCommand(std::move(command), std::move(stop_token));
}

Task<nlohmann::json> Mega::NewDownload(uint64_t id,
                                       stdx::stop_token stop_token) {
  nlohmann::json command;
  command["a"] = "g";
  command["g"] = 1;
  command["n"] = ToHandle(id);
  co_return co_await DoCommand(std::move(command), std::move(stop_token));
}

Task<nlohmann::json> Mega::GetAttribute(uint64_t id,
                                        stdx::stop_token stop_token) {
  nlohmann::json command;
  command["a"] = "ufa";
  command["r"] = 1;
  command["fah"] = ToAttributeHandle(id);
  co_return co_await DoCommand(std::move(command), std::move(stop_token));
}

Task<nlohmann::json> Mega::CreateUpload(int64_t size,
                                        stdx::stop_token stop_token) {
  nlohmann::json command;
  command["a"] = "u";
  command["s"] = size;
  co_return co_await DoCommand(std::move(command), std::move(stop_token));
}

void Mega::AddItem(Item e) {
  std::visit(
      [&]<typename T>(T&& item) {
        auto id = item.id;
        if constexpr (std::is_same_v<T, File> || std::is_same_v<T, Directory>) {
          auto& tree = file_tree_[item.parent];
          if (auto it = std::find(tree.begin(), tree.end(), item.id);
              it == tree.end()) {
            tree.emplace_back(item.id);
          }
        }
        items_.emplace(id, std::forward<T>(item));
      },
      std::move(e));
}

Task<> Mega::PollEvents(std::string ssn, stdx::stop_token stop_token) noexcept {
  int backoff_ms = 0;
  while (!stop_token.stop_requested()) {
    try {
      if (backoff_ms > 0) {
        co_await event_loop_->Wait(backoff_ms, stop_token);
      }
      nlohmann::json json = co_await FetchJsonWithBackoff(
          http::Request<std::string>{
              .url = util::StrCat(kApiEndpoint, "/sc", "?",
                                  http::FormDataToString({{"sn", ssn}})),
              .method = http::Method::kPost},
          kRetryCount, stop_token);
      if (json.contains("w")) {
        co_await http_->Fetch(std::string(json["w"]), stop_token);
        continue;
      }
      for (const nlohmann::json& event : json["a"]) {
        std::string type = event["a"];
        if (type == "t") {
          HandleAddItemEvent(event);
        } else if (type == "u") {
          HandleUpdateItemEvent(event);
        } else if (type == "d") {
          HandleRemoveItemEvent(DecodeHandle(std::string(event["n"])));
        } else if (type == "fa") {
          HandleAttributeUpdateEvent(std::string(event["fa"]),
                                     DecodeHandle(std::string(event["n"])));
        }
      }
      ssn = json["sn"];
      backoff_ms = 0;
    } catch (const CloudException&) {
      backoff_ms = std::max<int>(backoff_ms * 2, 100);
    } catch (const http::HttpException&) {
      backoff_ms = std::max<int>(backoff_ms * 2, 100);
    }
  }
}

auto Mega::HandleAttributeUpdateEvent(std::string_view attr, uint64_t handle)
    -> const Item* {
  if (auto it = items_.find(handle); it != items_.end()) {
    if (auto* file = std::get_if<File>(&it->second)) {
      if (auto thumbnail_attr = ::coro::cloudstorage::GetAttribute(attr, 0)) {
        file->thumbnail_id = DecodeHandle(*thumbnail_attr);
        return &it->second;
      }
    }
  }
  return nullptr;
}

void Mega::HandleAddItemEvent(const nlohmann::json& json) {
  for (const nlohmann::json& item : json["t"]["f"]) {
    AddItem(ToItem(item, auth_token_.pkey));
  }
}

void Mega::HandleUpdateItemEvent(const nlohmann::json& json) {
  uint64_t handle = DecodeHandle(std::string(json["n"]));
  if (auto it = items_.find(handle); it != items_.end()) {
    std::visit(
        [&]<typename T>(T& item) {
          if constexpr (std::is_same_v<T, File> ||
                        std::is_same_v<T, Directory>) {
            try {
              item.name = DecryptAttribute(GetItemKey(item),
                                           FromBase64(std::string(json["at"])))
                              .at("n");
            } catch (const nlohmann::json::exception&) {
              item.name = "MALFORMED ATTRIBUTES";
            } catch (const CloudException&) {
              item.name = "MALFORMED ATTRIBUTES";
            }
            item.timestamp = json["ts"];
          }
        },
        it->second);
  }
}

void Mega::HandleRemoveItemEvent(uint64_t handle) {
  if (auto it = items_.find(handle); it != items_.end()) {
    std::visit(
        [&]<typename T>(const T& d) {
          if constexpr (std::is_same_v<T, File> ||
                        std::is_same_v<T, Directory>) {
            auto& children = file_tree_[d.parent];
            if (auto it = std::find(children.begin(), children.end(), handle);
                it != children.end()) {
              children.erase(it);
            }
          }
        },
        it->second);
    items_.erase(it);
    file_tree_.erase(handle);
  }
}

Task<> Mega::DoInit::operator()() const {
  auto stop_token = p->stop_source_.get_token();
  auto json = co_await p->GetFileSystem(stop_token);
  if (stop_token.stop_requested()) {
    throw InterruptedException();
  }
  for (const auto& entry : json["ok"]) {
    p->skmap_[entry["h"]] = entry["k"];
  }
  for (const auto& entry : json["f"]) {
    p->AddItem(ToItem(entry, p->auth_token_.pkey));
  }
  RunTask(p->PollEvents(json["sn"], std::move(stop_token)));
}

auto Mega::Auth::AuthHandler::operator()(http::Request<> request,
                                         stdx::stop_token stop_token)
    -> Task<std::variant<http::Response<>, Auth::AuthToken>> {
  if (request.method == http::Method::kPost) {
    auto query =
        http::ParseQuery(co_await http::GetBody(std::move(*request.body)));
    auto it1 = query.find("email");
    auto it2 = query.find("password");
    if (it1 != std::end(query) && it2 != std::end(query)) {
      auto it3 = query.find("twofactor");
      Auth::UserCredential credential = {
          .email = it1->second,
          .password = it2->second,
          .twofactor = it3 != std::end(query) ? std::make_optional(it3->second)
                                              : std::nullopt};
      co_return co_await provider_.GetSession(std::move(credential),
                                              stop_token);
    } else {
      throw http::HttpException(http::HttpException::kBadRequest);
    }
  } else {
    co_return http::Response<>{
        .status = 200,
        .body = http::CreateBody(std::string(util::kAssetsHtmlMegaLoginHtml))};
  }
}

namespace util {

template <>
nlohmann::json ToJson<Mega::Auth::AuthToken>(Mega::Auth::AuthToken token) {
  nlohmann::json json;
  json["email"] = std::move(token.email);
  json["session"] = std::move(token.session);
  json["pkey"] = token.pkey;
  return json;
}

template <>
Mega::Auth::AuthToken ToAuthToken<Mega::Auth::AuthToken>(
    const nlohmann::json& json) {
  Mega::Auth::AuthToken auth_token = {
      .email = json.at("email"),
      .session = std::string(json.at("session")),
      .pkey = json.at("pkey")};
  return auth_token;
}

template <>
Mega::Auth::AuthData GetAuthData<Mega>(const nlohmann::json& json) {
  return {.api_key = json.at("api_key"), .app_name = json.at("app_name")};
}

template <>
auto AbstractCloudProvider::Create<Mega>(Mega p)
    -> std::unique_ptr<AbstractCloudProvider> {
  return CreateAbstractCloudProvider(std::move(p));
}

}  // namespace util

template auto Mega::ListDirectoryPage(Directory, std::optional<std::string>,
                                      coro::stdx::stop_token) -> Task<PageData>;

template auto Mega::ListDirectoryPage(Root, std::optional<std::string>,
                                      coro::stdx::stop_token) -> Task<PageData>;

template auto Mega::ListDirectoryPage(Inbox, std::optional<std::string>,
                                      coro::stdx::stop_token) -> Task<PageData>;

template auto Mega::ListDirectoryPage(Trash, std::optional<std::string>,
                                      coro::stdx::stop_token) -> Task<PageData>;

template auto Mega::CreateDirectory(Root, std::string, stdx::stop_token)
    -> Task<Directory>;

template auto Mega::CreateDirectory(Directory, std::string, stdx::stop_token)
    -> Task<Directory>;

template auto Mega::CreateFile(Directory, std::string_view, FileContent,
                               stdx::stop_token) -> Task<File>;

template auto Mega::CreateFile(Root, std::string_view, FileContent,
                               stdx::stop_token) -> Task<File>;

template auto Mega::RenameItem(File item, std::string new_name,
                               stdx::stop_token stop_token) -> Task<File>;

template auto Mega::RenameItem(Directory item, std::string new_name,
                               stdx::stop_token stop_token) -> Task<Directory>;

template auto Mega::MoveItem(File, Directory, stdx::stop_token) -> Task<File>;

template auto Mega::MoveItem(File, Inbox, stdx::stop_token) -> Task<File>;

template auto Mega::MoveItem(File, Trash, stdx::stop_token) -> Task<File>;

template auto Mega::MoveItem(File, Root, stdx::stop_token) -> Task<File>;

template auto Mega::MoveItem(Directory, Directory, stdx::stop_token)
    -> Task<Directory>;

template auto Mega::MoveItem(Directory, Inbox, stdx::stop_token)
    -> Task<Directory>;

template auto Mega::MoveItem(Directory, Trash, stdx::stop_token)
    -> Task<Directory>;

template auto Mega::MoveItem(Directory, Root, stdx::stop_token)
    -> Task<Directory>;

template Task<> Mega::RemoveItem(File, stdx::stop_token);

template Task<> Mega::RemoveItem(Directory, stdx::stop_token);

}  // namespace coro::cloudstorage
