#ifndef CORO_CLOUDSTORAGE_OPEN_STACK_H
#define CORO_CLOUDSTORAGE_OPEN_STACK_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/crypto_utils.h>
#include <coro/cloudstorage/util/fetch_json.h>
#include <coro/cloudstorage/util/file_utils.h>
#include <coro/cloudstorage/util/recursive_visit.h>
#include <coro/cloudstorage/util/serialize_utils.h>
#include <coro/cloudstorage/util/string_utils.h>
#include <coro/generator.h>

#include <optional>
#include <string>
#include <variant>

namespace coro::cloudstorage {

class OpenStack {
 public:
  struct ItemData {
    std::string id;
    std::string name;
    int64_t timestamp;
  };

  struct Directory : ItemData {};

  struct File : ItemData {
    std::string mime_type;
    int64_t size;
  };

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct FileContent {
    Generator<std::string> data;
    std::optional<int64_t> size;
  };

  struct Auth {
    struct AuthData {};

    struct AuthToken {
      std::string endpoint;
      std::string token;
      std::string bucket;
    };
  };

  struct AuthorizeRequest {
    template <typename Request>
    Request operator()(Request request, Auth::AuthToken token) const {
      request.headers.emplace_back("X-Auth-Token", token.token);
      return request;
    }
  };

  template <typename AuthManager>
  class CloudProvider;
};

template <typename AuthManager>
class OpenStack::CloudProvider
    : public coro::cloudstorage::CloudProvider<OpenStack,
                                               CloudProvider<AuthManager>> {
 public:
  using Request = http::Request<std::string>;

  explicit CloudProvider(AuthManager auth_manager)
      : auth_manager_(std::move(auth_manager)) {}

  Task<Directory> GetRoot(stdx::stop_token) const { co_return Directory{}; }

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
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
        page_data.items.emplace_back(ToItem(item));
        page_data.next_page_token = item["name"];
      }
    }
    co_return page_data;
  }

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) {
    auto response = co_await auth_manager_.Fetch(
        Request{.url = GetEndpoint(util::StrCat("/", http::EncodeUri(file.id))),
                .headers = {http::ToRangeHeader(range)}},
        std::move(stop_token));
    FOR_CO_AWAIT(std::string & chunk, response.body) {
      co_yield std::move(chunk);
    }
  }

  Task<Directory> CreateDirectory(Directory parent, std::string_view name,
                                  stdx::stop_token stop_token) {
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

  template <typename Item>
  Task<> RemoveItem(Item item, stdx::stop_token stop_token) {
    co_await Visit(
        item,
        [&](const auto& entry) -> Task<> {
          co_await RemoveItemImpl(entry.id, stop_token);
        },
        stop_token);
  }

  template <typename Item>
  Task<Item> MoveItem(Item source, Directory destination,
                      stdx::stop_token stop_token) {
    std::string destination_path = destination.id;
    if (!destination_path.empty()) {
      destination_path += "/";
    }
    destination_path += source.name;
    co_await Move(source, destination_path, stop_token);
    co_return co_await GetItem<Item>(destination_path, std::move(stop_token));
  }

  template <typename Item>
  Task<Item> RenameItem(Item item, std::string new_name,
                        stdx::stop_token stop_token) {
    auto destination_path = util::GetDirectoryPath(item.id);
    if (!destination_path.empty()) {
      destination_path += "/";
    }
    destination_path += new_name;
    co_await Move(item, destination_path, stop_token);
    co_return co_await GetItem<Item>(destination_path, std::move(stop_token));
  }

  template <typename Item>
  Task<Item> GetItem(std::string_view id, stdx::stop_token stop_token) {
    auto json = co_await auth_manager_.FetchJson(
        Request{.url = util::StrCat(GetEndpoint("/"), "?",
                                    http::FormDataToString({{"format", "json"},
                                                            {"prefix", id},
                                                            {"delimiter", "/"},
                                                            {"limit", "1"}}))},
        std::move(stop_token));
    co_return ToItemImpl<Item>(json[0]);
  }

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token) {
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
    co_await auth_manager_.GetHttp().Fetch(
        AuthorizeRequest{}(std::move(request), auth_token()), stop_token);
    co_return co_await GetItem<File>(new_id, std::move(stop_token));
  }

  const auto& auth_token() const { return auth_manager_.GetAuthToken(); }

 private:
  static Item ToItem(const nlohmann::json& json) {
    if (json["content_type"] == "application/directory") {
      return ToItemImpl<Directory>(json);
    } else {
      return ToItemImpl<File>(json);
    }
  }

  template <typename ItemT>
  static ItemT ToItemImpl(const nlohmann::json& json) {
    ItemT result;
    result.id = json["name"];
    result.name = util::GetFileName(result.id);
    result.timestamp = coro::http::ParseTime(
        util::StrCat(std::string(json["last_modified"]), "Z"));
    if constexpr (std::is_same_v<ItemT, File>) {
      result.mime_type = json["content_type"];
      result.size = json["bytes"];
    }
    return result;
  }

  Task<> RemoveItemImpl(std::string_view id, stdx::stop_token stop_token) {
    co_await auth_manager_.Fetch(
        Request{.url = GetEndpoint(util::StrCat("/", http::EncodeUriPath(id))),
                .method = http::Method::kDelete,
                .headers = {{"Content-Length", "0"}}},
        std::move(stop_token));
  }

  template <typename Item>
  Task<> Move(const Item& root, std::string_view destination,
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

  template <typename Item>
  Task<> MoveItemImpl(const Item& source, std::string_view destination,
                      stdx::stop_token stop_token) {
    Request request{
        .url = GetEndpoint(util::StrCat("/", http::EncodeUri(source.id))),
        .method = http::Method::kCopy,
        .headers = {
            {"Content-Length", "0"},
            {"Destination", util::StrCat("/", auth_token().bucket, "/",
                                         http::EncodeUri(destination))}}};
    co_await auth_manager_.Fetch(std::move(request), stop_token);
    co_await RemoveItemImpl(source.id, std::move(stop_token));
  }

  template <typename Item, typename F>
  Task<> Visit(Item item, const F& func, stdx::stop_token stop_token) {
    return util::RecursiveVisit(this, std::move(item), func,
                                std::move(stop_token));
  }

  std::string GetEndpoint(std::string_view endpoint) const {
    return util::StrCat(auth_token().endpoint, "/", auth_token().bucket,
                        endpoint);
  }

  AuthManager auth_manager_;
};

namespace util {
template <>
inline nlohmann::json ToJson<OpenStack::Auth::AuthToken>(
    OpenStack::Auth::AuthToken token) {
  nlohmann::json json;
  json["endpoint"] = token.endpoint;
  json["token"] = token.token;
  json["bucket"] = token.bucket;
  return json;
}

template <>
inline OpenStack::Auth::AuthToken ToAuthToken<OpenStack::Auth::AuthToken>(
    const nlohmann::json& json) {
  OpenStack::Auth::AuthToken token;
  token.endpoint = json.at("endpoint");
  token.token = json.at("token");
  token.bucket = json.at("bucket");
  return token;
}
}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_OPEN_STACK_H