#ifndef CORO_CLOUDSTORAGE_DROPBOX_H
#define CORO_CLOUDSTORAGE_DROPBOX_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/assets.h>
#include <coro/cloudstorage/util/auth_data.h>
#include <coro/cloudstorage/util/fetch_json.h>
#include <coro/http/http.h>
#include <coro/when_all.h>

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

  struct UploadSession {
    std::string id;
    std::string path;
  };

  struct FileContent {
    Generator<std::string> data;
    std::optional<int64_t> size;
  };

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    static inline constexpr std::string_view mime_type = "image/jpeg";
  };

  template <http::HttpClient Http>
  class CloudProvider;

  static constexpr std::string_view kId = "dropbox";
  static inline const auto kIcon = util::kAssetsProvidersDropboxPng;
};

template <http::HttpClient Http>
class Dropbox::CloudProvider
    : public coro::cloudstorage::CloudProvider<Dropbox, CloudProvider<Http>> {
 public:
  using json = nlohmann::json;
  using Request = http::Request<std::string>;

  CloudProvider(const Http& http, Dropbox::Auth::AuthToken auth_token)
      : http_(&http), auth_token_(std::move(auth_token)) {}

  Task<Directory> GetRoot(stdx::stop_token) {
    Directory d{{.id = ""}};
    co_return d;
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) {
    Task<json> task1 = util::FetchJson(
        *http_,
        Request{.url = GetEndpoint("/users/get_current_account"),
                .method = http::Method::kPost,
                .headers = {{"Content-Type", ""},
                            {"Authorization",
                             "Bearer " + auth_token_.access_token}},
                .flags = Request::kRead},
        stop_token);
    Task<json> task2 = util::FetchJson(
        *http_,
        Request{.url = GetEndpoint("/users/get_space_usage"),
                .method = http::Method::kPost,
                .headers = {{"Content-Type", ""},
                            {"Authorization",
                             "Bearer " + auth_token_.access_token}},
                .flags = Request::kRead},
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
      json body;
      body["cursor"] = *page_token;
      request = {.url = GetEndpoint("/files/list_folder/continue"),
                 .body = body.dump(),
                 .flags = Request::kRead};
    } else {
      json body;
      body["path"] = std::move(directory.id);
      request = {.url = GetEndpoint("/files/list_folder"),
                 .body = body.dump(),
                 .flags = Request::kRead};
    }
    auto response =
        co_await FetchJson(std::move(request), std::move(stop_token));

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
    json json;
    json["path"] = file.id;
    auto request = Request{
        .url = "https://content.dropboxapi.com/2/files/download",
        .method = http::Method::kPost,
        .headers = {{"Range", std::move(range_header).str()},
                    {"Content-Type", ""},
                    {"Dropbox-API-arg", json.dump()},
                    {"Authorization", "Bearer " + auth_token_.access_token}},
        .flags = Request::kRead};
    auto response =
        co_await http_->Fetch(std::move(request), std::move(stop_token));
    FOR_CO_AWAIT(std::string & body, response.body) {
      co_yield std::move(body);
    }
  }

  Task<Item> RenameItem(Item item, std::string new_name,
                        stdx::stop_token stop_token) {
    auto id = std::visit([](const auto& d) { return d.id; }, item);
    auto request = Request{.url = GetEndpoint("/files/move_v2"),
                           .method = http::Method::kPost};
    json json;
    json["from_path"] = id;
    json["to_path"] = GetDirectoryPath(id) + "/" + new_name;
    request.body = json.dump();
    auto response =
        co_await FetchJson(std::move(request), std::move(stop_token));
    co_return ToItem(response["metadata"]);
  }

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token) {
    auto request = Request{.url = GetEndpoint("/files/create_folder_v2"),
                           .method = http::Method::kPost};
    json json;
    json["path"] = parent.id + "/" + std::move(name);
    request.body = json.dump();
    auto response =
        co_await FetchJson(std::move(request), std::move(stop_token));
    co_return ToItemImpl<Directory>(response["metadata"]);
  }

  Task<> RemoveItem(Item item, stdx::stop_token stop_token) {
    auto request = Request{.url = GetEndpoint("/files/delete"),
                           .method = http::Method::kPost};
    json json;
    json["path"] = std::visit([](const auto& d) { return d.id; }, item);
    request.body = json.dump();
    co_await FetchJson(std::move(request), std::move(stop_token));
  }

  Task<Item> MoveItem(Item source, Directory destination,
                      stdx::stop_token stop_token) {
    auto request = Request{.url = GetEndpoint("/files/move_v2"),
                           .method = http::Method::kPost};
    json json;
    json["from_path"] = std::visit([](const auto& d) { return d.id; }, source);
    json["to_path"] = destination.id + "/" +
                      std::visit([](const auto& d) { return d.name; }, source);
    request.body = json.dump();
    auto response =
        co_await FetchJson(std::move(request), std::move(stop_token));
    co_return ToItem(response["metadata"]);
  }

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token) {
    if (content.size < 150 * 1024 * 1024) {
      json json;
      json["path"] = parent.id + "/" + std::string(name);
      json["mode"] = "overwrite";
      auto request = http::Request<>{
          .url = "https://content.dropboxapi.com/2/files/upload",
          .method = http::Method::kPost,
          .headers = {{"Dropbox-API-Arg", json.dump()},
                      {"Authorization", "Bearer " + auth_token_.access_token},
                      {"Content-Type", "application/octet-stream"}},
          .body = std::move(content.data)};
      auto response = co_await util::FetchJson(*http_, std::move(request),
                                               std::move(stop_token));
      co_return ToItemImpl<File>(response);
    } else {
      int64_t offset = 0;
      std::optional<UploadSession> session;
      auto it = co_await content.data.begin();
      while (true) {
        auto chunk_size = std::min<size_t>(
            150 * 1024 * 1024,
            static_cast<size_t>(
                content.size.value_or((std::numeric_limits<size_t>::max)()) -
                offset));
        FileContent chunk{.data = util::Take(it, chunk_size),
                          .size = chunk_size};
        if (!session) {
          session = co_await CreateUploadSession(std::move(parent), name,
                                                 std::move(chunk), stop_token);
        } else if (offset + static_cast<int64_t>(chunk_size) < content.size) {
          session = co_await WriteChunk(std::move(*session), std::move(chunk),
                                        offset, stop_token);
        } else {
          co_return co_await FinishUploadSession(std::move(*session),
                                                 std::move(chunk), offset,
                                                 std::move(stop_token));
        }
        offset += chunk_size;
      }
    }
  }

  Task<Thumbnail> GetItemThumbnail(File file, http::Range range,
                                   stdx::stop_token stop_token) {
    auto is_supported = [](std::string_view extension) {
      for (std::string_view e :
           {"jpg", "jpeg", "png", "tiff", "tif", "gif", "bmp", "mkv", "mp4"}) {
        if (e == extension) {
          return true;
        }
      }
      return true;
    };
    if (!is_supported(http::GetExtension(file.name))) {
      throw CloudException(CloudException::Type::kNotFound);
    }
    json json;
    json["resource"][".tag"] = "path";
    json["resource"]["path"] = file.id;
    auto request = Request{
        .url = "https://content.dropboxapi.com/2/files/get_thumbnail_v2",
        .method = http::Method::kPost,
        .headers = {{"Authorization", "Bearer " + auth_token_.access_token},
                    {"Dropbox-API-Arg", json.dump()},
                    ToRangeHeader(range)}};
    auto response =
        co_await http_->Fetch(std::move(request), std::move(stop_token));
    if (response.status / 100 != 2) {
      throw http::HttpException(
          response.status, co_await http::GetBody(std::move(response.body)));
    }
    Thumbnail result;
    result.size =
        std::stoll(http::GetHeader(response.headers, "Content-Length").value());
    result.data = std::move(response.body);
    co_return result;
  }

 private:
  static constexpr std::string_view kEndpoint = "https://api.dropboxapi.com/2";

  Task<UploadSession> CreateUploadSession(Directory parent,
                                          std::string_view name,
                                          FileContent content,
                                          stdx::stop_token stop_token) {
    http::Request<> request{
        .url = "https://content.dropboxapi.com/2/files/upload_session/start",
        .method = http::Method::kPost,
        .headers = {{"Authorization", "Bearer " + auth_token_.access_token},
                    {"Content-Type", "application/octet-stream"},
                    {"Dropbox-API-Arg", "{}"}},
        .body = std::move(content.data)};
    auto response = co_await util::FetchJson(*http_, std::move(request),
                                             std::move(stop_token));
    co_return UploadSession{.id = response["session_id"],
                            .path = parent.id + "/" + std::string(name)};
  }

  Task<UploadSession> WriteChunk(UploadSession session, FileContent content,
                                 int64_t offset, stdx::stop_token stop_token) {
    json json;
    json["cursor"]["session_id"] = std::move(session.id);
    json["cursor"]["offset"] = offset;
    http::Request<> request = {
        .url =
            "https://content.dropboxapi.com/2/files/upload_session/append_v2",
        .method = http::Method::kPost,
        .headers = {{"Authorization", "Bearer " + auth_token_.access_token},
                    {"Content-Type", "application/octet-stream"},
                    {"Dropbox-API-Arg", json.dump()}},
        .body = std::move(content.data)};
    auto response = co_await http_->Fetch(std::move(request), stop_token);
    if (response.status / 100 != 2) {
      throw coro::http::HttpException(
          response.status, co_await http::GetBody(std::move(response.body)));
    }
    co_return std::move(session);
  }

  Task<File> FinishUploadSession(UploadSession session, FileContent content,
                                 int64_t offset, stdx::stop_token stop_token) {
    json json;
    json["cursor"]["session_id"] = std::move(session.id);
    json["cursor"]["offset"] = offset;
    json["commit"]["path"] = std::move(session.path);
    json["commit"]["mode"] = "overwrite";
    http::Request<> request{
        .url = "https://content.dropboxapi.com/2/files/upload_session/finish",
        .method = http::Method::kPost,
        .headers = {{"Authorization", "Bearer " + auth_token_.access_token},
                    {"Content-Type", "application/octet-stream"},
                    {"Dropbox-API-Arg", json.dump()}},
        .body = std::move(content.data)};
    auto response =
        co_await util::FetchJson(*http_, std::move(request), stop_token);
    co_return ToItemImpl<File>(response);
  }

  static std::string GetDirectoryPath(std::string_view path) {
    auto it = path.find_last_of('/');
    if (it == std::string::npos) {
      throw CloudException("invalid path");
    } else {
      return std::string(path.begin(), path.begin() + it);
    }
  }

  static std::string GetEndpoint(std::string_view path) {
    return std::string(kEndpoint) + std::string(path);
  }

  auto FetchJson(Request request, stdx::stop_token stop_token) const {
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
