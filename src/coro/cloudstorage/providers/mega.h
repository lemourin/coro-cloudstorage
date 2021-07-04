#ifndef CORO_CLOUDSTORAGE_MEGA_H
#define CORO_CLOUDSTORAGE_MEGA_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/assets.h>
#include <coro/cloudstorage/util/auth_data.h>
#include <coro/cloudstorage/util/auth_handler.h>
#include <coro/cloudstorage/util/serialize_utils.h>
#include <coro/cloudstorage/util/thumbnail_generator.h>
#include <coro/cloudstorage/util/thumbnail_options.h>
#include <coro/stdx/stop_token.h>
#include <coro/util/event_loop.h>
#include <coro/util/function_traits.h>
#include <coro/util/raii_utils.h>
#include <coro/util/type_list.h>

#include <any>
#include <optional>

namespace coro::cloudstorage {

struct Mega {
  struct Auth {
    struct AuthToken {
      std::string email;
      std::string session;
    };

    struct AuthData {
      std::string api_key;
      std::string app_name;
    };

    struct UserCredential {
      std::string email;
      std::string password;
      std::optional<std::string> twofactor;
    };

    class AuthHandler;
  };

  struct ItemData {
    uint64_t id;
    std::string name;
    int64_t timestamp;
  };

  struct Directory : ItemData {};

  struct File : ItemData {
    int64_t size;
  };

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct GeneralData {
    std::string username;
    int64_t space_used;
    int64_t space_total;
  };

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    static inline constexpr std::string_view mime_type = "image/jpeg";
  };

  struct FileContent {
    Generator<std::string> data;
    int64_t size;
  };

  class CloudProvider;

  static constexpr std::string_view kId = "mega";
  static inline const auto& kIcon = util::kAssetsProvidersMegaPng;
};

class Mega::CloudProvider
    : public coro::cloudstorage::CloudProvider<Mega, CloudProvider> {
 public:
  using AuthToken = Auth::AuthToken;
  using AuthData = Auth::AuthData;
  using UserCredential = Auth::UserCredential;

  CloudProvider(coro::util::WaitF wait, http::FetchF fetch,
                util::ThumbnailGeneratorF thumbnail_generator,
                AuthToken auth_token, AuthData auth_data);

  Task<Item> RenameItem(Item item, std::string new_name,
                        coro::stdx::stop_token);
  template <typename ItemT>
  Task<ItemT> RenameItem(ItemT item, std::string new_name,
                         stdx::stop_token stop_token) {
    co_return std::get<ItemT>(co_await RenameItem(
        Item(std::move(item)), std::move(new_name), std::move(stop_token)));
  }

  Task<Item> MoveItem(Item source, Directory destination,
                      coro::stdx::stop_token);
  template <typename ItemT>
  Task<ItemT> MoveItem(ItemT source, Directory destination,
                       stdx::stop_token stop_token) {
    co_return std::get<ItemT>(co_await MoveItem(Item(std::move(source)),
                                                std::move(destination),
                                                std::move(stop_token)));
  }

  Task<GeneralData> GetGeneralData(coro::stdx::stop_token);
  Task<Directory> GetRoot(coro::stdx::stop_token);
  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   coro::stdx::stop_token);
  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  coro::stdx::stop_token);
  Task<> RemoveItem(Item item, coro::stdx::stop_token);
  Generator<std::string> GetFileContent(File file, http::Range range,
                                        coro::stdx::stop_token);
  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token);
  Task<Thumbnail> GetItemThumbnail(File item, http::Range range,
                                   stdx::stop_token stop_token);

  static Task<std::string> GetSession(coro::util::WaitF wait,
                                      http::FetchF fetch,
                                      UserCredential credential,
                                      AuthData auth_data,
                                      stdx::stop_token stop_token) {
    auto d =
        CreateDataImpl(std::move(wait), std::move(fetch), std::move(auth_data));
    co_return co_await GetSession(d.get(), credential, std::move(stop_token));
  }

 private:
  using ThumbnailGeneratorT = std::function<Task<std::string>(
      CloudProvider*, File, util::ThumbnailOptions, stdx::stop_token)>;

  struct ReadData;
  struct Data;
  struct App;
  struct DoLogIn;
  enum class RequestType;

  struct DataDeleter {
    void operator()(Data*) const;
  };

  static std::unique_ptr<Data, DataDeleter> CreateDataImpl(coro::util::WaitF,
                                                           http::FetchF,
                                                           const AuthData&);
  static Task<std::string> GetSession(Data* data, UserCredential credential,
                                      stdx::stop_token stop_token);

  Task<> SetThumbnail(const File& file, std::string thumbnail,
                      stdx::stop_token);

  AuthToken auth_token_;
  ThumbnailGeneratorT thumbnail_generator_;
  std::unique_ptr<Data, DataDeleter> d_;
};

class Mega::Auth::AuthHandler {
 public:
  AuthHandler(coro::util::WaitF wait, http::FetchF fetch,
              Mega::Auth::AuthData auth_data);

  Task<std::variant<http::Response<>, Mega::Auth::AuthToken>> operator()(
      coro::http::Request<> request, coro::stdx::stop_token stop_token) const;

 private:
  coro::util::WaitF wait_;
  http::FetchF fetch_;
  Mega::Auth::AuthData auth_data_;
};

namespace util {
template <>
inline nlohmann::json ToJson<Mega::Auth::AuthToken>(
    Mega::Auth::AuthToken token) {
  nlohmann::json json;
  json["email"] = std::move(token.email);
  json["session"] = http::ToBase64(token.session);
  return json;
}

template <>
inline Mega::Auth::AuthToken ToAuthToken<Mega::Auth::AuthToken>(
    const nlohmann::json& json) {
  return {.email = json.at("email"),
          .session = http::FromBase64(std::string(json.at("session")))};
}

template <>
inline Mega::Auth::AuthData GetAuthData<Mega>() {
  return {.api_key = "ZVhB0Czb", .app_name = "coro-cloudstorage"};
}

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_MEGA_H
