#include "coro/cloudstorage/providers/dropbox.h"

#include <nlohmann/json.hpp>

#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"
#include "coro/cloudstorage/util/generator_utils.h"
#include "coro/cloudstorage/util/string_utils.h"

namespace coro::cloudstorage {

namespace {

constexpr std::string_view kEndpoint = "https://api.dropboxapi.com/2";
constexpr int kChunkSize = 8 * 1024 * 1024;

using ::coro::cloudstorage::util::StrCat;
using ::coro::http::GetBody;

template <typename T>
T ToItemImpl(const nlohmann::json& json) {
  T result = {};
  result.id = json["id"];
  result.name = json["name"];
  if constexpr (std::is_same_v<T, Dropbox::File>) {
    result.size = json.at("size");
    result.timestamp = http::ParseTime(std::string(json["client_modified"]));
  }
  return result;
}

Task<Dropbox::UploadSession> CreateUploadSession(
    util::AuthManager<Dropbox::Auth>* auth_manager, Dropbox::Directory parent,
    std::string_view name, Dropbox::FileContent content,
    stdx::stop_token stop_token) {
  std::string body = co_await GetBody(std::move(content.data));
  http::Request<std::string> request{
      .url = "https://content.dropboxapi.com/2/files/upload_session/start",
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/octet-stream"},
                  {"Dropbox-API-Arg", "{}"}},
      .body = std::move(body)};
  auto response = co_await auth_manager->FetchJson(std::move(request),
                                                   std::move(stop_token));
  co_return Dropbox::UploadSession{.id = response["session_id"],
                                   .path = StrCat(parent.id, '/', name)};
}

Task<Dropbox::UploadSession> WriteChunk(
    util::AuthManager<Dropbox::Auth>* auth_manager,
    Dropbox::UploadSession session, Dropbox::FileContent content,
    int64_t offset, stdx::stop_token stop_token) {
  nlohmann::json json;
  json["cursor"]["session_id"] = session.id;
  json["cursor"]["offset"] = offset;
  std::string body = co_await GetBody(std::move(content.data));
  http::Request<std::string> request = {
      .url = "https://content.dropboxapi.com/2/files/upload_session/append_v2",
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/octet-stream"},
                  {"Dropbox-API-Arg", json.dump()}},
      .body = std::move(body)};
  co_await auth_manager->Fetch(std::move(request), stop_token);
  co_return std::move(session);
}

Task<Dropbox::File> FinishUploadSession(
    util::AuthManager<Dropbox::Auth>* auth_manager,
    Dropbox::UploadSession session, Dropbox::FileContent content,
    int64_t offset, stdx::stop_token stop_token) {
  nlohmann::json json;
  json["cursor"]["session_id"] = std::move(session.id);
  json["cursor"]["offset"] = offset;
  json["commit"]["path"] = std::move(session.path);
  json["commit"]["mode"] = "overwrite";

  std::string body = co_await GetBody(std::move(content.data));
  http::Request<std::string> request{
      .url = "https://content.dropboxapi.com/2/files/upload_session/finish",
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/octet-stream"},
                  {"Dropbox-API-Arg", json.dump()}},
      .body = std::move(body)};
  auto response =
      co_await auth_manager->FetchJson(std::move(request), stop_token);
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
  return StrCat(kEndpoint, path);
}

}  // namespace

auto Dropbox::Auth::RefreshAccessToken(const coro::http::Http& http,
                                       AuthData auth_data, AuthToken auth_token,
                                       stdx::stop_token stop_token)
    -> Task<AuthToken> {
  auto request = http::Request<std::string>{
      .url = "https://api.dropbox.com/oauth2/token",
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
      .body =
          http::FormDataToString({{"refresh_token", auth_token.refresh_token},
                                  {"client_id", auth_data.client_id},
                                  {"client_secret", auth_data.client_secret},
                                  {"grant_type", "refresh_token"}})};
  json json =
      co_await util::FetchJson(http, std::move(request), std::move(stop_token));
  auth_token.access_token = json["access_token"];
  co_return auth_token;
}

std::string Dropbox::Auth::GetAuthorizationUrl(const AuthData& data) {
  std::vector<std::pair<std::string, std::string>> params = {
      {"response_type", "code"},
      {"client_id", data.client_id},
      {"redirect_uri", data.redirect_uri},
      {"state", data.state},
      {"token_access_type", "offline"}};
  if (!data.code_verifier.empty()) {
    params.emplace_back("code_challenge_method", "plain");
    params.emplace_back("code_challenge", data.code_verifier);
  }
  return "https://www.dropbox.com/oauth2/authorize?" +
         http::FormDataToString(params);
}

auto Dropbox::Auth::ExchangeAuthorizationCode(const coro::http::Http& http,
                                              AuthData auth_data,
                                              std::string code,
                                              stdx::stop_token stop_token)
    -> Task<AuthToken> {
  std::vector<std::pair<std::string, std::string>> params = {
      {"grant_type", "authorization_code"},
      {"client_secret", auth_data.client_secret},
      {"client_id", auth_data.client_id},
      {"redirect_uri", auth_data.redirect_uri},
      {"code", std::move(code)}};
  if (!auth_data.code_verifier.empty()) {
    params.emplace_back("code_verifier", auth_data.code_verifier);
  }
  auto request = http::Request<std::string>{
      .url = "https://api.dropboxapi.com/oauth2/token",
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
      .body = http::FormDataToString(params)};
  json json =
      co_await util::FetchJson(http, std::move(request), std::move(stop_token));
  co_return AuthToken{.access_token = json["access_token"],
                      .refresh_token = json["refresh_token"]};
}

auto Dropbox::GetRoot(stdx::stop_token) -> Task<Directory> {
  Directory d{{.id = ""}};
  co_return d;
}

auto Dropbox::GetItem(std::string id, stdx::stop_token stop_token)
    -> Task<Item> {
  json body;
  body["path"] = std::move(id);
  http::Request<std::string> request{
      .url = GetEndpoint("/files/get_metadata"),
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/json"}},
      .body = body.dump(),
      .invalidates_cache = false};
  co_return ToItem(
      co_await auth_manager_.FetchJson(std::move(request), stop_token));
}

auto Dropbox::GetGeneralData(stdx::stop_token stop_token) -> Task<GeneralData> {
  Task<json> task1 = auth_manager_.FetchJson(
      Request{.url = GetEndpoint("/users/get_current_account"),
              .method = http::Method::kPost,
              .headers = {{"Content-Type", ""}},
              .invalidates_cache = false},
      stop_token);
  Task<json> task2 = auth_manager_.FetchJson(
      Request{.url = GetEndpoint("/users/get_space_usage"),
              .method = http::Method::kPost,
              .headers = {{"Content-Type", ""}},
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
               .method = http::Method::kPost,
               .headers = {{"Content-Type", "application/json"}},
               .body = body.dump(),
               .invalidates_cache = false};
  } else {
    json body;
    body["path"] = std::move(directory.id);
    request = {.url = GetEndpoint("/files/list_folder"),
               .method = http::Method::kPost,
               .headers = {{"Content-Type", "application/json"}},
               .body = body.dump(),
               .invalidates_cache = false};
  }
  auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                   std::move(stop_token));

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
  auto request =
      Request{.url = "https://content.dropboxapi.com/2/files/download",
              .method = http::Method::kPost,
              .headers = {http::ToRangeHeader(range),
                          {"Content-Type", ""},
                          {"Dropbox-API-arg", json.dump()}},
              .invalidates_cache = false};
  auto response =
      co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
  FOR_CO_AWAIT(std::string & body, response.body) { co_yield std::move(body); }
}

template <typename ItemT>
Task<ItemT> Dropbox::RenameItem(ItemT item, std::string new_name,
                                stdx::stop_token stop_token) {
  auto request = Request{
      .url = GetEndpoint("/files/move_v2"),
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/json"}},
  };
  json json;
  json["from_path"] = item.id;
  json["to_path"] = GetDirectoryPath(item.id) + "/" + new_name;
  request.body = json.dump();
  auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                   std::move(stop_token));
  co_return ToItemImpl<ItemT>(response["metadata"]);
}

auto Dropbox::CreateDirectory(Directory parent, std::string name,
                              stdx::stop_token stop_token) -> Task<Directory> {
  auto request = Request{
      .url = GetEndpoint("/files/create_folder_v2"),
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/json"}},
  };
  json json;
  json["path"] = parent.id + "/" + std::move(name);
  request.body = json.dump();
  auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                   std::move(stop_token));
  co_return ToItemImpl<Directory>(response["metadata"]);
}

Task<> Dropbox::RemoveItem(Item item, stdx::stop_token stop_token) {
  auto request = Request{
      .url = GetEndpoint("/files/delete"),
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/json"}},
  };
  json json;
  json["path"] = std::visit([](const auto& d) { return d.id; }, item);
  request.body = json.dump();
  co_await auth_manager_.FetchJson(std::move(request), std::move(stop_token));
}

template <typename ItemT>
Task<ItemT> Dropbox::MoveItem(ItemT source, Directory destination,
                              stdx::stop_token stop_token) {
  auto request = Request{
      .url = GetEndpoint("/files/move_v2"),
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/json"}},
  };
  json json;
  json["from_path"] = source.id;
  json["to_path"] = destination.id + "/" + source.name;
  request.body = json.dump();
  auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                   std::move(stop_token));
  co_return ToItemImpl<ItemT>(response["metadata"]);
}

auto Dropbox::CreateFile(Directory parent, std::string_view name,
                         FileContent content, stdx::stop_token stop_token)
    -> Task<File> {
  if (content.size < kChunkSize) {
    json json;
    json["path"] = StrCat(parent.id, '/', name);
    json["mode"] = "overwrite";
    std::string body = co_await GetBody(std::move(content.data));
    auto request = http::Request<std::string>{
        .url = "https://content.dropboxapi.com/2/files/upload",
        .method = http::Method::kPost,
        .headers = {{"Dropbox-API-Arg", json.dump()},
                    {"Content-Type", "application/octet-stream"}},
        .body = std::move(body)};
    auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                     std::move(stop_token));
    co_return ToItemImpl<File>(response);
  } else {
    int64_t offset = 0;
    std::optional<UploadSession> session;
    auto it = co_await content.data.begin();
    while (true) {
      auto chunk_size = std::min<size_t>(
          kChunkSize,
          static_cast<size_t>(
              content.size.value_or((std::numeric_limits<size_t>::max)()) -
              offset));
      FileContent chunk{.data = util::Take(content.data, it, chunk_size),
                        .size = chunk_size};
      if (!session) {
        session =
            co_await CreateUploadSession(&auth_manager_, std::move(parent),
                                         name, std::move(chunk), stop_token);
      } else if (offset + static_cast<int64_t>(chunk_size) < content.size) {
        session = co_await WriteChunk(&auth_manager_, std::move(*session),
                                      std::move(chunk), offset, stop_token);
      } else {
        co_return co_await FinishUploadSession(
            &auth_manager_, std::move(*session), std::move(chunk), offset,
            std::move(stop_token));
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
  json["size"] = "w256h256";
  auto request = Request{
      .url = "https://content.dropboxapi.com/2/files/get_thumbnail_v2",
      .method = http::Method::kPost,
      .headers = {{"Dropbox-API-Arg", json.dump()}, ToRangeHeader(range)}};
  auto response =
      co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
  Thumbnail result;
  result.size =
      std::stoll(http::GetHeader(response.headers, "Content-Length").value());
  result.data = std::move(response.body);
  co_return result;
}

auto Dropbox::ToItem(const nlohmann::json& json) -> Item {
  if (json[".tag"] == "folder") {
    return ToItemImpl<Directory>(json);
  } else {
    return ToItemImpl<File>(json);
  }
}

nlohmann::json Dropbox::ToJson(const Item& item) {
  return std::visit(
      []<typename T>(const T& item) {
        nlohmann::json json;
        json["id"] = item.id;
        json["name"] = item.name;
        if constexpr (std::is_same_v<T, File>) {
          json[".tag"] = "file";
          json["size"] = item.size;
          json["client_modified"] = http::ToTimeString(item.timestamp);
        } else {
          json[".tag"] = "folder";
        }
        return json;
      },
      item);
}

namespace util {

template <>
Dropbox::Auth::AuthData GetAuthData<Dropbox>(const nlohmann::json& json) {
  Dropbox::Auth::AuthData auth_data{
      .client_id = json.at("client_id"),
      .client_secret = json.at("client_secret"),
  };
  if (auto it = json.find("code_verifier");
      it != json.end() && it->is_string()) {
    auth_data.code_verifier = *it;
  }
  return auth_data;
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