#include "coro/cloudstorage/providers/box.h"

#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"
#include "coro/cloudstorage/util/auth_manager.h"

namespace coro::cloudstorage {

namespace {

constexpr std::string_view kSeparator = "Thnlg1ecwyUJHyhYYGrQ";
constexpr std::string_view kFileProperties = "name,id,size,modified_at";
constexpr std::string_view kEndpoint = "https://api.box.com/2.0";

using AuthManager = ::coro::cloudstorage::util::AuthManager<Box::Auth>;

std::string GetEndpoint(std::string_view path) {
  return std::string(kEndpoint) + std::string(path);
}

template <typename Item>
Item ToItemImpl(const nlohmann::json& json) {
  Item item;
  item.id = json["id"];
  item.size = json["size"];
  item.name = json["name"];
  item.timestamp = http::ParseTime(std::string(json["modified_at"]));
  return item;
}

Box::Item ToItem(const nlohmann::json& json) {
  if (json["type"] == "folder") {
    return ToItemImpl<Box::Directory>(json);
  } else {
    return ToItemImpl<Box::File>(json);
  }
}

Generator<std::string> GetUploadStream(Box::Directory parent,
                                       std::string_view name,
                                       Box::FileContent content) {
  nlohmann::json request;
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
  FOR_CO_AWAIT(std::string & chunk, content.data) { co_yield std::move(chunk); }
  co_yield "\r\n--" + std::string(kSeparator) + "--";
}

template <typename T>
Task<T> RenameItemImpl(AuthManager* auth_manager, std::string endpoint, T item,
                       std::string new_name, stdx::stop_token stop_token) {
  nlohmann::json request;
  request["name"] = std::move(new_name);
  auto response = co_await auth_manager->FetchJson(
      http::Request<std::string>{.url = GetEndpoint(endpoint + item.id),
                                 .method = http::Method::kPut,
                                 .body = request.dump()},
      std::move(stop_token));
  co_return ToItemImpl<T>(response);
}

template <typename T>
Task<T> MoveItemImpl(AuthManager* auth_manager, std::string endpoint, T source,
                     Box::Directory destination, stdx::stop_token stop_token) {
  nlohmann::json request;
  request["parent"]["id"] = std::move(destination.id);
  auto response = co_await auth_manager->FetchJson(
      http::Request<std::string>{.url = GetEndpoint(endpoint + source.id),
                                 .method = http::Method::kPut,
                                 .body = request.dump()},
      std::move(stop_token));
  co_return ToItemImpl<T>(response);
}

}  // namespace

std::string Box::Auth::GetAuthorizationUrl(const AuthData& data) {
  return "https://account.box.com/api/oauth2/authorize?" +
         http::FormDataToString({{"response_type", "code"},
                                 {"client_id", data.client_id},
                                 {"client_secret", data.client_secret},
                                 {"redirect_uri", data.redirect_uri},
                                 {"state", data.state}});
}

auto Box::Auth::ExchangeAuthorizationCode(const coro::http::Http& http,
                                          AuthData auth_data, std::string code,
                                          stdx::stop_token stop_token)
    -> Task<AuthToken> {
  auto request = http::Request<std::string>{
      .url = "https://api.box.com/oauth2/token",
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

auto Box::Auth::RefreshAccessToken(const coro::http::Http& http,
                                   AuthData auth_data, AuthToken auth_token,
                                   stdx::stop_token stop_token)
    -> Task<AuthToken> {
  auto request = http::Request<std::string>{
      .url = "https://api.box.com/oauth2/token",
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
      .body =
          http::FormDataToString({{"refresh_token", auth_token.refresh_token},
                                  {"client_id", auth_data.client_id},
                                  {"client_secret", auth_data.client_secret},
                                  {"grant_type", "refresh_token"}})};
  json json =
      co_await util::FetchJson(http, std::move(request), std::move(stop_token));
  co_return AuthToken{.access_token = json["access_token"],
                      .refresh_token = json["refresh_token"]};
}

auto Box::CloudProvider::GetRoot(stdx::stop_token) -> Task<Directory> {
  Directory root{{.id = "0"}};
  co_return root;
}

auto Box::CloudProvider::GetGeneralData(stdx::stop_token stop_token)
    -> Task<GeneralData> {
  auto json = co_await auth_manager_.FetchJson(
      Request{.url = GetEndpoint("/users/me")}, std::move(stop_token));
  co_return GeneralData{.username = json["login"],
                        .space_used = json["space_used"],
                        .space_total = json["space_amount"]};
}

auto Box::CloudProvider::ListDirectoryPage(
    Directory directory, std::optional<std::string> page_token,
    stdx::stop_token stop_token) -> Task<PageData> {
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

Generator<std::string> Box::CloudProvider::GetFileContent(
    File file, http::Range range, stdx::stop_token stop_token) {
  Request request{.url = GetEndpoint("/files/" + file.id + "/content"),
                  .headers = {http::ToRangeHeader(range)}};
  auto response = co_await auth_manager_.Fetch(std::move(request), stop_token);
  if (response.status / 100 == 3) {
    request = {.url = http::GetHeader(response.headers, "Location").value(),
               .headers = {http::ToRangeHeader(range)}};
    response = co_await http_->Fetch(std::move(request), std::move(stop_token));
  }
  FOR_CO_AWAIT(auto& chunk, response.body) { co_yield std::move(chunk); }
}

auto Box::CloudProvider::RenameItem(Directory item, std::string new_name,
                                    stdx::stop_token stop_token)
    -> Task<Directory> {
  return RenameItemImpl<Directory>(&auth_manager_, "/folders/", std::move(item),
                                   std::move(new_name), std::move(stop_token));
}

auto Box::CloudProvider::RenameItem(File item, std::string new_name,
                                    stdx::stop_token stop_token) -> Task<File> {
  return RenameItemImpl<File>(&auth_manager_, "/files/", std::move(item),
                              std::move(new_name), std::move(stop_token));
}

auto Box::CloudProvider::CreateDirectory(Directory parent, std::string name,
                                         stdx::stop_token stop_token)
    -> Task<Directory> {
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

Task<> Box::CloudProvider::RemoveItem(File item, stdx::stop_token stop_token) {
  Request request = {.url = GetEndpoint("/files/" + item.id),
                     .method = http::Method::kDelete};
  co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
}

Task<> Box::CloudProvider::RemoveItem(Directory item,
                                      stdx::stop_token stop_token) {
  Request request{.url = GetEndpoint("/folders/" + item.id) + "?" +
                         http::FormDataToString({{"recursive", "true"}}),
                  .method = http::Method::kDelete};
  co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
}

auto Box::CloudProvider::MoveItem(Directory source, Directory destination,
                                  stdx::stop_token stop_token)
    -> Task<Directory> {
  return MoveItemImpl<Directory>(&auth_manager_, "/folders/", std::move(source),
                                 std::move(destination), std::move(stop_token));
}

auto Box::CloudProvider::MoveItem(File source, Directory destination,
                                  stdx::stop_token stop_token) -> Task<File> {
  return MoveItemImpl<File>(&auth_manager_, "/files/", std::move(source),
                            std::move(destination), std::move(stop_token));
}

auto Box::CloudProvider::CreateFile(Directory parent, std::string_view name,
                                    FileContent content,
                                    stdx::stop_token stop_token) -> Task<File> {
  std::optional<std::string> id;
  FOR_CO_AWAIT(const PageData& page, this->ListDirectory(parent, stop_token)) {
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
                  {"Content-Type",
                   "multipart/form-data; boundary=" + std::string(kSeparator)},
                  {"Authorization",
                   "Bearer " + auth_manager_.GetAuthToken().access_token}},
      .body = GetUploadStream(std::move(parent), name, std::move(content))};
  auto response = co_await util::FetchJson(*http_, std::move(request),
                                           std::move(stop_token));
  co_return ToItemImpl<File>(response["entries"][0]);
}

auto Box::CloudProvider::GetItemThumbnail(File file, http::Range range,
                                          stdx::stop_token stop_token)
    -> Task<Thumbnail> {
  Request request{.url = GetEndpoint("/files/" + file.id + "/thumbnail.png"),
                  .headers = {ToRangeHeader(range)}};
  auto response =
      co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
  Thumbnail result;
  result.size =
      std::stoll(http::GetHeader(response.headers, "Content-Length").value());
  if (result.size == 0) {
    throw CloudException(CloudException::Type::kNotFound);
  }
  result.data = std::move(response.body);
  co_return result;
}

namespace util {

template <>
Box::Auth::AuthData GetAuthData<Box>() {
  return {
      .client_id = "zmiv9tv13hunxhyjk16zqv8dmdw0d773",
      .client_secret = "IZ0T8WsUpJin7Qt3rHMf7qDAIFAkYZ0R",
  };
}

template <>
auto AbstractCloudProvider::Create<Box::CloudProvider>(Box::CloudProvider p)
    -> std::unique_ptr<CloudProvider> {
  return std::make_unique<AbstractCloudProviderImpl<Box::CloudProvider>>(
      std::move(p));
}

}  // namespace util

}  // namespace coro::cloudstorage