#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/providers/google_drive.h>
#include <coro/generator.h>
#include <coro/http/curl_http.h>
#include <coro/http/http_parse.h>
#include <coro/http/http_server.h>
#include <coro/stdx/coroutine.h>
#include <coro/util/make_pointer.h>
#include <coro/wait_task.h>

#include <csignal>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

using ::coro::Generator;
using ::coro::InterruptedException;
using ::coro::Semaphore;
using ::coro::Task;
using ::coro::Wait;
using ::coro::cloudstorage::CloudProvider;
using ::coro::cloudstorage::GoogleDrive;
using ::coro::http::CurlHttp;
using ::coro::http::DecodeUri;
using ::coro::http::GetExtension;
using ::coro::http::GetMimeType;
using ::coro::http::HttpServer;
using ::coro::http::ParseQuery;
using ::coro::http::ParseRange;
using ::coro::http::ParseUri;
using ::coro::http::Request;
using ::coro::http::Response;
using ::coro::util::MakePointer;

constexpr std::string_view kRedirectUri = "http://localhost:12345";
constexpr std::string_view kTokenFile = "access-token.json";
constexpr std::string_view kGoogleDriveClientId =
    R"(646432077068-hmvk44qgo6d0a64a5h9ieue34p3j2dcv.apps.googleusercontent.com)";
constexpr std::string_view kGoogleDriveClientSecret =
    R"(1f0FG5ch-kKOanTAv1Bqdp9U)";

GoogleDrive::AuthData GetGoogleDriveAuthData() {
  return {.client_id = std::string(kGoogleDriveClientId),
          .client_secret = std::string(kGoogleDriveClientSecret),
          .redirect_uri = std::string(kRedirectUri) + "/google"};
}

template <coro::http::HttpClient HttpClient>
struct AuthHandler {
  Task<Response<>> operator()(const coro::http::Request<>& request,
                              coro::stdx::stop_token stop_token) const {
    auto query = ParseQuery(ParseUri(request.url).query.value_or(""));
    auto it = query.find("code");
    if (it != std::end(query)) {
      co_await ProcessCode(it->second, stop_token);
    }
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

  Task<> ProcessCode(std::string_view code,
                     coro::stdx::stop_token stop_token) const {
    token = co_await GoogleDrive(GetGoogleDriveAuthData())
                .ExchangeAuthorizationCode(http, std::string(code), stop_token);
    quit.resume();
  }

  event_base* event_loop;
  HttpClient& http;
  Semaphore& quit;
  GoogleDrive::Token& token;
};

template <coro::http::HttpClient HttpClient>
struct ProxyHandler {
  Task<Response<>> operator()(const coro::http::Request<>& request,
                              coro::stdx::stop_token stop_token) const {
    using ProviderImpl = GoogleDrive;
    std::cerr << "[" << request.method << "] " << request.url << "\n";
    CloudProvider<ProviderImpl> provider(GetGoogleDriveAuthData());
    std::string path = DecodeUri(ParseUri(request.url).path.value_or(""));
    auto it = request.headers.find("Range");
    coro::http::Range range = {};
    if (it != std::end(request.headers)) {
      range = ParseRange(it->second);
    }
    auto item =
        co_await provider.GetItemByPath(http, access_token, path, stop_token);
    const auto& file = std::get<ProviderImpl::File>(item);
    std::unordered_multimap<std::string, std::string> headers = {
        {"Content-Type", GetMimeType(GetExtension(std::move(path)))},
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
        .body = provider.GetFileContent(http, access_token, file, range,
                                        stop_token)};
  }

  void OnQuit() const {
    std::cerr << "QUITTING\n";
    quit.resume();
  }

  event_base* event_loop;
  HttpClient& http;
  std::string access_token;
  Semaphore& quit;
};

template <coro::http::HttpClient HttpClient>
Task<std::string> GetAccessToken(event_base* event_loop, HttpClient& http) {
  {
    std::ifstream token_file{std::string(kTokenFile)};
    if (token_file) {
      nlohmann::json json;
      token_file >> json;
      auto token =
          co_await CloudProvider<GoogleDrive>(GetGoogleDriveAuthData())
              .RefreshAccessToken(http, std::string(json["refresh_token"]));
      co_return token.access_token;
    }
  }
  Semaphore quit_semaphore;
  GoogleDrive::Token token;
  HttpServer http_server(
      event_loop, {.address = "0.0.0.0", .port = 12345},
      AuthHandler<HttpClient>{event_loop, http, quit_semaphore, token});
  std::cerr << "AUTHORIZATION URL: "
            << GoogleDrive::GetAuthorizationUrl(GetGoogleDriveAuthData())
            << "\n";
  co_await quit_semaphore;
  co_await http_server.Quit();

  std::ofstream token_file{std::string(kTokenFile)};
  nlohmann::json json;
  json["access_token"] = token.access_token;
  json["refresh_token"] = token.refresh_token;
  token_file << json;

  co_return token.access_token;
}

Task<> CoMain(event_base* event_loop) noexcept {
  try {
    CurlHttp http(event_loop);
    std::string access_token = co_await GetAccessToken(event_loop, http);
    std::cerr << "ACCESS TOKEN: " << access_token << "\n";
    Semaphore quit;
    HttpServer http_server(
        event_loop, {.address = "0.0.0.0", .port = 12345},
        ProxyHandler<CurlHttp>{event_loop, http, access_token, quit});
    co_await quit;
  } catch (const coro::http::HttpException& exception) {
    if (exception.status() == 401) {
      std::remove(std::string(kTokenFile).c_str());
    }
  } catch (const std::exception& exception) {
    std::cerr << "EXCEPTION: " << exception.what();
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