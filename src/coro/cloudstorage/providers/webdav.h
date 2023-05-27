#ifndef CORO_CLOUDSTORAGE_WEBDAV_H
#define CORO_CLOUDSTORAGE_WEBDAV_H

#include <nlohmann/json.hpp>
#include <sstream>

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/auth_handler.h"
#include "coro/cloudstorage/util/serialize_utils.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"
#include "coro/when_all.h"

namespace coro::cloudstorage {

class WebDAV {
 public:
  using Request = http::Request<std::string>;

  struct GeneralData {
    std::string username;
    std::optional<int64_t> space_used;
    std::optional<int64_t> space_total;
  };

  struct ItemData {
    std::string id;
    std::string name;
    std::optional<int64_t> timestamp;
  };

  struct Directory : ItemData {};

  struct File : ItemData {
    std::optional<int64_t> size;
    std::optional<std::string> mime_type;
  };

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct Auth {
    struct Credential {
      std::string username;
      std::string password;
    };
    struct AuthToken {
      std::string endpoint;
      std::optional<Credential> credential;
    };

    class AuthHandler;
  };

  struct FileContent {
    Generator<std::string> data;
    std::optional<int64_t> size;
  };

  static constexpr std::string_view kId = "webdav";
  static inline constexpr auto& kIcon = util::kWebDAVIcon;

  WebDAV(const coro::http::Http* http, WebDAV::Auth::AuthToken auth_token)
      : http_(http), auth_token_(std::move(auth_token)) {}

  Task<Directory> GetRoot(stdx::stop_token) const;

  Task<Item> GetItem(std::string id, stdx::stop_token) const;

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) const;

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) const;

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) const;

  template <typename Item>
  Task<Item> RenameItem(Item item, std::string new_name,
                        stdx::stop_token stop_token) const;

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token) const;

  template <typename Item>
  Task<> RemoveItem(Item item, stdx::stop_token stop_token) const;

  template <typename Item>
  Task<Item> MoveItem(Item source, Directory destination,
                      stdx::stop_token stop_token) const;

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token) const;

  static Item ToItem(const nlohmann::json&);
  static nlohmann::json ToJson(const Item&);

 private:
  template <typename T>
  Task<T> Move(T item, std::string destination,
               stdx::stop_token stop_token) const;

  std::string GetEndpoint(std::string_view href) const;

  const coro::http::Http* http_;
  WebDAV::Auth::AuthToken auth_token_;
};

class WebDAV::Auth::AuthHandler {
 public:
  Task<std::variant<http::Response<>, WebDAV::Auth::AuthToken>> operator()(
      http::Request<> request, stdx::stop_token) const;
};

namespace util {

template <>
nlohmann::json ToJson<WebDAV::Auth::AuthToken>(WebDAV::Auth::AuthToken token);

template <>
WebDAV::Auth::AuthToken ToAuthToken<WebDAV::Auth::AuthToken>(
    const nlohmann::json& json);

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_WEBDAV_H
