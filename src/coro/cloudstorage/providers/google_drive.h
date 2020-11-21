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

  struct File {
    std::string id;
    std::string name;
  };

  struct Directory : File {};

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  explicit GoogleDrive(AuthData data) : data_(std::move(data)) {}

  template <http::HttpClient HttpClient>
  [[nodiscard]] Task<PageData> ListDirectoryPage(
      HttpClient& http, std::string_view access_token,
      const Directory& directory, std::optional<std::string_view> page_token,
      stdx::stop_token stop_token) const {
    std::vector<std::pair<std::string, std::string>> params = {
        {"fields",
         R"(files(id,name,thumbnailLink,trashed,mimeType,iconLink,parents,size,modifiedTime),kind,nextPageToken)"}};
    if (page_token) {
      params.emplace_back("pageToken", *page_token);
    }
    json data = co_await util::FetchJson(
        http,
        http::Request<>{
            .url = GetEndpoint("/files") + "?" + http::FormDataToString(params),
            .headers = {{"Authorization",
                         "Bearer " + std::string(access_token)}}},
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
  [[nodiscard]] Task<GeneralData> GetGeneralData(
      HttpClient& http, std::string_view access_token,
      stdx::stop_token stop_token) const {
    json json = co_await util::FetchJson(
        http,
        http::Request<>{.url = GetEndpoint("/about?fields=user,storageQuota"),
                        .headers = {{"Authorization",
                                     "Bearer " + std::string(access_token)}}},
        std::move(stop_token));
    co_return GeneralData{.username = json["user"]["emailAddress"]};
  }

  template <http::HttpClient HttpClient>
  Task<Token> ExchangeAuthorizationCode(HttpClient& http, std::string_view code,
                                        stdx::stop_token stop_token) const {
    json json = co_await util::FetchJson(
        http,
        http::Request<std::string>{
            .url = "https://accounts.google.com/o/oauth2/token",
            .method = "POST",
            .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
            .body =
                http::FormDataToString({{"grant_type", "authorization_code"},
                                        {"client_secret", data_.client_secret},
                                        {"client_id", data_.client_id},
                                        {"redirect_uri", data_.redirect_uri},
                                        {"code", code}})},
        std::move(stop_token));
    co_return Token{.access_token = json["access_token"],
                    .refresh_token = json["refresh_token"]};
  }

  template <http::HttpClient HttpClient>
  Task<Token> RefreshAccessToken(HttpClient& http,
                                 std::string_view refresh_token,
                                 stdx::stop_token stop_token) const {
    json json = co_await util::FetchJson(
        http,
        http::Request<std::string>{
            .url = "https://accounts.google.com/o/oauth2/token",
            .method = "POST",
            .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
            .body =
                http::FormDataToString({{"refresh_token", refresh_token},
                                        {"client_id", data_.client_id},
                                        {"client_secret", data_.client_secret},
                                        {"grant_type", "refresh_token"}})},
        std::move(stop_token));

    co_return Token{.access_token = json["access_token"],
                    .refresh_token = std::string(refresh_token)};
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
    T result;
    result.id = json["id"];
    result.name = json["name"];
    return result;
  }

  AuthData data_;
};
}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_GOOGLE_DRIVE_H
