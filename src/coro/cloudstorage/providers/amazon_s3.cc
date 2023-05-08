#include "coro/cloudstorage/providers/amazon_s3.h"

#include <iomanip>
#include <string>
#include <utility>
#include <vector>

#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"
#include "coro/cloudstorage/util/crypto_utils.h"
#include "coro/cloudstorage/util/file_utils.h"
#include "coro/http/http_parse.h"

namespace coro::cloudstorage {

namespace {

using ::coro::cloudstorage::util::GetFileName;
using ::coro::cloudstorage::util::GetHMACSHA256;
using ::coro::cloudstorage::util::GetSHA256;
using ::coro::cloudstorage::util::ToHex;

Generator<std::string> GenerateLoginPage() {
  co_yield std::string(util::kAmazonS3LoginHtml);
}

std::string GetDate(std::chrono::system_clock::time_point now) {
  auto time = http::gmtime(std::chrono::system_clock::to_time_t(now));
  std::stringstream ss;
  ss << std::put_time(&time, "%Y%m%d");
  return ss.str();
}

std::string GetDateAndTime(std::chrono::system_clock::time_point now) {
  auto time = http::gmtime(std::chrono::system_clock::to_time_t(now));
  std::stringstream ss;
  ss << std::put_time(&time, "%Y%m%dT%H%M%SZ");
  return ss.str();
}

std::string GetAuthorization(
    std::string_view url, http::Method method,
    std::span<std::pair<std::string, std::string>> headers,
    const AmazonS3::Auth::AuthToken& auth_token,
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

template <typename Request>
void AuthorizeRequest(const AmazonS3::Auth::AuthToken& auth_token,
                      Request& request) {
  auto current_time = std::chrono::system_clock::now();
  request.headers.emplace_back("X-Amz-Date", GetDateAndTime(current_time));
  request.headers.emplace_back("X-Amz-Content-SHA256", "UNSIGNED-PAYLOAD");
  request.headers.emplace_back("Host",
                               http::ParseUri(request.url).host.value());
  request.headers.emplace_back(
      "Authorization",
      GetAuthorization(request.url, request.method, request.headers, auth_token,
                       current_time));
}

pugi::xml_document GetXmlDocument(std::string data) {
  pugi::xml_document document;
  std::stringstream stream(std::move(data));
  auto status = document.load(stream);
  if (!status) {
    throw CloudException(status.description());
  }
  return document;
}

AmazonS3::File ToFile(const pugi::xml_node& node) {
  AmazonS3::File entry;
  entry.id = node.child_value("Key");
  entry.name = GetFileName(entry.id);
  entry.size = std::stoll(node.child_value("Size"));
  entry.timestamp = http::ParseTime(node.child_value("LastModified"));
  return entry;
}

AmazonS3::PageData ToPageData(const AmazonS3::Directory& directory,
                              const pugi::xml_document& response) {
  AmazonS3::PageData result;
  for (auto node = response.document_element().child("CommonPrefixes"); node;
       node = node.next_sibling("CommonPrefixes")) {
    AmazonS3::Directory entry;
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

Task<AmazonS3::Auth::AuthToken> GetAuthToken(const coro::http::Http& http,
                                             std::string access_key_id,
                                             std::string secret_key,
                                             std::string endpoint,
                                             stdx::stop_token stop_token) {
  AmazonS3::Auth::AuthToken auth_token{
      .access_key_id = std::move(access_key_id),
      .secret_key = std::move(secret_key),
      .endpoint = std::move(endpoint),
      .region = "us-east-1"};
  http::Request<std::string> request{
      .url = util::StrCat(auth_token.endpoint, "/", "?", "location=")};
  AuthorizeRequest(auth_token, request);
  auto response = co_await http.Fetch(std::move(request), stop_token);
  pugi::xml_document document =
      GetXmlDocument(co_await http::GetBody(std::move(response.body)));
  if (auto node = document.child("Error")) {
    auth_token.region = node.child_value("Region");
  }
  if (auto node = document.child("LocationConstraint")) {
    auth_token.region = node.child_value();
  }
  request = http::Request<std::string>{
      .url = util::StrCat(
          auth_token.endpoint, "/", "?",
          http::FormDataToString(
              {{"list-type", "2"}, {"prefix", ""}, {"delimiter", "/"}}))};
  AuthorizeRequest(auth_token, request);
  response = co_await http.Fetch(std::move(request), std::move(stop_token));
  auto body = co_await http::GetBody(std::move(response.body));
  if (response.status / 100 != 2) {
    throw CloudException(std::move(body));
  }
  document = GetXmlDocument(std::move(body));
  auth_token.bucket = document.document_element().child_value("Name");
  co_return auth_token;
}

template <typename T>
T ToItemImpl(const nlohmann::json& json) {
  T item;
  item.id = json["id"];
  item.name = json["name"];
  if constexpr (std::is_same_v<T, AmazonS3::File>) {
    item.timestamp = json["timestamp"];
    item.size = json["size"];
  }
  return item;
}

}  // namespace

auto AmazonS3::GetRoot(stdx::stop_token) const -> Task<Directory> {
  Directory d{{.id = ""}};
  co_return d;
}

auto AmazonS3::GetGeneralData(stdx::stop_token) const -> Task<GeneralData> {
  GeneralData data{.username =
                       http::ParseUri(auth_token_.endpoint).host.value()};
  co_return data;
}

auto AmazonS3::ListDirectoryPage(Directory directory,
                                 std::optional<std::string> page_token,
                                 stdx::stop_token stop_token) const
    -> Task<PageData> {
  std::vector<std::pair<std::string, std::string>> params = {
      {"list-type", "2"}, {"prefix", directory.id}, {"delimiter", "/"}};
  if (page_token) {
    params.emplace_back("continuation-token", std::move(*page_token));
  }
  Request request{.url =
                      GetEndpoint("/") + "?" + http::FormDataToString(params)};
  pugi::xml_document response =
      co_await FetchXml(std::move(request), std::move(stop_token));
  co_return ToPageData(directory, response);
}

Generator<std::string> AmazonS3::GetFileContent(
    File file, http::Range range, stdx::stop_token stop_token) const {
  auto request = Request{
      .url = GetEndpoint(util::StrCat("/", http::EncodeUriPath(file.id))),
      .headers = {ToRangeHeader(range)}};
  auto response = co_await Fetch(std::move(request), std::move(stop_token));
  FOR_CO_AWAIT(std::string & body, response.body) { co_yield std::move(body); }
}

template <typename ItemT>
Task<ItemT> AmazonS3::RenameItem(ItemT item, std::string new_name,
                                 stdx::stop_token stop_token) {
  auto destination_path = util::GetDirectoryPath(item.id);
  if (!destination_path.empty()) {
    destination_path += "/";
  }
  destination_path += new_name;
  if constexpr (std::is_same_v<ItemT, Directory>) {
    destination_path += "/";
  }
  co_await Move(item, destination_path, stop_token);
  co_return co_await GetItem<ItemT>(destination_path, std::move(stop_token));
}

auto AmazonS3::CreateDirectory(Directory parent, std::string name,
                               stdx::stop_token stop_token) const
    -> Task<Directory> {
  auto id = util::StrCat(parent.id, name, "/");
  auto request =
      Request{.url = GetEndpoint(util::StrCat("/", http::EncodeUriPath(id))),
              .method = http::Method::kPut,
              .headers = {{"Content-Length", "0"}}};
  co_await Fetch(std::move(request), std::move(stop_token));
  Directory new_directory;
  new_directory.id = std::move(id);
  new_directory.name = std::move(name);
  co_return new_directory;
}

template <typename ItemT>
Task<> AmazonS3::RemoveItem(ItemT item, stdx::stop_token stop_token) {
  co_await Visit(
      item,
      [&](const auto& entry) -> Task<> {
        co_await RemoveItemImpl(entry.id, stop_token);
      },
      stop_token);
}

template <typename ItemT>
Task<ItemT> AmazonS3::MoveItem(ItemT source, Directory destination,
                               stdx::stop_token stop_token) {
  auto destination_path = util::StrCat(destination.id, source.name);
  if constexpr (std::is_same_v<ItemT, Directory>) {
    destination_path += "/";
  }
  co_await Move(source, destination_path, stop_token);
  co_return co_await GetItem<ItemT>(destination_path, std::move(stop_token));
}

auto AmazonS3::CreateFile(Directory parent, std::string_view name,
                          FileContent content,
                          stdx::stop_token stop_token) const -> Task<File> {
  auto new_id = util::StrCat(parent.id, name);
  auto request = http::Request<>{
      .url = GetEndpoint(util::StrCat("/", http::EncodeUriPath(new_id))),
      .method = http::Method::kPut,
      .headers = {{"Content-Length", std::to_string(content.size)}},
      .body = std::move(content.data)};
  co_await Fetch(std::move(request), stop_token);
  co_return co_await GetItem<File>(new_id, std::move(stop_token));
}

template <typename ItemT>
Task<ItemT> AmazonS3::GetItem(std::string_view id,
                              stdx::stop_token stop_token) const {
  pugi::xml_document response = co_await FetchXml(
      Request{
          .url = GetEndpoint("/") + "?" +
                 http::FormDataToString(
                     {{"list-type", "2"}, {"prefix", id}, {"delimiter", "/"}})},
      std::move(stop_token));
  if constexpr (std::is_same_v<ItemT, Directory>) {
    auto file = ToFile(response.document_element().child("Contents"));
    ItemT directory;
    directory.id = std::move(file.id);
    directory.name = std::move(file.name);
    co_return directory;
  } else {
    co_return ToFile(response.document_element().child("Contents"));
  }
}

Task<std::variant<http::Response<>, AmazonS3::Auth::AuthToken>>
AmazonS3::Auth::AuthHandler::operator()(http::Request<> request,
                                        stdx::stop_token stop_token) const {
  if (request.method == http::Method::kPost) {
    auto query =
        http::ParseQuery(co_await http::GetBody(std::move(*request.body)));
    std::string endpoint;
    if (auto it = query.find("endpoint");
        it != query.end() && !it->second.empty()) {
      endpoint = std::move(it->second);
    } else {
      throw CloudException("missing endpoint");
    }
    auto it1 = query.find("access_key_id");
    auto it2 = query.find("secret_key");
    std::string access_key_id;
    std::string secret_key;
    if (it1 != query.end() && it2 != query.end() && !it1->second.empty() &&
        !it2->second.empty()) {
      access_key_id = std::move(it1->second);
      secret_key = std::move(it2->second);
    } else {
      throw CloudException("missing credentials");
    }
    co_return co_await GetAuthToken(*http_, std::move(access_key_id),
                                    std::move(secret_key), std::move(endpoint),
                                    std::move(stop_token));
  } else {
    co_return http::Response<>{.status = 200, .body = GenerateLoginPage()};
  }
}

std::string AmazonS3::GetEndpoint(std::string_view href) const {
  return util::StrCat(auth_token_.endpoint, href);
}

template <typename ItemT, typename F>
Task<> AmazonS3::Visit(ItemT item, const F& func, stdx::stop_token stop_token) {
  return util::RecursiveVisit<AmazonS3>(this, std::move(item), func,
                                        std::move(stop_token));
}

Task<> AmazonS3::RemoveItemImpl(std::string_view id,
                                stdx::stop_token stop_token) const {
  co_await Fetch(
      Request{.url = GetEndpoint(util::StrCat("/", http::EncodeUriPath(id))),
              .method = http::Method::kDelete,
              .headers = {{"Content-Length", "0"}}},
      std::move(stop_token));
}

template <typename ItemT>
Task<> AmazonS3::Move(const ItemT& root, std::string_view destination,
                      stdx::stop_token stop_token) {
  co_await Visit(
      root,
      [&](const auto& source) -> Task<> {
        co_await MoveItemImpl(
            source,
            util::StrCat(destination, source.id.substr(root.id.length())),
            stop_token);
      },
      stop_token);
}

template <typename ItemT>
Task<> AmazonS3::MoveItemImpl(const ItemT& source, std::string_view destination,
                              stdx::stop_token stop_token) const {
  Request request{
      .url = GetEndpoint(util::StrCat("/", http::EncodeUriPath(destination))),
      .method = http::Method::kPut,
      .headers = {{"Content-Length", "0"}}};
  if constexpr (!std::is_same_v<ItemT, Directory>) {
    request.headers.emplace_back(
        "X-Amz-Copy-Source",
        http::EncodeUriPath(util::StrCat(auth_token_.bucket, "/", source.id)));
  }
  co_await Fetch(std::move(request), stop_token);
  co_await RemoveItemImpl(source.id, std::move(stop_token));
}

template <typename RequestT>
Task<http::Response<>> AmazonS3::Fetch(RequestT request,
                                       stdx::stop_token stop_token) const {
  AuthorizeRequest(auth_token_, request);
  co_return co_await http_->FetchOk(std::move(request), std::move(stop_token));
}

template <typename RequestT>
Task<pugi::xml_document> AmazonS3::FetchXml(RequestT request,
                                            stdx::stop_token stop_token) const {
  if (request.body) {
    request.headers.emplace_back("Content-Type", "application/xml");
  }
  request.headers.emplace_back("Accept", "application/xml");
  auto response = co_await Fetch(std::move(request), std::move(stop_token));
  co_return GetXmlDocument(co_await http::GetBody(std::move(response.body)));
}

auto AmazonS3::ToItem(std::string_view serialized) -> Item {
  auto json = nlohmann::json::parse(serialized);
  if (json.contains("size")) {
    return ToItemImpl<File>(json);
  } else {
    return ToItemImpl<Directory>(json);
  }
}

std::string AmazonS3::ToString(const Item& item) {
  return std::visit(
      []<typename T>(const T& item) {
        nlohmann::json json;
        json["id"] = item.id;
        json["name"] = item.name;
        if constexpr (std::is_same_v<T, File>) {
          json["timestamp"] = item.timestamp;
          json["size"] = item.size;
        }
        return json.dump();
      },
      item);
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
auto AbstractCloudProvider::Create<AmazonS3>(AmazonS3 p)
    -> std::unique_ptr<AbstractCloudProvider> {
  return CreateAbstractCloudProvider(std::move(p));
}

}  // namespace util

template auto AmazonS3::RenameItem(File item, std::string new_name,
                                   stdx::stop_token stop_token) -> Task<File>;

template auto AmazonS3::RenameItem(Directory item, std::string new_name,
                                   stdx::stop_token stop_token)
    -> Task<Directory>;

template auto AmazonS3::MoveItem(File, Directory, stdx::stop_token)
    -> Task<File>;

template auto AmazonS3::MoveItem(Directory, Directory, stdx::stop_token)
    -> Task<Directory>;

template auto AmazonS3::RemoveItem(File, stdx::stop_token) -> Task<>;

template auto AmazonS3::RemoveItem(Directory, stdx::stop_token) -> Task<>;

}  // namespace coro::cloudstorage
