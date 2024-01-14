#include <fmt/format.h>
#include <gtest/gtest.h>

#include "coro/cloudstorage/test/fake_cloud_factory_context.h"
#include "coro/cloudstorage/test/fake_http_client.h"
#include "coro/cloudstorage/test/test_utils.h"

namespace coro::cloudstorage::test {
namespace {

using ::coro::cloudstorage::util::AbstractCloudProvider;
using ::coro::cloudstorage::util::CloudProviderAccount;

TEST(MegaTest, ListDirectory) {
  constexpr std::string_view kSessionId =
      "LN3KEM3MSrrzp8ValrFuL3dFa3A3a0pROFA0a77Ucyxi6RY2Fgv5BsnDqg";

  FakeHttpClient http;
  http.Expect(
          HttpRequest("https://g.api.mega.co.nz/cs?id=0")
              .WithBody(R"js([{"a":"us0","user":"mega-test@lemourin.net"}])js")
              .WillReturn(
                  R"js([{"s":"j1uuKZeM4mjX-clk_D2R-Cerhr1CrzpBkl-VAoki80U","v":2}])js"))
      .Expect(
          HttpRequest("https://g.api.mega.co.nz/cs?id=1")
              .WithBody(
                  R"js([{"a":"us","sek":"AAAAAAAAAAAAAAAAAAAAAA","uh":"s3611_8e4daBC8DcihjrAg","user":"mega-test@lemourin.net"}])js")
              .WillReturn(R"js([{
                "ach":1,
                "csid":"CABxVLg-Bfswny3SIPbdzvuUUrg6C8opVScwJRhttdvaGEP7h2_acWB8SzGO0CCe1elFCDfGQ4pGlTZKks9yGqvP9YyqQx4QNCkeBBTZMh4ZlUhjvmyTScsitJ7Pi8LwcP1-oB98lronBs8bjfROg9PlGGjwyOPSAhaVZZO6dQig58S7RKg2DjcwsPxQLBGncR2Bbgtb8v4katqDVnQQmaeD6LD4zYCMVHk8ov-FjNXsLsox8EKxFpquGNpzOb39FzUT4PAxF1VgcPe82l8RUmE6YBjcW7DqBoB3iyZMDMA8rkev9KBcrUm14xSaNN1ZfL80B6xpJD2hoe8K9zfl6hSf","k":"e1DrGVah071Lla-UBz8EOw",
                "privk":"NOH8s2EH0aS2xiR7G9okkq-FKEN3XSakv4mRHCFL8hTdBmpYaEv_OjztJcybiJAkDxAQp52mrOtrKR7tHbIAQcm4GnxDslDvKsvbU3Eq_Dm5sPus8KdPlE7dhILA8KuqdlVP3ttKMBp3Ci7d6SgChbP4OX2xRGjkQVcwOCMb2jdo-nmOIi8v_3QprFw4yzmenK_ERxXBJdMyhRieC9KX3PMQr-uuI1SPonKrw_866Yz2gIW82bvnXRVsrzao-IqcTO4Hp3RYz5eJAdb5SriK7ZEy-PhgNaGeDXi2PQXAivGmwNUrxrHbNJS2E92cmzfPiBPj0TuXcOJV8DUdUB54eMF4YA0HtJ9yuf--OVLoRLDQgJnt2mwyfoGcfLrHjSfNgZxY7e6B0mt0a1tl9T6-0sIYLBZVFhUVKzYBoBZGqsQ2xzd7JWgHY8POfaX-aAXlv8OcMSKQGcj2okTAeKqS1K410g2LkSEZKFDLKA4UJYD6xL03LHmXhs5KmO4osIRP7m83tSZX363AksCLIS75r61twSATIzr5xbnrNrLN5R_NkQG6xjlAuplHSHLuWBawjTNoaSQBYJXEgPnViQfTo_NHTpC98-tfbjR5BQAriucMQRpo0YLMU3t2I5VB7rHkOA_ZAJwI2DfFqyRHDtGdVajBcPItntSSdNe3xq_30ICkDmnEOpzyHpyu3_nUxWMCbLKcxa7Z7hQgXotosyDc7vXJD8MBP985gHVEx4kfHtvvJO9QxvHRUee-A49CbelLdQz405PDNXK4NtGbyuGVn_PVMPpdHzTNVITfV0grEB0aRB7PsIl8nVxXTjr8X20uf4sfATJSiXmrIp_fnZHsolnUF62uQtprpwSb8qyQuj0",
                "sek":"AAAAAAAAAAAAAAAAAAAAAA",
                "u":"wEkp7kJQ8P4"
              }])js"))
      .Expect(HttpRequest(fmt::format("https://g.api.mega.co.nz/cs?{}",
                                      http::FormDataToString(
                                          {{"id", "0"}, {"sid", kSessionId}})))
                  .WithBody(R"js([{"a":"uq","strg":1,"xfer":1}])js")
                  .WillReturn(R"js([{"cstrg": 2137, "mstrg": 7312}])js"))
      .Expect(HttpRequest(fmt::format("https://g.api.mega.co.nz/cs?{}",
                                      http::FormDataToString(
                                          {{"id", "0"}, {"sid", kSessionId}})))
                  .WithBody(R"js([{"a":"f","c":1}])js")
                  .WillReturn(R"js([{
                    "aesp": {
                      "e": [],
                      "p": [],
                      "s": []
                    },
                    "f": [
                      {
                        "a": "",
                        "h": "ND0ASLbb",
                        "p": "",
                        "t": 2,
                        "ts": 1705157016,
                        "u": "wEkp7kJQ8P4"
                      },
                      {
                        "a": "",
                        "h": "YD8mXbiA",
                        "p": "",
                        "t": 3,
                        "ts": 1705157016,
                        "u": "wEkp7kJQ8P4"
                      },
                      {
                        "a": "",
                        "h": "oblm0RzD",
                        "p": "",
                        "t": 4,
                        "ts": 1705157016,
                        "u": "wEkp7kJQ8P4"
                      },
                      {
                        "a": "qpYR61ZwVIuN3IiCrulm6c0pFeWqAezwm3oBXAOGNLc",
                        "h": "pblFCRqJ",
                        "k": "wEkp7kJQ8P4:e4zf6XwhnAu5tX99Etl7NA",
                        "p": "ND0ASLbb",
                        "t": 1,
                        "ts": 1705163791,
                        "u": "wEkp7kJQ8P4"
                      },
                      {
                        "a": "Lli2h2EgGDlAoR8dj-oKGXG-aWRdWS86c4kiPrgBZjL40YNBv3hWvuM8fFMWJJmc-d76lehi3VtQMUxI9CcISQ",
                        "h": "pesFQRQI",
                        "k": "wEkp7kJQ8P4:RmHH12ckbtFycJiCS4OTJu7-M2AZwGy6zs-kOnGOtTE",
                        "p": "ND0ASLbb",
                        "s": 13,
                        "t": 0,
                        "ts": 1705163822,
                        "u": "wEkp7kJQ8P4"
                      }
                    ],
                    "ipc": [],
                    "mcf": {
                      "c": [],
                      "cf": [],
                      "pc": [],
                      "pcf": []
                    },
                    "mcna": [],
                    "mcpna": [],
                    "mcsm": [],
                    "noc": 1,
                    "ok": [],
                    "ok0": [],
                    "opc": [],
                    "ph": [],
                    "ps": [],
                    "pup": [],
                    "s": [],
                    "sn": "k2-dD1whLjU",
                    "st": "S9QfAX",
                    "tct": 0,
                    "u": [
                      {
                        "+puCu255": "Jb0kBG2tSDKzD7gZIRv93gpoCZbY1KeApjmTzHUTWz8",
                        "+puEd255": "C59HNhqcben-LazGX1oSad2IYx84nJ7z1-B5sG8TyQk",
                        "+sigCu255": "AAAAAGWiodYOrvCIsnELCjAvfzKPdGjmFKLsx8D1pfHLUDyNxCq1ZfVEnpkBhTV5q7B_cyr50TG2Q3aPSgFwmPhWbdoX_AMF",
                        "+sigPubk": "AAAAAGWiodZOnXDkfNb6t6ET6DFnELgpEk02SAuV7lAlpVTwvx2x5s17T2jtnLZQxJiJfMXZZ13jdYE43fUJ5cGd8JFObt8D",
                        "c": 2,
                        "m": "mega-test@lemourin.net",
                        "m2": [
                          "mega-test@lemourin.net"
                        ],
                        "pubk": "CACz_nCKHGAKZ977KY-KXv0Tbu2JGpSo-7FaCqrt63shkLzTObvf5haJVpUQ-3_283L7oFyTBqJsQIXhxrO-9mcBEpfE0FgQzrWZrV5VwcG3M_yjQVNbnQhLlJ85n1vPg1UTJthFAgZB_ZfVZITjcnWdZUISVP8qu8QZPnfOUGAvtZqiI-T-6A-h55CtJkpxjvItrEyo5BsssvdS912qP6eND8hYl-3jQhY6A9oYhEPVfyW97pyihY94E4o1U_df1FRC4FDTFMyTbbb4Hcd9sDjxKiTIBtG4Zob05YBzCmR3XJAkM6B4Tk5WdaaOuUpONTOYPdOCGN7I93U9GpNxmLg_ACAAAAEB",
                        "u": "wEkp7kJQ8P4"
                      }
                    ],
                    "uph": []
                  }])js"))
      .Expect(HttpRequest(
                  fmt::format("https://g.api.mega.co.nz/sc?{}",
                              http::FormDataToString({{"sn", "k2-dD1whLjU"},
                                                      {"id", "1"},
                                                      {"sid", kSessionId}})))
                  .WillReturn(R"js({
                    "a":[{"a":"ua","st":"S9P!9F","u":"wEkp7kJQ8P4","ua":["^!stbmp"],"v":["E6kvbsgVtFU"]}],
                    "sn":"E6kvbsgVtFU"
                  })js"))
      .Expect(HttpRequest(
                  fmt::format("https://g.api.mega.co.nz/sc?{}",
                              http::FormDataToString({{"sn", "E6kvbsgVtFU"},
                                                      {"id", "2"},
                                                      {"sid", kSessionId}})))
                  .WillReturn(R"js({
                    "w": "http://w.api.mega.co.nz/PMUo-UZroum372P-l7XwfZ8_07g"
                  })js"))
      .Expect(HttpRequest("http://w.api.mega.co.nz/PMUo-UZroum372P-l7XwfZ8_07g")
                  .WillNotReturn());
  FakeCloudFactoryContext test_helper(std::move(http));
  ASSERT_EQ(test_helper
                .Fetch({.url = fmt::format("/auth/mega"),
                        .method = http::Method::kPost,
                        .body = http::FormDataToString(
                            {{"email", "mega-test@lemourin.net"},
                             {"password", "test-password"}})})
                .status,
            302);

  auto account = test_helper.GetAccount(
      {.type = "mega", .username = "mega-test@lemourin.net"});
  auto page_data =
      account.ListDirectoryPage(account.GetRoot(), /*page_token=*/std::nullopt);

  EXPECT_FALSE(page_data.next_page_token.has_value());
  ASSERT_EQ(page_data.items.size(), 2);

  const auto* directory =
      std::get_if<AbstractCloudProvider::Directory>(&page_data.items[0]);
  ASSERT_NE(directory, nullptr);
  EXPECT_EQ(directory->name, "test-folder");
  EXPECT_FALSE(directory->size.has_value());
  EXPECT_EQ(directory->id, "1912059519575868681");
  EXPECT_EQ(directory->timestamp, 1705163791);

  const auto* file =
      std::get_if<AbstractCloudProvider::File>(&page_data.items[1]);
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->name, "test-file.txt");
  EXPECT_EQ(file->size, 13);
  EXPECT_EQ(file->id, "1443403683355886913");
  EXPECT_EQ(file->timestamp, 1705163822);
}

}  // namespace
}  // namespace coro::cloudstorage::test