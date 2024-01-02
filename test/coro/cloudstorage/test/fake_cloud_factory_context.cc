#include "fake_cloud_factory_context.h"

#include "coro/cloudstorage/test/test_utils.h"
#include "coro/cloudstorage/util/cloud_factory_context.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/util/event_loop.h"

namespace coro::cloudstorage::test {

namespace {

using ::coro::cloudstorage::util::AuthData;
using ::coro::cloudstorage::util::CloudFactoryContext;
using ::coro::cloudstorage::util::CloudProviderAccount;
using ::coro::cloudstorage::util::StrCat;
using ::coro::http::CreateHttpServer;
using ::coro::http::GetBody;
using ::coro::util::EventLoop;
using ::coro::util::TcpServer;

class AccountListener {
 public:
  explicit AccountListener(
      std::vector<CloudProviderAccount>* accounts = nullptr)
      : accounts_(accounts) {}

  void OnCreate(CloudProviderAccount account) {
    if (accounts_) {
      accounts_->push_back(std::move(account));
    }
  }

  void OnDestroy(const CloudProviderAccount& account) {
    if (accounts_) {
      accounts_->erase(
          std::find_if(accounts_->begin(), accounts_->end(),
                       [&](const auto& e) { return e.id() == account.id(); }));
    }
  }

 private:
  std::vector<CloudProviderAccount>* accounts_;
};

CloudFactoryContext CreateContext(const EventLoop* event_loop,
                                  http::Http http) {
  return CloudFactoryContext(
      {.event_loop = event_loop,
       .config_path = StrCat(kTestRunDirectory, "/config.sqlite"),
       .cache_path = StrCat(kTestRunDirectory, "/cache.sqlite"),
       .auth_data =
           AuthData("http://localhost:12345", nlohmann::json::parse(R"js({
             "google": {
               "client_id": "google_client_id",
               "client_secret": "google_client_secret"
             },
             "box": {
               "client_id": "box_client_id",
               "client_secret": "box_client_secret"
             },
             "dropbox": {
                "client_id": "dropbox_client_id",
                "client_secret": "dropbox_client_secret"
             },
             "mega": {
               "api_key": "mega_api_key",
               "app_name": "mega_app_name"
             },
             "onedrive": {
               "client_id": "onedrive_client_key",
               "client_secret": "onedrive_client_secret"
             },
             "pcloud": {
               "client_id": "pcloud_client_id",
               "client_secret": "pcloud_client_secret"
             },
             "yandex": {
               "client_id": "yandex_client_id",
               "client_secret": "yandex_client_secret"
             },
             "youtube": {
               "client_id": "youtube_client_id",
               "client_secret": "youtube_client_secret"
             }
           })js")),
       .http = std::move(http)});
}

}  // namespace

CloudProviderAccount TestCloudProviderAccount::GetAccount() const {
  for (const auto& account : accounts_) {
    if (account.id() == id_) {
      return account;
    }
  }
  throw CloudException(CloudException::Type::kNotFound);
}

FakeCloudFactoryContext::FakeCloudFactoryContext(FakeHttpClient http)
    : thread_([this, http = std::move(http)]() mutable {
        RunThread(std::move(http));
      }) {
  ready_.get_future().get();
}

FakeCloudFactoryContext::~FakeCloudFactoryContext() {
  state_->event_loop().RunOnEventLoop([&] { state_->quit().SetValue(); });
  thread_.join();
}

ResponseContent FakeCloudFactoryContext::Fetch(
    http::Request<std::string> request) {
  request.url = StrCat(*address_, request.url);
  return state_->event_loop().Do(
      [this, request = std::move(request)]() mutable -> Task<ResponseContent> {
        auto response = co_await state_->http().Fetch(std::move(request));
        auto body = co_await GetBody(std::move(response.body));
        co_return ResponseContent{.status = response.status,
                                  .headers = std::move(response.headers),
                                  .body = std::move(body)};
      });
}

TestCloudProviderAccount FakeCloudFactoryContext::GetAccount(
    CloudProviderAccount::Id id) {
  return {&state_->event_loop(), std::move(id), state_->accounts()};
}

void FakeCloudFactoryContext::RunThread(FakeHttpClient http) {
  state_.emplace(std::move(http));
  auto at_scope_exit = [&] { state_.reset(); };
  std::exception_ptr exception;
  RunTask([&]() -> Task<> {
    try {
      auto http_server = CreateHttpServer(
          state_->context().CreateAccountManagerHandler(
              AccountListener{&state_->accounts()}),
          &state_->event_loop(),
          TcpServer::Config{.address = "127.0.0.1", .port = 0});
      address_ = "http://127.0.0.1:" + std::to_string(http_server.GetPort());
      ready_.set_value();
      co_await state_->quit();
      co_await http_server.Quit();
    } catch (...) {
      exception = std::current_exception();
    }
  });
  state_->event_loop().EnterLoop();
  if (exception) {
    std::rethrow_exception(exception);
  }
}

FakeCloudFactoryContext::ThreadState::ThreadState(FakeHttpClient http)
    : context_(CreateContext(&event_loop_, coro::http::Http(std::move(http)))) {
}

}  // namespace coro::cloudstorage::test