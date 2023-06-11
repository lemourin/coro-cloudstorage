#ifndef CORO_CLOUDSTORAGE_OPEN_STACK_H
#define CORO_CLOUDSTORAGE_OPEN_STACK_H

#include <optional>
#include <string>
#include <variant>

#include "coro/cloudstorage/util/assets.h"
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
  using Request = http::Request<std::string>;

  struct GeneralData {
    std::string username;
  };

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
    class AuthHandler;

    struct AuthToken {
      std::string endpoint;
      std::string token;
      std::string bucket;
    };
  };

  static constexpr std::string_view kId = "openstack";
  static inline constexpr auto& kIcon = util::kOpenStackIcon;

  OpenStack(const coro::http::Http* http, Auth::AuthToken auth_token)
      : http_(http), auth_token_(std::move(auth_token)) {}

  Task<GeneralData> GetGeneralData(stdx::stop_token) const;

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

  static Item ToItem(const nlohmann::json&);
  static nlohmann::json ToJson(const Item&);

 private:
  Task<nlohmann::json> FetchJson(http::Request<std::string> request,
                                 stdx::stop_token stop_token) const;
  template <typename Request>
  Task<http::Response<>> FetchOk(Request, stdx::stop_token stop_token) const;

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

  const coro::http::Http* http_;
  Auth::AuthToken auth_token_;
};

class OpenStack::Auth::AuthHandler {
 public:
  explicit AuthHandler(const http::Http* http) : http_(http) {}

  Task<std::variant<http::Response<>, Auth::AuthToken>> operator()(
      http::Request<> request, stdx::stop_token stop_token) const;

 private:
  const http::Http* http_;
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