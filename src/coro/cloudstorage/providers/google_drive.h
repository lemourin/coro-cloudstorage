#ifndef CORO_CLOUDSTORAGE_GOOGLE_DRIVE_H
#define CORO_CLOUDSTORAGE_GOOGLE_DRIVE_H

#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/auth_manager.h"
#include "coro/cloudstorage/util/fetch_json.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"
#include "coro/stdx/coroutine.h"
#include "coro/stdx/stop_token.h"
#include "coro/task.h"

namespace coro::cloudstorage {

class GoogleDrive {
 public:
  using Request = http::Request<std::string>;
  using json = nlohmann::json;

  struct Auth {
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

    static Task<AuthToken> RefreshAccessToken(const coro::http::Http& http,
                                              AuthData auth_data,
                                              AuthToken auth_token,
                                              stdx::stop_token stop_token);

    static std::string GetAuthorizationUrl(const AuthData& data);

    static Task<AuthToken> ExchangeAuthorizationCode(
        const coro::http::Http& http, AuthData auth_data, std::string code,
        stdx::stop_token stop_token);
  };

  struct GeneralData {
    std::string username;
    int64_t space_used;
    std::optional<int64_t> space_total;
  };

  struct ItemData {
    std::string id;
    std::string name;
    int64_t timestamp;
    std::vector<std::string> parents;
    std::string thumbnail_url;
  };

  struct Directory : ItemData {};

  struct File : ItemData {
    std::optional<std::string> mime_type;
    std::optional<int64_t> size;
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

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    std::string mime_type;
  };

  static constexpr std::string_view kId = "google";
  static inline constexpr auto& kIcon = util::kGoogleDriveIcon;

  GoogleDrive(util::AuthManager<Auth> auth_manager,
              const coro::http::Http* http)
      : auth_manager_(std::move(auth_manager)), http_(http) {}

  Task<Directory> GetRoot(stdx::stop_token);

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token);

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token);

  Task<Item> GetItem(std::string id, stdx::stop_token stop_token);

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token);

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token);

  Task<> RemoveItem(Item item, stdx::stop_token stop_token);

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token);

  Task<File> CreateOrUpdateFile(Directory parent, std::string_view name,
                                FileContent content,
                                stdx::stop_token stop_token);

  template <typename Item>
  Task<Thumbnail> GetItemThumbnail(Item item, http::Range range,
                                   stdx::stop_token stop_token);

  template <typename ItemT>
  Task<ItemT> MoveItem(ItemT source, Directory destination,
                       stdx::stop_token stop_token);

  template <typename Item>
  Task<Item> RenameItem(Item item, std::string new_name,
                        stdx::stop_token stop_token);

 private:
  Task<File> UploadFile(std::optional<std::string_view> id,
                        nlohmann::json metadata, FileContent content,
                        stdx::stop_token stop_token);

  util::AuthManager<Auth> auth_manager_;
  const coro::http::Http* http_;
};

namespace util {
template <>

GoogleDrive::Auth::AuthData GetAuthData<GoogleDrive>(
    const nlohmann::json& json);

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_GOOGLE_DRIVE_H
