#ifndef CORO_CLOUDSTORAGE_YANDEX_DISK_H
#define CORO_CLOUDSTORAGE_YANDEX_DISK_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/assets.h>
#include <coro/cloudstorage/util/auth_data.h>
#include <coro/cloudstorage/util/fetch_json.h>
#include <coro/http/http.h>

#include <nlohmann/json.hpp>
#include <sstream>

namespace coro::cloudstorage {

struct YandexDisk {
  struct GeneralData {
    std::string username;
    int64_t space_used;
    int64_t space_total;
  };

  struct ItemData {
    std::string id;
    std::string name;
    int64_t timestamp;
  };

  struct Directory : ItemData {};

  struct File : ItemData {
    int64_t size;
    std::optional<std::string> thumbnail_url;
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
      return "https://oauth.yandex.com/authorize?" +
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
          .url = "https://oauth.yandex.com/token",
          .method = http::Method::kPost,
          .headers = {{"Content-Type", "application/x-www-form-urlencoded"}},
          .body = http::FormDataToString(
              {{"grant_type", "authorization_code"},
               {"client_id", auth_data.client_id},
               {"client_secret", auth_data.client_secret},
               {"code", std::move(code)}})};
      json json = co_await util::FetchJson(http, std::move(request),
                                           std::move(stop_token));
      co_return AuthToken{.access_token = json["access_token"]};
    }
  };

  struct FileContent {
    Generator<std::string> data;
    std::optional<int64_t> size;
  };

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    std::string mime_type;
  };

  template <typename Http = class HttpT, typename EventLoop = class EventLoopT>
  class CloudProvider;

  static constexpr std::string_view kId = "yandex";
  static inline constexpr auto& kIcon = util::kAssetsProvidersYandexPng;
};

template <typename Http, typename EventLoop>
class YandexDisk::CloudProvider
    : public coro::cloudstorage::CloudProvider<YandexDisk,
                                               CloudProvider<Http, EventLoop>> {
 public:
  using json = nlohmann::json;
  using Request = http::Request<std::string>;

  CloudProvider(const Http* http, const EventLoop* event_loop,
                YandexDisk::Auth::AuthToken auth_token)
      : http_(http),
        event_loop_(event_loop),
        auth_token_(std::move(auth_token)) {}

  Task<Directory> GetRoot(stdx::stop_token) {
    Directory d{{.id = "disk:/"}};
    co_return d;
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) {
    Task<json> task1 =
        FetchJson(Request{.url = "https://login.yandex.ru/info"}, stop_token);
    Task<json> task2 =
        FetchJson(Request{.url = GetEndpoint("/disk")}, stop_token);
    auto [json1, json2] = co_await WhenAll(std::move(task1), std::move(task2));
    co_return GeneralData{.username = json1["login"],
                          .space_used = json2["used_space"],
                          .space_total = json2["total_space"]};
  }

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
    std::vector<std::pair<std::string, std::string>> params = {
        {"path", directory.id}};
    if (page_token) {
      params.emplace_back("offset", *page_token);
    }
    Request request{.url = GetEndpoint("/disk/resources") + "?" +
                           http::FormDataToString(std::move(params))};
    auto response =
        co_await FetchJson(std::move(request), std::move(stop_token));
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

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) {
    Request request{.url = GetEndpoint("/disk/resources/download") + "?" +
                           http::FormDataToString({{"path", file.id}})};
    auto url_response = co_await FetchJson(std::move(request), stop_token);
    request = {.url = url_response["href"],
               .headers = {http::ToRangeHeader(range)}};
    auto response = co_await http_->Fetch(std::move(request), stop_token);
    if (response.status / 100 == 3) {
      request = {.url = http::GetHeader(response.headers, "Location").value(),
                 .headers = {http::ToRangeHeader(range)}};
      response =
          co_await http_->Fetch(std::move(request), std::move(stop_token));
    }
    FOR_CO_AWAIT(std::string & body, response.body) {
      co_yield std::move(body);
    }
  }

  template <typename ItemT>
  Task<ItemT> RenameItem(ItemT item, std::string new_name,
                         stdx::stop_token stop_token) {
    co_return co_await MoveItem<ItemT>(item.id,
                                       GetParentPath(item.id) + "/" + new_name,
                                       std::move(stop_token));
  }

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token) {
    Request request{
        .url = GetEndpoint("/disk/resources/") + "?" +
               http::FormDataToString({{"path", Concatenate(parent.id, name)}}),
        .method = http::Method::kPut};
    auto response = co_await FetchJson(std::move(request), stop_token);
    request = {.url = response["href"]};
    co_return ToItemImpl<Directory>(
        co_await FetchJson(std::move(request), std::move(stop_token)));
  }

  Task<> RemoveItem(Item item, stdx::stop_token stop_token) {
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
  Task<ItemT> MoveItem(ItemT source, Directory destination,
                       stdx::stop_token stop_token) {
    co_return co_await MoveItem<ItemT>(source.id,
                                       Concatenate(destination.id, source.name),
                                       std::move(stop_token));
  }

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token) {
    Request request{
        .url = GetEndpoint("/disk/resources/upload") + "?" +
               http::FormDataToString({{"path", Concatenate(parent.id, name)},
                                       {"overwrite", "true"}})};
    auto response = co_await FetchJson(std::move(request), stop_token);
    http::Request<> upload_request = {.url = response["href"],
                                      .method = http::Method::kPut,
                                      .body = std::move(content.data)};
    co_await http_->Fetch(std::move(upload_request), stop_token);
    request = {.url = GetEndpoint("/disk/resources/") + "?" +
                      http::FormDataToString(
                          {{"path", Concatenate(parent.id, name)}})};
    co_return ToItemImpl<File>(
        co_await FetchJson(std::move(request), std::move(stop_token)));
  }

  Task<Thumbnail> GetItemThumbnail(File item, http::Range range,
                                   stdx::stop_token stop_token) {
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
    result.mime_type =
        http::GetHeader(response.headers, "Content-Type").value();
    result.size =
        std::stoll(http::GetHeader(response.headers, "Content-Length").value());
    result.data = std::move(response.body);
    co_return result;
  }

 private:
  static constexpr std::string_view kEndpoint =
      "https://cloud-api.yandex.net/v1";

  static std::string GetEndpoint(std::string_view path) {
    return std::string(kEndpoint) + std::string(path);
  }

  static std::string Concatenate(std::string_view path,
                                 std::string_view child) {
    return std::string(path) +
           (!path.empty() && path.back() == '/' ? "" : "/") +
           std::string(child);
  }

  static std::string GetParentPath(std::string result) {
    if (result.back() == '/') result.pop_back();
    return result.substr(0, result.find_last_of('/'));
  }

  template <typename ItemT>
  Task<ItemT> MoveItem(std::string_view from, std::string_view path,
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

  Task<> PollStatus(std::string_view url, stdx::stop_token stop_token) {
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

  auto FetchJson(Request request, stdx::stop_token stop_token) const {
    request.headers.emplace_back("Content-Type", "application/json");
    request.headers.emplace_back("Authorization",
                                 "OAuth " + auth_token_.access_token);
    return util::FetchJson(*http_, std::move(request), std::move(stop_token));
  }

  static Item ToItem(const json& json) {
    if (json["type"] == "dir") {
      return ToItemImpl<Directory>(json);
    } else {
      return ToItemImpl<File>(json);
    }
  }

  template <typename T>
  static T ToItemImpl(const json& json) {
    T result = {};
    result.id = json["path"];
    result.name = json["name"];
    result.timestamp = http::ParseTime(std::string(json["modified"]));
    if constexpr (std::is_same_v<T, File>) {
      result.size = json["size"];
      if (json.contains("preview")) {
        result.thumbnail_url = json["preview"];
      }
    }
    return result;
  }

  const Http* http_;
  const EventLoop* event_loop_;
  YandexDisk::Auth::AuthToken auth_token_;
};

namespace util {
template <>
inline YandexDisk::Auth::AuthData GetAuthData<YandexDisk>() {
  return {
      .client_id = "04d700d432884c4381c07e760213ed8a",
      .client_secret = "197f9693caa64f0ebb51d201110074f9",
  };
}
}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_YANDEX_DISK_H
