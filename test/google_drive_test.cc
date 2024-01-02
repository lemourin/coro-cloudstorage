#include <fmt/format.h>

#include "coro/cloudstorage/test/fake_cloud_factory_context.h"
#include "coro/cloudstorage/test/fake_http_client.h"
#include "coro/cloudstorage/test/test_utils.h"

namespace coro::cloudstorage::test {
namespace {

using ::coro::cloudstorage::util::AbstractCloudProvider;
using ::coro::cloudstorage::util::CloudProviderAccount;

class GoogleDriveTest : public ::testing::Test {
 private:
  TestDataScope scope_;
};

TEST_F(GoogleDriveTest, ListDirectory) {
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

}  // namespace
}  // namespace coro::cloudstorage::test
