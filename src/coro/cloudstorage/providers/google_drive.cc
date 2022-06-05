#include "coro/cloudstorage/providers/google_drive.h"

#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"

namespace coro::cloudstorage {

namespace {

constexpr std::string_view kSeparator = "fWoDm9QNn3v3Bq3bScUX";

constexpr std::string_view kEndpoint = "https://www.googleapis.com/drive/v3";
constexpr std::string_view kFileProperties =
    "id,name,thumbnailLink,trashed,mimeType,iconLink,parents,size,"
    "modifiedTime";
constexpr int kThumbnailSize = 256;

std::string GetEndpoint(std::string_view path) {
  return std::string(kEndpoint) + std::string(path);
}

std::string GetIconLink(std::string_view link) {
  const auto kDefaultSize = "16";
  auto it = link.find(kDefaultSize);
  if (it == std::string::npos) {
    return std::string(link);
  }
  return std::string(link.begin(), link.begin() + it) +
         std::to_string(kThumbnailSize) +
         std::string(link.begin() + it + strlen(kDefaultSize), link.end());
}

template <typename T>
T ToItemImpl(const nlohmann::json& json) {
  T result = {};
  result.id = json["id"];
  result.name = json["name"];
  result.timestamp = http::ParseTime(std::string(json["modifiedTime"]));
  if (json.contains("thumbnailLink")) {
    result.thumbnail_url = json["thumbnailLink"];
  } else {
    result.thumbnail_url = GetIconLink(std::string(json["iconLink"]));
  }
  for (std::string parents : json["parents"]) {
    result.parents.emplace_back(std::move(parents));
  }
  if constexpr (std::is_same_v<T, GoogleDrive::File>) {
    if (json.contains("size")) {
      result.size = std::stoll(std::string(json["size"]));
    }
    result.mime_type = json["mimeType"];
  }
  return result;
}

auto ToItem(const nlohmann::json& json) -> GoogleDrive::Item {
  if (json["mimeType"] == "application/vnd.google-apps.folder") {
    return ToItemImpl<GoogleDrive::Directory>(json);
  } else {
    return ToItemImpl<GoogleDrive::File>(json);
  }
}

Generator<std::string> GetUploadForm(nlohmann::json metadata,
                                     GoogleDrive::FileContent content) {
  co_yield "--";
  co_yield std::string(kSeparator);
  co_yield "\r\n";
  co_yield "Content-Type: application/json; charset=UTF-8\r\n\r\n";
  co_yield metadata.dump();
  co_yield "\r\n";
  co_yield "--";
  co_yield std::string(kSeparator);
  co_yield "\r\n";
  co_yield "Content-Type: application/octet-stream\r\n\r\n";

  FOR_CO_AWAIT(std::string & chunk, content.data) { co_yield std::move(chunk); }

  co_yield "\r\n--";
  co_yield std::string(kSeparator);
  co_yield "--\r\n";
}

}  // namespace

auto GoogleDrive::Auth::RefreshAccessToken(const coro::http::Http& http,
                                           AuthData auth_data,
                                           AuthToken auth_token,
                                           stdx::stop_token stop_token)
    -> Task<AuthToken> {
  auto request = http::Request<std::string>{
      .url = "https://accounts.google.com/o/oauth2/token",
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

std::string GoogleDrive::Auth::GetAuthorizationUrl(const AuthData& data) {
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

auto GoogleDrive::Auth::ExchangeAuthorizationCode(const coro::http::Http& http,
                                                  AuthData auth_data,
                                                  std::string code,
                                                  stdx::stop_token stop_token)
    -> Task<AuthToken> {
  auto request = http::Request<std::string>{
      .url = "https://accounts.google.com/o/oauth2/token",
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
  co_return AuthToken{.access_token = json["access_token"],
                      .refresh_token = json["refresh_token"]};
}

auto GoogleDrive::CloudProvider::GetRoot(stdx::stop_token) -> Task<Directory> {
  Directory d{{.id = "root"}};
  co_return d;
}

auto GoogleDrive::CloudProvider::ListDirectoryPage(
    Directory directory, std::optional<std::string> page_token,
    stdx::stop_token stop_token) -> Task<PageData> {
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

auto GoogleDrive::CloudProvider::GetGeneralData(stdx::stop_token stop_token)
    -> Task<GeneralData> {
  auto request = Request{.url = GetEndpoint("/about?fields=user,storageQuota")};
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

auto GoogleDrive::CloudProvider::GetItem(std::string id,
                                         stdx::stop_token stop_token)
    -> Task<Item> {
  auto request =
      Request{.url = GetEndpoint("/files/" + std::move(id)) + "?" +
                     http::FormDataToString({{"fields", kFileProperties}})};
  json json = co_await auth_manager_.FetchJson(std::move(request),
                                               std::move(stop_token));
  co_return ToItem(json);
}

Generator<std::string> GoogleDrive::CloudProvider::GetFileContent(
    File file, http::Range range, stdx::stop_token stop_token) {
  auto request = Request{.url = GetEndpoint("/files/" + file.id) + "?alt=media",
                         .headers = {ToRangeHeader(range)}};
  auto response =
      co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
  FOR_CO_AWAIT(std::string & body, response.body) { co_yield std::move(body); }
}

auto GoogleDrive::CloudProvider::CreateDirectory(Directory parent,
                                                 std::string name,
                                                 stdx::stop_token stop_token)
    -> Task<Directory> {
  auto request =
      Request{.url = GetEndpoint("/files/") + "?" +
                     http::FormDataToString({{"fields", kFileProperties}}),
              .method = http::Method::kPost,
              .headers = {{"Content-Type", "application/json"}}};
  json json;
  json["mimeType"] = "application/vnd.google-apps.folder";
  json["name"] = std::move(name);
  json["parents"] = {std::move(parent.id)};
  request.body = json.dump();
  auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                   std::move(stop_token));
  co_return std::get<Directory>(ToItem(response));
}

Task<> GoogleDrive::CloudProvider::RemoveItem(Item item,
                                              stdx::stop_token stop_token) {
  auto request =
      Request{.url = GetEndpoint("/files/") +
                     std::visit([](const auto& d) { return d.id; }, item),
              .method = http::Method::kDelete};
  co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
}

auto GoogleDrive::CloudProvider::CreateFile(Directory parent,
                                            std::string_view name,
                                            FileContent content,
                                            stdx::stop_token stop_token)
    -> Task<File> {
  return CreateOrUpdateFile(std::move(parent), name, std::move(content),
                            std::move(stop_token));
}

auto GoogleDrive::CloudProvider::CreateOrUpdateFile(Directory parent,
                                                    std::string_view name,
                                                    FileContent content,
                                                    stdx::stop_token stop_token)
    -> Task<File> {
  auto request =
      Request{.url = GetEndpoint("/files") + "?" +
                     http::FormDataToString(
                         {{"q", "'" + parent.id + "' in parents and name = '" +
                                    std::string(name) + "'"},
                          {"fields", "files(id)"}})};
  auto response =
      co_await auth_manager_.FetchJson(std::move(request), stop_token);
  if (response["files"].size() == 0) {
    co_return co_await CreateFileImpl(
        std::move(parent), name, std::move(content), std::move(stop_token));
  } else if (response["files"].size() == 1) {
    co_return co_await UpdateFile(std::string(response["files"][0]["id"]),
                                  std::move(content), std::move(stop_token));
  } else {
    throw CloudException("ambiguous file reference");
  }
}

auto GoogleDrive::CloudProvider::UpdateFile(std::string_view id,
                                            FileContent content,
                                            stdx::stop_token stop_token)
    -> Task<File> {
  http::Request<> request{
      .url = "https://www.googleapis.com/upload/drive/v3/files/" +
             std::string(id) + "?" +
             http::FormDataToString(
                 {{"uploadType", "multipart"}, {"fields", kFileProperties}}),
      .method = http::Method::kPatch,
      .headers = {{"Accept", "application/json"},
                  {"Content-Type",
                   "multipart/related; boundary=" + std::string(kSeparator)},
                  {"Authorization",
                   "Bearer " /* + auth_manager_.GetAuthToken().access_token*/}},
      .body = GetUploadForm(json(), std::move(content))};
  auto response = co_await util::FetchJson(*http_, std::move(request),
                                           std::move(stop_token));
  co_return ToItemImpl<File>(response);
}

auto GoogleDrive::CloudProvider::CreateFileImpl(Directory parent,
                                                std::string_view name,
                                                FileContent content,
                                                stdx::stop_token stop_token)
    -> Task<File> {
  json metadata;
  metadata["name"] = name;
  metadata["parents"].push_back(parent.id);
  http::Request<> request{
      .url = "https://www.googleapis.com/upload/drive/v3/files?" +
             http::FormDataToString(
                 {{"uploadType", "multipart"}, {"fields", kFileProperties}}),
      .method = http::Method::kPost,
      .headers = {{"Accept", "application/json"},
                  {"Content-Type",
                   "multipart/related; boundary=" + std::string(kSeparator)},
                  {"Authorization",
                   "Bearer " /* + auth_manager_.GetAuthToken().access_token*/}},
      .body = GetUploadForm(std::move(metadata), std::move(content))};
  auto response = co_await util::FetchJson(*http_, std::move(request),
                                           std::move(stop_token));
  co_return ToItemImpl<File>(response);
}

template <typename ItemT>
auto GoogleDrive::CloudProvider::GetItemThumbnail(ItemT item, http::Range range,
                                                  stdx::stop_token stop_token)
    -> Task<Thumbnail> {
  Request request{.url = std::move(item.thumbnail_url),
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

template <typename ItemT>
auto GoogleDrive::CloudProvider::MoveItem(ItemT source, Directory destination,
                                          stdx::stop_token stop_token)
    -> Task<ItemT> {
  std::string remove_parents;
  for (auto& parent : source.parents) {
    remove_parents += parent + ",";
  }
  remove_parents.pop_back();
  auto request = Request{
      .url =
          GetEndpoint("/files/" + source.id) + "?" +
          http::FormDataToString({{"fields", kFileProperties},
                                  {"removeParents", std::move(remove_parents)},
                                  {"addParents", destination.id}}),
      .method = http::Method::kPatch,
      .headers = {{"Content-Type", "application/json"}}};
  request.body = json::object().dump();
  auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                   std::move(stop_token));
  co_return ToItemImpl<ItemT>(response);
}

template <typename ItemT>
Task<ItemT> GoogleDrive::CloudProvider::RenameItem(
    ItemT item, std::string new_name, stdx::stop_token stop_token) {
  auto request =
      Request{.url = GetEndpoint("/files/" + item.id) + "?" +
                     http::FormDataToString({{"fields", kFileProperties}}),
              .method = http::Method::kPatch,
              .headers = {{"Content-Type", "application/json"}}};
  json json;
  json["name"] = std::move(new_name);
  request.body = json.dump();
  auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                   std::move(stop_token));
  co_return ToItemImpl<ItemT>(response);
}

namespace util {

template <>
GoogleDrive::Auth::AuthData GetAuthData<GoogleDrive>() {
  return {.client_id = GOOGLE_DRIVE_CLIENT_ID,
          .client_secret = GOOGLE_DRIVE_CLIENT_SECRET};
}

template <>
auto AbstractCloudProvider::Create<GoogleDrive::CloudProvider>(
    GoogleDrive::CloudProvider p) -> std::unique_ptr<CloudProvider> {
  return CreateAbstractCloudProvider<GoogleDrive>(std::move(p));
}

}  // namespace util

template auto GoogleDrive::CloudProvider::RenameItem(
    File item, std::string new_name, stdx::stop_token stop_token) -> Task<File>;

template auto GoogleDrive::CloudProvider::RenameItem(
    Directory item, std::string new_name, stdx::stop_token stop_token)
    -> Task<Directory>;

template auto GoogleDrive::CloudProvider::MoveItem(File, Directory,
                                                   stdx::stop_token)
    -> Task<File>;

template auto GoogleDrive::CloudProvider::MoveItem(Directory, Directory,
                                                   stdx::stop_token)
    -> Task<Directory>;

template auto GoogleDrive::CloudProvider::GetItemThumbnail(File, http::Range,
                                                           stdx::stop_token)
    -> Task<Thumbnail>;

template auto GoogleDrive::CloudProvider::GetItemThumbnail(Directory,
                                                           http::Range,
                                                           stdx::stop_token)
    -> Task<Thumbnail>;

}  // namespace coro::cloudstorage