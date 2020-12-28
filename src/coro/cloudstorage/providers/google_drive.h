#ifndef CORO_CLOUDSTORAGE_GOOGLE_DRIVE_H
#define CORO_CLOUDSTORAGE_GOOGLE_DRIVE_H

#include <coro/cloudstorage/cloud_provider.h>
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

template <typename Auth>
struct GoogleDriveImpl;

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
  using Impl = GoogleDriveImpl<AuthManager>;

  struct GeneralData {
    std::string username;
    int64_t space_used;
    std::optional<int64_t> space_total;
  };

  struct Directory {
    std::string id;
    std::string name;
    int64_t timestamp;
  };

  struct File : Directory {
    std::optional<std::string> mime_type;
    std::optional<int64_t> size;
  };

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  static constexpr std::string_view kId = "google";
};

template <typename AuthManager>
struct GoogleDriveImpl : GoogleDrive {
  using Request = http::Request<std::string>;

  explicit GoogleDriveImpl(AuthManager auth_manager)
      : auth_manager_(std::move(auth_manager)) {}

  Task<Directory> GetRoot(stdx::stop_token) {
    Directory d;
    d.id = "root";
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
    std::stringstream range_header;
    range_header << "bytes=" << range.start << "-";
    if (range.end) {
      range_header << *range.end;
    }
    auto request =
        Request{.url = GetEndpoint("/files/" + file.id) + "?alt=media",
                .headers = {{"Range", std::move(range_header).str()}}};
    auto response =
        co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
    FOR_CO_AWAIT(std::string & body, response.body) {
      co_yield std::move(body);
    }
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
    result.timestamp = http::ParseTime(std::string(json["modifiedTime"]));
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
