#ifndef CORO_CLOUDSTORAGE_AMAZON_S3_H
#define CORO_CLOUDSTORAGE_AMAZON_S3_H

#include <chrono>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <span>
#include <sstream>

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/auth_handler.h"
#include "coro/cloudstorage/util/auth_token_manager.h"
#include "coro/cloudstorage/util/file_utils.h"
#include "coro/cloudstorage/util/recursive_visit.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"
#include "coro/when_all.h"

namespace coro::cloudstorage {

class AmazonS3 {
 public:
  struct GeneralData {
    std::string username;
  };

  struct ItemData {
    std::string id;
    std::string name;
  };

  struct Directory : ItemData {};

  struct File : ItemData {
    int64_t size;
    int64_t timestamp;
  };

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct Auth {
    struct AuthToken {
      std::string access_key_id;
      std::string secret_key;
      std::string endpoint;
      std::string region;
      std::string bucket;
    };
    struct AuthData {};

    class AuthHandler;
  };

  struct FileContent {
    Generator<std::string> data;
    int64_t size;
  };

  class CloudProvider;

  static constexpr std::string_view kId = "amazons3";
  static inline constexpr auto& kIcon = util::kAssetsProvidersAmazons3Png;
};

class AmazonS3::CloudProvider
    : public coro::cloudstorage::CloudProvider<AmazonS3, CloudProvider> {
 public:
  using Request = http::Request<std::string>;

  CloudProvider(const coro::http::Http* http,
                AmazonS3::Auth::AuthToken auth_token)
      : http_(http), auth_token_(std::move(auth_token)) {}

  Task<Directory> GetRoot(stdx::stop_token) const;

  Task<GeneralData> GetGeneralData(stdx::stop_token) const;

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) const;

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) const;

  template <typename Item>
  Task<Item> RenameItem(Item item, std::string new_name,
                        stdx::stop_token stop_token);

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token) const;

  template <typename Item>
  Task<> RemoveItem(Item item, stdx::stop_token stop_token);

  template <typename Item>
  Task<Item> MoveItem(Item source, Directory destination,
                      stdx::stop_token stop_token);

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token) const;

  template <typename Item>
  Task<Item> GetItem(std::string_view id, stdx::stop_token stop_token) const;

 private:
  std::string GetEndpoint(std::string_view href) const;

  template <typename Item, typename F>
  Task<> Visit(Item item, const F& func, stdx::stop_token stop_token);

  Task<> RemoveItemImpl(std::string_view id, stdx::stop_token stop_token) const;

  template <typename Item>
  Task<> Move(const Item& root, std::string_view destination,
              stdx::stop_token stop_token);

  template <typename Item>
  Task<> MoveItemImpl(const Item& source, std::string_view destination,
                      stdx::stop_token stop_token) const;

  template <typename RequestT>
  Task<http::Response<>> Fetch(RequestT request,
                               stdx::stop_token stop_token) const;

  template <typename RequestT>
  Task<pugi::xml_document> FetchXml(RequestT request,
                                    stdx::stop_token stop_token) const;

  const coro::http::Http* http_;
  AmazonS3::Auth::AuthToken auth_token_;
};

class AmazonS3::Auth::AuthHandler {
 public:
  explicit AuthHandler(const coro::http::Http* http) : http_(http) {}

  Task<std::variant<http::Response<>, AmazonS3::Auth::AuthToken>> operator()(
      http::Request<> request, stdx::stop_token stop_token) const;

 private:
  const coro::http::Http* http_;
};

namespace util {

template <>
nlohmann::json ToJson<AmazonS3::Auth::AuthToken>(
    AmazonS3::Auth::AuthToken token);

template <>
AmazonS3::Auth::AuthToken ToAuthToken<AmazonS3::Auth::AuthToken>(
    const nlohmann::json& json);

template <>
AmazonS3::Auth::AuthData GetAuthData<AmazonS3>();

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_AMAZON_S3_H
