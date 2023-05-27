#include "coro/cloudstorage/providers/one_drive.h"

#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"
#include "coro/cloudstorage/util/generator_utils.h"
#include "coro/cloudstorage/util/string_utils.h"

namespace coro::cloudstorage {

namespace {

using ::coro::cloudstorage::util::StrCat;

constexpr std::string_view kFileProperties =
    "name,folder,audio,image,photo,video,id,size,lastModifiedDateTime,"
    "thumbnails,@content.downloadUrl,mimeType";

template <typename T>
T ToItemImpl(const nlohmann::json& json) {
  T result = {};
  result.id = json["id"];
  result.name = json["name"];
  result.timestamp = http::ParseTime(std::string(json["lastModifiedDateTime"]));
  if (json.contains("thumbnails") && !json["thumbnails"].empty()) {
    result.thumbnail_url = json["thumbnails"][0]["small"]["url"];
  }
  if constexpr (std::is_same_v<T, OneDrive::File>) {
    result.size = json.at("size");
    if (json.contains("mimeType")) {
      result.mime_type = json["mimeType"];
    }
  }
  return result;
}

Task<nlohmann::json> WriteChunk(const coro::http::Http& http,
                                OneDrive::UploadSession session,
                                OneDrive::FileContent content, int64_t offset,
                                int64_t total_size,
                                stdx::stop_token stop_token) {
  std::stringstream range_header;
  range_header << "bytes " << offset << "-" << offset + content.size - 1 << "/"
               << total_size;
  http::Request<> request{
      .url = session.upload_url,
      .method = http::Method::kPut,
      .headers = {{"Content-Length", std::to_string(content.size)},
                  {"Content-Range", range_header.str()},
                  {"Content-Type", "application/octet-stream"}},
      .body = std::move(content.data)};
  auto response =
      co_await util::FetchJson(http, std::move(request), std::move(stop_token));
  co_return std::move(response);
}

}  // namespace

auto OneDrive::Auth::RefreshAccessToken(const coro::http::Http& http,
                                        AuthData auth_data,
                                        AuthToken auth_token,
                                        stdx::stop_token stop_token)
    -> Task<AuthToken> {
  auto request = http::Request<std::string>{
      .url = "https://login.microsoftonline.com/common/oauth2/v2.0/token",
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

std::string OneDrive::Auth::GetAuthorizationUrl(const AuthData& data) {
  return "https://login.microsoftonline.com/common/oauth2/v2.0/authorize?" +
         http::FormDataToString(
             {{"response_type", "code"},
              {"client_id", data.client_id},
              {"redirect_uri", data.redirect_uri},
              {"scope", "offline_access user.read files.read"},
              {"state", data.state}});
}

auto OneDrive::Auth::ExchangeAuthorizationCode(const coro::http::Http& http,
                                               AuthData auth_data,
                                               std::string code,
                                               stdx::stop_token stop_token)
    -> Task<AuthToken> {
  auto request = http::Request<std::string>{
      .url = "https://login.microsoftonline.com/common/oauth2/v2.0/token",
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
      .body =
          http::FormDataToString({{"grant_type", "authorization_code"},
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
  json user_data = co_await util::FetchJson(http, std::move(user_data_request),
                                            std::move(stop_token));
  auth_token.endpoint = user_data.contains("mySite")
                            ? user_data["mySite"]
                            : "https://graph.microsoft.com/v1.0";
  co_return auth_token;
}

auto OneDrive::GetRoot(stdx::stop_token) -> Task<Directory> {
  Directory d{{.id = "root"}};
  co_return d;
}

auto OneDrive::GetItem(std::string id, stdx::stop_token stop_token)
    -> Task<Item> {
  auto request = Request{
      .url = StrCat(GetEndpoint(StrCat("/drive/items/", id)), '?',
                    http::FormDataToString({{"expand", "thumbnails"},
                                            {"select", kFileProperties}}))};
  json data = co_await auth_manager_.FetchJson(std::move(request),
                                               std::move(stop_token));
  co_return ToItem(data);
}

auto OneDrive::GetGeneralData(stdx::stop_token stop_token)
    -> Task<GeneralData> {
  Task<json> task1 =
      auth_manager_.FetchJson(Request{.url = GetEndpoint("/me")}, stop_token);
  Task<json> task2 = auth_manager_.FetchJson(
      Request{.url = GetEndpoint("/me/drive")}, stop_token);
  auto [json1, json2] = co_await WhenAll(std::move(task1), std::move(task2));
  co_return GeneralData{.username = json1["userPrincipalName"],
                        .space_used = json2["quota"]["used"],
                        .space_total = json2["quota"]["total"]};
}

auto OneDrive::ListDirectoryPage(Directory directory,
                                 std::optional<std::string> page_token,
                                 stdx::stop_token stop_token)
    -> Task<PageData> {
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

Generator<std::string> OneDrive::GetFileContent(File file, http::Range range,
                                                stdx::stop_token stop_token) {
  auto request =
      Request{.url = GetEndpoint("/drive/items/" + file.id + "/content"),
              .headers = {http::ToRangeHeader(range)}};
  auto response = co_await auth_manager_.Fetch(std::move(request), stop_token);
  if (response.status == 302) {
    auto redirect_request = Request{
        .url = coro::http::GetHeader(response.headers, "Location").value(),
        .headers = {http::ToRangeHeader(range)}};
    response = co_await auth_manager_.Fetch(std::move(redirect_request),
                                            std::move(stop_token));
  }
  FOR_CO_AWAIT(std::string & body, response.body) { co_yield std::move(body); }
}

template <typename ItemT>
Task<ItemT> OneDrive::RenameItem(ItemT item, std::string new_name,
                                 stdx::stop_token stop_token) {
  auto request =
      Request{.url = GetEndpoint("/drive/items/" + item.id) + "?" +
                     http::FormDataToString({{"select", kFileProperties}}),
              .method = http::Method::kPatch,
              .headers = {{"Content-Type", "application/json"}}};
  json json;
  json["name"] = std::move(new_name);
  request.body = json.dump();
  auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                   std::move(stop_token));
  co_return ToItemImpl<ItemT>(response);
}

auto OneDrive::CreateDirectory(Directory parent, std::string name,
                               stdx::stop_token stop_token) -> Task<Directory> {
  auto request = Request{
      .url = GetEndpoint("/drive/items/") + std::move(parent.id) + "/children",
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/json"}}};
  json json;
  json["folder"] = json::object();
  json["name"] = std::move(name);
  request.body = json.dump();
  auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                   std::move(stop_token));
  co_return ToItemImpl<Directory>(response);
}

Task<> OneDrive::RemoveItem(Item item, stdx::stop_token stop_token) {
  auto request =
      Request{.url = GetEndpoint("/drive/items/") +
                     std::visit([](const auto& d) { return d.id; }, item),
              .method = http::Method::kDelete};
  co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
}

template <typename ItemT>
Task<ItemT> OneDrive::MoveItem(ItemT source, Directory destination,
                               stdx::stop_token stop_token) {
  auto request = Request{.url = GetEndpoint("/drive/items/") + source.id,
                         .method = http::Method::kPatch,
                         .headers = {{"Content-Type", "application/json"}}};
  json json;
  if (destination.id == "root") {
    json["parentReference"]["path"] = "/drive/root";
  } else {
    json["parentReference"]["id"] = std::move(destination.id);
  }
  request.body = json.dump();
  auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                   std::move(stop_token));
  co_return ToItemImpl<ItemT>(response);
}

auto OneDrive::CreateFile(Directory parent, std::string_view name,
                          FileContent content, stdx::stop_token stop_token)
    -> Task<File> {
  if (content.size <= 4 * 1024 * 1024) {
    std::string body = co_await http::GetBody(std::move(content.data));
    http::Request<std::string> request{
        .url = GetEndpoint("/me/drive/items/") + parent.id + ":/" +
               http::EncodeUri(name) + ":/content",
        .method = http::Method::kPut,
        .headers = {{"Accept", "application/json"},
                    {"Content-Type", "application/octet-stream"}},
        .body = std::move(body)};
    auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                     std::move(stop_token));
    co_return ToItemImpl<File>(response);
  } else {
    auto session =
        co_await CreateUploadSession(std::move(parent), name, stop_token);
    auto it = co_await content.data.begin();
    int64_t offset = 0;
    while (true) {
      auto chunk_size = std::min<size_t>(
          60 * 1024 * 1024, static_cast<size_t>(content.size - offset));
      FileContent chunk{.data = util::Take(content.data, it, chunk_size),
                        .size = static_cast<int64_t>(chunk_size)};
      auto response = co_await WriteChunk(*http_, session, std::move(chunk),
                                          offset, content.size, stop_token);
      offset += chunk_size;
      if (offset >= content.size) {
        co_return ToItemImpl<File>(response);
      }
    }
  }
}

template <typename ItemT>
auto OneDrive::GetItemThumbnail(ItemT item, http::Range range,
                                stdx::stop_token stop_token)
    -> Task<Thumbnail> {
  if (!item.thumbnail_url) {
    throw CloudException(CloudException::Type::kNotFound);
  }
  Request request{.url = std::move(*item.thumbnail_url),
                  .headers = {ToRangeHeader(range)}};
  auto response =
      co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
  Thumbnail result;
  result.mime_type = http::GetHeader(response.headers, "Content-Type").value();
  result.size =
      std::stoll(http::GetHeader(response.headers, "Content-Length").value());
  result.data = std::move(response.body);
  co_return result;
}

std::string OneDrive::GetEndpoint(std::string_view path) const {
  const std::string& endpoint = auth_manager_.GetAuthToken().endpoint;
  if (endpoint.empty()) {
    throw CloudException(CloudException::Type::kUnauthorized);
  }
  return endpoint + std::string(path);
}

auto OneDrive::CreateUploadSession(Directory parent, std::string_view name,
                                   stdx::stop_token stop_token)
    -> Task<UploadSession> {
  http::Request<std::string> request{
      .url = GetEndpoint("/me/drive/items/") + parent.id + ":/" +
             http::EncodeUri(name) + ":/createUploadSession",
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/json"}},
      .body = "{}"};
  auto response =
      co_await auth_manager_.FetchJson(std::move(request), stop_token);
  co_return UploadSession{.upload_url = std::string(response["uploadUrl"])};
}

auto OneDrive::ToItem(const nlohmann::json& json) -> Item {
  if (json.contains("folder")) {
    return ToItemImpl<OneDrive::Directory>(json);
  } else {
    return ToItemImpl<OneDrive::File>(json);
  }
}

nlohmann::json OneDrive::ToJson(const Item& item) {
  return std::visit(
      []<typename ItemT>(const ItemT& i) {
        nlohmann::json json;
        json["id"] = i.id;
        json["name"] = i.name;
        json["lastModifiedDateTime"] = http::ToTimeString(i.timestamp);
        if (i.thumbnail_url) {
          nlohmann::json thumbnail;
          thumbnail["small"]["url"] = *i.thumbnail_url;
          json["thumbnails"].push_back(std::move(thumbnail));
        }
        if constexpr (std::is_same_v<ItemT, File>) {
          json["size"] = i.size;
          if (i.mime_type) {
            json["mimeType"] = *i.mime_type;
          }
        } else {
          json["folder"] = true;
        }
        return json;
      },
      item);
}

namespace util {

template <>
OneDrive::Auth::AuthData GetAuthData<OneDrive>(const nlohmann::json& json) {
  return {.client_id = json.at("client_id"),
          .client_secret = json.at("client_secret")};
}

template <>
auto AbstractCloudProvider::Create<OneDrive>(OneDrive p)
    -> std::unique_ptr<AbstractCloudProvider> {
  return CreateAbstractCloudProvider(std::move(p));
}

}  // namespace util

template auto OneDrive::RenameItem(File item, std::string new_name,
                                   stdx::stop_token stop_token) -> Task<File>;

template auto OneDrive::RenameItem(Directory item, std::string new_name,
                                   stdx::stop_token stop_token)
    -> Task<Directory>;

template auto OneDrive::MoveItem(File, Directory, stdx::stop_token)
    -> Task<File>;

template auto OneDrive::MoveItem(Directory, Directory, stdx::stop_token)
    -> Task<Directory>;

template auto OneDrive::GetItemThumbnail(File, http::Range, stdx::stop_token)
    -> Task<Thumbnail>;

template auto OneDrive::GetItemThumbnail(Directory, http::Range,
                                         stdx::stop_token) -> Task<Thumbnail>;

}  // namespace coro::cloudstorage