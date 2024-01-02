#include <fmt/format.h>
#include <gtest/gtest.h>

#include "coro/cloudstorage/test/fake_cloud_factory_context.h"
#include "coro/cloudstorage/test/fake_http_client.h"
#include "coro/cloudstorage/test/test_utils.h"

namespace coro::cloudstorage::test {
namespace {

class MuxerTest : public ::testing::Test {
 private:
  TestDataScope scope_;
};

TEST_F(MuxerTest, Mp4Test) {
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

TEST_F(MuxerTest, MuxerSeekableMp4Output) {
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

TEST_F(MuxerTest, MuxerWebmTest) {
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

TEST_F(MuxerTest, MuxerWebmSeekableOutput) {
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

}  // namespace
}  // namespace coro::cloudstorage::test
