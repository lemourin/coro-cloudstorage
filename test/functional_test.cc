#include <coro/http/curl_http.h>
#include <coro/http/http.h>
#include <coro/http/http_server.h>
#include <fmt/format.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <span>

#include "coro/cloudstorage/util/cloud_factory_context.h"
#include "fake_http_client.h"
#include "test_utils.h"

namespace coro::cloudstorage {
namespace {

using Request = http::Request<>;
using Response = http::Response<>;

using ::coro::cloudstorage::test::AreVideosEquiv;
using ::coro::cloudstorage::test::FakeHttpClient;
using ::coro::cloudstorage::test::GetTestFileContent;
using ::coro::cloudstorage::test::HttpRequest;
using ::coro::cloudstorage::test::kTestDataDirectory;
using ::coro::cloudstorage::test::kTestRunDirectory;
using ::coro::cloudstorage::test::ResponseContent;
using ::coro::cloudstorage::util::AbstractCloudProvider;
using ::coro::cloudstorage::util::AccountManagerHandler;
using ::coro::cloudstorage::util::AuthData;
using ::coro::cloudstorage::util::CloudFactoryContext;
using ::coro::cloudstorage::util::CloudProviderAccount;
using ::coro::cloudstorage::util::CreateDirectory;
using ::coro::cloudstorage::util::GetDirectoryPath;
using ::coro::cloudstorage::util::StrCat;
using ::coro::http::CreateHttpServer;
using ::coro::http::CurlHttp;
using ::coro::http::GetBody;
using ::coro::http::Http;
using ::coro::util::EventLoop;
using ::coro::util::TcpServer;
using ::testing::StrEq;

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

class TestCloudProviderAccount {
 public:
  template <typename F>
  auto WithAccount(F func) const {
    return event_loop_->Do([this, func = std::move(func)]() mutable {
      return std::move(func)(GetAccount());
    });
  }

  auto GetRoot() const {
    return WithAccount([](CloudProviderAccount account) {
      return account.provider()->GetRoot(stdx::stop_token());
    });
  }

  template <typename... Ts>
  auto ListDirectoryPage(Ts... args) const {
    return WithAccount(
        [... args = std::move(args)](CloudProviderAccount account) {
          return account.provider()->ListDirectoryPage(std::move(args)...,
                                                       stdx::stop_token());
        });
  }

 private:
  friend class TestHelper;

  TestCloudProviderAccount(EventLoop* event_loop, CloudProviderAccount::Id id,
                           std::span<const CloudProviderAccount> accounts)
      : event_loop_(event_loop), id_(std::move(id)), accounts_(accounts) {}

  CloudProviderAccount GetAccount() const {
    for (const auto& account : accounts_) {
      if (account.id() == id_) {
        return account;
      }
    }
    throw CloudException(CloudException::Type::kNotFound);
  }

  EventLoop* event_loop_;
  CloudProviderAccount::Id id_;
  std::span<const CloudProviderAccount> accounts_;
};

class TestHelper {
 public:
  explicit TestHelper(FakeHttpClient http = {})
      : thread_([this, http = std::move(http)]() mutable {
          RunThread(std::move(http));
        }) {
    ready_.get_future().get();
  }
  TestHelper(const TestHelper&) = delete;
  TestHelper(TestHelper&&) = delete;
  TestHelper& operator=(const TestHelper&) = delete;
  TestHelper& operator=(TestHelper&&) = delete;
  ~TestHelper() {
    state_->event_loop().RunOnEventLoop([&] { state_->quit().SetValue(); });
    thread_.join();
  }

  ResponseContent Fetch(http::Request<std::string> request) {
    request.url = StrCat(*address_, request.url);
    return state_->event_loop().Do(
        [this,
         request = std::move(request)]() mutable -> Task<ResponseContent> {
          auto response = co_await state_->http().Fetch(std::move(request));
          auto body = co_await GetBody(std::move(response.body));
          co_return ResponseContent{.status = response.status,
                                    .headers = std::move(response.headers),
                                    .body = std::move(body)};
        });
  }

  TestCloudProviderAccount GetAccount(CloudProviderAccount::Id id) {
    return {&state_->event_loop(), std::move(id), state_->accounts()};
  }

 private:
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
        accounts_->erase(std::find_if(
            accounts_->begin(), accounts_->end(),
            [&](const auto& e) { return e.id() == account.id(); }));
      }
    }

   private:
    std::vector<CloudProviderAccount>* accounts_;
  };

  void RunThread(FakeHttpClient http) {
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

  class ThreadState {
   public:
    explicit ThreadState(FakeHttpClient http)
        : context_(CreateContext(&event_loop_, Http(std::move(http)))) {}

    EventLoop& event_loop() { return event_loop_; }
    Http& http() { return http_; }
    CloudFactoryContext& context() { return context_; }
    Promise<void>& quit() { return quit_; }
    std::vector<CloudProviderAccount>& accounts() { return accounts_; }

   private:
    EventLoop event_loop_;
    Http http_{CurlHttp{&event_loop_}};
    CloudFactoryContext context_;
    Promise<void> quit_;
    std::vector<CloudProviderAccount> accounts_;
  };
  std::optional<ThreadState> state_;
  std::promise<void> ready_;
  std::optional<std::string> address_;
  std::thread thread_;
};

class FunctionalTest : public ::testing::Test {
 public:
  FunctionalTest() {
    std::filesystem::remove_all(kTestRunDirectory);
    std::filesystem::create_directory(kTestRunDirectory);
  }

  ~FunctionalTest() override { std::filesystem::remove_all(kTestRunDirectory); }
};

TEST_F(FunctionalTest, Runs) {
  TestHelper test_helper;
  auto response = test_helper.Fetch({.url = "/"});
  EXPECT_EQ(response.body, GetTestFileContent("empty_home_page.html"));
}

TEST_F(FunctionalTest, CreateAccount) {
  FakeHttpClient http;
  http.Expect(HttpRequest("https://accounts.google.com/o/oauth2/token")
                  .WithBody(http::FormDataToString(
                      {{"grant_type", "authorization_code"},
                       {"client_secret", "google_client_secret"},
                       {"client_id", "google_client_id"},
                       {"redirect_uri", "http://localhost:12345/auth/google"},
                       {"code", "test"}}))
                  .WillReturn(R"js({
                    "access_token": "access_token",
                    "refresh_token": "refresh_token"
                  })js"))
      .Expect(HttpRequest("https://www.googleapis.com/drive/v3/"
                          "about?fields=user,storageQuota")
                  .WillReturn(R"js({
                    "user": {
                      "emailAddress": "test@gmail.com"
                    },
                    "storageQuota": {
                      "usage": "2137"
                    }
                  })js"));
  TestHelper test_helper(std::move(http));
  auto response = test_helper.Fetch({.url = "/auth/google?code=test"});

  EXPECT_THAT(response.status, 302);
  EXPECT_EQ(http::GetHeader(response.headers, "Location").value_or(""),
            "/list/google/test%40gmail.com/");
}

TEST_F(FunctionalTest, ListDirectory) {
  FakeHttpClient http;
  http.Expect(HttpRequest("https://accounts.google.com/o/oauth2/token")
                  .WillReturn(R"js({
                    "access_token": "access_token",
                    "refresh_token": "refresh_token"
                  })js"))
      .Expect(HttpRequest("https://www.googleapis.com/drive/v3/"
                          "about?fields=user,storageQuota")
                  .WillReturn(R"js({
                    "user": {
                      "emailAddress": "test@gmail.com"
                    },
                    "storageQuota": {
                      "usage": "2137"
                    }
                  })js"))
      .Expect(
          HttpRequest(
              fmt::format(
                  "https://www.googleapis.com/drive/v3/files?{}",
                  http::FormDataToString(
                      {{"q", "'root' in parents"},
                       {"fields",
                        "files(id,name,thumbnailLink,trashed,mimeType,iconLink,"
                        "parents,size,modifiedTime),kind,nextPageToken"}})))
              .WillReturn(R"js({
                "files": [
                  {
                    "id": "id1",
                    "name": "name1.mp4",
                    "thumbnailLink": "thumbnail-link",
                    "modifiedTime": "2023-12-29T12:29:03Z",
                    "parents": [ "root" ],
                    "size": "2137",
                    "mimeType": "video/mp4"
                  }
                ],
                "nextPageToken": "next-page-token"
              })js"));
  TestHelper test_helper(std::move(http));
  ASSERT_EQ(test_helper.Fetch({.url = "/auth/google?code=test"}).status, 302);

  auto account = test_helper.GetAccount(
      CloudProviderAccount::Id{.type = "google", .username = "test@gmail.com"});
  auto page_data =
      account.ListDirectoryPage(account.GetRoot(), /*page_token=*/std::nullopt);

  EXPECT_EQ(page_data.next_page_token, "next-page-token");

  ASSERT_EQ(page_data.items.size(), 1);
  const AbstractCloudProvider::File* file =
      std::get_if<AbstractCloudProvider::File>(&page_data.items[0]);
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->id, "id1");
  EXPECT_EQ(file->name, "name1.mp4");
  EXPECT_EQ(file->size, 2137);
  EXPECT_EQ(file->timestamp, 1703852943);
  EXPECT_EQ(file->mime_type, "video/mp4");
}

TEST_F(FunctionalTest, RestoresAccounts) {
  {
    FakeHttpClient http;
    http.Expect(HttpRequest("https://accounts.google.com/o/oauth2/token")
                    .WillReturn(R"js({
                      "access_token": "access_token",
                      "refresh_token": "refresh_token"
                    })js"))
        .Expect(HttpRequest("https://www.googleapis.com/drive/v3/"
                            "about?fields=user,storageQuota")
                    .WillReturn(R"js({
                      "user": {
                        "emailAddress": "test@gmail.com"
                      },
                      "storageQuota": {
                        "usage": "2137"
                      }
                    })js"));
    TestHelper test_helper(std::move(http));
    ASSERT_EQ(test_helper.Fetch({.url = "/auth/google?code=test"}).status, 302);
  }
  {
    TestHelper test_helper(FakeHttpClient{});
    auto account = test_helper.GetAccount(CloudProviderAccount::Id{
        .type = "google", .username = "test@gmail.com"});
    auto root = account.GetRoot();
    EXPECT_EQ(root.id, "root");
  }
}

TEST_F(FunctionalTest, GetThumbnailTest) {
  FakeHttpClient http;
  http.Expect(HttpRequest("https://accounts.google.com/o/oauth2/token")
                  .WillReturn(R"js({
                    "access_token": "access_token",
                    "refresh_token": "refresh_token"
                  })js"))
      .Expect(HttpRequest("https://www.googleapis.com/drive/v3/"
                          "about?fields=user,storageQuota")
                  .WillReturn(R"js({
                    "user": {
                      "emailAddress": "test@gmail.com"
                    },
                    "storageQuota": {
                      "usage": "2137"
                    }
                  })js"))
      .Expect(
          HttpRequest(
              fmt::format("https://www.googleapis.com/drive/v3/files/id1?{}",
                          http::FormDataToString(
                              {{"fields",
                                "id,name,thumbnailLink,trashed,mimeType,"
                                "iconLink,parents,size,modifiedTime"}})))
              .WillReturn(R"js({
                "id": "id1",
                "name": "name1.mp4",
                "thumbnailLink": "thumbnail-link",
                "modifiedTime": "2023-12-29T12:29:03Z",
                "parents": [ "root" ],
                "size": "2137",
                "mimeType": "video/mp4"
              })js"))
      .Expect(HttpRequest("thumbnail-link").WillReturn("thumbnail"));
  TestHelper test_helper(std::move(http));
  ASSERT_EQ(test_helper.Fetch({.url = "/auth/google?code=test"}).status, 302);

  auto account = test_helper.GetAccount(
      CloudProviderAccount::Id{.type = "google", .username = "test@gmail.com"});
  auto response =
      test_helper.Fetch({.url = "/thumbnail/google/test%40gmail.com/id1"});
  EXPECT_EQ(response.status, 200);
  EXPECT_THAT(response.body, "thumbnail");
}

TEST_F(FunctionalTest, ThumbnailGeneratorTest) {
  FakeHttpClient http;
  http.Expect(HttpRequest("https://accounts.google.com/o/oauth2/token")
                  .WillReturn(R"js({
                    "access_token": "access_token",
                    "refresh_token": "refresh_token"
                  })js"))
      .Expect(HttpRequest("https://www.googleapis.com/drive/v3/"
                          "about?fields=user,storageQuota")
                  .WillReturn(R"js({
                    "user": {
                      "emailAddress": "test@gmail.com"
                    },
                    "storageQuota": {
                      "usage": "2137"
                    }
                  })js"))
      .Expect(
          HttpRequest(
              fmt::format("https://www.googleapis.com/drive/v3/files/id1?{}",
                          http::FormDataToString(
                              {{"fields",
                                "id,name,thumbnailLink,trashed,mimeType,"
                                "iconLink,parents,size,modifiedTime"}})))
              .WillReturn(R"js({
                "id": "id1",
                "name": "name1.mp4",
                "thumbnailLink": "thumbnail-link",
                "modifiedTime": "2023-12-29T12:29:03Z",
                "parents": [ "root" ],
                "size": "2508570",
                "mimeType": "video/mp4"
              })js"))
      .Expect(HttpRequest("thumbnail-link")
                  .WillReturn(ResponseContent{.status = 404}))
      .Expect(
          HttpRequest("https://www.googleapis.com/drive/v3/files/id1?alt=media")
              .WillRespondToRangeRequestWith(GetTestFileContent("video.mp4")));
  TestHelper test_helper(std::move(http));
  ASSERT_EQ(test_helper.Fetch({.url = "/auth/google?code=test"}).status, 302);

  auto account = test_helper.GetAccount(
      CloudProviderAccount::Id{.type = "google", .username = "test@gmail.com"});
  auto response =
      test_helper.Fetch({.url = "/thumbnail/google/test%40gmail.com/id1"});
  EXPECT_EQ(response.status, 200);
  EXPECT_TRUE(AreVideosEquiv(response.body, GetTestFileContent("thumbnail.png"),
                             "png"));
}

TEST_F(FunctionalTest, MuxerTest) {
  FakeHttpClient http;
  http.Expect(HttpRequest("https://accounts.google.com/o/oauth2/token")
                  .WillReturn(R"js({
                    "access_token": "access_token",
                    "refresh_token": "refresh_token"
                  })js"))
      .Expect(HttpRequest("https://www.googleapis.com/drive/v3/"
                          "about?fields=user,storageQuota")
                  .WillReturn(R"js({
                    "user": {
                      "emailAddress": "test@gmail.com"
                    },
                    "storageQuota": {
                      "usage": "2137"
                    }
                  })js"))
      .Expect(
          HttpRequest(
              fmt::format("https://www.googleapis.com/drive/v3/files/id1?{}",
                          http::FormDataToString(
                              {{"fields",
                                "id,name,thumbnailLink,trashed,mimeType,"
                                "iconLink,parents,size,modifiedTime"}})))
              .WillReturn(R"js({
                "id": "id1",
                "name": "video.mp4",
                "thumbnailLink": "thumbnail-link",
                "modifiedTime": "2023-12-29T12:29:03Z",
                "parents": [ "root" ],
                "size": "2508570",
                "mimeType": "video/mp4"
              })js"))
      .Expect(
          HttpRequest(
              fmt::format("https://www.googleapis.com/drive/v3/files/id2?{}",
                          http::FormDataToString(
                              {{"fields",
                                "id,name,thumbnailLink,trashed,mimeType,"
                                "iconLink,parents,size,modifiedTime"}})))
              .WillReturn(R"js({
                "id": "id2",
                "name": "audio.m4a",
                "iconLink": "icon-link",
                "modifiedTime": "2023-12-29T12:29:03Z",
                "parents": [ "root" ],
                "size": "245256",
                "mimeType": "audio/mp4"
              })js"))
      .Expect(
          HttpRequest("https://www.googleapis.com/drive/v3/files/id1?alt=media")
              .WillRespondToRangeRequestWith(GetTestFileContent("video.mp4")))
      .Expect(
          HttpRequest("https://www.googleapis.com/drive/v3/files/id2?alt=media")
              .WillRespondToRangeRequestWith(GetTestFileContent("audio.m4a")));
  TestHelper test_helper(std::move(http));
  ASSERT_EQ(test_helper.Fetch({.url = "/auth/google?code=test"}).status, 302);

  auto response = test_helper.Fetch(
      {.url = fmt::format(
           "/mux?{}",
           http::FormDataToString({{"video_account_type", "google"},
                                   {"video_account_name", "test@gmail.com"},
                                   {"audio_account_type", "google"},
                                   {"audio_account_name", "test@gmail.com"},
                                   {"video_id", "id1"},
                                   {"audio_id", "id2"},
                                   {"format", "mp4"},
                                   {"seekable", "false"}}))});
  EXPECT_EQ(response.status, 200);
  EXPECT_TRUE(AreVideosEquiv(
      response.body, GetTestFileContent("muxed-nonseekable.mp4"), "mov"));
}

TEST_F(FunctionalTest, MuxerSeekableOutput) {
  FakeHttpClient http;
  http.Expect(HttpRequest("https://accounts.google.com/o/oauth2/token")
                  .WillReturn(R"js({
                    "access_token": "access_token",
                    "refresh_token": "refresh_token"
                  })js"))
      .Expect(HttpRequest("https://www.googleapis.com/drive/v3/"
                          "about?fields=user,storageQuota")
                  .WillReturn(R"js({
                    "user": {
                      "emailAddress": "test@gmail.com"
                    },
                    "storageQuota": {
                      "usage": "2137"
                    }
                  })js"))
      .Expect(
          HttpRequest(
              fmt::format("https://www.googleapis.com/drive/v3/files/id1?{}",
                          http::FormDataToString(
                              {{"fields",
                                "id,name,thumbnailLink,trashed,mimeType,"
                                "iconLink,parents,size,modifiedTime"}})))
              .WillReturn(R"js({
                "id": "id1",
                "name": "video.mp4",
                "thumbnailLink": "thumbnail-link",
                "modifiedTime": "2023-12-29T12:29:03Z",
                "parents": [ "root" ],
                "size": "2508570",
                "mimeType": "video/mp4"
              })js"))
      .Expect(
          HttpRequest(
              fmt::format("https://www.googleapis.com/drive/v3/files/id2?{}",
                          http::FormDataToString(
                              {{"fields",
                                "id,name,thumbnailLink,trashed,mimeType,"
                                "iconLink,parents,size,modifiedTime"}})))
              .WillReturn(R"js({
                "id": "id2",
                "name": "audio.m4a",
                "iconLink": "icon-link",
                "modifiedTime": "2023-12-29T12:29:03Z",
                "parents": [ "root" ],
                "size": "245256",
                "mimeType": "audio/mp4"
              })js"))
      .Expect(
          HttpRequest("https://www.googleapis.com/drive/v3/files/id1?alt=media")
              .WillRespondToRangeRequestWith(GetTestFileContent("video.mp4")))
      .Expect(
          HttpRequest("https://www.googleapis.com/drive/v3/files/id2?alt=media")
              .WillRespondToRangeRequestWith(GetTestFileContent("audio.m4a")));
  TestHelper test_helper(std::move(http));
  ASSERT_EQ(test_helper.Fetch({.url = "/auth/google?code=test"}).status, 302);

  auto response = test_helper.Fetch(
      {.url = fmt::format(
           "/mux?{}",
           http::FormDataToString({{"video_account_type", "google"},
                                   {"video_account_name", "test@gmail.com"},
                                   {"audio_account_type", "google"},
                                   {"audio_account_name", "test@gmail.com"},
                                   {"video_id", "id1"},
                                   {"audio_id", "id2"},
                                   {"format", "mp4"},
                                   {"seekable", "true"}}))});
  EXPECT_EQ(response.status, 200);
  EXPECT_TRUE(AreVideosEquiv(response.body,
                             GetTestFileContent("muxed-seekable.mp4"), "mov"));
}

TEST_F(FunctionalTest, MuxerWebmTest) {
  FakeHttpClient http;
  http.Expect(HttpRequest("https://accounts.google.com/o/oauth2/token")
                  .WillReturn(R"js({
                    "access_token": "access_token",
                    "refresh_token": "refresh_token"
                  })js"))
      .Expect(HttpRequest("https://www.googleapis.com/drive/v3/"
                          "about?fields=user,storageQuota")
                  .WillReturn(R"js({
                    "user": {
                      "emailAddress": "test@gmail.com"
                    },
                    "storageQuota": {
                      "usage": "2137"
                    }
                  })js"))
      .Expect(
          HttpRequest(
              fmt::format("https://www.googleapis.com/drive/v3/files/id1?{}",
                          http::FormDataToString(
                              {{"fields",
                                "id,name,thumbnailLink,trashed,mimeType,"
                                "iconLink,parents,size,modifiedTime"}})))
              .WillReturn(R"js({
                "id": "id1",
                "name": "video.webm",
                "thumbnailLink": "thumbnail-link",
                "modifiedTime": "2023-12-29T12:29:03Z",
                "parents": [ "root" ],
                "size": "197787",
                "mimeType": "video/webm"
              })js"))
      .Expect(
          HttpRequest(
              fmt::format("https://www.googleapis.com/drive/v3/files/id2?{}",
                          http::FormDataToString(
                              {{"fields",
                                "id,name,thumbnailLink,trashed,mimeType,"
                                "iconLink,parents,size,modifiedTime"}})))
              .WillReturn(R"js({
                "id": "id2",
                "name": "audio.webm",
                "iconLink": "icon-link",
                "modifiedTime": "2023-12-29T12:29:03Z",
                "parents": [ "root" ],
                "size": "249177",
                "mimeType": "audio/webm"
              })js"))
      .Expect(
          HttpRequest("https://www.googleapis.com/drive/v3/files/id1?alt=media")
              .WillRespondToRangeRequestWith(GetTestFileContent("video.webm")))
      .Expect(
          HttpRequest("https://www.googleapis.com/drive/v3/files/id2?alt=media")
              .WillRespondToRangeRequestWith(GetTestFileContent("audio.webm")));
  TestHelper test_helper(std::move(http));
  ASSERT_EQ(test_helper.Fetch({.url = "/auth/google?code=test"}).status, 302);

  auto response = test_helper.Fetch(
      {.url = fmt::format(
           "/mux?{}",
           http::FormDataToString({{"video_account_type", "google"},
                                   {"video_account_name", "test@gmail.com"},
                                   {"audio_account_type", "google"},
                                   {"audio_account_name", "test@gmail.com"},
                                   {"video_id", "id1"},
                                   {"audio_id", "id2"},
                                   {"format", "webm"},
                                   {"seekable", "false"}}))});
  EXPECT_EQ(response.status, 200);
  EXPECT_TRUE(AreVideosEquiv(
      response.body, GetTestFileContent("muxed-nonseekable.webm"), "webm"));
}

TEST_F(FunctionalTest, MuxerWebmSeekableOutput) {
  FakeHttpClient http;
  http.Expect(HttpRequest("https://accounts.google.com/o/oauth2/token")
                  .WillReturn(R"js({
                    "access_token": "access_token",
                    "refresh_token": "refresh_token"
                  })js"))
      .Expect(HttpRequest("https://www.googleapis.com/drive/v3/"
                          "about?fields=user,storageQuota")
                  .WillReturn(R"js({
                    "user": {
                      "emailAddress": "test@gmail.com"
                    },
                    "storageQuota": {
                      "usage": "2137"
                    }
                  })js"))
      .Expect(
          HttpRequest(
              fmt::format("https://www.googleapis.com/drive/v3/files/id1?{}",
                          http::FormDataToString(
                              {{"fields",
                                "id,name,thumbnailLink,trashed,mimeType,"
                                "iconLink,parents,size,modifiedTime"}})))
              .WillReturn(R"js({
                "id": "id1",
                "name": "video.webm",
                "thumbnailLink": "thumbnail-link",
                "modifiedTime": "2023-12-29T12:29:03Z",
                "parents": [ "root" ],
                "size": "197787",
                "mimeType": "video/webm"
              })js"))
      .Expect(
          HttpRequest(
              fmt::format("https://www.googleapis.com/drive/v3/files/id2?{}",
                          http::FormDataToString(
                              {{"fields",
                                "id,name,thumbnailLink,trashed,mimeType,"
                                "iconLink,parents,size,modifiedTime"}})))
              .WillReturn(R"js({
                "id": "id2",
                "name": "audio.webm",
                "iconLink": "icon-link",
                "modifiedTime": "2023-12-29T12:29:03Z",
                "parents": [ "root" ],
                "size": "249177",
                "mimeType": "audio/webm"
              })js"))
      .Expect(
          HttpRequest("https://www.googleapis.com/drive/v3/files/id1?alt=media")
              .WillRespondToRangeRequestWith(GetTestFileContent("video.webm")))
      .Expect(
          HttpRequest("https://www.googleapis.com/drive/v3/files/id2?alt=media")
              .WillRespondToRangeRequestWith(GetTestFileContent("audio.webm")));
  TestHelper test_helper(std::move(http));
  ASSERT_EQ(test_helper.Fetch({.url = "/auth/google?code=test"}).status, 302);

  auto response = test_helper.Fetch(
      {.url = fmt::format(
           "/mux?{}",
           http::FormDataToString({{"video_account_type", "google"},
                                   {"video_account_name", "test@gmail.com"},
                                   {"audio_account_type", "google"},
                                   {"audio_account_name", "test@gmail.com"},
                                   {"video_id", "id1"},
                                   {"audio_id", "id2"},
                                   {"format", "webm"},
                                   {"seekable", "true"}}))});
  EXPECT_EQ(response.status, 200);
  EXPECT_TRUE(AreVideosEquiv(
      response.body, GetTestFileContent("muxed-seekable.webm"), "webm"));
}

TEST_F(FunctionalTest, ThumbnailGeneratorRespectsExifOrientation) {
  FakeHttpClient http;
  http.Expect(HttpRequest("https://accounts.google.com/o/oauth2/token")
                  .WillReturn(R"js({
                    "access_token": "access_token",
                    "refresh_token": "refresh_token"
                  })js"))
      .Expect(HttpRequest("https://www.googleapis.com/drive/v3/"
                          "about?fields=user,storageQuota")
                  .WillReturn(R"js({
                    "user": {
                      "emailAddress": "test@gmail.com"
                    },
                    "storageQuota": {
                      "usage": "2137"
                    }
                  })js"))
      .Expect(
          HttpRequest(
              fmt::format("https://www.googleapis.com/drive/v3/files/id1?{}",
                          http::FormDataToString(
                              {{"fields",
                                "id,name,thumbnailLink,trashed,mimeType,"
                                "iconLink,parents,size,modifiedTime"}})))
              .WillReturn(R"js({
                "id": "id1",
                "name": "frame.jpg",
                "thumbnailLink": "thumbnail-link",
                "modifiedTime": "2023-12-29T12:29:03Z",
                "parents": [ "root" ],
                "size": "9447",
                "mimeType": "image/jpeg"
              })js"))
      .Expect(HttpRequest("thumbnail-link")
                  .WillReturn(ResponseContent{.status = 404}))
      .Expect(
          HttpRequest("https://www.googleapis.com/drive/v3/files/id1?alt=media")
              .WillRespondToRangeRequestWith(
                  GetTestFileContent("frame-exif.jpg")));
  TestHelper test_helper(std::move(http));
  ASSERT_EQ(test_helper.Fetch({.url = "/auth/google?code=test"}).status, 302);

  auto account = test_helper.GetAccount(
      CloudProviderAccount::Id{.type = "google", .username = "test@gmail.com"});
  auto response =
      test_helper.Fetch({.url = "/thumbnail/google/test%40gmail.com/id1"});
  EXPECT_EQ(response.status, 200);
  EXPECT_TRUE(AreVideosEquiv(response.body,
                             GetTestFileContent("thumbnail-exif.png"), "png"));
}

}  // namespace
}  // namespace coro::cloudstorage
