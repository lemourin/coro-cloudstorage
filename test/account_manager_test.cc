#include <coro/http/http.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "coro/cloudstorage/test/fake_cloud_factory_context.h"
#include "coro/cloudstorage/test/fake_http_client.h"
#include "coro/cloudstorage/test/test_utils.h"

namespace coro::cloudstorage::test {
namespace {

using ::coro::cloudstorage::util::AbstractCloudProvider;
using ::coro::cloudstorage::util::CloudProviderAccount;
using ::testing::StrEq;

TEST(AccountManagerTest, Runs) {
  FakeCloudFactoryContext test_helper;
  auto response = test_helper.Fetch({.url = "/"});
  EXPECT_EQ(response.body, GetTestFileContent("empty_home_page.html"));
}

TEST(AccountManagerTest, CreateAccount) {
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
  FakeCloudFactoryContext test_helper(std::move(http));
  auto response = test_helper.Fetch({.url = "/auth/google?code=test"});

  EXPECT_THAT(response.status, 302);
  EXPECT_EQ(http::GetHeader(response.headers, "Location").value_or(""),
            "/list/google/test%40gmail.com/");
}

TEST(AccountManagerTest, RestoresAccounts) {
  TemporaryFile config_file;
  TemporaryFile cache_file;
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
    FakeCloudFactoryContext test_helper({.config_file = std::nullopt,
                                         .cache_file = std::nullopt,
                                         .config_file_path{config_file.path()},
                                         .cache_file_path{cache_file.path()},
                                         .http = std::move(http)});
    ASSERT_EQ(test_helper.Fetch({.url = "/auth/google?code=test"}).status, 302);
  }
  {
    FakeCloudFactoryContext test_helper({.config_file = std::nullopt,
                                         .cache_file = std::nullopt,
                                         .config_file_path{config_file.path()},
                                         .cache_file_path{cache_file.path()}});
    auto account = test_helper.GetAccount(CloudProviderAccount::Id{
        .type = "google", .username = "test@gmail.com"});
    auto root = account.GetRoot();
    EXPECT_EQ(root.id, "root");
  }
}

}  // namespace
}  // namespace coro::cloudstorage::test
