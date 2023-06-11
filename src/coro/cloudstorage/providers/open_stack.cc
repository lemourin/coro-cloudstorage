#include "coro/cloudstorage/providers/open_stack.h"

#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"
#include "coro/cloudstorage/util/fetch_json.h"

namespace coro::cloudstorage {

namespace {

using ::coro::cloudstorage::util::StrCat;

template <typename Request>
Request AuthorizeRequest(Request request,
                         const OpenStack::Auth::AuthToken& token) {
  request.headers.emplace_back("X-Auth-Token", token.token);
  return request;
}

Generator<std::string> GenerateLoginPage() {
  co_yield std::string(util::kOpenStackLoginHtml);
}

template <typename ItemT>
ItemT ToItemImpl(const nlohmann::json& json) {
  ItemT result;
  result.id = json["name"];
  result.name = util::GetFileName(result.id);
  result.timestamp =
      http::ParseTime(StrCat(std::string(json["last_modified"]), 'Z'));
  if constexpr (std::is_same_v<ItemT, OpenStack::File>) {
    result.mime_type = json["content_type"];
    result.size = json["bytes"];
  }
  return result;
}

}  // namespace

auto OpenStack::GetGeneralData(stdx::stop_token) const -> Task<GeneralData> {
  GeneralData data{.username =
                       StrCat(auth_token_.bucket, '@', auth_token_.endpoint)};
  co_return data;
}

auto OpenStack::GetRoot(stdx::stop_token) const -> Task<Directory> {
  co_return Directory{};
}

auto OpenStack::ListDirectoryPage(Directory directory,
                                  std::optional<std::string> page_token,
                                  stdx::stop_token stop_token)
    -> Task<PageData> {
  auto response = co_await FetchJson(
      Request{.url = GetEndpoint(StrCat(
                  '/', '?',
                  http::FormDataToString({{"format", "json"},
                                          {"marker", page_token.value_or("")},
                                          {"path", directory.id}})))},
      std::move(stop_token));
  PageData page_data;
  for (const auto& item : response) {
    if (!item.contains("subdir")) {
      page_data.items.emplace_back(ToItem(item));
      page_data.next_page_token = item["name"];
    }
  }
  co_return page_data;
}

Generator<std::string> OpenStack::GetFileContent(File file, http::Range range,
                                                 stdx::stop_token stop_token) {
  auto response = co_await FetchOk(
      Request{.url = GetEndpoint(StrCat('/', http::EncodeUri(file.id))),
              .headers = {http::ToRangeHeader(range)}},
      std::move(stop_token));
  FOR_CO_AWAIT(std::string & chunk, response.body) {
    co_yield std::move(chunk);
  }
}

auto OpenStack::CreateDirectory(Directory parent, std::string_view name,
                                stdx::stop_token stop_token)
    -> Task<Directory> {
  std::string new_id;
  new_id += parent.id;
  if (!new_id.empty()) {
    new_id += '/';
  }
  new_id += name;
  co_await FetchOk(
      Request{.url = GetEndpoint(StrCat('/', http::EncodeUri(new_id))),
              .method = http::Method::kPut,
              .headers = {{"Content-Type", "application/directory"},
                          {"Content-Length", "0"}}},
      stop_token);
  co_return co_await GetItem<Directory>(new_id, std::move(stop_token));
}

template <typename ItemT>
Task<> OpenStack::RemoveItem(ItemT item, stdx::stop_token stop_token) {
  co_await Visit(
      item,
      [&](const auto& entry) -> Task<> {
        co_await RemoveItemImpl(entry.id, stop_token);
      },
      stop_token);
}

template <typename ItemT>
Task<ItemT> OpenStack::MoveItem(ItemT source, Directory destination,
                                stdx::stop_token stop_token) {
  std::string destination_path = destination.id;
  if (!destination_path.empty()) {
    destination_path += '/';
  }
  destination_path += source.name;
  co_await Move(source, destination_path, stop_token);
  co_return co_await GetItem<ItemT>(destination_path, std::move(stop_token));
}

template <typename ItemT>
Task<ItemT> OpenStack::RenameItem(ItemT item, std::string new_name,
                                  stdx::stop_token stop_token) {
  auto destination_path = util::GetDirectoryPath(item.id);
  if (!destination_path.empty()) {
    destination_path += '/';
  }
  destination_path += new_name;
  co_await Move(item, destination_path, stop_token);
  co_return co_await GetItem<ItemT>(destination_path, std::move(stop_token));
}

template <typename ItemT>
Task<ItemT> OpenStack::GetItem(std::string_view id,
                               stdx::stop_token stop_token) {
  auto json = co_await FetchJson(
      Request{.url = StrCat(GetEndpoint("/"), '?',
                            http::FormDataToString({{"format", "json"},
                                                    {"prefix", id},
                                                    {"delimiter", "/"},
                                                    {"limit", "1"}}))},
      std::move(stop_token));
  co_return ToItemImpl<ItemT>(json[0]);
}

auto OpenStack::CreateFile(Directory parent, std::string_view name,
                           FileContent content, stdx::stop_token stop_token)
    -> Task<File> {
  auto new_id = parent.id;
  if (!new_id.empty()) {
    new_id += '/';
  }
  new_id += name;
  auto request =
      http::Request<>{.url = GetEndpoint(StrCat('/', http::EncodeUri(new_id))),
                      .method = http::Method::kPut,
                      .body = std::move(content.data)};
  if (content.size) {
    request.headers.emplace_back("Content-Length",
                                 std::to_string(*content.size));
  }
  co_await FetchOk(std::move(request), stop_token);
  co_return co_await GetItem<File>(new_id, std::move(stop_token));
}

Task<> OpenStack::RemoveItemImpl(std::string_view id,
                                 stdx::stop_token stop_token) {
  co_await FetchJson(
      Request{.url = GetEndpoint(StrCat('/', http::EncodeUriPath(id))),
              .method = http::Method::kDelete,
              .headers = {{"Content-Length", "0"}}},
      std::move(stop_token));
}

template <typename ItemT>
Task<> OpenStack::Move(const ItemT& root, std::string_view destination,
                       stdx::stop_token stop_token) {
  co_await Visit(
      root,
      [&](const auto& source) -> Task<> {
        co_await MoveItemImpl(
            source, StrCat(destination, source.id.substr(root.id.length())),
            stop_token);
      },
      stop_token);
}

template <typename ItemT>
Task<> OpenStack::MoveItemImpl(const ItemT& source,
                               std::string_view destination,
                               stdx::stop_token stop_token) {
  Request request{
      .url = GetEndpoint(StrCat('/', http::EncodeUri(source.id))),
      .method = http::Method::kCopy,
      .headers = {{"Content-Length", "0"},
                  {"Destination", StrCat('/', auth_token_.bucket, '/',
                                         http::EncodeUri(destination))}}};
  co_await FetchOk(std::move(request), stop_token);
  co_await RemoveItemImpl(source.id, std::move(stop_token));
}

template <typename ItemT, typename F>
Task<> OpenStack::Visit(ItemT item, const F& func,
                        stdx::stop_token stop_token) {
  return util::RecursiveVisit<OpenStack>(this, std::move(item), func,
                                         std::move(stop_token));
}

std::string OpenStack::GetEndpoint(std::string_view endpoint) const {
  return StrCat(auth_token_.endpoint, '/', auth_token_.bucket, endpoint);
}

auto OpenStack::ToItem(const nlohmann::json& json) -> Item {
  if (json["content_type"] == "application/directory") {
    return ToItemImpl<Directory>(json);
  } else {
    return ToItemImpl<File>(json);
  }
}

nlohmann::json OpenStack::ToJson(const Item& item) {
  return std::visit(
      []<typename T>(const T& item) {
        nlohmann::json json;
        json["name"] = item.id;
        json["last_modified"] = [&] {
          std::string timestamp = http::ToTimeString(item.timestamp);
          timestamp.pop_back();
          return timestamp;
        }();
        if constexpr (std::is_same_v<T, File>) {
          json["content_type"] = item.mime_type;
          json["bytes"] = item.size;
        } else {
          json["content_type"] = "application/directory";
        }
        return json;
      },
      item);
}

Task<nlohmann::json> OpenStack::FetchJson(http::Request<std::string> request,
                                          stdx::stop_token stop_token) const {
  request = AuthorizeRequest(std::move(request), auth_token_);
  co_return co_await ::coro::cloudstorage::util::FetchJson(
      *http_, std::move(request), std::move(stop_token));
}

template <typename Request>
Task<http::Response<>> OpenStack::FetchOk(Request request,
                                          stdx::stop_token stop_token) const {
  auto response = co_await http_->Fetch(
      AuthorizeRequest(std::move(request), auth_token_), stop_token);
  if (response.status / 100 != 2) {
    throw http::HttpException(response.status);
  }
  co_return response;
}

auto OpenStack::Auth::AuthHandler::operator()(http::Request<> request,
                                              stdx::stop_token stop_token) const
    -> Task<std::variant<http::Response<>, Auth::AuthToken>> {
  if (request.method == http::Method::kPost) {
    auto query =
        http::ParseQuery(co_await http::GetBody(std::move(*request.body)));
    std::string auth_endpoint;
    if (auto it = query.find("auth_endpoint");
        it != query.end() && !it->second.empty()) {
      auth_endpoint = std::move(it->second);
    } else {
      throw CloudException("missing endpoint");
    }
    auto it1 = query.find("bucket");
    auto it2 = query.find("user");
    auto it3 = query.find("key");
    std::string bucket;
    std::string user;
    std::string key;
    if (it1 != query.end() && it2 != query.end() && it3 != query.end() &&
        !it1->second.empty() && !it2->second.empty() && !it3->second.empty()) {
      bucket = std::move(it1->second);
      user = std::move(it2->second);
      key = std::move(it3->second);
    } else {
      throw CloudException("missing credentials");
    }
    auto response = co_await http_->FetchOk(
        http::Request<std::string>{.url = std::move(auth_endpoint),
                                   .headers = {{"X-Auth-User", std::move(user)},
                                               {"X-Auth-Key", std::move(key)}}},
        std::move(stop_token));
    co_return Auth::AuthToken{
        .endpoint = http::GetHeader(response.headers, "X-Storage-Url").value(),
        .token = http::GetHeader(response.headers, "X-Auth-Token").value(),
        .bucket = std::move(bucket)};
  } else {
    co_return http::Response<>{.status = 200, .body = GenerateLoginPage()};
  }
}

namespace util {

template <>
nlohmann::json ToJson<OpenStack::Auth::AuthToken>(
    OpenStack::Auth::AuthToken token) {
  nlohmann::json json;
  json["endpoint"] = token.endpoint;
  json["token"] = token.token;
  json["bucket"] = token.bucket;
  return json;
}

template <>
OpenStack::Auth::AuthToken ToAuthToken<OpenStack::Auth::AuthToken>(
    const nlohmann::json& json) {
  OpenStack::Auth::AuthToken token;
  token.endpoint = json.at("endpoint");
  token.token = json.at("token");
  token.bucket = json.at("bucket");
  return token;
}

template <>
auto AbstractCloudProvider::Create<OpenStack>(OpenStack p)
    -> std::unique_ptr<AbstractCloudProvider> {
  return CreateAbstractCloudProvider(std::move(p));
}

}  // namespace util

template auto OpenStack::RenameItem(File item, std::string new_name,
                                    stdx::stop_token stop_token) -> Task<File>;

template auto OpenStack::RenameItem(Directory item, std::string new_name,
                                    stdx::stop_token stop_token)
    -> Task<Directory>;

template auto OpenStack::MoveItem(File, Directory, stdx::stop_token)
    -> Task<File>;

template auto OpenStack::MoveItem(Directory, Directory, stdx::stop_token)
    -> Task<Directory>;

template auto OpenStack::RemoveItem(File item, stdx::stop_token) -> Task<>;

template auto OpenStack::RemoveItem(Directory item, stdx::stop_token) -> Task<>;

}  // namespace coro::cloudstorage