#ifndef CORO_CLOUDSTORAGE_PCLOUD_H
#define CORO_CLOUDSTORAGE_PCLOUD_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/auth_data.h>
#include <coro/cloudstorage/util/fetch_json.h>
#include <coro/http/http.h>
#include <coro/when_all.h>

#include <nlohmann/json.hpp>
#include <sstream>

namespace coro::cloudstorage {

struct PCloud {
  struct GeneralData {
    std::string username;
    int64_t space_used;
    int64_t space_total;
  };

  struct ItemData {
    int64_t id;
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
    using json = nlohmann::json;

    struct AuthToken {
      std::string access_token;
      std::string hostname;
    };

    struct AuthData {
      std::string client_id;
      std::string client_secret;
      std::string redirect_uri;
      std::string state;
    };

    static std::string GetAuthorizationUrl(const AuthData& data) {
      return "https://my.pcloud.com/oauth2/authorize?" +
             http::FormDataToString({{"response_type", "code"},
                                     {"client_id", data.client_id},
                                     {"redirect_uri", data.redirect_uri},
                                     {"state", data.state}});
    }

    template <http::HttpClient Http>
    static Task<AuthToken> ExchangeAuthorizationCode(
        const Http& http, AuthData auth_data, std::string code,
        std::string hostname, stdx::stop_token stop_token) {
      auto request = http::Request<std::string>{
          .url = hostname + "/oauth2_token",
          .method = http::Method::kPost,
          .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
          .body = http::FormDataToString(
              {{"client_secret", auth_data.client_secret},
               {"client_id", auth_data.client_id},
               {"code", std::move(code)}})};
      json json = co_await util::FetchJson(http, std::move(request),
                                           std::move(stop_token));
      co_return AuthToken{.access_token = std::string(json["access_token"]),
                          .hostname = std::move(hostname)};
    }
  };

  struct FileContent {
    Generator<std::string> data;
    int64_t size;
  };

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    std::string mime_type;
  };

  template <http::HttpClient Http>
  class CloudProvider;

  static constexpr std::string_view kId = "pcloud";
};

template <http::HttpClient Http>
class PCloud::CloudProvider
    : public coro::cloudstorage::CloudProvider<PCloud, CloudProvider<Http>> {
 public:
  using json = nlohmann::json;
  using Request = http::Request<std::string>;

  CloudProvider(const Http& http, PCloud::Auth::AuthToken auth_token)
      : http_(&http), auth_token_(std::move(auth_token)) {}

  Task<Directory> GetRoot(stdx::stop_token) {
    Directory d{{.id = 0}};
    co_return d;
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) {
    Request request{.url = GetEndpoint("/userinfo")};
    auto response =
        co_await FetchJson(std::move(request), std::move(stop_token));
    co_return GeneralData{.username = std::string(response["email"]),
                          .space_used = response["usedquota"],
                          .space_total = response["quota"]};
  }

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
    Request request{.url = GetEndpoint("/listfolder") + "?" +
                           http::FormDataToString(
                               {{"folderid", std::to_string(directory.id)},
                                {"timeformat", "timestamp"}})};
    auto response =
        co_await FetchJson(std::move(request), std::move(stop_token));
    PageData result;
    for (const auto& entry : response["metadata"]["contents"]) {
      result.items.emplace_back(ToItem(entry));
    }
    co_return result;
  }

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) {
    Request request{
        .url = GetEndpoint("/getfilelink") + "?" +
               http::FormDataToString({{"fileid", std::to_string(file.id)}})};
    auto url_response = co_await FetchJson(std::move(request), stop_token);
    std::stringstream range_header;
    range_header << "bytes=" << range.start << "-";
    if (range.end) {
      range_header << *range.end;
    }
    request = {.url = "https://" + std::string(url_response["hosts"][0]) +
                      std::string(url_response["path"]),
               .headers = {{"Range", std::move(range_header).str()}}};
    auto content_response =
        co_await http_->Fetch(std::move(request), std::move(stop_token));
    FOR_CO_AWAIT(std::string & body, content_response.body) {
      co_yield std::move(body);
    }
  }

  Task<Directory> RenameItem(Directory item, std::string new_name,
                             stdx::stop_token stop_token) {
    Request request{
        .url = GetEndpoint("/renamefolder") + "?" +
               http::FormDataToString({{"folderid", std::to_string(item.id)},
                                       {"toname", std::move(new_name)},
                                       {"timeformat", "timestamp"}}),
        .flags = Request::kWrite};
    auto response =
        co_await FetchJson(std::move(request), std::move(stop_token));
    co_return ToItemImpl<Directory>(response["metadata"]);
  }

  Task<File> RenameItem(File item, std::string new_name,
                        stdx::stop_token stop_token) {
    Request request{
        .url = GetEndpoint("/renamefile") + "?" +
               http::FormDataToString({{"fileid", std::to_string(item.id)},
                                       {"toname", std::move(new_name)},
                                       {"timeformat", "timestamp"}}),
        .flags = Request::kWrite};
    auto response =
        co_await FetchJson(std::move(request), std::move(stop_token));
    co_return ToItemImpl<File>(response["metadata"]);
  }

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token) {
    Request request{
        .url = GetEndpoint("/createfolder") + "?" +
               http::FormDataToString({{"folderid", std::to_string(parent.id)},
                                       {"name", std::move(name)},
                                       {"timeformat", "timestamp"}}),
        .flags = Request::kWrite};
    auto response =
        co_await FetchJson(std::move(request), std::move(stop_token));
    co_return ToItemImpl<Directory>(response["metadata"]);
  }

  Task<> RemoveItem(File item, stdx::stop_token stop_token) {
    Request request{
        .url = GetEndpoint("/deletefile") + "?" +
               http::FormDataToString({{"fileid", std::to_string(item.id)}}),
        .flags = Request::kWrite};
    co_await Fetch(std::move(request), std::move(stop_token));
  }

  Task<> RemoveItem(Directory item, stdx::stop_token stop_token) {
    Request request{
        .url = GetEndpoint("/deletefolderrecursive") + "?" +
               http::FormDataToString({{"folderid", std::to_string(item.id)}}),
        .flags = Request::kWrite};
    co_await Fetch(std::move(request), std::move(stop_token));
  }

  Task<Directory> MoveItem(Directory source, Directory destination,
                           stdx::stop_token stop_token) {
    Request request{.url = GetEndpoint("/renamefolder") + "?" +
                           http::FormDataToString(
                               {{"folderid", std::to_string(source.id)},
                                {"tofolderid", std::to_string(destination.id)},
                                {"timeformat", "timestamp"}}),
                    .flags = Request::kWrite};
    auto response =
        co_await FetchJson(std::move(request), std::move(stop_token));
    co_return ToItemImpl<Directory>(response["metadata"]);
  }

  Task<File> MoveItem(File source, Directory destination,
                      stdx::stop_token stop_token) {
    Request request{.url = GetEndpoint("/renamefile") + "?" +
                           http::FormDataToString(
                               {{"fileid", std::to_string(source.id)},
                                {"tofolderid", std::to_string(destination.id)},
                                {"timeformat", "timestamp"}}),
                    .flags = Request::Flag::kWrite};
    auto response =
        co_await FetchJson(std::move(request), std::move(stop_token));
    co_return ToItemImpl<File>(response["metadata"]);
  }

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token) {
    http::Request<> request{
        .url = GetEndpoint("/uploadfile") + "?" +
               http::FormDataToString({{"folderid", std::to_string(parent.id)},
                                       {"filename", name},
                                       {"timeformat", "timestamp"}}),
        .method = http::Method::kPost,
        .headers = {{"Content-Type", "multipart/form-data; boundary=" +
                                         std::string(kSeparator)},
                    {"Content-Length",
                     std::to_string(GetUploadStreamPrefix(name).length() +
                                    content.size +
                                    GetUploadStreamSuffix().length())}},
        .body = GetUploadStream(name, std::move(content)),
        .flags = http::Request<>::kWrite};
    auto response =
        co_await FetchJson(std::move(request), std::move(stop_token));
    co_return ToItemImpl<File>(response["metadata"][0]);
  }

  Task<Thumbnail> GetItemThumbnail(File file, http::Range range,
                                   stdx::stop_token stop_token) {
    Request request{
        .url = GetEndpoint("/getthumb") + "?" +
               http::FormDataToString(
                   {{"fileid", std::to_string(file.id)}, {"size", "64x64"}}),
        .headers = {ToRangeHeader(range)}};
    auto response = co_await Fetch(std::move(request), std::move(stop_token));
    Thumbnail result;
    result.mime_type =
        http::GetHeader(response.headers, "Content-Type").value();
    result.size =
        std::stoll(http::GetHeader(response.headers, "Content-Length").value());
    result.data = std::move(response.body);
    co_return result;
  }

 private:
  static constexpr std::string_view kSeparator = "Thnlg1ecwyUJHyhYYGrQ";

  std::string GetEndpoint(std::string_view path) const {
    return auth_token_.hostname + std::string(path);
  }

  static std::string GetUploadStreamPrefix(std::string_view name) {
    std::stringstream chunk;
    chunk << "--" << kSeparator << "\r\n"
          << "Content-Disposition: form-data; name=\"filename\"; "
          << "filename=\"" << name << "\"\r\n"
          << "Content-Type: application/octet-stream\r\n\r\n";
    return std::move(chunk).str();
  }

  static std::string GetUploadStreamSuffix() {
    return "\r\n--" + std::string(kSeparator) + "--";
  }

  static Generator<std::string> GetUploadStream(std::string_view name,
                                                FileContent content) {
    co_yield GetUploadStreamPrefix(name);
    FOR_CO_AWAIT(std::string & chunk, content.data) {
      co_yield std::move(chunk);
    }
    co_yield GetUploadStreamSuffix();
  }

  template <typename Request>
  Task<http::Response<>> Fetch(Request request,
                               stdx::stop_token stop_token) const {
    request.headers.emplace_back("Authorization",
                                 "Bearer " + auth_token_.access_token);
    http::ResponseLike auto response =
        co_await http_->Fetch(std::move(request), std::move(stop_token));
    if (response.status / 100 != 2) {
      std::string body = co_await http::GetBody(std::move(response.body));
      throw coro::http::HttpException(response.status, std::move(body));
    }
    if (auto error = http::GetHeader(response.headers, "x-error")) {
      throw CloudException("pcloud error " + *error);
    }
    co_return response;
  }

  template <typename Request>
  Task<json> FetchJson(Request request, stdx::stop_token stop_token) const {
    if (!http::GetHeader(request.headers, "Content-Type")) {
      request.headers.emplace_back("Content-Type", "application/json");
    }
    request.headers.emplace_back("Accept", "application/json");
    http::ResponseLike auto response =
        co_await Fetch(std::move(request), std::move(stop_token));
    std::string body = co_await http::GetBody(std::move(response.body));
    co_return nlohmann::json::parse(std::move(body));
  }

  static Item ToItem(const json& json) {
    if (json["isfolder"]) {
      return ToItemImpl<Directory>(json);
    } else {
      return ToItemImpl<File>(json);
    }
  }

  template <typename T>
  static T ToItemImpl(const json& json) {
    T result = {};
    result.name = json["name"];
    if constexpr (std::is_same_v<T, File>) {
      result.id = json["fileid"];
      result.size = json["size"];
      result.timestamp = json["modified"];
    } else {
      result.id = json["folderid"];
    }
    return result;
  }

  const Http* http_;
  PCloud::Auth::AuthToken auth_token_;
};

namespace util {
template <>
inline nlohmann::json ToJson<PCloud::Auth::AuthToken>(
    PCloud::Auth::AuthToken token) {
  nlohmann::json json;
  json["access_token"] = std::move(token.access_token);
  json["hostname"] = token.hostname;
  return json;
}

template <>
inline PCloud::Auth::AuthToken ToAuthToken<PCloud::Auth::AuthToken>(
    const nlohmann::json& json) {
  return {.access_token = std::string(json.at("access_token")),
          .hostname = std::string(json.at("hostname"))};
}
template <coro::http::HttpClient HttpClient>
class PCloudAuthHandler {
 public:
  PCloudAuthHandler(const HttpClient& http, PCloud::Auth::AuthData auth_data)
      : http_(&http), auth_data_(std::move(auth_data)) {}

  Task<PCloud::Auth::AuthToken> operator()(
      coro::http::Request<> request, coro::stdx::stop_token stop_token) const {
    auto query =
        http::ParseQuery(http::ParseUri(request.url).query.value_or(""));
    auto code_it = query.find("code");
    auto hostname_it = query.find("hostname");
    if (code_it != std::end(query) && hostname_it != std::end(query)) {
      co_return co_await PCloud::Auth::ExchangeAuthorizationCode(
          *http_, auth_data_, code_it->second, hostname_it->second,
          std::move(stop_token));
    } else {
      throw http::HttpException(http::HttpException::kBadRequest);
    }
  }

 private:
  const HttpClient* http_;
  PCloud::Auth::AuthData auth_data_;
};

template <>
struct CreateAuthHandler<PCloud> {
  template <typename CloudFactory>
  auto operator()(const CloudFactory& cloud_factory,
                  PCloud::Auth::AuthData auth_data) const {
    return PCloudAuthHandler(*cloud_factory.http_, std::move(auth_data));
  }
};
}  // namespace util

template <>
struct CreateCloudProvider<PCloud> {
  template <typename CloudFactory, typename... Args>
  auto operator()(const CloudFactory& factory,
                  PCloud::Auth::AuthToken auth_token, Args&&...) const {
    return PCloud::CloudProvider(*factory.http_, std::move(auth_token));
  }
};

namespace util {
template <>
inline PCloud::Auth::AuthData GetAuthData<PCloud>() {
  return {
      .client_id = "rjR7bUpwgdz",
      .client_secret = "zNtirCfoYfmX5aFzoavWikKWyMlV",
  };
}
}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_PCLOUD_H
