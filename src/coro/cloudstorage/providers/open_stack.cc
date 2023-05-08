#include "coro/cloudstorage/providers/open_stack.h"

namespace coro::cloudstorage {

namespace {

template <typename ItemT>
ItemT ToItemImpl(const nlohmann::json& json) {
  ItemT result;
  result.id = json["name"];
  result.name = util::GetFileName(result.id);
  result.timestamp = coro::http::ParseTime(
      util::StrCat(std::string(json["last_modified"]), "Z"));
  if constexpr (std::is_same_v<ItemT, OpenStack::File>) {
    result.mime_type = json["content_type"];
    result.size = json["bytes"];
  }
  return result;
}

OpenStack::Item ToItem(const nlohmann::json& json) {
  if (json["content_type"] == "application/directory") {
    return ToItemImpl<OpenStack::Directory>(json);
  } else {
    return ToItemImpl<OpenStack::File>(json);
  }
}

}  // namespace

auto OpenStack::GetRoot(stdx::stop_token) const -> Task<Directory> {
  co_return Directory{};
}

auto OpenStack::ListDirectoryPage(Directory directory,
                                  std::optional<std::string> page_token,
                                  stdx::stop_token stop_token)
    -> Task<PageData> {
  auto response = co_await auth_manager_.FetchJson(
      Request{.url = GetEndpoint(util::StrCat(
                  "/", "?",
                  http::FormDataToString({{"format", "json"},
                                          {"marker", page_token.value_or("")},
                                          {"path", directory.id}})))},
      std::move(stop_token));
  PageData page_data;
  for (const auto& item : response) {
    if (!item.contains("subdir")) {
      page_data.items.emplace_back(coro::cloudstorage::ToItem(item));
      page_data.next_page_token = item["name"];
    }
  }
  co_return page_data;
}

Generator<std::string> OpenStack::GetFileContent(File file, http::Range range,
                                                 stdx::stop_token stop_token) {
  auto response = co_await auth_manager_.Fetch(
      Request{.url = GetEndpoint(util::StrCat("/", http::EncodeUri(file.id))),
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
    new_id += "/";
  }
  new_id += name;
  co_await auth_manager_.Fetch(
      Request{.url = GetEndpoint(util::StrCat("/", http::EncodeUri(new_id))),
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
    destination_path += "/";
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
    destination_path += "/";
  }
  destination_path += new_name;
  co_await Move(item, destination_path, stop_token);
  co_return co_await GetItem<ItemT>(destination_path, std::move(stop_token));
}

template <typename ItemT>
Task<ItemT> OpenStack::GetItem(std::string_view id,
                               stdx::stop_token stop_token) {
  auto json = co_await auth_manager_.FetchJson(
      Request{.url = util::StrCat(GetEndpoint("/"), "?",
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
    new_id += "/";
  }
  new_id += name;
  auto request = http::Request<>{
      .url = GetEndpoint(util::StrCat("/", http::EncodeUri(new_id))),
      .method = http::Method::kPut,
      .body = std::move(content.data)};
  if (content.size) {
    request.headers.emplace_back("Content-Length",
                                 std::to_string(*content.size));
  }
  co_await http_->Fetch(AuthorizeRequest{}(std::move(request), auth_token()),
                        stop_token);
  co_return co_await GetItem<File>(new_id, std::move(stop_token));
}

Task<> OpenStack::RemoveItemImpl(std::string_view id,
                                 stdx::stop_token stop_token) {
  co_await auth_manager_.Fetch(
      Request{.url = GetEndpoint(util::StrCat("/", http::EncodeUriPath(id))),
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
            source,
            util::StrCat(destination, source.id.substr(root.id.length())),
            stop_token);
      },
      stop_token);
}

template <typename ItemT>
Task<> OpenStack::MoveItemImpl(const ItemT& source,
                               std::string_view destination,
                               stdx::stop_token stop_token) {
  Request request{
      .url = GetEndpoint(util::StrCat("/", http::EncodeUri(source.id))),
      .method = http::Method::kCopy,
      .headers = {{"Content-Length", "0"},
                  {"Destination", util::StrCat("/", auth_token().bucket, "/",
                                               http::EncodeUri(destination))}}};
  co_await auth_manager_.Fetch(std::move(request), stop_token);
  co_await RemoveItemImpl(source.id, std::move(stop_token));
}

template <typename ItemT, typename F>
Task<> OpenStack::Visit(ItemT item, const F& func,
                        stdx::stop_token stop_token) {
  return util::RecursiveVisit<OpenStack>(this, std::move(item), func,
                                         std::move(stop_token));
}

std::string OpenStack::GetEndpoint(std::string_view endpoint) const {
  return util::StrCat(auth_token().endpoint, "/", auth_token().bucket,
                      endpoint);
}

auto OpenStack::ToItem(std::string_view serialized) -> Item {
  return coro::cloudstorage::ToItem(nlohmann::json::parse(serialized));
}

std::string OpenStack::ToString(const Item& item) {
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
        return json.dump();
      },
      item);
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