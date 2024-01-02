#include <coro/http/http.h>
#include <fmt/format.h>
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

class FunctionalTest : public ::testing::Test {
 private:
  TestDataScope scope_;
};

TEST_F(FunctionalTest, Runs) {
  FakeCloudFactoryContext test_helper;
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
  FakeCloudFactoryContext test_helper(std::move(http));
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
  FakeCloudFactoryContext test_helper(std::move(http));
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
    FakeCloudFactoryContext test_helper(std::move(http));
    ASSERT_EQ(test_helper.Fetch({.url = "/auth/google?code=test"}).status, 302);
  }
  {
    FakeCloudFactoryContext test_helper(FakeHttpClient{});
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
  FakeCloudFactoryContext test_helper(std::move(http));
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
  FakeCloudFactoryContext test_helper(std::move(http));
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
  FakeCloudFactoryContext test_helper(std::move(http));
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
  FakeCloudFactoryContext test_helper(std::move(http));
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
  FakeCloudFactoryContext test_helper(std::move(http));
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
  FakeCloudFactoryContext test_helper(std::move(http));
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
  FakeCloudFactoryContext test_helper(std::move(http));
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
}  // namespace coro::cloudstorage::test
