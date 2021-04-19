#ifndef CORO_CLOUDSTORAGE_GOOGLE_DRIVE_H
#define CORO_CLOUDSTORAGE_GOOGLE_DRIVE_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/assets.h>
#include <coro/cloudstorage/util/auth_data.h>
#include <coro/cloudstorage/util/auth_manager.h>
#include <coro/cloudstorage/util/fetch_json.h>
#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/stdx/coroutine.h>
#include <coro/stdx/stop_token.h>
#include <coro/task.h>

#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace coro::cloudstorage {

struct GoogleDrive {
  using json = nlohmann::json;

  struct Auth {
    struct AuthToken {
      std::string access_token;
      std::string refresh_token;
    };

    struct AuthData {
      std::string client_id;
      std::string client_secret;
      std::string redirect_uri;
      std::string state;
    };

    template <http::HttpClient Http>
    static Task<AuthToken> RefreshAccessToken(const Http& http,
                                              AuthData auth_data,
                                              AuthToken auth_token,
                                              stdx::stop_token stop_token) {
      auto request = http::Request<std::string>{
          .url = "https://accounts.google.com/o/oauth2/token",
          .method = http::Method::kPost,
          .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
          .body = http::FormDataToString(
              {{"refresh_token", auth_token.refresh_token},
               {"client_id", auth_data.client_id},
               {"client_secret", auth_data.client_secret},
               {"grant_type", "refresh_token"}})};
      json json = co_await util::FetchJson(http, std::move(request),
                                           std::move(stop_token));
      auth_token.access_token = json["access_token"];
      co_return auth_token;
    }

    static std::string GetAuthorizationUrl(const AuthData& data) {
      return "https://accounts.google.com/o/oauth2/auth?" +
             http::FormDataToString(
                 {{"response_type", "code"},
                  {"client_id", data.client_id},
                  {"redirect_uri", data.redirect_uri},
                  {"scope", "https://www.googleapis.com/auth/drive"},
                  {"access_type", "offline"},
                  {"prompt", "consent"},
                  {"state", data.state}});
    }

    template <http::HttpClient Http>
    static Task<AuthToken> ExchangeAuthorizationCode(
        const Http& http, AuthData auth_data, std::string code,
        stdx::stop_token stop_token) {
      auto request = http::Request<std::string>{
          .url = "https://accounts.google.com/o/oauth2/token",
          .method = http::Method::kPost,
          .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
          .body = http::FormDataToString(
              {{"grant_type", "authorization_code"},
               {"client_secret", auth_data.client_secret},
               {"client_id", auth_data.client_id},
               {"redirect_uri", auth_data.redirect_uri},
               {"code", std::move(code)}})};
      json json = co_await util::FetchJson(http, std::move(request),
                                           std::move(stop_token));
      co_return AuthToken{.access_token = json["access_token"],
                          .refresh_token = json["refresh_token"]};
    }
  };

  template <typename AuthManager>
  struct CloudProvider;

  struct GeneralData {
    std::string username;
    int64_t space_used;
    std::optional<int64_t> space_total;
  };

  struct ItemData {
    std::string id;
    std::string name;
    int64_t timestamp;
    std::vector<std::string> parents;
    std::string thumbnail_url;
  };

  struct Directory : ItemData {};

  struct File : ItemData {
    std::optional<std::string> mime_type;
    std::optional<int64_t> size;
  };

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct FileContent {
    Generator<std::string> data;
    std::optional<int64_t> size;
  };

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    std::string mime_type;
  };

  static constexpr std::string_view kId = "google";
  static inline constexpr auto& kIcon = util::kAssetsProvidersGooglePng;
};

template <typename AuthManager>
struct GoogleDrive::CloudProvider
    : coro::cloudstorage::CloudProvider<GoogleDrive,
                                        CloudProvider<AuthManager>> {
  using Request = http::Request<std::string>;
  explicit CloudProvider(AuthManager auth_manager)
      : auth_manager_(std::move(auth_manager)) {}

  Task<Directory> GetRoot(stdx::stop_token) {
    Directory d{{.id = "root"}};
    co_return d;
  }

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
    std::vector<std::pair<std::string, std::string>> params = {
        {"q", "'" + directory.id + "' in parents"},
        {"fields",
         "files(" + std::string(kFileProperties) + "),kind,nextPageToken"}};
    if (page_token) {
      params.emplace_back("pageToken", std::move(*page_token));
    }
    auto request = Request{.url = GetEndpoint("/files") + "?" +
                                  http::FormDataToString(params)};
    json data = co_await auth_manager_.FetchJson(std::move(request),
                                                 std::move(stop_token));
    std::vector<Item> result;
    for (const json& item : data["files"]) {
      result.emplace_back(std::move(ToItem(item)));
    }
    co_return PageData{
        .items = std::move(result),
        .next_page_token = data.contains("nextPageToken")
                               ? std::make_optional(data["nextPageToken"])
                               : std::nullopt};
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) {
    auto request =
        Request{.url = GetEndpoint("/about?fields=user,storageQuota")};
    json json = co_await auth_manager_.FetchJson(std::move(request),
                                                 std::move(stop_token));
    co_return GeneralData{
        .username = json["user"].contains("emailAddress")
                        ? json["user"]["emailAddress"]
                        : json["user"]["displayName"],
        .space_used = std::stoll(std::string(json["storageQuota"]["usage"])),
        .space_total = json["storageQuota"].contains("limit")
                           ? std::make_optional(std::stoll(
                                 std::string(json["storageQuota"]["limit"])))
                           : std::nullopt};
  }

  Task<Item> GetItem(std::string id, stdx::stop_token stop_token) {
    auto request =
        Request{.url = GetEndpoint("/files/" + std::move(id)) + "?" +
                       http::FormDataToString({{"fields", kFileProperties}})};
    json json = co_await auth_manager_.FetchJson(std::move(request),
                                                 std::move(stop_token));
    co_return ToItem(json);
  }

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) {
    auto request =
        Request{.url = GetEndpoint("/files/" + file.id) + "?alt=media",
                .headers = {ToRangeHeader(range)}};
    auto response =
        co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
    FOR_CO_AWAIT(std::string & body, response.body) {
      co_yield std::move(body);
    }
  }

  Task<Item> RenameItem(Item item, std::string new_name,
                        stdx::stop_token stop_token) {
    auto id = std::visit([](auto d) { return d.id; }, item);
    auto request =
        Request{.url = GetEndpoint("/files/" + std::move(id)) + "?" +
                       http::FormDataToString({{"fields", kFileProperties}}),
                .method = http::Method::kPatch,
                .headers = {{"Content-Type", "application/json"}}};
    json json;
    json["name"] = std::move(new_name);
    request.body = json.dump();
    auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                     std::move(stop_token));
    co_return ToItem(response);
  }

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token) {
    auto request =
        Request{.url = GetEndpoint("/files/") + "?" +
                       http::FormDataToString({{"fields", kFileProperties}}),
                .method = http::Method::kPost,
                .headers = {{"Content-Type", "application/json"}}};
    json json;
    json["mimeType"] = "application/vnd.google-apps.folder";
    json["name"] = std::move(name);
    json["parents"] = {std::move(parent.id)};
    request.body = json.dump();
    auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                     std::move(stop_token));
    co_return std::get<Directory>(ToItem(response));
  }

  Task<> RemoveItem(Item item, stdx::stop_token stop_token) {
    auto request =
        Request{.url = GetEndpoint("/files/") +
                       std::visit([](const auto& d) { return d.id; }, item),
                .method = http::Method::kDelete};
    co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
  }

  Task<Item> MoveItem(Item source, Directory destination,
                      stdx::stop_token stop_token) {
    auto id = std::visit([](const auto& d) { return d.id; }, source);
    auto remove_parents = std::visit(
        [](const auto& d) {
          std::string r;
          for (auto& parent : d.parents) {
            r += parent + ",";
          }
          r.pop_back();
          return r;
        },
        source);
    auto request =
        Request{.url = GetEndpoint("/files/" + std::move(id)) + "?" +
                       http::FormDataToString(
                           {{"fields", kFileProperties},
                            {"removeParents", std::move(remove_parents)},
                            {"addParents", destination.id}}),
                .method = http::Method::kPatch,
                .headers = {{"Content-Type", "application/json"}}};
    request.body = json::object().dump();
    auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                     std::move(stop_token));
    co_return ToItem(response);
  }

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token) {
    return CreateOrUpdateFile(std::move(parent), name, std::move(content),
                              std::move(stop_token));
  }

  Task<File> CreateOrUpdateFile(Directory parent, std::string_view name,
                                FileContent content,
                                stdx::stop_token stop_token) {
    auto request = Request{
        .url = GetEndpoint("/files") + "?" +
               http::FormDataToString(
                   {{"q", "'" + parent.id + "' in parents and name = '" +
                              std::string(name) + "'"},
                    {"fields", "files(id)"}})};
    auto response =
        co_await auth_manager_.FetchJson(std::move(request), stop_token);
    if (response["files"].size() == 0) {
      co_return co_await CreateFileImpl(
          std::move(parent), name, std::move(content), std::move(stop_token));
    } else if (response["files"].size() == 1) {
      co_return co_await UpdateFile(std::string(response["files"][0]["id"]),
                                    std::move(content), std::move(stop_token));
    } else {
      throw CloudException("ambiguous file reference");
    }
  }

  Task<File> UpdateFile(std::string_view id, FileContent content,
                        stdx::stop_token stop_token) {
    http::Request<> request{
        .url = "https://www.googleapis.com/upload/drive/v3/files/" +
               std::string(id) + "?" +
               http::FormDataToString(
                   {{"uploadType", "multipart"}, {"fields", kFileProperties}}),
        .method = http::Method::kPatch,
        .headers = {{"Accept", "application/json"},
                    {"Content-Type",
                     "multipart/related; boundary=" + std::string(kSeparator)},
                    {"Authorization",
                     "Bearer " + auth_manager_.GetAuthToken().access_token}},
        .body = GetUploadForm(json(), std::move(content))};
    auto response = co_await util::FetchJson(
        auth_manager_.GetHttp(), std::move(request), std::move(stop_token));
    co_return ToItemImpl<File>(response);
  }

  template <typename Item>
  Task<Thumbnail> GetItemThumbnail(Item item, http::Range range,
                                   stdx::stop_token stop_token) {
    Request request{.url = std::move(item.thumbnail_url),
                    .headers = {ToRangeHeader(range)}};
    auto response =
        co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
    Thumbnail result;
    result.mime_type =
        http::GetHeader(response.headers, "Content-Type").value();
    result.size =
        std::stoll(http::GetHeader(response.headers, "Content-Length").value());
    result.data = std::move(response.body);
    co_return result;
  }

 private:
  Task<File> CreateFileImpl(Directory parent, std::string_view name,
                            FileContent content, stdx::stop_token stop_token) {
    json metadata;
    metadata["name"] = std::move(name);
    metadata["parents"].push_back(parent.id);
    http::Request<> request{
        .url = "https://www.googleapis.com/upload/drive/v3/files?" +
               http::FormDataToString(
                   {{"uploadType", "multipart"}, {"fields", kFileProperties}}),
        .method = http::Method::kPost,
        .headers = {{"Accept", "application/json"},
                    {"Content-Type",
                     "multipart/related; boundary=" + std::string(kSeparator)},
                    {"Authorization",
                     "Bearer " + auth_manager_.GetAuthToken().access_token}},
        .body = GetUploadForm(std::move(metadata), std::move(content))};
    auto response = co_await util::FetchJson(
        auth_manager_.GetHttp(), std::move(request), std::move(stop_token));
    co_return ToItemImpl<File>(response);
  }

  Generator<std::string> GetUploadForm(json metadata, FileContent content) {
    co_yield "--";
    co_yield std::string(kSeparator);
    co_yield "\r\n";
    co_yield "Content-Type: application/json; charset=UTF-8\r\n\r\n";
    co_yield metadata.dump();
    co_yield "\r\n";
    co_yield "--";
    co_yield std::string(kSeparator);
    co_yield "\r\n";
    co_yield "Content-Type: application/octet-stream\r\n\r\n";

    FOR_CO_AWAIT(std::string & chunk, content.data) {
      co_yield std::move(chunk);
    }

    co_yield "\r\n--";
    co_yield std::string(kSeparator);
    co_yield "--\r\n";
  }

  static constexpr std::string_view kSeparator = "fWoDm9QNn3v3Bq3bScUX";

  static constexpr std::string_view kEndpoint =
      "https://www.googleapis.com/drive/v3";
  static constexpr std::string_view kFileProperties =
      "id,name,thumbnailLink,trashed,mimeType,iconLink,parents,size,"
      "modifiedTime";
  static constexpr int kThumbnailSize = 256;

  static std::string GetEndpoint(std::string_view path) {
    return std::string(kEndpoint) + std::string(path);
  }

  static std::string GetIconLink(std::string_view link) {
    const auto kDefaultSize = "16";
    auto it = link.find(kDefaultSize);
    if (it == std::string::npos) {
      return std::string(link);
    }
    return std::string(link.begin(), link.begin() + it) +
           std::to_string(kThumbnailSize) +
           std::string(link.begin() + it + strlen(kDefaultSize), link.end());
  }

  static Item ToItem(const json& json) {
    if (json["mimeType"] == "application/vnd.google-apps.folder") {
      return ToItemImpl<Directory>(json);
    } else {
      return ToItemImpl<File>(json);
    }
  }

  template <typename T>
  static T ToItemImpl(const json& json) {
    T result = {};
    result.id = json["id"];
    result.name = json["name"];
    result.timestamp = http::ParseTime(std::string(json["modifiedTime"]));
    if (json.contains("thumbnailLink")) {
      result.thumbnail_url = json["thumbnailLink"];
    } else {
      result.thumbnail_url = GetIconLink(std::string(json["iconLink"]));
    }
    for (std::string parents : json["parents"]) {
      result.parents.emplace_back(std::move(parents));
    }
    if constexpr (std::is_same_v<T, File>) {
      if (json.contains("size")) {
        result.size = std::stoll(std::string(json["size"]));
      }
      result.mime_type = json["mimeType"];
    }
    return result;
  }

  AuthManager auth_manager_;
};

namespace util {
template <>
inline GoogleDrive::Auth::AuthData GetAuthData<GoogleDrive>() {
  return {
      .client_id =
          R"(646432077068-hmvk44qgo6d0a64a5h9ieue34p3j2dcv.apps.googleusercontent.com)",
      .client_secret = "1f0FG5ch-kKOanTAv1Bqdp9U"};
}
}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_GOOGLE_DRIVE_H
