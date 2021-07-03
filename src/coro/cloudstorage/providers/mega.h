#ifndef CORO_CLOUDSTORAGE_MEGA_H
#define CORO_CLOUDSTORAGE_MEGA_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/assets.h>
#include <coro/cloudstorage/util/auth_data.h>
#include <coro/cloudstorage/util/auth_handler.h>
#include <coro/cloudstorage/util/serialize_utils.h>
#include <coro/cloudstorage/util/thumbnail_options.h>
#include <coro/stdx/stop_token.h>
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

  template <typename EventLoop, http::HttpClient HttpClient,
            typename ThumbnailGenerator>
  CloudProvider(const EventLoop& event_loop, const HttpClient& http,
                const ThumbnailGenerator& thumbnail_generator,
                AuthToken auth_token, AuthData auth_data)
      : auth_token_(std::move(auth_token)),
        thumbnail_generator_(
            [&](CloudProvider* provider, File file,
                util::ThumbnailOptions options,
                stdx::stop_token stop_token) -> Task<std::string> {
              co_return co_await thumbnail_generator(
                  provider, std::move(file), options, std::move(stop_token));
            }),
        d_(CreateData(event_loop, http, std::move(auth_data))) {}

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

  template <typename EventLoop, http::HttpClient HttpClient>
  static Task<std::string> GetSession(
      const EventLoop& event_loop, const HttpClient& http,
      UserCredential credential, AuthData auth_data,
      stdx::stop_token stop_token = stdx::stop_token()) {
    auto d = CreateData(event_loop, http, std::move(auth_data));
    co_return co_await GetSession(d.get(), credential, std::move(stop_token));
  }

 private:
  using WaitT = std::function<Task<>(int ms, stdx::stop_token)>;
  using FetchT = std::function<Task<http::Response<>>(
      http::Request<std::string>, stdx::stop_token)>;
  using ThumbnailGeneratorT = std::function<Task<std::string>(
      CloudProvider*, File, util::ThumbnailOptions, stdx::stop_token)>;

  struct ReadData;
  struct Data;
  struct App;
  struct DoLogIn;
  enum class RequestType;

  template <typename EventLoop, http::HttpClient HttpClient>
  static auto CreateData(const EventLoop& event_loop, const HttpClient& http,
                         const AuthData& auth_data) {
    return CreateDataImpl(
        [event_loop = &event_loop](int ms, stdx::stop_token stop_token)
            -> Task<> { co_await event_loop->Wait(ms, std::move(stop_token)); },
        [http = &http](http::Request<std::string> request,
                       stdx::stop_token stop_token) -> Task<http::Response<>> {
          co_return co_await http->Fetch(std::move(request),
                                         std::move(stop_token));
        },
        std::move(auth_data));
  }

  struct DataDeleter {
    void operator()(Data*) const;
  };

  static std::unique_ptr<Data, DataDeleter> CreateDataImpl(WaitT, FetchT,
                                                           const AuthData&);
  static Task<std::string> GetSession(Data* data, UserCredential credential,
                                      stdx::stop_token stop_token);

  Task<> SetThumbnail(const File& file, std::string thumbnail,
                      stdx::stop_token);

  AuthToken auth_token_;
  ThumbnailGeneratorT thumbnail_generator_;
  std::unique_ptr<Data, DataDeleter> d_;
};

template <>
struct CreateCloudProvider<Mega> {
  template <typename CloudFactory, typename... Args>
  auto operator()(const CloudFactory& factory, Mega::Auth::AuthToken auth_token,
                  Args&&...) const {
    return Mega::CloudProvider(
        *factory.event_loop_, *factory.http_, *factory.thumbnail_generator_,
        std::move(auth_token), factory.auth_data_.template operator()<Mega>());
  }
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

template <typename EventLoop, http::HttpClient HttpClient>
class MegaAuthHandler {
 public:
  MegaAuthHandler(const EventLoop& event_loop, const HttpClient& http,
                  Mega::Auth::AuthData auth_data)
      : event_loop_(&event_loop),
        http_(&http),
        auth_data_(std::move(auth_data)) {}

  Task<std::variant<http::Response<>, Mega::Auth::AuthToken>> operator()(
      coro::http::Request<> request, coro::stdx::stop_token stop_token) const {
    if (request.method == http::Method::kPost) {
      auto query =
          http::ParseQuery(co_await http::GetBody(std::move(*request.body)));
      auto it1 = query.find("email");
      auto it2 = query.find("password");
      if (it1 != std::end(query) && it2 != std::end(query)) {
        auto it3 = query.find("twofactor");
        Mega::Auth::UserCredential credential = {
            .email = it1->second,
            .password = it2->second,
            .twofactor = it3 != std::end(query)
                             ? std::make_optional(it3->second)
                             : std::nullopt};
        std::string session = co_await Mega::CloudProvider::GetSession(
            *event_loop_, *http_, std::move(credential), auth_data_,
            stop_token);
        co_return Mega::Auth::AuthToken{.email = it1->second,
                                        .session = std::move(session)};
      } else {
        throw http::HttpException(http::HttpException::kBadRequest);
      }
    } else {
      co_return http::Response<>{.status = 200, .body = GenerateLoginPage()};
    }
  }

 private:
  Generator<std::string> GenerateLoginPage() const {
    co_yield std::string(kAssetsHtmlMegaLoginHtml);
  }

  const EventLoop* event_loop_;
  const HttpClient* http_;
  Mega::Auth::AuthData auth_data_;
};

template <>
struct CreateAuthHandler<Mega> {
  template <typename CloudFactory>
  auto operator()(const CloudFactory& cloud_factory,
                  Mega::Auth::AuthData auth_data) const {
    return MegaAuthHandler(*cloud_factory.event_loop_, *cloud_factory.http_,
                           std::move(auth_data));
  }
};

template <>
inline Mega::Auth::AuthData GetAuthData<Mega>() {
  return {.api_key = "ZVhB0Czb", .app_name = "coro-cloudstorage"};
}

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_MEGA_H
