#include <coro/cloudstorage/cloud_factory.h>
#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/providers/google_drive.h>
#include <coro/cloudstorage/providers/mega.h>
#include <coro/generator.h>
#include <coro/http/curl_http.h>
#include <coro/http/http_parse.h>
#include <coro/http/http_server.h>
#include <coro/promise.h>
#include <coro/stdx/coroutine.h>
#include <coro/util/make_pointer.h>
#include <coro/wait_task.h>

#include <csignal>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using ::coro::Generator;
using ::coro::InterruptedException;
using ::coro::Promise;
using ::coro::Semaphore;
using ::coro::Task;
using ::coro::Wait;
using ::coro::cloudstorage::CloudProvider;
using ::coro::cloudstorage::GoogleDrive;
using ::coro::cloudstorage::MakeCloudFactory;
using ::coro::cloudstorage::MakeCloudProvider;
using ::coro::cloudstorage::Mega;
using ::coro::cloudstorage::util::MakeAuthManager;
using ::coro::http::CurlHttp;
using ::coro::http::DecodeUri;
using ::coro::http::FromBase64;
using ::coro::http::GetExtension;
using ::coro::http::GetMimeType;
using ::coro::http::HttpServer;
using ::coro::http::ParseQuery;
using ::coro::http::ParseRange;
using ::coro::http::ParseUri;
using ::coro::http::Request;
using ::coro::http::Response;
using ::coro::http::ToBase64;
using ::coro::util::MakePointer;

constexpr std::string_view kRedirectUri = "http://localhost:12345";
constexpr std::string_view kTokenFile = "access-token.json";
constexpr std::string_view kGoogleDriveClientId =
    R"(646432077068-hmvk44qgo6d0a64a5h9ieue34p3j2dcv.apps.googleusercontent.com)";
constexpr std::string_view kGoogleDriveClientSecret =
    R"(1f0FG5ch-kKOanTAv1Bqdp9U)";

template <typename>
struct AuthData;

template <>
struct AuthData<GoogleDrive> {
  auto operator()() const {
    return GoogleDrive::Auth::AuthData{
        .client_id = std::string(kGoogleDriveClientId),
        .client_secret = std::string(kGoogleDriveClientSecret),
        .redirect_uri = std::string(kRedirectUri) + "/google"};
  }
};

template <>
struct AuthData<Mega> {
  auto operator()() const {
    return Mega::Auth::AuthData{.api_key = "ZVhB0Czb",
                                .app_name = "coro-cloudstorage"};
  }
};

template <typename CloudProvider, coro::http::HttpClient HttpClient>
struct AuthHandler {
  Task<Response<>> operator()(const coro::http::Request<>& request,
                              coro::stdx::stop_token stop_token) const {
    auto query = ParseQuery(ParseUri(request.url).query.value_or(""));
    if constexpr (std::is_same_v<CloudProvider, Mega>) {
      auto it1 = query.find("email");
      auto it2 = query.find("password");
      if (it1 != std::end(query) && it2 != std::end(query)) {
        auto it3 = query.find("twofactor");
        Mega::UserCredential credential = {
            .email = it1->second,
            .password_hash = Mega::GetPasswordHash(it2->second),
            .twofactor = it3 != std::end(query)
                             ? std::make_optional(it3->second)
                             : std::nullopt};
        auto session =
            co_await Mega::GetSession(event_loop, http, std::move(credential),
                                      AuthData<Mega>{}(), stop_token);
        token = {.session = std::move(session)};
      } else {
        throw std::logic_error("invalid credentials");
      }
    } else {
      auto it = query.find("code");
      if (it != std::end(query)) {
        token = co_await CloudProvider::Auth::ExchangeAuthorizationCode(
            http, AuthData<CloudProvider>{}(), it->second, stop_token);
      }
    }
    quit.resume();
    co_return Response<>{.status = 200,
                         .body = GenerateBody(std::move(stop_token))};
  }

  void OnQuit() const { quit.resume(); }

  Generator<std::string> GenerateBody(coro::stdx::stop_token stop_token) const {
    try {
      for (int i = 0; i < 5; i++) {
        co_await Wait(event_loop, 1000, stop_token);
        co_yield "DUPA\n";
      }
    } catch (const InterruptedException&) {
      std::cerr << "INTERRUPTED RESPONSE\n";
    }
  }

  event_base* event_loop;
  HttpClient& http;
  Semaphore& quit;
  typename CloudProvider::Auth::AuthToken& token;
};

template <typename CloudProvider, coro::http::HttpClient HttpClient>
auto MakeAuthHandler(event_base* event_loop, HttpClient& http,
                     Semaphore& semaphore,
                     typename CloudProvider::Auth::AuthToken& token) {
  return AuthHandler<CloudProvider, HttpClient>{event_loop, http, semaphore,
                                                token};
}

template <typename CloudProvider>
class ProxyHandler {
 public:
  using File = typename CloudProvider::File;
  using Item = typename CloudProvider::Item;
  using Directory = typename CloudProvider::Directory;

  ProxyHandler(CloudProvider& provider, Semaphore& quit)
      : provider_(provider), quit_(quit) {}
  ProxyHandler(ProxyHandler&& handler)
      : provider_(handler.provider_),
        quit_(handler.quit_),
        shared_data_(std::move(handler.shared_data_)) {}

  ProxyHandler(const ProxyHandler&) = delete;
  ProxyHandler& operator=(const ProxyHandler&) = delete;

  ~ProxyHandler() {
    if (shared_data_) {
      shared_data_->stop_source.request_stop();
    }
  }

  Task<Response<>> operator()(const coro::http::Request<>& request,
                              coro::stdx::stop_token stop_token) const {
    std::cerr << "[" << request.method << "] " << request.url << " ";
    std::string path = DecodeUri(ParseUri(request.url).path.value_or(""));
    auto it = request.headers.find("Range");
    coro::http::Range range = {};
    if (it != std::end(request.headers)) {
      range = ParseRange(it->second);
      std::cerr << it->second;
    }
    std::cerr << "\n";
    auto item = co_await GetItem(path, stop_token);
    if (std::holds_alternative<File>(item)) {
      auto file = std::get<File>(item);
      std::unordered_multimap<std::string, std::string> headers = {
          {"Content-Type", file.mime_type},
          {"Content-Disposition", "inline; filename=\"" + file.name + "\""},
          {"Access-Control-Allow-Origin", "*"},
          {"Access-Control-Allow-Headers", "*"}};
      if (file.size) {
        if (!range.end) {
          range.end = *file.size - 1;
        }
        headers.insert({"Accept-Ranges", "bytes"});
        headers.insert(
            {"Content-Length", std::to_string(*range.end - range.start + 1)});
        if (it != std::end(request.headers)) {
          std::stringstream range_str;
          range_str << "bytes " << range.start << "-" << *range.end << "/"
                    << *file.size;
          headers.insert({"Content-Range", std::move(range_str).str()});
        }
      }
      co_return Response<>{
          .status = it == std::end(request.headers) || !file.size ? 200 : 206,
          .headers = std::move(headers),
          .body = provider_.GetFileContent(file, range, stop_token)};
    } else {
      auto directory = std::get<Directory>(item);
      co_return Response<>{
          .status = 200,
          .headers = {{"Content-Type", "text/html"}},
          .body = GetDirectoryContent(path, directory, stop_token)};
    }
  }

  void OnQuit() const {
    std::cerr << "QUITTING\n";
    quit_.resume();
  }

  Task<Item> GetItem(std::string path,
                     coro::stdx::stop_token stop_token) const {
    auto it = shared_data_->tasks.find(path);
    if (it == std::end(shared_data_->tasks)) {
      auto promise = Promise<Item>(
          [path, stop_token = shared_data_->stop_source.get_token(),
           this]() -> Task<Item> {
            co_return co_await provider_.GetItemByPath(path, stop_token);
          });
      it = shared_data_->tasks.insert({path, std::move(promise)}).first;
    }
    co_return co_await it->second.Get(stop_token);
  }

  Generator<std::string> GetDirectoryContent(
      std::string path, Directory directory,
      coro::stdx::stop_token stop_token) const {
    co_yield "<!DOCTYPE html>"
        "<html><head><meta charset='UTF-8'></head><body><table>";
    if (!path.empty() && path.back() != '/') {
      path += '/';
    }
    FOR_CO_AWAIT(
        const auto& page, provider_.ListDirectory(directory, stop_token), {
          for (const auto& item : page.items) {
            auto name = std::visit([](auto item) { return item.name; }, item);
            std::string type =
                std::holds_alternative<Directory>(item) ? "DIR" : "FILE";
            co_yield "<tr><td>[" + type + "]</td><td><a href='" + path +
                coro::http::EncodeUri(name) + "'>" + name + "</a></td></tr>";
          }
        });
    co_yield "</table></body></html>";
  }

 private:
  CloudProvider& provider_;
  Semaphore& quit_;
  struct SharedData {
    std::unordered_map<std::string, Promise<Item>> tasks;
    coro::stdx::stop_source stop_source;
  };
  std::shared_ptr<SharedData> shared_data_ = std::make_shared<SharedData>();
};

template <typename CloudProvider>
auto MakeProxyHandler(CloudProvider& cloud_provider, Semaphore& semaphore) {
  return ProxyHandler<CloudProvider>(cloud_provider, semaphore);
}

template <typename AuthToken>
auto ToJson(AuthToken token) {
  nlohmann::json json;
  json["access_token"] = std::move(token.access_token);
  json["refresh_token"] = std::move(token.refresh_token);
  return json;
}

template <>
auto ToJson<Mega::AuthToken>(Mega::AuthToken token) {
  nlohmann::json json;
  json["session"] = ToBase64(token.session);
  return json;
}

template <typename AuthToken>
auto ToAuthToken(const nlohmann::json& json) {
  return AuthToken{.access_token = json.at("access_token"),
                   .refresh_token = json.at("refresh_token")};
}

template <>
auto ToAuthToken<Mega::AuthToken>(const nlohmann::json& json) {
  return Mega::AuthToken{.session =
                             FromBase64(std::string(json.at("session")))};
}

template <typename AuthToken>
std::optional<AuthToken> LoadToken() {
  std::ifstream token_file{std::string(kTokenFile)};
  if (token_file) {
    try {
      nlohmann::json json;
      token_file >> json;
      return ToAuthToken<AuthToken>(json);
    } catch (const nlohmann::json::exception&) {
    }
  }
  return std::nullopt;
}

template <typename AuthToken>
void SaveToken(AuthToken token) {
  std::ofstream{std::string(kTokenFile)} << ToJson(std::move(token));
}

template <typename CloudProvider, coro::http::HttpClient HttpClient>
Task<typename CloudProvider::Auth::AuthToken> GetAuthToken(
    event_base* event_loop, HttpClient& http) {
  using AuthToken = typename CloudProvider::Auth::AuthToken;
  {
    auto token = LoadToken<AuthToken>();
    if (token) {
      co_return* token;
    }
  }
  Semaphore quit_semaphore;
  AuthToken token;
  HttpServer http_server(
      event_loop, {.address = "0.0.0.0", .port = 12345},
      MakeAuthHandler<CloudProvider>(event_loop, http, quit_semaphore, token));
  if constexpr (std::is_same_v<CloudProvider, Mega>) {
    std::cerr << "AUTHORIZATION URL: http://localhost:12345\n";
  } else {
    std::cerr << "AUTHORIZATION URL: "
              << CloudProvider::Auth::GetAuthorizationUrl(
                     AuthData<CloudProvider>{}())
              << "\n";
  }
  co_await quit_semaphore;
  co_await http_server.Quit();
  SaveToken(token);
  co_return token;
}

Task<> CoMain(event_base* event_loop) noexcept {
  try {
    using CloudProvider = Mega;

    CurlHttp http(event_loop);
    Semaphore quit;
    auto cloud_factory = MakeCloudFactory<AuthData>(event_loop, http);

    auto auth_token = co_await GetAuthToken<CloudProvider>(event_loop, http);
    auto cloud_provider = cloud_factory.Create<CloudProvider>(
        auth_token, [](auto token) { SaveToken(token); });
    HttpServer http_server(event_loop, {.address = "0.0.0.0", .port = 12345},
                           MakeProxyHandler(cloud_provider, quit));
    co_await quit;
  } catch (const coro::http::HttpException& exception) {
    if (exception.status() == 401) {
      std::remove(std::string(kTokenFile).c_str());
    }
  } catch (const std::exception& exception) {
    std::cerr << "EXCEPTION: " << exception.what() << "\n";
  }
}

int main() {
#ifdef _WIN32
  WORD version_requested = MAKEWORD(2, 2);
  WSADATA wsa_data;

  (void)WSAStartup(version_requested, &wsa_data);
#endif

#ifdef SIGPIPE
  signal(SIGPIPE, SIG_IGN);
#endif

  auto base = MakePointer(event_base_new(), event_base_free);
  CoMain(base.get());
  return event_base_dispatch(base.get());
}