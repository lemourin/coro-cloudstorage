#ifndef CORO_CLOUDSTORAGE_BOX_H
#define CORO_CLOUDSTORAGE_BOX_H

#include <nlohmann/json.hpp>
#include <sstream>

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/auth_manager.h"
#include "coro/cloudstorage/util/fetch_json.h"
#include "coro/http/http.h"
#include "coro/when_all.h"

namespace coro::cloudstorage {

struct Box {
  struct GeneralData {
    std::string username;
    int64_t space_used;
    int64_t space_total;
  };

  struct ItemData {
    std::string id;
    std::string name;
    int64_t size;
    int64_t timestamp;
  };

  struct Directory : ItemData {};

  struct File : ItemData {};

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct Auth {
    using json = nlohmann::json;

    struct AuthToken {
      std::string access_token;
      std::string refresh_token;
    };

    struct AuthData {
      std::string client_id;
      std::string client_secret;
      std::string redirect_uri;
      std::string state;
    };

    static std::string GetAuthorizationUrl(const AuthData& data);

    static Task<AuthToken> ExchangeAuthorizationCode(
        const coro::http::Http& http, AuthData auth_data, std::string code,
        stdx::stop_token stop_token);

    static Task<AuthToken> RefreshAccessToken(const coro::http::Http& http,
                                              AuthData auth_data,
                                              AuthToken auth_token,
                                              stdx::stop_token stop_token);
  };

  struct FileContent {
    Generator<std::string> data;
    std::optional<int64_t> size;
  };

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    static inline constexpr std::string_view mime_type = "image/png";
  };

  class CloudProvider;

  static constexpr std::string_view kId = "box";
  static inline constexpr auto& kIcon = util::kAssetsProvidersBoxPng;
};

class Box::CloudProvider
    : public coro::cloudstorage::CloudProvider<Box, CloudProvider> {
 public:
  using json = nlohmann::json;
  using Request = http::Request<std::string>;

  CloudProvider(coro::cloudstorage::util::AuthManager<Auth> auth_manager,
                const coro::http::Http* http)
      : auth_manager_(std::move(auth_manager)), http_(http) {}

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
  coro::cloudstorage::util::AuthManager<Auth> auth_manager_;
  const coro::http::Http* http_;
};

namespace util {

template <>
Box::Auth::AuthData GetAuthData<Box>();

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_BOX_H
