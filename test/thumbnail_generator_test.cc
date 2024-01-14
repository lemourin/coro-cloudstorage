#include <fmt/format.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "coro/cloudstorage/test/fake_cloud_factory_context.h"
#include "coro/cloudstorage/test/fake_http_client.h"
#include "coro/cloudstorage/test/test_utils.h"

namespace coro::cloudstorage::test {
namespace {

using ::coro::cloudstorage::util::CloudProviderAccount;

TEST(ThumbnailGeneratorTest, GetCloudThumbnailTest) {
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

TEST(ThumbnailGeneratorTest, ThumbnailGeneratorTest) {
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

TEST(ThumbnailGeneratorTest, ThumbnailGeneratorRespectsExifOrientation) {
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
