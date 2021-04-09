#ifndef CORO_CLOUDSTORAGE_BOX_H
#define CORO_CLOUDSTORAGE_BOX_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/auth_data.h>
#include <coro/cloudstorage/util/fetch_json.h>
#include <coro/http/http.h>
#include <coro/when_all.h>

#include <nlohmann/json.hpp>
#include <sstream>

namespace coro::cloudstorage {

struct Box {
  struct GeneralData {
    std::string username;
    int64_t space_used;
    int64_t space_total;
  };

  struct ItemData {
    std::string id;
    std::string name;
    int64_t size;
    int64_t timestamp;
  };

  struct Directory : ItemData {};

  struct File : ItemData {};

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct Auth {
    using json = nlohmann::json;

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

    static std::string GetAuthorizationUrl(const AuthData& data) {
      return "https://account.box.com/api/oauth2/authorize?" +
             http::FormDataToString({{"response_type", "code"},
                                     {"client_id", data.client_id},
                                     {"client_secret", data.client_secret},
                                     {"redirect_uri", data.redirect_uri},
                                     {"state", data.state}});
    }

    template <http::HttpClient Http>
    static Task<AuthToken> ExchangeAuthorizationCode(
        const Http& http, AuthData auth_data, std::string code,
        stdx::stop_token stop_token) {
      auto request = http::Request<std::string>{
          .url = "https://api.box.com/oauth2/token",
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

    template <http::HttpClient Http>
    static Task<AuthToken> RefreshAccessToken(const Http& http,
                                              AuthData auth_data,
                                              AuthToken auth_token,
                                              stdx::stop_token stop_token) {
      auto request = http::Request<std::string>{
          .url = "https://api.box.com/oauth2/token",
          .method = http::Method::kPost,
          .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
          .body = http::FormDataToString(
              {{"refresh_token", auth_token.refresh_token},
               {"client_id", auth_data.client_id},
               {"client_secret", auth_data.client_secret},
               {"grant_type", "refresh_token"}})};
      json json = co_await util::FetchJson(http, std::move(request),
                                           std::move(stop_token));
      co_return AuthToken{.access_token = json["access_token"],
                          .refresh_token = json["refresh_token"]};
    }
  };

  struct FileContent {
    Generator<std::string> data;
    std::optional<int64_t> size;
  };

  template <typename AuthManager>
  class CloudProvider;

  static constexpr std::string_view kId = "box";
};

template <typename AuthManager>
class Box::CloudProvider
    : public coro::cloudstorage::CloudProvider<Box,
                                               CloudProvider<AuthManager>> {
 public:
  using json = nlohmann::json;
  using Request = http::Request<std::string>;

  explicit CloudProvider(AuthManager auth_manager)
      : auth_manager_(std::move(auth_manager)) {}

  Task<Directory> GetRoot(stdx::stop_token) {
    Directory root{{.id = "0"}};
    co_return root;
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) {
    auto json = co_await auth_manager_.FetchJson(
        Request{.url = GetEndpoint("/users/me")}, std::move(stop_token));
    co_return GeneralData{.username = json["login"],
                          .space_used = json["space_used"],
                          .space_total = json["space_amount"]};
  }

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
    std::vector<std::pair<std::string, std::string>> params = {
        {"fields", std::string(kFileProperties)}};
    if (page_token) {
      params.emplace_back("offset", std::move(*page_token));
    }
    Request request{.url = GetEndpoint("/folders/") + directory.id + "/items?" +
                           http::FormDataToString(std::move(params))};
    auto json = co_await auth_manager_.FetchJson(std::move(request),
                                                 std::move(stop_token));
    PageData result;
    for (const auto& entry : json["entries"]) {
      result.items.emplace_back(ToItem(entry));
    }
    int64_t offset = json["offset"];
    int64_t limit = json["limit"];
    int64_t total_count = json["total_count"];
    if (offset + limit < total_count) {
      result.next_page_token = std::to_string(offset + limit);
    }
    co_return std::move(result);
  }

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) {
    std::stringstream range_header;
    range_header << "bytes=" << range.start << "-";
    if (range.end) {
      range_header << *range.end;
    }
    Request request{.url = GetEndpoint("/files/" + file.id + "/content"),
                    .headers = {{"Range", range_header.str()}}};
    auto response =
        co_await auth_manager_.Fetch(std::move(request), stop_token);
    if (response.status / 100 == 3) {
      request = {.url = http::GetHeader(response.headers, "Location").value(),
                 .headers = {{"Range", range_header.str()}}};
      response = co_await auth_manager_.GetHttp().Fetch(std::move(request),
                                                        std::move(stop_token));
    }
    FOR_CO_AWAIT(auto& chunk, response.body) { co_yield std::move(chunk); }
  }

  Task<Directory> RenameItem(Directory item, std::string new_name,
                             stdx::stop_token stop_token) {
    return RenameItemImpl<Directory>("/folders/", std::move(item),
                                     std::move(new_name),
                                     std::move(stop_token));
  }

  Task<File> RenameItem(File item, std::string new_name,
                        stdx::stop_token stop_token) {
    return RenameItemImpl<File>("/files/", std::move(item), std::move(new_name),
                                std::move(stop_token));
  }

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token) {
    json request;
    request["name"] = std::move(name);
    request["parent"]["id"] = std::move(parent.id);
    auto response =
        co_await auth_manager_.FetchJson(Request{.url = GetEndpoint("/folders"),
                                                 .method = http::Method::kPost,
                                                 .body = request.dump()},
                                         std::move(stop_token));
    co_return ToItemImpl<Directory>(response);
  }

  Task<> RemoveItem(File item, stdx::stop_token stop_token) {
    Request request = {.url = GetEndpoint("/files/" + item.id),
                       .method = http::Method::kDelete};
    co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
  }

  Task<> RemoveItem(Directory item, stdx::stop_token stop_token) {
    Request request{.url = GetEndpoint("/folders/" + item.id) + "?" +
                           http::FormDataToString({{"recursive", "true"}}),
                    .method = http::Method::kDelete};
    co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
  }

  Task<Directory> MoveItem(Directory source, Directory destination,
                           stdx::stop_token stop_token) {
    return MoveItemImpl<Directory>("/folders/", std::move(source),
                                   std::move(destination),
                                   std::move(stop_token));
  }

  Task<File> MoveItem(File source, Directory destination,
                      stdx::stop_token stop_token) {
    return MoveItemImpl<File>("/files/", std::move(source),
                              std::move(destination), std::move(stop_token));
  }

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token) {
    std::optional<std::string> id;
    FOR_CO_AWAIT(const PageData& page,
                 this->ListDirectory(parent, stop_token)) {
      for (const auto& item : page.items) {
        if (std::visit([](const auto& d) { return d.name; }, item) == name) {
          id = std::visit([](const auto& d) { return d.id; }, item);
          break;
        }
      }
      if (id) {
        break;
      }
    }
    http::Request<> request{
        .url = id ? "https://upload.box.com/api/2.0/files/" + *id + "/content"
                  : "https://upload.box.com/api/2.0/files/content",
        .method = http::Method::kPost,
        .headers = {{"Accept", "application/json"},
                    {"Content-Type", "multipart/form-data; boundary=" +
                                         std::string(kSeparator)},
                    {"Authorization",
                     "Bearer " + auth_manager_.GetAuthToken().access_token}},
        .body = GetUploadStream(std::move(parent), name, std::move(content))};
    auto response = co_await util::FetchJson(
        auth_manager_.GetHttp(), std::move(request), std::move(stop_token));
    co_return ToItemImpl<File>(response["entries"][0]);
  }

 private:
  static constexpr std::string_view kSeparator = "Thnlg1ecwyUJHyhYYGrQ";
  static constexpr std::string_view kFileProperties =
      "name,id,size,modified_at";
  static constexpr std::string_view kEndpoint = "https://api.box.com/2.0";

  static std::string GetEndpoint(std::string_view path) {
    return std::string(kEndpoint) + std::string(path);
  }

  static Item ToItem(const json& json) {
    if (json["type"] == "folder") {
      return ToItemImpl<Directory>(json);
    } else {
      return ToItemImpl<File>(json);
    }
  }

  template <typename Item>
  static Item ToItemImpl(const json& json) {
    Item item;
    item.id = json["id"];
    item.size = json["size"];
    item.name = json["name"];
    item.timestamp = http::ParseTime(std::string(json["modified_at"]));
    return item;
  }

  static Generator<std::string> GetUploadStream(Directory parent,
                                                std::string_view name,
                                                FileContent content) {
    json request;
    request["name"] = name;
    request["parent"]["id"] = std::move(parent.id);
    std::stringstream chunk;
    chunk << "--" << kSeparator << "\r\n"
          << "Content-Disposition: form-data; name=\"attributes\""
          << "\r\n\r\n"
          << request.dump() << "\r\n"
          << "--" << kSeparator << "\r\n"
          << "Content-Disposition: form-data; name=\"file\"; filename=\""
          << http::EncodeUri(name) << "\"\r\n"
          << "Content-Type: application/octet-stream\r\n\r\n";
    co_yield chunk.str();
    FOR_CO_AWAIT(std::string & chunk, content.data) {
      co_yield std::move(chunk);
    }
    co_yield "\r\n--" + std::string(kSeparator) + "--";
  }

  template <typename T>
  Task<T> RenameItemImpl(std::string endpoint, T item, std::string new_name,
                         stdx::stop_token stop_token) {
    json request;
    request["name"] = std::move(new_name);
    auto response = co_await auth_manager_.FetchJson(
        Request{.url = GetEndpoint(endpoint + item.id),
                .method = http::Method::kPut,
                .body = request.dump()},
        std::move(stop_token));
    co_return ToItemImpl<T>(response);
  }

  template <typename T>
  Task<T> MoveItemImpl(std::string endpoint, T source, Directory destination,
                       stdx::stop_token stop_token) {
    json request;
    request["parent"]["id"] = std::move(destination.id);
    auto response = co_await auth_manager_.FetchJson(
        Request{.url = GetEndpoint(endpoint + source.id),
                .method = http::Method::kPut,
                .body = request.dump()},
        std::move(stop_token));
    co_return ToItemImpl<T>(response);
  }

  AuthManager auth_manager_;
};

namespace util {
template <>
inline Box::Auth::AuthData GetAuthData<Box>() {
  return {
      .client_id = "zmiv9tv13hunxhyjk16zqv8dmdw0d773",
      .client_secret = "IZ0T8WsUpJin7Qt3rHMf7qDAIFAkYZ0R",
  };
}
}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_BOX_H
