#include "coro/cloudstorage/providers/pcloud.h"

#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"

namespace coro::cloudstorage {

namespace {

constexpr std::string_view kSeparator = "Thnlg1ecwyUJHyhYYGrQ";

using json = nlohmann::json;
using Request = http::Request<std::string>;

std::string GetUploadStreamPrefix(std::string_view name) {
  std::stringstream chunk;
  chunk << "--" << kSeparator << "\r\n"
        << "Content-Disposition: form-data; name=\"filename\"; "
        << "filename=\"" << name << "\"\r\n"
        << "Content-Type: application/octet-stream\r\n\r\n";
  return std::move(chunk).str();
}

std::string GetUploadStreamSuffix() {
  return "\r\n--" + std::string(kSeparator) + "--";
}

Generator<std::string> GetUploadStream(std::string_view name,
                                       PCloud::FileContent content) {
  co_yield GetUploadStreamPrefix(name);
  FOR_CO_AWAIT(std::string & chunk, content.data) { co_yield std::move(chunk); }
  co_yield GetUploadStreamSuffix();
}

template <typename T>
T ToItemImpl(const nlohmann::json& json) {
  T result = {};
  result.name = json["name"];
  if constexpr (std::is_same_v<T, PCloud::File>) {
    result.id = json["fileid"];
    result.size = json["size"];
    result.timestamp = json["modified"];
  } else {
    result.id = json["folderid"];
  }
  return result;
}

PCloud::Item ToItem(const nlohmann::json& json) {
  if (json["isfolder"]) {
    return ToItemImpl<PCloud::Directory>(json);
  } else {
    return ToItemImpl<PCloud::File>(json);
  }
}

template <typename Request>
Task<http::Response<>> Fetch(const coro::http::Http& http,
                             std::string access_token, Request request,
                             stdx::stop_token stop_token) {
  request.headers.emplace_back("Authorization", "Bearer " + access_token);
  http::ResponseLike auto response =
      co_await http.Fetch(std::move(request), std::move(stop_token));
  if (response.status / 100 != 2) {
    std::string body = co_await http::GetBody(std::move(response.body));
    throw coro::http::HttpException(response.status, std::move(body));
  }
  if (auto error = http::GetHeader(response.headers, "x-error")) {
    throw CloudException("pcloud error " + *error);
  }
  co_return response;
}

template <typename Request>
Task<nlohmann::json> FetchJson(const coro::http::Http& http,
                               std::string access_token, Request request,
                               stdx::stop_token stop_token) {
  if (!http::GetHeader(request.headers, "Content-Type")) {
    request.headers.emplace_back("Content-Type", "application/json");
  }
  request.headers.emplace_back("Accept", "application/json");
  http::ResponseLike auto response = co_await Fetch(
      http, std::move(access_token), std::move(request), std::move(stop_token));
  std::string body = co_await http::GetBody(std::move(response.body));
  co_return nlohmann::json::parse(std::move(body));
}

}  // namespace

std::string PCloud::Auth::GetAuthorizationUrl(const AuthData& data) {
  return "https://my.pcloud.com/oauth2/authorize?" +
         http::FormDataToString({{"response_type", "code"},
                                 {"client_id", data.client_id},
                                 {"redirect_uri", data.redirect_uri},
                                 {"state", data.state}});
}

auto PCloud::Auth::ExchangeAuthorizationCode(
    const coro::http::Http& http, AuthData auth_data, std::string code,
    std::string hostname, stdx::stop_token stop_token) -> Task<AuthToken> {
  auto request = http::Request<std::string>{
      .url = hostname + "/oauth2_token",
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
      .body =
          http::FormDataToString({{"client_secret", auth_data.client_secret},
                                  {"client_id", auth_data.client_id},
                                  {"code", std::move(code)}})};
  json json =
      co_await util::FetchJson(http, std::move(request), std::move(stop_token));
  co_return AuthToken{.access_token = std::string(json["access_token"]),
                      .hostname = std::move(hostname)};
}

auto PCloud::GetRoot(stdx::stop_token) -> Task<Directory> {
  Directory d{{.id = 0}};
  co_return d;
}

auto PCloud::GetGeneralData(stdx::stop_token stop_token) -> Task<GeneralData> {
  Request request{.url = GetEndpoint("/userinfo")};
  auto response = co_await FetchJson(*http_, auth_token_.access_token,
                                     std::move(request), std::move(stop_token));
  co_return GeneralData{.username = std::string(response["email"]),
                        .space_used = response["usedquota"],
                        .space_total = response["quota"]};
}

auto PCloud::ListDirectoryPage(Directory directory,
                               std::optional<std::string> page_token,
                               stdx::stop_token stop_token) -> Task<PageData> {
  Request request{
      .url = GetEndpoint("/listfolder") + "?" +
             http::FormDataToString({{"folderid", std::to_string(directory.id)},
                                     {"timeformat", "timestamp"}})};
  auto response = co_await FetchJson(*http_, auth_token_.access_token,
                                     std::move(request), std::move(stop_token));
  PageData result;
  for (const auto& entry : response["metadata"]["contents"]) {
    result.items.emplace_back(ToItem(entry));
  }
  co_return result;
}

Generator<std::string> PCloud::GetFileContent(File file, http::Range range,
                                              stdx::stop_token stop_token) {
  Request request{
      .url = GetEndpoint("/getfilelink") + "?" +
             http::FormDataToString({{"fileid", std::to_string(file.id)}})};
  auto url_response = co_await FetchJson(*http_, auth_token_.access_token,
                                         std::move(request), stop_token);
  request = {.url = "https://" + std::string(url_response["hosts"][0]) +
                    std::string(url_response["path"]),
             .headers = {http::ToRangeHeader(range)}};
  auto content_response =
      co_await http_->Fetch(std::move(request), std::move(stop_token));
  FOR_CO_AWAIT(std::string & body, content_response.body) {
    co_yield std::move(body);
  }
}

auto PCloud::RenameItem(Directory item, std::string new_name,
                        stdx::stop_token stop_token) -> Task<Directory> {
  Request request{
      .url = GetEndpoint("/renamefolder") + "?" +
             http::FormDataToString({{"folderid", std::to_string(item.id)},
                                     {"toname", std::move(new_name)},
                                     {"timeformat", "timestamp"}}),
      .flags = Request::kWrite};
  auto response = co_await FetchJson(*http_, auth_token_.access_token,
                                     std::move(request), std::move(stop_token));
  co_return ToItemImpl<Directory>(response["metadata"]);
}

auto PCloud::RenameItem(File item, std::string new_name,
                        stdx::stop_token stop_token) -> Task<File> {
  Request request{
      .url = GetEndpoint("/renamefile") + "?" +
             http::FormDataToString({{"fileid", std::to_string(item.id)},
                                     {"toname", std::move(new_name)},
                                     {"timeformat", "timestamp"}}),
      .flags = Request::kWrite};
  auto response = co_await FetchJson(*http_, auth_token_.access_token,
                                     std::move(request), std::move(stop_token));
  co_return ToItemImpl<File>(response["metadata"]);
}

auto PCloud::CreateDirectory(Directory parent, std::string name,
                             stdx::stop_token stop_token) -> Task<Directory> {
  Request request{
      .url = GetEndpoint("/createfolder") + "?" +
             http::FormDataToString({{"folderid", std::to_string(parent.id)},
                                     {"name", std::move(name)},
                                     {"timeformat", "timestamp"}}),
      .flags = Request::kWrite};
  auto response = co_await FetchJson(*http_, auth_token_.access_token,
                                     std::move(request), std::move(stop_token));
  co_return ToItemImpl<Directory>(response["metadata"]);
}

Task<> PCloud::RemoveItem(File item, stdx::stop_token stop_token) {
  Request request{
      .url = GetEndpoint("/deletefile") + "?" +
             http::FormDataToString({{"fileid", std::to_string(item.id)}}),
      .flags = Request::kWrite};
  co_await Fetch(*http_, auth_token_.access_token, std::move(request),
                 std::move(stop_token));
}

Task<> PCloud::RemoveItem(Directory item, stdx::stop_token stop_token) {
  Request request{
      .url = GetEndpoint("/deletefolderrecursive") + "?" +
             http::FormDataToString({{"folderid", std::to_string(item.id)}}),
      .flags = Request::kWrite};
  co_await Fetch(*http_, auth_token_.access_token, std::move(request),
                 std::move(stop_token));
}

auto PCloud::MoveItem(Directory source, Directory destination,
                      stdx::stop_token stop_token) -> Task<Directory> {
  Request request{.url = GetEndpoint("/renamefolder") + "?" +
                         http::FormDataToString(
                             {{"folderid", std::to_string(source.id)},
                              {"tofolderid", std::to_string(destination.id)},
                              {"timeformat", "timestamp"}}),
                  .flags = Request::kWrite};
  auto response = co_await FetchJson(*http_, auth_token_.access_token,
                                     std::move(request), std::move(stop_token));
  co_return ToItemImpl<Directory>(response["metadata"]);
}

auto PCloud::MoveItem(File source, Directory destination,
                      stdx::stop_token stop_token) -> Task<File> {
  Request request{.url = GetEndpoint("/renamefile") + "?" +
                         http::FormDataToString(
                             {{"fileid", std::to_string(source.id)},
                              {"tofolderid", std::to_string(destination.id)},
                              {"timeformat", "timestamp"}}),
                  .flags = Request::Flag::kWrite};
  auto response = co_await FetchJson(*http_, auth_token_.access_token,
                                     std::move(request), std::move(stop_token));
  co_return ToItemImpl<File>(response["metadata"]);
}

auto PCloud::CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token)
    -> Task<File> {
  http::Request<> request{
      .url = GetEndpoint("/uploadfile") + "?" +
             http::FormDataToString({{"folderid", std::to_string(parent.id)},
                                     {"filename", name},
                                     {"timeformat", "timestamp"}}),
      .method = http::Method::kPost,
      .headers = {{"Content-Type",
                   "multipart/form-data; boundary=" + std::string(kSeparator)},
                  {"Content-Length",
                   std::to_string(GetUploadStreamPrefix(name).length() +
                                  content.size +
                                  GetUploadStreamSuffix().length())}},
      .body = GetUploadStream(name, std::move(content)),
      .flags = http::Request<>::kWrite};
  auto response = co_await FetchJson(*http_, auth_token_.access_token,
                                     std::move(request), std::move(stop_token));
  co_return ToItemImpl<File>(response["metadata"][0]);
}

auto PCloud::GetItemThumbnail(File file, http::Range range,
                              stdx::stop_token stop_token) -> Task<Thumbnail> {
  Request request{
      .url = GetEndpoint("/getthumb") + "?" +
             http::FormDataToString(
                 {{"fileid", std::to_string(file.id)}, {"size", "64x64"}}),
      .headers = {ToRangeHeader(range)}};
  auto response = co_await Fetch(*http_, auth_token_.access_token,
                                 std::move(request), std::move(stop_token));
  Thumbnail result;
  result.mime_type = http::GetHeader(response.headers, "Content-Type").value();
  result.size =
      std::stoll(http::GetHeader(response.headers, "Content-Length").value());
  result.data = std::move(response.body);
  co_return result;
}

std::string PCloud::GetEndpoint(std::string_view path) const {
  return auth_token_.hostname + std::string(path);
}

Task<PCloud::Auth::AuthToken> PCloud::Auth::AuthHandler::operator()(
    coro::http::Request<> request, coro::stdx::stop_token stop_token) const {
  auto query = http::ParseQuery(http::ParseUri(request.url).query.value_or(""));
  auto code_it = query.find("code");
  auto hostname_it = query.find("hostname");
  if (code_it != std::end(query) && hostname_it != std::end(query)) {
    co_return co_await PCloud::Auth::ExchangeAuthorizationCode(
        *http_, auth_data_, code_it->second, hostname_it->second,
        std::move(stop_token));
  } else {
    throw http::HttpException(http::HttpException::kBadRequest);
  }
}

namespace util {

template <>
nlohmann::json ToJson<PCloud::Auth::AuthToken>(PCloud::Auth::AuthToken token) {
  nlohmann::json json;
  json["access_token"] = std::move(token.access_token);
  json["hostname"] = token.hostname;
  return json;
}

template <>
PCloud::Auth::AuthToken ToAuthToken<PCloud::Auth::AuthToken>(
    const nlohmann::json& json) {
  return {.access_token = std::string(json.at("access_token")),
          .hostname = std::string(json.at("hostname"))};
}

template <>
PCloud::Auth::AuthData GetAuthData<PCloud>() {
  return {
      .client_id = PCLOUD_CLIENT_ID,
      .client_secret = PCLOUD_CLIENT_SECRET,
  };
}

template <>
auto AbstractCloudProvider::Create<PCloud>(PCloud p)
    -> std::unique_ptr<AbstractCloudProvider> {
  return CreateAbstractCloudProvider(std::move(p));
}

}  // namespace util

}  // namespace coro::cloudstorage