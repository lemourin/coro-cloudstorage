#ifndef CORO_CLOUDSTORAGE_ONE_DRIVE_H
#define CORO_CLOUDSTORAGE_ONE_DRIVE_H

#include <coro/cloudstorage/util/auth_data.h>

namespace coro::cloudstorage {

template <typename Auth>
struct OneDriveImpl;

struct OneDrive {
  using json = nlohmann::json;

  static constexpr std::string_view kId = "onedrive";

  struct Auth {
    struct AuthToken {
      std::string access_token;
      std::string refresh_token;
      std::string endpoint;
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
          .url = "https://login.microsoftonline.com/common/oauth2/v2.0/token",
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
      return "https://login.microsoftonline.com/common/oauth2/v2.0/authorize?" +
             http::FormDataToString(
                 {{"response_type", "code"},
                  {"client_id", data.client_id},
                  {"redirect_uri", data.redirect_uri},
                  {"scope", "offline_access user.read files.read"},
                  {"state", data.state}});
    }

    template <http::HttpClient Http>
    static Task<AuthToken> ExchangeAuthorizationCode(
        const Http& http, AuthData auth_data, std::string code,
        stdx::stop_token stop_token) {
      auto request = http::Request<std::string>{
          .url = "https://login.microsoftonline.com/common/oauth2/v2.0/token",
          .method = http::Method::kPost,
          .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
          .body = http::FormDataToString(
              {{"grant_type", "authorization_code"},
               {"client_secret", auth_data.client_secret},
               {"client_id", auth_data.client_id},
               {"redirect_uri", auth_data.redirect_uri},
               {"code", std::move(code)}})};
      json response =
          co_await util::FetchJson(http, std::move(request), stop_token);
      AuthToken auth_token = {.access_token = response["access_token"],
                              .refresh_token = response["refresh_token"]};
      auto user_data_request = http::Request<std::string>{
          .url = "https://graph.microsoft.com/v1.0/me",
          .headers = {{"Authorization", "Bearer " + auth_token.access_token}}};
      json user_data = co_await util::FetchJson(
          http, std::move(user_data_request), std::move(stop_token));
      auth_token.endpoint = user_data.contains("mySite")
                                ? user_data["mySite"]
                                : "https://graph.microsoft.com/v1.0";
      co_return auth_token;
    }
  };

  template <typename AuthManager>
  using Impl = OneDriveImpl<AuthManager>;

  struct GeneralData {
    std::string username;
  };

  struct Directory {
    std::string id;
    std::string name;
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
};

template <typename AuthManager>
struct OneDriveImpl : OneDrive {
  using Request = http::Request<std::string>;

  explicit OneDriveImpl(AuthManager auth_manager)
      : auth_manager_(std::move(auth_manager)) {}

  Task<Directory> GetRoot(stdx::stop_token) {
    Directory d;
    d.id = "root";
    co_return d;
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) {
    auto request = Request{.url = GetEndpoint("/me")};
    json json = co_await auth_manager_.FetchJson(std::move(request),
                                                 std::move(stop_token));
    co_return GeneralData{.username = json["userPrincipalName"]};
  }

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
    auto request = Request{
        .url = page_token.value_or(
            GetEndpoint("/drive/items/" + directory.id + "/children") + "?" +
            http::FormDataToString(
                {{"expand", "thumbnails"}, {"select", kFileProperties}}))};
    json data = co_await auth_manager_.FetchJson(std::move(request),
                                                 std::move(stop_token));
    std::vector<Item> result;
    for (const json& item : data["value"]) {
      result.emplace_back(ToItem(item));
    }
    co_return PageData{
        .items = std::move(result),
        .next_page_token = data.contains("@odata.nextLink")
                               ? std::make_optional(data["@odata.nextLink"])
                               : std::nullopt};
  }

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) {
    std::stringstream range_header;
    range_header << "bytes=" << range.start << "-";
    if (range.end) {
      range_header << *range.end;
    }
    auto request =
        Request{.url = GetEndpoint("/drive/items/" + file.id + "/content"),
                .headers = {{"Range", range_header.str()}}};
    auto response =
        co_await auth_manager_.Fetch(std::move(request), stop_token);
    if (response.status == 302) {
      auto redirect_request = Request{
          .url = coro::http::GetHeader(response.headers, "Location").value(),
          .headers = {{"Range", std::move(range_header).str()}}};
      response = co_await auth_manager_.Fetch(std::move(redirect_request),
                                              std::move(stop_token));
    }
    FOR_CO_AWAIT(std::string body, response.body, { co_yield body; });
  }

 private:
  static constexpr std::string_view kFileProperties =
      "name,folder,audio,image,photo,video,id,size,lastModifiedDateTime,"
      "thumbnails,@content.downloadUrl,mimeType";

  std::string GetEndpoint(std::string_view path) const {
    const std::string& endpoint = auth_manager_.GetAuthToken().endpoint;
    if (endpoint.empty()) {
      throw CloudException(CloudException::Type::kUnauthorized);
    }
    return endpoint + std::string(path);
  }

  static Item ToItem(const json& json) {
    if (json.contains("folder")) {
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
        result.size = json["size"];
      }
      if (json.contains("mimeType")) {
        result.mime_type = json["mimeType"];
      }
    }
    return result;
  }

  AuthManager auth_manager_;
};

namespace util {
template <>
inline OneDrive::Auth::AuthData GetAuthData<OneDrive>() {
  return {.client_id = "56a1d60f-ea71-40e9-a489-b87fba12a23e",
          .client_secret = "zJRAsd0o4E9c33q4OLc7OhY"};
}
}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_ONE_DRIVE_H
