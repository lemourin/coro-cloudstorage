#include "amazon_s3.h"

#include <coro/http/http_parse.h>
#include <cryptopp/cryptlib.h>
#include <cryptopp/hex.h>
#include <cryptopp/hmac.h>
#include <cryptopp/sha.h>

#include <iomanip>
#include <regex>

namespace coro::cloudstorage {

namespace {

std::string GetDate(std::chrono::system_clock::time_point now) {
  auto time = http::gmtime(
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count());
  std::stringstream ss;
  ss << std::put_time(&time, "%Y%m%d");
  return ss.str();
}

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

}  // namespace

std::string AmazonS3::GetDateAndTime(
    std::chrono::system_clock::time_point now) {
  auto time = http::gmtime(
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count());
  std::stringstream ss;
  ss << std::put_time(&time, "%Y%m%dT%H%M%SZ");
  return ss.str();
}

std::string AmazonS3::GetAuthorization(
    std::string_view url, http::Method method,
    std::span<std::pair<std::string, std::string>> headers,
    const Auth::AuthToken& auth_token,
    std::chrono::system_clock::time_point current_time) {
  std::string current_date = GetDate(current_time);
  std::string time = GetDateAndTime(current_time);
  std::string scope =
      util::StrCat(current_date, "/", auth_token.region, "/s3/aws4_request");
  std::stringstream canonical_request;
  auto uri = http::ParseUri(url);
  canonical_request << http::MethodToString(method) << "\n"
                    << uri.path.value_or("") << "\n";
  std::vector<std::pair<std::string, std::string>> query_params;
  for (const auto& [key, value] : http::ParseQuery(uri.query.value_or(""))) {
    query_params.emplace_back(http::EncodeUri(key), http::EncodeUri(value));
  }
  std::sort(query_params.begin(), query_params.end());
  bool first = true;
  for (const auto& [key, value] : query_params) {
    if (first) {
      first = false;
    } else {
      canonical_request << "&";
    }
    canonical_request << key << "=" << value;
  }
  canonical_request << "\n";
  std::vector<std::pair<std::string, std::string>> header_params;
  for (const auto& [key, value] : headers) {
    header_params.emplace_back(http::ToLowerCase(key),
                               http::TrimWhitespace(value));
  }
  std::sort(header_params.begin(), header_params.end());
  for (const auto& [key, value] : header_params) {
    canonical_request << key << ":" << value << "\n";
  }
  canonical_request << "\n";
  first = true;
  std::stringstream headers_str;
  for (const auto& [key, value] : header_params) {
    if (first) {
      first = false;
    } else {
      headers_str << ";";
    }
    headers_str << key;
  }
  canonical_request << headers_str.str() << "\nUNSIGNED-PAYLOAD";

  std::stringstream string_to_sign;
  string_to_sign << "AWS4-HMAC-SHA256\n"
                 << time << "\n"
                 << scope << "\n"
                 << ToHex(GetSHA256(std::move(canonical_request).str()));

  std::string signature = ToHex(GetHMACSHA256(
      GetHMACSHA256(
          GetHMACSHA256(
              GetHMACSHA256(
                  GetHMACSHA256(util::StrCat("AWS4", auth_token.secret_key),
                                current_date),
                  auth_token.region),
              "s3"),
          "aws4_request"),
      std::move(string_to_sign).str()));

  std::stringstream authorization_header;
  authorization_header << "AWS4-HMAC-SHA256 Credential="
                       << auth_token.access_key_id << "/" << scope
                       << ",SignedHeaders=" << std::move(headers_str).str()
                       << ",Signature=" << signature;

  return std::move(authorization_header).str();
}

std::string AmazonS3::GetFileName(std::string path) {
  if (!path.empty() && path.back() == '/') {
    path.pop_back();
  }
  auto it = path.find_last_of('/');
  return path.substr(it == std::string::npos ? 0 : it + 1);
}

std::string AmazonS3::GetDirectoryPath(std::string path) {
  if (!path.empty() && path.back() == '/') {
    path.pop_back();
  }
  auto it = path.find_last_of('/');
  if (it == std::string::npos) {
    return "";
  } else {
    return path.substr(0, it);
  }
}

AmazonS3::File AmazonS3::ToFile(const pugi::xml_node& node) {
  File entry;
  entry.id = node.child_value("Key");
  entry.name = GetFileName(entry.id);
  entry.size = std::stoll(node.child_value("Size"));
  entry.timestamp = http::ParseTime(node.child_value("LastModified"));
  return entry;
}

AmazonS3::PageData AmazonS3::ToPageData(const Directory& directory,
                                        const pugi::xml_document& response) {
  PageData result;
  for (auto node = response.document_element().child("CommonPrefixes"); node;
       node = node.next_sibling("CommonPrefixes")) {
    Directory entry;
    entry.id = node.child_value("Prefix");
    entry.name = GetFileName(entry.id);
    result.items.emplace_back(std::move(entry));
  }
  for (auto node = response.document_element().child("Contents"); node;
       node = node.next_sibling("Contents")) {
    auto entry = ToFile(node);
    if (entry.id == directory.id) {
      continue;
    }
    result.items.emplace_back(std::move(entry));
  }
  if (auto node = response.document_element().child("IsTruncated");
      node.child_value() == std::string("true")) {
    result.next_page_token =
        response.document_element().child_value("NextContinuationToken");
  }
  return result;
}

pugi::xml_document AmazonS3::GetXmlDocument(std::string data) {
  pugi::xml_document document;
  std::stringstream stream(std::move(data));
  auto status = document.load(stream);
  if (!status) {
    throw CloudException(status.description());
  }
  return document;
}

namespace util {

template <>
nlohmann::json ToJson<AmazonS3::Auth::AuthToken>(
    AmazonS3::Auth::AuthToken token) {
  nlohmann::json json;
  json["endpoint"] = std::move(token.endpoint);
  json["access_key_id"] = std::move(token.access_key_id);
  json["secret_key"] = std::move(token.secret_key);
  json["region"] = std::move(token.region);
  json["bucket"] = std::move(token.bucket);
  return json;
}

template <>
AmazonS3::Auth::AuthToken ToAuthToken<AmazonS3::Auth::AuthToken>(
    const nlohmann::json& json) {
  AmazonS3::Auth::AuthToken auth_token;
  auth_token.endpoint = json.at("endpoint");
  auth_token.access_key_id = json.at("access_key_id");
  auth_token.secret_key = json.at("secret_key");
  auth_token.region = json.at("region");
  auth_token.bucket = json.at("bucket");
  return auth_token;
}

template <>
AmazonS3::Auth::AuthData GetAuthData<AmazonS3>() {
  return {};
}

}  // namespace util

}  // namespace coro::cloudstorage