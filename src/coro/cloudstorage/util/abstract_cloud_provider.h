#ifndef CORO_CLOUDSTORAGE_UTIL_ABSTRACT_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_UTIL_ABSTRACT_CLOUD_PROVIDER_H

#include <any>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "coro/cloudstorage/cloud_exception.h"
#include "coro/cloudstorage/cloud_provider.h"
#include "coro/generator.h"
#include "coro/http/http.h"
#include "coro/stdx/stop_token.h"
#include "coro/task.h"
#include "coro/util/type_list.h"

namespace coro::cloudstorage::util {

class AbstractCloudProvider {
 private:
  struct ItemData {
    std::string id;
    std::string name;
    std::optional<int64_t> size;
    std::optional<int64_t> timestamp;
    std::any impl;
  };

 public:
  enum class Type {
    kAmazonS3,
    kBox,
    kDropbox,
    kGoogleDrive,
    kHubiC,
    kLocalFileSystem,
    kMega,
    kOneDrive,
    kPCloud,
    kWebDAV,
    kYandexDisk,
  };

  class Auth {
   public:
    struct AuthToken {
      Type type;
      std::any impl;
    };

    class AuthHandler {
     public:
      virtual ~AuthHandler() = default;

      virtual Task<std::variant<http::Response<>, AuthToken>> OnRequest(
          http::Request<> request, stdx::stop_token stop_token) = 0;
    };

    virtual ~Auth() = default;

    virtual std::string_view GetId() const = 0;

    virtual std::string_view GetIcon() const = 0;

    virtual nlohmann::json ToJson(const AuthToken&) const = 0;

    virtual AuthToken ToAuthToken(const nlohmann::json&) const = 0;

    virtual std::optional<std::string> GetAuthorizationUrl() const = 0;

    virtual std::unique_ptr<AuthHandler> CreateAuthHandler() const = 0;
  };

  struct File : ItemData {
    std::string mime_type;
  };

  struct Directory : ItemData {};

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct GeneralData {
    std::string username;
    std::optional<int64_t> space_used;
    std::optional<int64_t> space_total;
  };

  struct FileContent {
    Generator<std::string> data;
    std::optional<int64_t> size;
  };

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    std::string mime_type;
  };

  class CloudProvider;

  template <typename CloudProviderT>
  static std::unique_ptr<CloudProvider> Create(CloudProviderT);
};

class AbstractCloudProvider::CloudProvider
    : public coro::cloudstorage::CloudProvider<AbstractCloudProvider,
                                               CloudProvider> {
 public:
  virtual ~CloudProvider() = default;

  virtual std::string_view GetId() const = 0;

  virtual Task<Directory> GetRoot(stdx::stop_token) const = 0;

  virtual bool IsFileContentSizeRequired(const Directory&) const = 0;

  virtual Task<PageData> ListDirectoryPage(
      Directory directory, std::optional<std::string> page_token,
      stdx::stop_token stop_token) const = 0;

  virtual Task<GeneralData> GetGeneralData(stdx::stop_token) const = 0;

  virtual Generator<std::string> GetFileContent(
      File file, http::Range range, stdx::stop_token stop_token) const = 0;

  virtual Task<Directory> RenameItem(Directory item, std::string new_name,
                                     stdx::stop_token stop_token) const = 0;
  virtual Task<File> RenameItem(File item, std::string new_name,
                                stdx::stop_token stop_token) const = 0;

  virtual Task<Directory> CreateDirectory(
      Directory parent, std::string name,
      stdx::stop_token stop_token) const = 0;

  virtual Task<> RemoveItem(Directory item,
                            stdx::stop_token stop_token) const = 0;
  virtual Task<> RemoveItem(File item, stdx::stop_token stop_token) const = 0;

  virtual Task<File> MoveItem(File source, Directory destination,
                              stdx::stop_token stop_token) const = 0;
  virtual Task<Directory> MoveItem(Directory source, Directory destination,
                                   stdx::stop_token stop_token) const = 0;

  virtual Task<File> CreateFile(Directory parent, std::string_view name,
                                FileContent content,
                                stdx::stop_token stop_token) const = 0;

  virtual Task<Thumbnail> GetItemThumbnail(
      File item, http::Range range, stdx::stop_token stop_token) const = 0;

  virtual Task<Thumbnail> GetItemThumbnail(
      Directory item, http::Range range, stdx::stop_token stop_token) const = 0;
};

}  // namespace coro::cloudstorage::util

#endif