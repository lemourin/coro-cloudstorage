#ifndef CORO_CLOUDSTORAGE_AMAZON_S3_H
#define CORO_CLOUDSTORAGE_AMAZON_S3_H

#include <chrono>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <span>
#include <sstream>

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/auth_handler.h"
#include "coro/cloudstorage/util/auth_token_manager.h"
#include "coro/cloudstorage/util/file_utils.h"
#include "coro/cloudstorage/util/recursive_visit.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"
#include "coro/when_all.h"

namespace coro::cloudstorage {

class AmazonS3 {
 public:
  struct GeneralData {
    std::string username;
  };

  struct ItemData {
    std::string id;
    std::string name;
  };

  struct Directory : ItemData {};

  struct File : ItemData {
    int64_t size;
    int64_t timestamp;
  };

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct Auth {
    struct AuthToken {
      std::string access_key_id;
      std::string secret_key;
      std::string endpoint;
      std::string region;
      std::string bucket;
    };
    struct AuthData {};

    template <typename Http = class HttpT>
    class AuthHandler;
  };

  struct FileContent {
    Generator<std::string> data;
    int64_t size;
  };

  template <typename Http = class HttpT>
  class CloudProvider;

  static constexpr std::string_view kId = "amazons3";
  static inline constexpr auto& kIcon = util::kAssetsProvidersAmazons3Png;

  template <http::HttpClient Http>
  static Task<Auth::AuthToken> GetAuthToken(const Http& http,
                                            std::string access_key_id,
                                            std::string secret_key,
                                            std::string endpoint,
                                            stdx::stop_token stop_token) {
    Auth::AuthToken auth_token{.access_key_id = std::move(access_key_id),
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

 private:
  static std::string GetAuthorization(
      std::string_view url, http::Method method,
      std::span<std::pair<std::string, std::string>> headers,
      const Auth::AuthToken&,
      std::chrono::system_clock::time_point current_time);
  static std::string GetDateAndTime(std::chrono::system_clock::time_point);
  template <typename Request>
  static void AuthorizeRequest(const Auth::AuthToken& auth_token,
                               Request& request) {
    auto current_time = std::chrono::system_clock::now();
    request.headers.emplace_back("X-Amz-Date", GetDateAndTime(current_time));
    request.headers.emplace_back("X-Amz-Content-SHA256", "UNSIGNED-PAYLOAD");
    request.headers.emplace_back("Host",
                                 http::ParseUri(request.url).host.value());
    request.headers.emplace_back(
        "Authorization",
        GetAuthorization(request.url, request.method, request.headers,
                         auth_token, current_time));
  }
  static File ToFile(const pugi::xml_node&);
  static PageData ToPageData(const Directory&, const pugi::xml_document&);
  static pugi::xml_document GetXmlDocument(std::string data);
};

template <typename Http>
class AmazonS3::CloudProvider
    : public coro::cloudstorage::CloudProvider<AmazonS3, CloudProvider<Http>> {
 public:
  using Request = http::Request<std::string>;

  CloudProvider(const Http* http, AmazonS3::Auth::AuthToken auth_token)
      : http_(http), auth_token_(std::move(auth_token)) {}

  Task<Directory> GetRoot(stdx::stop_token) const {
    Directory d{{.id = ""}};
    co_return d;
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token) const {
    GeneralData data{.username =
                         http::ParseUri(auth_token_.endpoint).host.value()};
    co_return data;
  }

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) const {
    std::vector<std::pair<std::string, std::string>> params = {
        {"list-type", "2"}, {"prefix", directory.id}, {"delimiter", "/"}};
    if (page_token) {
      params.emplace_back("continuation-token", std::move(*page_token));
    }
    Request request{.url = GetEndpoint("/") + "?" +
                           http::FormDataToString(params)};
    pugi::xml_document response =
        co_await FetchXml(std::move(request), std::move(stop_token));
    co_return ToPageData(directory, response);
  }

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) const {
    auto request = Request{
        .url = GetEndpoint(util::StrCat("/", http::EncodeUriPath(file.id))),
        .headers = {ToRangeHeader(range)}};
    auto response = co_await Fetch(std::move(request), std::move(stop_token));
    FOR_CO_AWAIT(std::string & body, response.body) {
      co_yield std::move(body);
    }
  }

  template <typename Item>
  Task<Item> RenameItem(Item item, std::string new_name,
                        stdx::stop_token stop_token) {
    auto destination_path = util::GetDirectoryPath(item.id);
    if (!destination_path.empty()) {
      destination_path += "/";
    }
    destination_path += new_name;
    if constexpr (IsDirectory<Item, CloudProvider>) {
      destination_path += "/";
    }
    co_await Move(item, destination_path, stop_token);
    co_return co_await GetItem<Item>(destination_path, std::move(stop_token));
  }

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token) const {
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

  template <typename Item>
  Task<> RemoveItem(Item item, stdx::stop_token stop_token) {
    co_await Visit(
        item,
        [&](const auto& entry) -> Task<> {
          co_await RemoveItemImpl(entry.id, stop_token);
        },
        stop_token);
  }

  template <typename Item>
  Task<Item> MoveItem(Item source, Directory destination,
                      stdx::stop_token stop_token) {
    auto destination_path = util::StrCat(destination.id, source.name);
    if constexpr (IsDirectory<Item, CloudProvider>) {
      destination_path += "/";
    }
    co_await Move(source, destination_path, stop_token);
    co_return co_await GetItem<Item>(destination_path, std::move(stop_token));
  }

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content,
                        stdx::stop_token stop_token) const {
    auto new_id = util::StrCat(parent.id, name);
    auto request = http::Request<>{
        .url = GetEndpoint(util::StrCat("/", http::EncodeUriPath(new_id))),
        .method = http::Method::kPut,
        .headers = {{"Content-Length", std::to_string(content.size)}},
        .body = std::move(content.data)};
    co_await Fetch(std::move(request), stop_token);
    co_return co_await GetItem<File>(new_id, std::move(stop_token));
  }

  template <typename Item>
  Task<Item> GetItem(std::string_view id, stdx::stop_token stop_token) const {
    pugi::xml_document response = co_await FetchXml(
        Request{.url = GetEndpoint("/") + "?" +
                       http::FormDataToString({{"list-type", "2"},
                                               {"prefix", id},
                                               {"delimiter", "/"}})},
        std::move(stop_token));
    if constexpr (IsDirectory<Item, CloudProvider>) {
      auto file = ToFile(response.document_element().child("Contents"));
      Item directory;
      directory.id = std::move(file.id);
      directory.name = std::move(file.name);
      co_return directory;
    } else {
      co_return ToFile(response.document_element().child("Contents"));
    }
  }

 private:
  std::string GetEndpoint(std::string_view href) const {
    return util::StrCat(auth_token_.endpoint, href);
  }

  template <typename Item, typename F>
  Task<> Visit(Item item, const F& func, stdx::stop_token stop_token) {
    return util::RecursiveVisit(this, std::move(item), func,
                                std::move(stop_token));
  }

  Task<> RemoveItemImpl(std::string_view id,
                        stdx::stop_token stop_token) const {
    co_await Fetch(
        Request{.url = GetEndpoint(util::StrCat("/", http::EncodeUriPath(id))),
                .method = http::Method::kDelete,
                .headers = {{"Content-Length", "0"}}},
        std::move(stop_token));
  }

  template <typename Item>
  Task<> Move(const Item& root, std::string_view destination,
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

  template <typename Item>
  Task<> MoveItemImpl(const Item& source, std::string_view destination,
                      stdx::stop_token stop_token) const {
    Request request{
        .url = GetEndpoint(util::StrCat("/", http::EncodeUriPath(destination))),
        .method = http::Method::kPut,
        .headers = {{"Content-Length", "0"}}};
    if constexpr (!IsDirectory<Item, CloudProvider>) {
      request.headers.emplace_back("X-Amz-Copy-Source",
                                   http::EncodeUriPath(util::StrCat(
                                       auth_token_.bucket, "/", source.id)));
    }
    co_await Fetch(std::move(request), stop_token);
    co_await RemoveItemImpl(source.id, std::move(stop_token));
  }

  template <typename RequestT>
  Task<typename Http::ResponseType> Fetch(RequestT request,
                                          stdx::stop_token stop_token) const {
    AuthorizeRequest(auth_token_, request);
    http::ResponseLike auto response =
        co_await http_->Fetch(std::move(request), std::move(stop_token));
    if (response.status / 100 != 2) {
      throw coro::http::HttpException(
          response.status, co_await http::GetBody(std::move(response.body)));
    }
    co_return response;
  }

  template <typename RequestT>
  Task<pugi::xml_document> FetchXml(RequestT request,
                                    stdx::stop_token stop_token) const {
    if (request.body) {
      request.headers.emplace_back("Content-Type", "application/xml");
    }
    request.headers.emplace_back("Accept", "application/xml");
    http::ResponseLike auto response =
        co_await Fetch(std::move(request), std::move(stop_token));
    co_return GetXmlDocument(co_await http::GetBody(std::move(response.body)));
  }

  const Http* http_;
  AmazonS3::Auth::AuthToken auth_token_;
};

template <typename HttpT>
class AmazonS3::Auth::AuthHandler {
 public:
  explicit AuthHandler(const HttpT* http) : http_(http) {}

  Task<std::variant<http::Response<>, AmazonS3::Auth::AuthToken>> operator()(
      http::Request<> request, stdx::stop_token stop_token) const {
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
      co_return co_await AmazonS3::GetAuthToken(
          *http_, std::move(access_key_id), std::move(secret_key),
          std::move(endpoint), std::move(stop_token));
    } else {
      co_return http::Response<>{.status = 200, .body = GenerateLoginPage()};
    }
  }

 private:
  static Generator<std::string> GenerateLoginPage() {
    co_yield std::string(util::kAssetsHtmlAmazons3LoginHtml);
  }

  const HttpT* http_;
};

namespace util {

template <>
nlohmann::json ToJson<AmazonS3::Auth::AuthToken>(
    AmazonS3::Auth::AuthToken token);

template <>
AmazonS3::Auth::AuthToken ToAuthToken<AmazonS3::Auth::AuthToken>(
    const nlohmann::json& json);

template <>
AmazonS3::Auth::AuthData GetAuthData<AmazonS3>();

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_AMAZON_S3_H
