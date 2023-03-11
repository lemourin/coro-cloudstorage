#ifndef CORO_CLOUDSTORAGE_YANDEX_DISK_H
#define CORO_CLOUDSTORAGE_YANDEX_DISK_H

#include <nlohmann/json.hpp>
#include <sstream>

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/fetch_json.h"
#include "coro/http/http.h"
#include "coro/util/event_loop.h"
#include "coro/when_all.h"

namespace coro::cloudstorage {

class YandexDisk {
 public:
  using json = nlohmann::json;
  using Request = http::Request<std::string>;

  struct GeneralData {
    std::string username;
    int64_t space_used;
    int64_t space_total;
  };

  struct ItemData {
    std::string id;
    std::string name;
    int64_t timestamp;
  };

  struct Directory : ItemData {};

  struct File : ItemData {
    int64_t size;
    std::optional<std::string> thumbnail_url;
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

  struct FileContent {
    Generator<std::string> data;
    std::optional<int64_t> size;
  };

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    std::string mime_type;
  };

  static constexpr std::string_view kId = "yandex";
  static inline constexpr auto& kIcon = util::kYandexIcon;

  YandexDisk(const coro::http::Http* http,
             const coro::util::EventLoop* event_loop,
             YandexDisk::Auth::AuthToken auth_token)
      : http_(http),
        event_loop_(event_loop),
        auth_token_(std::move(auth_token)) {}

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

  Task<Thumbnail> GetItemThumbnail(File item, http::Range range,
                                   stdx::stop_token stop_token);

 private:
  template <typename ItemT>
  Task<ItemT> MoveItem(std::string_view from, std::string_view path,
                       stdx::stop_token stop_token);

  Task<> PollStatus(std::string_view url, stdx::stop_token stop_token);

  Task<nlohmann::json> FetchJson(Request request,
                                 stdx::stop_token stop_token) const;

  const coro::http::Http* http_;
  const coro::util::EventLoop* event_loop_;
  YandexDisk::Auth::AuthToken auth_token_;
};

namespace util {
template <>
YandexDisk::Auth::AuthData GetAuthData<YandexDisk>(const nlohmann::json&);
}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_YANDEX_DISK_H
