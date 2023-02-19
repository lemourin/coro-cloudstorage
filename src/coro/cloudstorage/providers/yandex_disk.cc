#include "coro/cloudstorage/providers/yandex_disk.h"

#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"

namespace coro::cloudstorage {

namespace {

constexpr std::string_view kEndpoint = "https://cloud-api.yandex.net/v1";

std::string GetEndpoint(std::string_view path) {
  return std::string(kEndpoint) + std::string(path);
}

std::string Concatenate(std::string_view path, std::string_view child) {
  return std::string(path) + (!path.empty() && path.back() == '/' ? "" : "/") +
         std::string(child);
}

std::string GetParentPath(std::string result) {
  if (result.back() == '/') result.pop_back();
  return result.substr(0, result.find_last_of('/'));
}

template <typename T>
T ToItemImpl(const nlohmann::json& json) {
  T result = {};
  result.id = json["path"];
  result.name = json["name"];
  result.timestamp = http::ParseTime(std::string(json["modified"]));
  if constexpr (std::is_same_v<T, YandexDisk::File>) {
    result.size = json["size"];
    if (json.contains("preview")) {
      result.thumbnail_url = json["preview"];
    }
  }
  return result;
}

YandexDisk::Item ToItem(const nlohmann::json& json) {
  if (json["type"] == "dir") {
    return ToItemImpl<YandexDisk::Directory>(json);
  } else {
    return ToItemImpl<YandexDisk::File>(json);
  }
}

}  // namespace

std::string YandexDisk::Auth::GetAuthorizationUrl(const AuthData& data) {
  return "https://oauth.yandex.com/authorize?" +
         http::FormDataToString({{"response_type", "code"},
                                 {"client_id", data.client_id},
                                 {"redirect_uri", data.redirect_uri},
                                 {"state", data.state},
                                 {"force_confirm", "yes"}});
}

auto YandexDisk::Auth::ExchangeAuthorizationCode(const coro::http::Http& http,
                                                 AuthData auth_data,
                                                 std::string code,
                                                 stdx::stop_token stop_token)
    -> Task<AuthToken> {
  auto request = http::Request<std::string>{
      .url = "https://oauth.yandex.com/token",
      .method = http::Method::kPost,
      .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
      .body =
          http::FormDataToString({{"grant_type", "authorization_code"},
                                  {"client_id", auth_data.client_id},
                                  {"client_secret", auth_data.client_secret},
                                  {"code", std::move(code)}})};
  json json =
      co_await util::FetchJson(http, std::move(request), std::move(stop_token));
  co_return AuthToken{.access_token = json["access_token"]};
}

auto YandexDisk::GetRoot(stdx::stop_token) -> Task<Directory> {
  Directory d{{.id = "disk:/"}};
  co_return d;
}

auto YandexDisk::GetGeneralData(stdx::stop_token stop_token)
    -> Task<GeneralData> {
  Task<json> task1 =
      FetchJson(Request{.url = "https://login.yandex.ru/info"}, stop_token);
  Task<json> task2 =
      FetchJson(Request{.url = GetEndpoint("/disk")}, stop_token);
  auto [json1, json2] = co_await WhenAll(std::move(task1), std::move(task2));
  co_return GeneralData{.username = json1["login"],
                        .space_used = json2["used_space"],
                        .space_total = json2["total_space"]};
}

auto YandexDisk::ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token)
    -> Task<PageData> {
  std::vector<std::pair<std::string, std::string>> params = {
      {"path", directory.id}};
  if (page_token) {
    params.emplace_back("offset", *page_token);
  }
  Request request{.url = GetEndpoint("/disk/resources") + "?" +
                         http::FormDataToString(std::move(params))};
  auto response = co_await FetchJson(std::move(request), std::move(stop_token));
  PageData page_data;
  for (const auto& v : response["_embedded"]["items"]) {
    page_data.items.emplace_back(ToItem(v));
  }
  int64_t offset = response["_embedded"]["offset"];
  int64_t limit = response["_embedded"]["limit"];
  int64_t total_count = response["_embedded"]["total"];
  if (offset + limit < total_count) {
    page_data.next_page_token = std::to_string(offset + limit);
  }
  co_return page_data;
}

auto YandexDisk::GetFileContent(File file, http::Range range,
                                stdx::stop_token stop_token)
    -> Generator<std::string> {
  Request request{.url = GetEndpoint("/disk/resources/download") + "?" +
                         http::FormDataToString({{"path", file.id}})};
  auto url_response = co_await FetchJson(std::move(request), stop_token);
  request = {.url = url_response["href"],
             .headers = {http::ToRangeHeader(range)}};
  auto response = co_await http_->Fetch(std::move(request), stop_token);
  if (response.status / 100 == 3) {
    request = {.url = http::GetHeader(response.headers, "Location").value(),
               .headers = {http::ToRangeHeader(range)}};
    response = co_await http_->Fetch(std::move(request), std::move(stop_token));
  }
  FOR_CO_AWAIT(std::string & body, response.body) { co_yield std::move(body); }
}

template <typename ItemT>
Task<ItemT> YandexDisk::RenameItem(ItemT item, std::string new_name,
                                   stdx::stop_token stop_token) {
  co_return co_await MoveItem<ItemT>(
      item.id, GetParentPath(item.id) + "/" + new_name, std::move(stop_token));
}

auto YandexDisk::CreateDirectory(Directory parent, std::string name,
                                 stdx::stop_token stop_token)
    -> Task<Directory> {
  Request request{
      .url = GetEndpoint("/disk/resources/") + "?" +
             http::FormDataToString({{"path", Concatenate(parent.id, name)}}),
      .method = http::Method::kPut};
  auto response = co_await FetchJson(std::move(request), stop_token);
  request = {.url = response["href"]};
  co_return ToItemImpl<Directory>(
      co_await FetchJson(std::move(request), std::move(stop_token)));
}

Task<> YandexDisk::RemoveItem(Item item, stdx::stop_token stop_token) {
  Request request{
      .url =
          GetEndpoint("/disk/resources") + "?" +
          http::FormDataToString(
              {{"path", std::visit([](const auto& d) { return d.id; }, item)},
               {"permanently", "true"}}),
      .method = http::Method::kDelete,
      .headers = {{"Authorization", "OAuth " + auth_token_.access_token}}};
  auto response = co_await http_->Fetch(std::move(request), stop_token);
  std::string body = co_await http::GetBody(std::move(response.body));
  if (response.status / 100 != 2) {
    throw http::HttpException(response.status, std::move(body));
  }
  if (response.status == 202) {
    auto json = nlohmann::json::parse(std::move(body));
    co_await PollStatus(std::string(json["href"]), std::move(stop_token));
  }
}

template <typename ItemT>
Task<ItemT> YandexDisk::MoveItem(ItemT source, Directory destination,
                                 stdx::stop_token stop_token) {
  co_return co_await MoveItem<ItemT>(source.id,
                                     Concatenate(destination.id, source.name),
                                     std::move(stop_token));
}

auto YandexDisk::CreateFile(Directory parent, std::string_view name,
                            FileContent content, stdx::stop_token stop_token)
    -> Task<File> {
  Request request{
      .url = GetEndpoint("/disk/resources/upload") + "?" +
             http::FormDataToString({{"path", Concatenate(parent.id, name)},
                                     {"overwrite", "true"}})};
  auto response = co_await FetchJson(std::move(request), stop_token);
  http::Request<> upload_request = {.url = response["href"],
                                    .method = http::Method::kPut,
                                    .body = std::move(content.data)};
  co_await http_->Fetch(std::move(upload_request), stop_token);
  request = {
      .url = GetEndpoint("/disk/resources/") + "?" +
             http::FormDataToString({{"path", Concatenate(parent.id, name)}})};
  co_return ToItemImpl<File>(
      co_await FetchJson(std::move(request), std::move(stop_token)));
}

auto YandexDisk::GetItemThumbnail(File item, http::Range range,
                                  stdx::stop_token stop_token)
    -> Task<Thumbnail> {
  if (!item.thumbnail_url) {
    throw CloudException(CloudException::Type::kNotFound);
  }
  Request request{
      .url = std::move(*item.thumbnail_url),
      .headers = {ToRangeHeader(range),
                  {"Authorization", "OAuth " + auth_token_.access_token}}};
  auto response =
      co_await http_->Fetch(std::move(request), std::move(stop_token));
  Thumbnail result;
  result.mime_type = http::GetHeader(response.headers, "Content-Type").value();
  result.size =
      std::stoll(http::GetHeader(response.headers, "Content-Length").value());
  result.data = std::move(response.body);
  co_return result;
}

template <typename ItemT>
Task<ItemT> YandexDisk::MoveItem(std::string_view from, std::string_view path,
                                 stdx::stop_token stop_token) {
  Request request{
      .url = GetEndpoint("/disk/resources/move") + "?" +
             http::FormDataToString({{"from", from}, {"path", path}}),
      .method = http::Method::kPost,
      .headers = {{"Authorization", "OAuth " + auth_token_.access_token}}};
  auto response = co_await http_->Fetch(std::move(request), stop_token);
  std::string body = co_await http::GetBody(std::move(response.body));
  if (response.status / 100 != 2) {
    throw http::HttpException(response.status, std::move(body));
  }
  if (response.status == 202) {
    auto json = nlohmann::json::parse(std::move(body));
    co_await PollStatus(std::string(json["href"]), stop_token);
  }
  request = {.url = GetEndpoint("/disk/resources") + "?" +
                    http::FormDataToString({{"path", path}})};
  co_return ToItemImpl<ItemT>(
      co_await FetchJson(std::move(request), std::move(stop_token)));
}

Task<> YandexDisk::PollStatus(std::string_view url,
                              stdx::stop_token stop_token) {
  int backoff = 100;
  while (true) {
    Request request{.url = std::string(url)};
    auto json = co_await FetchJson(std::move(request), stop_token);
    if (json["status"] == "success") {
      break;
    } else if (json["status"] == "failure") {
      throw CloudException(json.dump());
    } else if (json["status"] == "in-progress") {
      co_await event_loop_->Wait(backoff, stop_token);
      backoff *= 2;
      continue;
    } else {
      throw CloudException("unknown status");
    }
  }
}

Task<nlohmann::json> YandexDisk::FetchJson(Request request,
                                           stdx::stop_token stop_token) const {
  request.headers.emplace_back("Content-Type", "application/json");
  request.headers.emplace_back("Authorization",
                               "OAuth " + auth_token_.access_token);
  return util::FetchJson(*http_, std::move(request), std::move(stop_token));
}

namespace util {

template <>
YandexDisk::Auth::AuthData GetAuthData<YandexDisk>(const nlohmann::json& json) {
  return {
      .client_id = json.at("client_id"),
      .client_secret = json.at("client_secret"),
  };
}

template <>
auto AbstractCloudProvider::Create<YandexDisk>(YandexDisk p)
    -> std::unique_ptr<AbstractCloudProvider> {
  return CreateAbstractCloudProvider(std::move(p));
}

}  // namespace util

template auto YandexDisk::RenameItem<YandexDisk::File>(
    File item, std::string new_name, stdx::stop_token stop_token) -> Task<File>;

template auto YandexDisk::RenameItem<YandexDisk::Directory>(
    Directory item, std::string new_name, stdx::stop_token stop_token)
    -> Task<Directory>;

template auto YandexDisk::MoveItem<YandexDisk::File>(File, Directory,
                                                     stdx::stop_token)
    -> Task<File>;

template auto YandexDisk::MoveItem<YandexDisk::Directory>(Directory, Directory,
                                                          stdx::stop_token)
    -> Task<Directory>;

}  // namespace coro::cloudstorage