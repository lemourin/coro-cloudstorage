#ifndef CORO_CLOUDSTORAGE_ONE_DRIVE_H
#define CORO_CLOUDSTORAGE_ONE_DRIVE_H

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/auth_manager.h"
#include "coro/cloudstorage/util/fetch_json.h"
#include "coro/when_all.h"

namespace coro::cloudstorage {

class OneDrive {
 public:
  using Request = http::Request<std::string>;

  using json = nlohmann::json;

  static constexpr std::string_view kId = "onedrive";
  static inline constexpr auto& kIcon = util::kAssetsProvidersOnedrivePng;

  struct Auth {
    struct AuthToken {
      std::string access_token;
      std::string refresh_token;
      std::string endpoint;
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
    int64_t space_total;
  };

  struct ItemData {
    std::string id;
    std::string name;
    int64_t timestamp;
    std::optional<std::string> thumbnail_url;
  };

  struct Directory : ItemData {};

  struct File : ItemData {
    std::optional<std::string> mime_type;
    int64_t size;
  };

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct FileContent {
    Generator<std::string> data;
    int64_t size;
  };

  struct UploadSession {
    std::string upload_url;
  };

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    std::string mime_type;
  };

  OneDrive(util::AuthManager<Auth> auth_manager, const coro::http::Http* http)
      : auth_manager_(std::move(auth_manager)), http_(http) {}

  Task<Directory> GetRoot(stdx::stop_token);

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token);

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token);

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token);

  template <typename ItemT>
  Task<ItemT> RenameItem(ItemT item, std::string new_name,
                         stdx::stop_token stop_token);

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token);

  Task<> RemoveItem(Item item, stdx::stop_token stop_token);

  template <typename ItemT>
  Task<ItemT> MoveItem(ItemT source, Directory destination,
                       stdx::stop_token stop_token);

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token);

  template <typename Item>
  Task<Thumbnail> GetItemThumbnail(Item item, http::Range range,
                                   stdx::stop_token stop_token);

 private:
  std::string GetEndpoint(std::string_view path) const;

  Task<UploadSession> CreateUploadSession(Directory parent,
                                          std::string_view name,
                                          stdx::stop_token stop_token);

  util::AuthManager<Auth> auth_manager_;
  const coro::http::Http* http_;
};

namespace util {
template <>
OneDrive::Auth::AuthData GetAuthData<OneDrive>(const nlohmann::json&);
}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_ONE_DRIVE_H
