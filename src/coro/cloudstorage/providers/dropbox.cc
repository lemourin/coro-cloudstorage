#include "coro/cloudstorage/providers/dropbox.h"

#include <nlohmann/json.hpp>

#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"
#include "coro/cloudstorage/util/generator_utils.h"

namespace coro::cloudstorage {

namespace {

constexpr std::string_view kEndpoint = "https://api.dropboxapi.com/2";

template <typename T>
T ToItemImpl(const nlohmann::json& json) {
  T result = {};
  result.id = json["path_display"];
  result.name = json["name"];
  if constexpr (std::is_same_v<T, Dropbox::File>) {
    result.size = json.at("size");
    result.timestamp = http::ParseTime(std::string(json["client_modified"]));
  }
  return result;
}

Dropbox::Item ToItem(const nlohmann::json& json) {
  if (json[".tag"] == "folder") {
    return ToItemImpl<Dropbox::Directory>(json);
  } else {
    return ToItemImpl<Dropbox::File>(json);
  }
}

Task<Dropbox::UploadSession> CreateUploadSession(const coro::http::Http& http,
                                                 std::string access_token,
                                                 Dropbox::Directory parent,
                                                 std::string_view name,
                                                 Dropbox::FileContent content,
                                                 stdx::stop_token stop_token) {
  http::Request<> request{
      .url = "https://content.dropboxapi.com/2/files/upload_session/start",
      .method = http::Method::kPost,
      .headers = {{"Authorization", "Bearer " + access_token},
                  {"Content-Type", "application/octet-stream"},
                  {"Dropbox-API-Arg", "{}"}},
      .body = std::move(content.data)};
  auto response =
      co_await util::FetchJson(http, std::move(request), std::move(stop_token));
  co_return Dropbox::UploadSession{.id = response["session_id"],
                                   .path = parent.id + "/" + std::string(name)};
}

Task<Dropbox::UploadSession> WriteChunk(const coro::http::Http& http,
                                        std::string access_token,
                                        Dropbox::UploadSession session,
                                        Dropbox::FileContent content,
                                        int64_t offset,
                                        stdx::stop_token stop_token) {
  nlohmann::json json;
  json["cursor"]["session_id"] = std::move(session.id);
  json["cursor"]["offset"] = offset;
  http::Request<> request = {
      .url = "https://content.dropboxapi.com/2/files/upload_session/append_v2",
      .method = http::Method::kPost,
      .headers = {{"Authorization", "Bearer " + access_token},
                  {"Content-Type", "application/octet-stream"},
                  {"Dropbox-API-Arg", json.dump()}},
      .body = std::move(content.data)};
  co_await http.FetchOk(std::move(request), stop_token);
  co_return std::move(session);
}

Task<Dropbox::File> FinishUploadSession(const coro::http::Http& http,
                                        std::string access_token,
                                        Dropbox::UploadSession session,
                                        Dropbox::FileContent content,
                                        int64_t offset,
                                        stdx::stop_token stop_token) {
  nlohmann::json json;
  json["cursor"]["session_id"] = std::move(session.id);
  json["cursor"]["offset"] = offset;
  json["commit"]["path"] = std::move(session.path);
  json["commit"]["mode"] = "overwrite";
  http::Request<> request{
      .url = "https://content.dropboxapi.com/2/files/upload_session/finish",
      .method = http::Method::kPost,
      .headers = {{"Authorization", "Bearer " + access_token},
                  {"Content-Type", "application/octet-stream"},
                  {"Dropbox-API-Arg", json.dump()}},
      .body = std::move(content.data)};
  auto response =
      co_await util::FetchJson(http, std::move(request), stop_token);
  co_return ToItemImpl<Dropbox::File>(response);
}

std::string GetDirectoryPath(std::string_view path) {
  auto it = path.find_last_of('/');
  if (it == std::string::npos) {
    throw CloudException("invalid path");
  } else {
    return std::string(path.begin(), path.begin() + it);
  }
}

std::string GetEndpoint(std::string_view path) {
  return std::string(kEndpoint) + std::string(path);
}

Task<nlohmann::json> FetchJson(const coro::http::Http& http,
                               std::string access_token,
                               http::Request<std::string> request,
                               stdx::stop_token stop_token) {
  request.method = http::Method::kPost;
  request.headers.emplace_back("Content-Type", "application/json");
  request.headers.emplace_back("Authorization", "Bearer " + access_token);
  return util::FetchJson(http, std::move(request), std::move(stop_token));
}

}  // namespace

std::string Dropbox::Auth::GetAuthorizationUrl(const AuthData& data) {
  return "https://www.dropbox.com/oauth2/authorize?" +
         http::FormDataToString({{"response_type", "code"},
                                 {"client_id", data.client_id},
                                 {"redirect_uri", data.redirect_uri},
                                 {"state", data.state}});
}

auto Dropbox::Auth::ExchangeAuthorizationCode(const coro::http::Http& http,
                                              AuthData auth_data,
                                              std::string code,
                                              stdx::stop_token stop_token)
    -> Task<AuthToken> {
  auto request = http::Request<std::string>{
      .url = "https://api.dropboxapi.com/oauth2/token",
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
      .body =
          http::FormDataToString({{"grant_type", "authorization_code"},
                                  {"client_secret", auth_data.client_secret},
                                  {"client_id", auth_data.client_id},
                                  {"redirect_uri", auth_data.redirect_uri},
                                  {"code", std::move(code)}})};
  json json =
      co_await util::FetchJson(http, std::move(request), std::move(stop_token));
  co_return AuthToken{.access_token = json["access_token"]};
}

auto Dropbox::GetRoot(stdx::stop_token) -> Task<Directory> {
  Directory d{{.id = ""}};
  co_return d;
}

auto Dropbox::GetGeneralData(stdx::stop_token stop_token) -> Task<GeneralData> {
  Task<json> task1 = util::FetchJson(
      *http_,
      Request{
          .url = GetEndpoint("/users/get_current_account"),
          .method = http::Method::kPost,
          .headers = {{"Content-Type", ""},
                      {"Authorization", "Bearer " + auth_token_.access_token}},
          .invalidates_cache = false},
      stop_token);
  Task<json> task2 = util::FetchJson(
      *http_,
      Request{
          .url = GetEndpoint("/users/get_space_usage"),
          .method = http::Method::kPost,
          .headers = {{"Content-Type", ""},
                      {"Authorization", "Bearer " + auth_token_.access_token}},
          .invalidates_cache = false},
      stop_token);
  auto [json1, json2] = co_await WhenAll(std::move(task1), std::move(task2));
  co_return GeneralData{.username = json1["email"],
                        .space_used = json2["used"],
                        .space_total = json2["allocation"]["allocated"]};
}

auto Dropbox::ListDirectoryPage(Directory directory,
                                std::optional<std::string> page_token,
                                stdx::stop_token stop_token) -> Task<PageData> {
  http::Request<std::string> request;
  if (page_token) {
    json body;
    body["cursor"] = *page_token;
    request = {.url = GetEndpoint("/files/list_folder/continue"),
               .body = body.dump(),
               .invalidates_cache = false};
  } else {
    json body;
    body["path"] = std::move(directory.id);
    request = {.url = GetEndpoint("/files/list_folder"),
               .body = body.dump(),
               .invalidates_cache = false};
  }
  auto response = co_await FetchJson(*http_, auth_token_.access_token,
                                     std::move(request), std::move(stop_token));

  PageData page_data;
  for (const json& entry : response["entries"]) {
    page_data.items.emplace_back(ToItem(entry));
  }
  if (response["has_more"]) {
    page_data.next_page_token = response["cursor"];
  }
  co_return page_data;
}

Generator<std::string> Dropbox::GetFileContent(File file, http::Range range,
                                               stdx::stop_token stop_token) {
  json json;
  json["path"] = file.id;
  auto request = Request{
      .url = "https://content.dropboxapi.com/2/files/download",
      .method = http::Method::kPost,
      .headers = {http::ToRangeHeader(range),
                  {"Content-Type", ""},
                  {"Dropbox-API-arg", json.dump()},
                  {"Authorization", "Bearer " + auth_token_.access_token}},
      .invalidates_cache = false};
  auto response =
      co_await http_->Fetch(std::move(request), std::move(stop_token));
  FOR_CO_AWAIT(std::string & body, response.body) { co_yield std::move(body); }
}

template <typename ItemT>
Task<ItemT> Dropbox::RenameItem(ItemT item, std::string new_name,
                                stdx::stop_token stop_token) {
  auto request = Request{.url = GetEndpoint("/files/move_v2"),
                         .method = http::Method::kPost};
  json json;
  json["from_path"] = item.id;
  json["to_path"] = GetDirectoryPath(item.id) + "/" + new_name;
  request.body = json.dump();
  auto response = co_await FetchJson(*http_, auth_token_.access_token,
                                     std::move(request), std::move(stop_token));
  co_return ToItemImpl<ItemT>(response["metadata"]);
}

auto Dropbox::CreateDirectory(Directory parent, std::string name,
                              stdx::stop_token stop_token) -> Task<Directory> {
  auto request = Request{.url = GetEndpoint("/files/create_folder_v2"),
                         .method = http::Method::kPost};
  json json;
  json["path"] = parent.id + "/" + std::move(name);
  request.body = json.dump();
  auto response = co_await FetchJson(*http_, auth_token_.access_token,
                                     std::move(request), std::move(stop_token));
  co_return ToItemImpl<Directory>(response["metadata"]);
}

Task<> Dropbox::RemoveItem(Item item, stdx::stop_token stop_token) {
  auto request = Request{.url = GetEndpoint("/files/delete"),
                         .method = http::Method::kPost};
  json json;
  json["path"] = std::visit([](const auto& d) { return d.id; }, item);
  request.body = json.dump();
  co_await FetchJson(*http_, auth_token_.access_token, std::move(request),
                     std::move(stop_token));
}

template <typename ItemT>
Task<ItemT> Dropbox::MoveItem(ItemT source, Directory destination,
                              stdx::stop_token stop_token) {
  auto request = Request{.url = GetEndpoint("/files/move_v2"),
                         .method = http::Method::kPost};
  json json;
  json["from_path"] = source.id;
  json["to_path"] = destination.id + "/" + source.name;
  request.body = json.dump();
  auto response = co_await FetchJson(*http_, auth_token_.access_token,
                                     std::move(request), std::move(stop_token));
  co_return ToItemImpl<ItemT>(response["metadata"]);
}

auto Dropbox::CreateFile(Directory parent, std::string_view name,
                         FileContent content, stdx::stop_token stop_token)
    -> Task<File> {
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
      FileContent chunk{.data = util::Take(content.data, it, chunk_size),
                        .size = chunk_size};
      if (!session) {
        session = co_await CreateUploadSession(*http_, auth_token_.access_token,
                                               std::move(parent), name,
                                               std::move(chunk), stop_token);
      } else if (offset + static_cast<int64_t>(chunk_size) < content.size) {
        session = co_await WriteChunk(*http_, auth_token_.access_token,
                                      std::move(*session), std::move(chunk),
                                      offset, stop_token);
      } else {
        co_return co_await FinishUploadSession(
            *http_, auth_token_.access_token, std::move(*session),
            std::move(chunk), offset, std::move(stop_token));
      }
      offset += chunk_size;
    }
  }
}

auto Dropbox::GetItemThumbnail(File file, http::Range range,
                               stdx::stop_token stop_token) -> Task<Thumbnail> {
  auto is_supported = [](std::string_view extension) {
    for (std::string_view e :
         {"jpg", "jpeg", "png", "tiff", "tif", "gif", "bmp", "mkv", "mp4"}) {
      if (e == extension) {
        return true;
      }
    }
    return false;
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
      co_await http_->FetchOk(std::move(request), std::move(stop_token));
  Thumbnail result;
  result.size =
      std::stoll(http::GetHeader(response.headers, "Content-Length").value());
  result.data = std::move(response.body);
  co_return result;
}

namespace util {

template <>
Dropbox::Auth::AuthData GetAuthData<Dropbox>() {
  return {
      .client_id = DROPBOX_CLIENT_ID,
      .client_secret = DROPBOX_CLIENT_SECRET,
  };
}

template <>
auto AbstractCloudProvider::Create<Dropbox>(Dropbox p)
    -> std::unique_ptr<AbstractCloudProvider> {
  return CreateAbstractCloudProvider(std::move(p));
}

}  // namespace util

template auto Dropbox::RenameItem(File item, std::string new_name,
                                  stdx::stop_token stop_token) -> Task<File>;

template auto Dropbox::RenameItem(Directory item, std::string new_name,
                                  stdx::stop_token stop_token)
    -> Task<Directory>;

template auto Dropbox::MoveItem(File, Directory, stdx::stop_token)
    -> Task<File>;

template auto Dropbox::MoveItem(Directory, Directory, stdx::stop_token)
    -> Task<Directory>;

}  // namespace coro::cloudstorage