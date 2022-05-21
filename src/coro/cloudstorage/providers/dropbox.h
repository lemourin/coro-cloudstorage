#ifndef CORO_CLOUDSTORAGE_DROPBOX_H
#define CORO_CLOUDSTORAGE_DROPBOX_H

#include <nlohmann/json.hpp>
#include <sstream>

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/fetch_json.h"
#include "coro/http/http.h"
#include "coro/when_all.h"

namespace coro::cloudstorage {

struct Dropbox {
  struct GeneralData {
    std::string username;
    int64_t space_used;
    int64_t space_total;
  };

  struct ItemData {
    std::string id;
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
  };

  struct UploadSession {
    std::string id;
    std::string path;
  };

  struct FileContent {
    Generator<std::string> data;
    std::optional<int64_t> size;
  };

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    static inline constexpr std::string_view mime_type = "image/jpeg";
  };

  class CloudProvider;

  static constexpr std::string_view kId = "dropbox";
  static inline constexpr auto& kIcon = util::kAssetsProvidersDropboxPng;
};

class Dropbox::CloudProvider
    : public coro::cloudstorage::CloudProvider<Dropbox, CloudProvider> {
 public:
  using json = nlohmann::json;
  using Request = http::Request<std::string>;

  CloudProvider(const coro::http::Http* http,
                Dropbox::Auth::AuthToken auth_token)
      : http_(http), auth_token_(std::move(auth_token)) {}

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

  Task<Thumbnail> GetItemThumbnail(File file, http::Range range,
                                   stdx::stop_token stop_token);

 private:
  const coro::http::Http* http_;
  Dropbox::Auth::AuthToken auth_token_;
};

namespace util {

template <>
Dropbox::Auth::AuthData GetAuthData<Dropbox>();

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_DROPBOX_H
