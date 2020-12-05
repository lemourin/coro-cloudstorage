#ifndef CORO_CLOUDSTORAGE_GOOGLE_DRIVE_H
#define CORO_CLOUDSTORAGE_GOOGLE_DRIVE_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/fetch_json.h>
#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/stdx/coroutine.h>
#include <coro/stdx/stop_token.h>
#include <coro/task.h>
#include <coro/wait_task.h>

#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace coro::cloudstorage {

class GoogleDrive {
 public:
  using json = nlohmann::json;

  struct GeneralData {
    std::string username;
  };

  struct Token {
    std::string access_token;
    std::string refresh_token;
  };

  struct AuthData {
    std::string client_id;
    std::string client_secret;
    std::string redirect_uri;
    std::string state;
  };

  struct Directory {
    std::string id;
    std::string name;
  };

  struct File : Directory {
    std::optional<int64_t> size;
  };

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  explicit GoogleDrive(AuthData data) : data_(std::move(data)) {}

  Task<Directory> GetRoot() const { co_return Directory{"root"}; }

  template <http::HttpClient HttpClient>
  Task<PageData> ListDirectoryPage(HttpClient& http, std::string access_token,
                                   Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) const {
    std::vector<std::pair<std::string, std::string>> params = {
        {"q", "'" + directory.id + "' in parents"},
        {"fields",
         "files(" + std::string(kFileProperties) + "),kind,nextPageToken"}};
    if (page_token) {
      params.emplace_back("pageToken", std::move(*page_token));
    }
    auto request = http::Request<>{
        .url = GetEndpoint("/files") + "?" + http::FormDataToString(params),
        .headers = {{"Authorization", "Bearer " + std::move(access_token)}}};
    json data = co_await util::FetchJson(http, std::move(request),
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

  template <http::HttpClient HttpClient>
  Task<GeneralData> GetGeneralData(HttpClient& http, std::string access_token,
                                   stdx::stop_token stop_token) const {
    auto request = http::Request<>{
        .url = GetEndpoint("/about?fields=user,storageQuota"),
        .headers = {{"Authorization", "Bearer " + std::move(access_token)}}};
    json json = co_await util::FetchJson(http, std::move(request),
                                         std::move(stop_token));
    co_return GeneralData{.username = json["user"]["emailAddress"]};
  }

  template <http::HttpClient HttpClient>
  Task<Item> GetItem(HttpClient& http, std::string access_token, std::string id,
                     stdx::stop_token stop_token) const {
    auto request = http::Request<>{
        .url = GetEndpoint("/files/" + std::move(id)) + "?" +
               http::FormDataToString({{"fields", kFileProperties}}),
        .headers = {{"Authorization", "Bearer " + std::move(access_token)}}};
    json json = co_await util::FetchJson(http, std::move(request),
                                         std::move(stop_token));
    co_return ToItem(json);
  }

  template <http::HttpClient HttpClient>
  Generator<std::string> GetFileContent(HttpClient& http,
                                        std::string access_token, File file,
                                        http::Range range,
                                        stdx::stop_token stop_token) const {
    std::stringstream range_header;
    range_header << "bytes=" << range.start << "-";
    if (range.end) {
      range_header << *range.end;
    }
    auto request = http::Request<>{
        .url = GetEndpoint("/files/" + file.id) + "?alt=media",
        .headers = {{"Authorization", "Bearer " + std::move(access_token)},
                    {"Range", std::move(range_header).str()}}};
    auto response =
        co_await http.Fetch(std::move(request), std::move(stop_token));
    FOR_CO_AWAIT(std::string body, response.body, { co_yield body; });
  }

  template <http::HttpClient HttpClient>
  Task<Token> ExchangeAuthorizationCode(HttpClient& http, std::string code,
                                        stdx::stop_token stop_token) const {
    auto request = http::Request<std::string>{
        .url = "https://accounts.google.com/o/oauth2/token",
        .method = "POST",
        .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
        .body = http::FormDataToString({{"grant_type", "authorization_code"},
                                        {"client_secret", data_.client_secret},
                                        {"client_id", data_.client_id},
                                        {"redirect_uri", data_.redirect_uri},
                                        {"code", std::move(code)}})};
    json json = co_await util::FetchJson(http, std::move(request),
                                         std::move(stop_token));
    co_return Token{.access_token = json["access_token"],
                    .refresh_token = json["refresh_token"]};
  }

  template <http::HttpClient HttpClient>
  Task<Token> RefreshAccessToken(HttpClient& http, std::string refresh_token,
                                 stdx::stop_token stop_token) const {
    auto request = http::Request<std::string>{
        .url = "https://accounts.google.com/o/oauth2/token",
        .method = "POST",
        .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
        .body = http::FormDataToString({{"refresh_token", refresh_token},
                                        {"client_id", data_.client_id},
                                        {"client_secret", data_.client_secret},
                                        {"grant_type", "refresh_token"}})};
    json json = co_await util::FetchJson(http, std::move(request),
                                         std::move(stop_token));

    co_return Token{.access_token = json["access_token"],
                    .refresh_token = std::move(refresh_token)};
  }

  [[nodiscard]] static std::string GetAuthorizationUrl(const AuthData& data) {
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

 private:
  static constexpr std::string_view kEndpoint =
      "https://www.googleapis.com/drive/v3";
  static constexpr std::string_view kFileProperties =
      "id,name,thumbnailLink,trashed,mimeType,iconLink,parents,size,"
      "modifiedTime";

  static std::string GetEndpoint(std::string_view path) {
    return std::string(kEndpoint) + std::string(path);
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
    if constexpr (std::is_same_v<T, File>) {
      if (json.contains("size")) {
        result.size = std::stoll(std::string(json["size"]));
      }
    }
    return result;
  }

  AuthData data_;
};
}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_GOOGLE_DRIVE_H
