#ifndef CORO_CLOUDSTORAGE_DROPBOX_H
#define CORO_CLOUDSTORAGE_DROPBOX_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/auth_data.h>
#include <coro/cloudstorage/util/fetch_json.h>
#include <coro/http/http.h>

#include <nlohmann/json.hpp>
#include <sstream>

namespace coro::cloudstorage {

struct Dropbox {
  struct GeneralData {
    std::string username;
    int64_t space_used;
    int64_t space_total;
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
    using json = nlohmann::json;

    struct AuthToken {
      std::string access_token;
    };

    struct AuthData {
      std::string client_id;
      std::string client_secret;
      std::string redirect_uri;
      std::string state;
    };

    static std::string GetAuthorizationUrl(const AuthData& data) {
      return "https://www.dropbox.com/oauth2/authorize?" +
             http::FormDataToString({{"response_type", "code"},
                                     {"client_id", data.client_id},
                                     {"redirect_uri", data.redirect_uri},
                                     {"state", data.state}});
    }

    template <http::HttpClient Http>
    static Task<AuthToken> ExchangeAuthorizationCode(
        const Http& http, AuthData auth_data, std::string code,
        stdx::stop_token stop_token) {
      auto request = http::Request<std::string>{
          .url = "https://api.dropboxapi.com/oauth2/token",
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
      co_return AuthToken{.access_token = json["access_token"]};
    }
  };

  template <http::HttpClient Http>
  class CloudProvider;

  static constexpr std::string_view kId = "dropbox";
};

template <http::HttpClient Http>
class Dropbox::CloudProvider : public Dropbox {
 public:
  using json = nlohmann::json;
  using Request = http::Request<std::string>;

  CloudProvider(const Http& http, Dropbox::Auth::AuthToken auth_token)
      : http_(&http), auth_token_(std::move(auth_token)) {}

  Task<Directory> GetRoot(stdx::stop_token) {
    Directory d{};
    co_return d;
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) {
    Task<json> task1 = util::FetchJson(
        *http_,
        Request{.url = GetEndpoint("/users/get_current_account"),
                .method = http::Method::kPost,
                .headers = {{"Content-Type", ""},
                            {"Authorization",
                             "Bearer " + auth_token_.access_token}}},
        stop_token);
    Task<json> task2 = util::FetchJson(
        *http_,
        Request{.url = GetEndpoint("/users/get_space_usage"),
                .method = http::Method::kPost,
                .headers = {{"Content-Type", ""},
                            {"Authorization",
                             "Bearer " + auth_token_.access_token}}},
        stop_token);
    auto [json1, json2] = co_await WhenAll(std::move(task1), std::move(task2));
    co_return GeneralData{.username = json1["email"],
                          .space_used = json2["used"],
                          .space_total = json2["allocation"]["allocated"]};
  }

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
    http::Request<std::string> request;
    if (page_token) {
      request = {.url = GetEndpoint("/files/list_folder/continue"),
                 .body = R"({"cursor":")" + *page_token + R"("})"};
    } else {
      request = {.url = GetEndpoint("/files/list_folder"),
                 .body = R"({"path":")" + directory.id + R"("})"};
    }
    auto response = co_await FetchJson(request, std::move(stop_token));

    PageData page_data;
    for (const json& entry : response["entries"]) {
      page_data.items.emplace_back(ToItem(entry));
    }
    if (response["has_more"]) {
      page_data.next_page_token = response["cursor"];
    }
    co_return page_data;
  }

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) {
    std::stringstream range_header;
    range_header << "bytes=" << range.start << "-";
    if (range.end) {
      range_header << *range.end;
    }
    auto request = Request{
        .url = "https://content.dropboxapi.com/2/files/download",
        .method = http::Method::kPost,
        .headers = {{"Range", std::move(range_header).str()},
                    {"Content-Type", ""},
                    {"Dropbox-API-arg", R"({"path":")" + file.id + R"("})"},
                    {"Authorization", "Bearer " + auth_token_.access_token}}};
    auto response =
        co_await http_->Fetch(std::move(request), std::move(stop_token));
    FOR_CO_AWAIT(std::string & body, response.body) {
      co_yield std::move(body);
    }
  }

 private:
  static constexpr std::string_view kEndpoint = "https://api.dropboxapi.com/2";

  static std::string GetEndpoint(std::string_view path) {
    return std::string(kEndpoint) + std::string(path);
  }

  auto FetchJson(http::Request<std::string> request,
                 stdx::stop_token stop_token) const {
    request.method = http::Method::kPost;
    request.headers.emplace_back("Content-Type", "application/json");
    request.headers.emplace_back("Authorization",
                                 "Bearer " + auth_token_.access_token);
    return util::FetchJson(*http_, std::move(request), std::move(stop_token));
  }

  static Item ToItem(const json& json) {
    if (json[".tag"] == "folder") {
      return ToItemImpl<Directory>(json);
    } else {
      return ToItemImpl<File>(json);
    }
  }

  template <typename T>
  static T ToItemImpl(const json& json) {
    T result = {};
    result.id = json["path_display"];
    result.name = json["name"];
    if constexpr (std::is_same_v<T, File>) {
      result.size = json.at("size");
      result.timestamp = http::ParseTime(std::string(json["client_modified"]));
    }
    return result;
  }

  const Http* http_;
  Dropbox::Auth::AuthToken auth_token_;
};

template <>
struct CreateCloudProvider<Dropbox> {
  template <typename CloudFactory, typename... Args>
  auto operator()(const CloudFactory& factory,
                  Dropbox::Auth::AuthToken auth_token, Args&&...) const {
    return Dropbox::CloudProvider(*factory.http_, std::move(auth_token));
  }
};

namespace util {
template <>
inline Dropbox::Auth::AuthData GetAuthData<Dropbox>() {
  return {
      .client_id = "ktryxp68ae5cicj",
      .client_secret = "6evu94gcxnmyr59",
  };
}
}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_DROPBOX_H
