#ifndef CORO_CLOUDSTORAGE_OPEN_STACK_H
#define CORO_CLOUDSTORAGE_OPEN_STACK_H

#include <optional>
#include <string>
#include <variant>

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/auth_manager.h"
#include "coro/cloudstorage/util/crypto_utils.h"
#include "coro/cloudstorage/util/fetch_json.h"
#include "coro/cloudstorage/util/file_utils.h"
#include "coro/cloudstorage/util/recursive_visit.h"
#include "coro/cloudstorage/util/serialize_utils.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/generator.h"

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

  class CloudProvider;
};

class OpenStack::CloudProvider
    : public coro::cloudstorage::CloudProvider<OpenStack, CloudProvider> {
 public:
  using Request = http::Request<std::string>;

  CloudProvider(util::AuthManager<Auth> auth_manager,
                const coro::http::Http* http)
      : auth_manager_(std::move(auth_manager)), http_(http) {}

  Task<Directory> GetRoot(stdx::stop_token) const;

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token);

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token);

  Task<Directory> CreateDirectory(Directory parent, std::string_view name,
                                  stdx::stop_token stop_token);

  template <typename Item>
  Task<> RemoveItem(Item item, stdx::stop_token stop_token);

  template <typename Item>
  Task<Item> MoveItem(Item source, Directory destination,
                      stdx::stop_token stop_token);

  template <typename Item>
  Task<Item> RenameItem(Item item, std::string new_name,
                        stdx::stop_token stop_token);

  template <typename Item>
  Task<Item> GetItem(std::string_view id, stdx::stop_token stop_token);

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token);

  const Auth::AuthToken& auth_token() const {
    return auth_manager_.GetAuthToken();
  }

 private:
  Task<> RemoveItemImpl(std::string_view id, stdx::stop_token stop_token);

  template <typename Item>
  Task<> Move(const Item& root, std::string_view destination,
              stdx::stop_token stop_token);

  template <typename Item>
  Task<> MoveItemImpl(const Item& source, std::string_view destination,
                      stdx::stop_token stop_token);

  template <typename Item, typename F>
  Task<> Visit(Item item, const F& func, stdx::stop_token stop_token);

  std::string GetEndpoint(std::string_view endpoint) const;

  util::AuthManager<Auth> auth_manager_;
  const coro::http::Http* http_;
};

namespace util {

template <>
nlohmann::json ToJson<OpenStack::Auth::AuthToken>(
    OpenStack::Auth::AuthToken token);

template <>
OpenStack::Auth::AuthToken ToAuthToken<OpenStack::Auth::AuthToken>(
    const nlohmann::json& json);

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_OPEN_STACK_H