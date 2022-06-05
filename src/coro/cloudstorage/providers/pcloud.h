#ifndef CORO_CLOUDSTORAGE_PCLOUD_H
#define CORO_CLOUDSTORAGE_PCLOUD_H

#include <nlohmann/json.hpp>
#include <sstream>

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/auth_token_manager.h"
#include "coro/cloudstorage/util/fetch_json.h"
#include "coro/http/http.h"
#include "coro/when_all.h"

namespace coro::cloudstorage {

struct PCloud {
  struct GeneralData {
    std::string username;
    int64_t space_used;
    int64_t space_total;
  };

  struct ItemData {
    int64_t id;
    std::string name;
  };

  struct Directory : ItemData {};

  struct File : ItemData {
    int64_t size;
    int64_t timestamp;
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
      std::string hostname;
    };

    struct AuthData {
      std::string client_id;
      std::string client_secret;
      std::string redirect_uri;
      std::string state;
    };

    class AuthHandler;

    static std::string GetAuthorizationUrl(const AuthData& data);

    static Task<AuthToken> ExchangeAuthorizationCode(
        const coro::http::Http& http, AuthData auth_data, std::string code,
        std::string hostname, stdx::stop_token stop_token);
  };

  struct FileContent {
    Generator<std::string> data;
    int64_t size;
  };

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    std::string mime_type;
  };

  class CloudProvider;

  static constexpr std::string_view kId = "pcloud";
  static inline constexpr auto& kIcon = util::kAssetsProvidersPcloudPng;
};

class PCloud::CloudProvider {
 public:
  CloudProvider(const coro::http::Http* http,
                PCloud::Auth::AuthToken auth_token)
      : http_(http), auth_token_(std::move(auth_token)) {}

  Task<Directory> GetRoot(stdx::stop_token);

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token);

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token);

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token);

  Task<Directory> RenameItem(Directory item, std::string new_name,
                             stdx::stop_token stop_token);

  Task<File> RenameItem(File item, std::string new_name,
                        stdx::stop_token stop_token);

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token);

  Task<> RemoveItem(File item, stdx::stop_token stop_token);

  Task<> RemoveItem(Directory item, stdx::stop_token stop_token);

  Task<Directory> MoveItem(Directory source, Directory destination,
                           stdx::stop_token stop_token);

  Task<File> MoveItem(File source, Directory destination,
                      stdx::stop_token stop_token);

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token);

  Task<Thumbnail> GetItemThumbnail(File file, http::Range range,
                                   stdx::stop_token stop_token);

 private:
  std::string GetEndpoint(std::string_view path) const;

  const coro::http::Http* http_;
  PCloud::Auth::AuthToken auth_token_;
};

class PCloud::Auth::AuthHandler {
 public:
  AuthHandler(const coro::http::Http* http, PCloud::Auth::AuthData auth_data)
      : http_(http), auth_data_(std::move(auth_data)) {}

  Task<PCloud::Auth::AuthToken> operator()(
      coro::http::Request<> request, coro::stdx::stop_token stop_token) const;

 private:
  const coro::http::Http* http_;
  PCloud::Auth::AuthData auth_data_;
};

namespace util {

template <>
nlohmann::json ToJson<PCloud::Auth::AuthToken>(PCloud::Auth::AuthToken token);

template <>
PCloud::Auth::AuthToken ToAuthToken<PCloud::Auth::AuthToken>(
    const nlohmann::json& json);

template <>
PCloud::Auth::AuthData GetAuthData<PCloud>();

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_PCLOUD_H
