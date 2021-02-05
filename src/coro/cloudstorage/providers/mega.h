#ifndef CORO_CLOUDSTORAGE_MEGA_H
#define CORO_CLOUDSTORAGE_MEGA_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/providers/mega/file_system_access.h>
#include <coro/cloudstorage/providers/mega/http_io.h>
#include <coro/cloudstorage/util/auth_data.h>
#include <coro/cloudstorage/util/auth_handler.h>
#include <coro/cloudstorage/util/serialize_utils.h>
#include <coro/stdx/stop_token.h>
#include <coro/util/function_traits.h>
#include <coro/util/raii_utils.h>
#include <coro/util/type_list.h>
#include <mega.h>

#ifdef CreateDirectory
#undef CreateDirectory
#endif

#ifdef CreateFile
#undef CreateFile
#endif

#include <any>
#include <optional>
#include <random>

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
    ::mega::handle id;
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

  class CloudProvider;

  static constexpr std::string_view kId = "mega";
};

class Mega::CloudProvider
    : public coro::cloudstorage::CloudProvider<Mega, CloudProvider> {
 public:
  using AuthToken = Auth::AuthToken;
  using AuthData = Auth::AuthData;
  using UserCredential = Auth::UserCredential;

  template <typename EventLoop, http::HttpClient HttpClient>
  CloudProvider(const EventLoop& event_loop, const HttpClient& http,
                AuthToken auth_token, const AuthData& auth_data)
      : auth_token_(std::move(auth_token)),
        d_(std::make_unique<Data>(event_loop, http, auth_data)) {}

  template <auto Method, typename... Args>
  auto Do(stdx::stop_token stop_token, Args&&... args) {
    return d_->Do<Method>(std::move(stop_token), std::forward<Args>(args)...);
  }

  Task<Item> RenameItem(Item item, std::string new_name,
                        coro::stdx::stop_token);
  Task<Item> MoveItem(Item source, Directory destination,
                      coro::stdx::stop_token);
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

  template <typename EventLoop, http::HttpClient HttpClient>
  static Task<std::string> GetSession(
      const EventLoop& event_loop, const HttpClient& http,
      UserCredential credential, AuthData auth_data,
      stdx::stop_token stop_token = stdx::stop_token()) {
    Data d(event_loop, http, auth_data);
    co_return co_await d.GetSession(credential, std::move(stop_token));
  }

 private:
  static constexpr auto kLogin = static_cast<void (::mega::MegaClient::*)(
      const char*, const uint8_t*, const char*)>(&::mega::MegaClient::login);
  static constexpr auto kLoginWithSalt =
      static_cast<void (::mega::MegaClient::*)(const char*, const char*,
                                               std::string*, const char*)>(
          &::mega::MegaClient::login2);
  static constexpr auto kSessionLogin =
      static_cast<void (::mega::MegaClient::*)(const uint8_t*, int)>(
          &::mega::MegaClient::login);
  static constexpr auto kPutNodes = static_cast<void (::mega::MegaClient::*)(
      ::mega::handle, ::mega::NewNode*, int, const char*)>(
      &::mega::MegaClient::putnodes);

  struct ReadData {
    std::deque<std::string> buffer;
    std::exception_ptr exception;
    Promise<void> semaphore;
    bool paused = true;
    int size = 0;
  };

  struct Data;

  struct App : ::mega::MegaApp {
    static constexpr int kBufferSize = 1 << 20;

    void prelogin_result(int version, std::string* email, std::string* salt,
                         ::mega::error e) final {
      SetResult(std::make_tuple(version, *email, *salt, e));
    }

    void login_result(::mega::error e) final { SetResult(e); }

    void fetchnodes_result(::mega::error e) final {
      SetResult(::mega::error(e));
    }

    void account_details(::mega::AccountDetails* details, bool, bool, bool,
                         bool, bool, bool) final {
      SetResult(std::move(*details));
      delete details;
    }

    void account_details(::mega::AccountDetails* details,
                         ::mega::error e) final {
      delete details;
      SetResult(e);
    }

    void setattr_result(::mega::handle handle, ::mega::error e) final {
      SetResult(std::make_tuple(handle, e));
    }

    void putnodes_result(::mega::error e, ::mega::targettype_t,
                         ::mega::NewNode*) final {
      if (e == ::mega::API_OK) {
        SetResult(client->nodenotify.back()->nodehandle);
      } else {
        SetResult(e);
      }
    }

    void unlink_result(::mega::handle handle, ::mega::error e) final {
      SetResult(std::make_tuple(handle, e));
    }

    void rename_result(::mega::handle handle, ::mega::error e) final {
      SetResult(std::make_tuple(handle, e));
    }

    ::mega::dstime pread_failure(::mega::error e, int retry, void* user_data,
                                 ::mega::dstime) final {
      const int kMaxRetryCount = 14;
      std::cerr << "[MEGA] PREAD FAILURE " << GetErrorDescription(e) << " "
                << retry << "\n";
      auto it = read_data.find(reinterpret_cast<intptr_t>(user_data));
      if (it == std::end(read_data)) {
        return ~static_cast<::mega::dstime>(0);
      }
      if (retry >= kMaxRetryCount) {
        it->second->exception =
            std::make_exception_ptr(CloudException(GetErrorDescription(e)));
        it->second->semaphore.SetValue();
        return ~static_cast<::mega::dstime>(0);
      } else {
        ::coro::Invoke(Retry(1 << (retry / 2)));
        return 1 << (retry / 2);
      }
    }

    bool pread_data(uint8_t* data, m_off_t length, m_off_t, m_off_t, m_off_t,
                    void* user_data) final {
      auto it = read_data.find(reinterpret_cast<intptr_t>(user_data));
      if (it == std::end(read_data)) {
        return false;
      }
      it->second->buffer.emplace_back(reinterpret_cast<const char*>(data),
                                      length);
      it->second->size += static_cast<int>(length);
      if (it->second->size >= kBufferSize) {
        it->second->paused = true;
        it->second->semaphore.SetValue();
        return false;
      }
      it->second->semaphore.SetValue();
      return true;
    }

    void notify_retry(::mega::dstime time, ::mega::retryreason_t reason) final {
      ::coro::Invoke(Retry(time, /*abortbackoff=*/false));
    }

    Task<> Retry(::mega::dstime time, bool abortbackoff = true) {
      std::cerr << "[MEGA] RETRYING IN " << time * 100 << "\n";
      co_await d->wait_(100 * time, d->stop_source.get_token());
      if (abortbackoff) {
        d->mega_client.abortbackoff();
      }
      std::cerr << "[MEGA] RETRYING NOW\n";
      d->OnEvent();
    }

    template <typename T>
    void SetResult(T result) {
      auto it = semaphore.find(client->restag);
      if (it != std::end(semaphore)) {
        last_result = std::move(result);
        it->second->SetValue();
      }
    }

    auto GetSemaphore(int tag) {
      auto result = coro::util::MakePointer(new Promise<void>,
                                            [this, tag](Promise<void>* s) {
                                              semaphore.erase(tag);
                                              delete s;
                                            });
      semaphore.insert({tag, result.get()});
      return result;
    }

    explicit App(Data* d) : d(d) {}

    std::unordered_map<int, Promise<void>*> semaphore;
    std::any last_result;
    std::unordered_map<intptr_t, std::shared_ptr<ReadData>> read_data;
    Data* d;
  };

  struct DoLogIn {
    Task<> operator()() { co_await d->LogIn(std::move(session)); }
    Data* d;
    std::string session;
  };

  struct Data {
    stdx::stop_source stop_source;
    std::function<Task<>(int ms, stdx::stop_token)> wait_;
    App mega_app;
    std::unique_ptr<::mega::HttpIO> http_io;
    mega::FileSystemAccess fs;
    ::mega::MegaClient mega_client;
    bool exec_pending = false;
    bool recursive_exec = false;
    std::optional<SharedPromise<DoLogIn>> current_login;
    std::default_random_engine random_engine;

    void OnEvent();

    template <auto Method, typename... Args>
    Task<std::any> Do(stdx::stop_token stop_token, Args... args) {
      auto tag = mega_client.nextreqtag();
      (mega_client.*Method)(args...);
      OnEvent();
      auto semaphore = mega_app.GetSemaphore(tag);
      stdx::stop_callback callback(
          stop_token, [&] { semaphore->SetException(InterruptedException()); });
      auto& semaphore_ref = *semaphore;
      co_await semaphore_ref;
      auto result = std::move(mega_app.last_result);
      co_await wait_(0, std::move(stop_token));
      co_return result;
    }

    Task<std::string> GetSession(UserCredential credentials,
                                 stdx::stop_token stop_token);
    Task<> EnsureLoggedIn(std::string session, stdx::stop_token);
    Task<> LogIn(std::string session);

    template <typename EventLoop, http::HttpClient HttpClient>
    Data(const EventLoop& event_loop, const HttpClient& http,
         const AuthData& auth_data)
        : stop_source(),
          wait_([event_loop = &event_loop](
                    int ms, stdx::stop_token stop_token) -> Task<> {
            co_await event_loop->Wait(ms, std::move(stop_token));
          }),
          mega_app(this),
          http_io(std::make_unique<mega::HttpIO<HttpClient>>(
              http, [this] { OnEvent(); })),
          mega_client(&mega_app, /*waiter=*/nullptr, /*http_io=*/http_io.get(),
                      /*fs=*/&fs, /*db_access=*/nullptr,
                      /*gfx_proc=*/nullptr, auth_data.api_key.c_str(),
                      auth_data.app_name.c_str()),
          random_engine(std::random_device()()) {}

    ~Data() { stop_source.request_stop(); }
  };

  static const char* GetErrorDescription(::mega::error e);

  static void Check(::mega::error e) {
    if (e != ::mega::API_OK) {
      throw CloudException(GetErrorDescription(e));
    }
  }

  ::mega::Node* GetNode(::mega::handle) const;

  AuthToken auth_token_;
  std::unique_ptr<Data> d_;
};

template <>
struct CreateCloudProvider<Mega> {
  template <typename CloudFactory, typename... Args>
  auto operator()(const CloudFactory& factory, Mega::Auth::AuthToken auth_token,
                  Args&&...) const {
    return Mega::CloudProvider(*factory.event_loop_, *factory.http_,
                               std::move(auth_token),
                               factory.auth_data_.template operator()<Mega>());
  }
};

namespace util {
template <>
inline auto ToJson<Mega::Auth::AuthToken>(Mega::Auth::AuthToken token) {
  nlohmann::json json;
  json["email"] = std::move(token.email);
  json["session"] = http::ToBase64(token.session);
  return json;
}

template <>
inline auto ToAuthToken<Mega::Auth::AuthToken>(const nlohmann::json& json) {
  return Mega::Auth::AuthToken{
      .email = json.at("email"),
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
    co_yield R"(
      <html>
        <body>
          <form method="post">
            <table>
              <tr>
                <td><label for="email">email:</label></td>
                <td><input type="text" id="email" name="email"/></td>
              </tr>
              <tr>
                <td><label for="password">password:</label></td>
                <td><input type="password" id="password" name="password"/></td>
              </tr>
              <tr><td><input type="submit" value="Submit"/></td></tr>
            </table>
          </form>
        </body>
      </html>
    )";
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
