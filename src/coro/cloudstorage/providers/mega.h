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
#include <coro/util/make_pointer.h>
#include <coro/util/type_list.h>
#include <mega.h>

#include <any>
#include <optional>

namespace coro::cloudstorage {

struct MegaAuth {
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

class Mega : public MegaAuth {
 public:
  using Auth = MegaAuth;

  static constexpr std::string_view kId = "mega";

  struct Directory {
    ::mega::handle id;
    std::string name;
  };

  struct File : Directory {
    std::optional<int64_t> size;
    std::optional<std::string> mime_type;
  };

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct GeneralData {
    std::string username;
  };

  template <typename EventLoop, http::HttpClient HttpClient>
  Mega(const EventLoop& event_loop, const HttpClient& http,
       AuthToken auth_token, const AuthData& auth_data)
      : auth_token_(std::move(auth_token)),
        d_(std::make_unique<Data>(event_loop, http, auth_data)) {}

  template <auto Method, auto ResultMethod, typename... Args>
  auto Do(stdx::stop_token stop_token, Args&&... args) {
    return Do<Method, ResultMethod>(*d_, std::move(stop_token),
                                    std::forward<Args>(args)...);
  }

  Task<GeneralData> GetGeneralData(coro::stdx::stop_token);
  Task<Directory> GetRoot(coro::stdx::stop_token);
  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   coro::stdx::stop_token);
  Generator<std::string> GetFileContent(File file, http::Range range,
                                        coro::stdx::stop_token);

  template <typename EventLoop, http::HttpClient HttpClient>
  static Task<std::string> GetSession(
      const EventLoop& event_loop, const HttpClient& http,
      UserCredential credential, AuthData auth_data,
      stdx::stop_token stop_token = stdx::stop_token()) {
    Data d(event_loop, http, auth_data);
    co_return co_await GetSession(d, credential, std::move(stop_token));
  }

 private:
  static constexpr auto kLogin = static_cast<void (::mega::MegaClient::*)(
      const char*, const ::mega::byte*, const char*)>(
      &::mega::MegaClient::login);
  static constexpr auto kLoginWithSalt =
      static_cast<void (::mega::MegaClient::*)(const char*, const char*,
                                               std::string*, const char*)>(
          &::mega::MegaClient::login2);
  static constexpr auto kSessionLogin =
      static_cast<void (::mega::MegaClient::*)(const ::mega::byte*, int)>(
          &::mega::MegaClient::login);

  struct ReadData {
    std::deque<std::string> buffer;
    std::exception_ptr exception;
    Semaphore semaphore;
    bool paused = true;
    int size = 0;
  };

  struct Data;

  struct App : ::mega::MegaApp {
    static constexpr int kBufferSize = 1 << 20;

    void prelogin_result(int version, std::string* email, std::string* salt,
                         ::mega::error e) final {
      last_result = std::make_tuple(version, email, salt, e);
      Resume();
    }

    void login_result(::mega::error e) final {
      last_result = std::make_tuple(e);
      Resume();
    }

    void fetchnodes_result(const ::mega::Error& e) final {
      last_result = std::make_tuple(e);
      Resume();
    }

    ::mega::dstime pread_failure(const ::mega::Error& e, int retry,
                                 void* user_data, ::mega::dstime) final {
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
        it->second->semaphore.resume();
        return ~static_cast<::mega::dstime>(0);
      } else {
        Retry(1 << (retry / 2));
        return 1 << (retry / 2);
      }
    }

    bool pread_data(::mega::byte* data, m_off_t length, m_off_t, m_off_t,
                    m_off_t, void* user_data) final {
      auto it = read_data.find(reinterpret_cast<intptr_t>(user_data));
      if (it == std::end(read_data)) {
        return false;
      }
      it->second->buffer.emplace_back(reinterpret_cast<const char*>(data),
                                      length);
      it->second->size += static_cast<int>(length);
      if (it->second->size >= kBufferSize) {
        it->second->paused = true;
        it->second->semaphore.resume();
        return false;
      }
      it->second->semaphore.resume();
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

    void Resume() {
      auto it = semaphore.find(client->restag);
      if (it != std::end(semaphore)) {
        it->second->resume();
      }
    }

    auto GetSemaphore(int tag) {
      auto result =
          coro::util::MakePointer(new Semaphore, [this, tag](Semaphore* s) {
            semaphore.erase(tag);
            delete s;
          });
      semaphore.insert({tag, result.get()});
      return result;
    }

    explicit App(Data* d) : d(d) {}

    std::unordered_map<int, Semaphore*> semaphore;
    std::any last_result;
    std::unordered_map<intptr_t, std::shared_ptr<ReadData>> read_data;
    Data* d;
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
    AuthToken auth_token;
    std::optional<Promise<int>> current_login;

    void OnEvent();

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
                      auth_data.app_name.c_str(), 0) {}

    ~Data() { stop_source.request_stop(); }
  };

  template <typename>
  struct Invoke;

  template <typename... Result>
  struct Invoke<coro::util::TypeList<Result...>> {
    template <auto Method, typename... Args>
    static Task<std::tuple<std::remove_cvref_t<Result>...>> Call(
        Data& d, stdx::stop_token stop_token, Args... args) {
      auto tag = d.mega_client.nextreqtag();
      (d.mega_client.*Method)(args...);
      d.OnEvent();
      auto semaphore = d.mega_app.GetSemaphore(tag);
      stdx::stop_callback callback(stop_token, [&] { semaphore->resume(); });
      auto& semaphore_ref = *semaphore;
      co_await semaphore_ref;
      if (stop_token.stop_requested()) {
        throw InterruptedException();
      }
      co_return std::any_cast<std::tuple<std::remove_cvref_t<Result>...>>(
          d.mega_app.last_result);
    }
  };

  template <auto Method, auto ResultMethod, typename... Args>
  static auto Do(Data& d, stdx::stop_token stop_token, Args&&... args) {
    return Invoke<coro::util::ArgumentListTypeT<decltype(ResultMethod)>>::
        template Call<Method>(d, std::move(stop_token),
                              std::forward<Args>(args)...);
  }

  static const char* GetErrorDescription(::mega::error e);

  Task<> CoCheck(::mega::error e, stdx::stop_token stop_token) {
    co_await d_->wait_(0, stop_token);
    Check(e);
  }

  static void Check(::mega::error e) {
    if (e != ::mega::API_OK) {
      throw CloudException(GetErrorDescription(e));
    }
  }

  ::mega::Node* GetNode(::mega::handle) const;

  Task<> EnsureLoggedIn(stdx::stop_token);
  Task<> LogIn();
  static Task<std::string> GetSession(Data&, UserCredential, stdx::stop_token);

  AuthToken auth_token_;
  std::unique_ptr<Data> d_;
};

template <>
struct CreateCloudProvider<Mega> {
  template <typename CloudFactory, typename... Args>
  auto operator()(const CloudFactory& factory, Mega::AuthToken auth_token,
                  Args&&...) const {
    return CloudProvider(Mega(*factory.event_loop_, *factory.http_,
                              std::move(auth_token),
                              factory.auth_data_.template operator()<Mega>()));
  }
};

namespace util {
template <>
inline auto ToJson<Mega::AuthToken>(Mega::AuthToken token) {
  nlohmann::json json;
  json["email"] = std::move(token.email);
  json["session"] = http::ToBase64(token.session);
  return json;
}

template <>
inline auto ToAuthToken<Mega::AuthToken>(const nlohmann::json& json) {
  return Mega::AuthToken{
      .email = json.at("email"),
      .session = http::FromBase64(std::string(json.at("session")))};
}

template <typename EventLoop, http::HttpClient HttpClient>
class MegaAuthHandler {
 public:
  MegaAuthHandler(const EventLoop& event_loop, const HttpClient& http,
                  Mega::AuthData auth_data)
      : event_loop_(&event_loop),
        http_(&http),
        auth_data_(std::move(auth_data)) {}

  Task<std::variant<http::Response<>, Mega::AuthToken>> operator()(
      coro::http::Request<> request, coro::stdx::stop_token stop_token) const {
    if (request.method == http::Method::kPost) {
      auto query =
          http::ParseQuery(co_await http::GetBody(std::move(*request.body)));
      auto it1 = query.find("email");
      auto it2 = query.find("password");
      if (it1 != std::end(query) && it2 != std::end(query)) {
        auto it3 = query.find("twofactor");
        Mega::UserCredential credential = {
            .email = it1->second,
            .password = it2->second,
            .twofactor = it3 != std::end(query)
                             ? std::make_optional(it3->second)
                             : std::nullopt};
        std::string session = co_await Mega::GetSession(*event_loop_, *http_,
                                                        std::move(credential),
                                                        auth_data_, stop_token);
        co_return Mega::AuthToken{.email = it1->second,
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
  Mega::AuthData auth_data_;
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
inline Mega::AuthData GetAuthData<Mega>() {
  return {.api_key = "ZVhB0Czb", .app_name = "coro-cloudstorage"};
}

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_MEGA_H
