#ifndef CORO_CLOUDSTORAGE_FUSE_MEGA_H
#define CORO_CLOUDSTORAGE_FUSE_MEGA_H

#include <array>

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/auth_handler.h"
#include "coro/cloudstorage/util/fetch_json.h"
#include "coro/cloudstorage/util/random_number_generator.h"
#include "coro/cloudstorage/util/serialize_utils.h"
#include "coro/cloudstorage/util/thumbnail_generator.h"
#include "coro/cloudstorage/util/thumbnail_options.h"
#include "coro/http/http_parse.h"
#include "coro/stdx/stop_token.h"
#include "coro/util/event_loop.h"
#include "coro/util/function_traits.h"
#include "coro/util/raii_utils.h"
#include "coro/util/type_list.h"

namespace coro::cloudstorage {

struct Mega {
  struct Auth {
    struct AuthToken {
      std::string email;
      std::string session;
      std::array<uint8_t, 16> pkey;
    };

    struct AuthData {
      std::string api_key;
      std::string app_name;
    };

    struct UserCredential {
      std::string email;
      std::string password;
      std::optional<std::string> twofactor;
    };

    class AuthHandler;
  };

  struct ItemData {
    uint64_t id;
    int64_t timestamp;
  };

  struct Directory : ItemData {
    uint64_t parent;
    std::string name;
    std::string user;
    nlohmann::json attr;
    std::array<uint8_t, 16> compkey;
  };

  struct File : ItemData {
    uint64_t parent;
    int64_t size;
    std::string name;
    std::string user;
    nlohmann::json attr;
    std::array<uint8_t, 32> compkey;
    std::optional<uint64_t> thumbnail_id;
  };

  struct Root : ItemData {
    static inline const std::string name = "Root";
  };

  struct Trash : ItemData {
    static inline const std::string name = "Trash";
  };

  struct Inbox : ItemData {
    static inline const std::string name = "Inbox";
  };

  using Item = std::variant<File, Directory, Root, Trash, Inbox>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct GeneralData {
    std::string username;
    int64_t space_used;
    int64_t space_total;
  };

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    static inline constexpr std::string_view mime_type = "image/jpeg";
  };

  struct FileContent {
    Generator<std::string> data;
    int64_t size;
  };

  class CloudProvider;

  static constexpr std::string_view kId = "mega";
  static inline const auto& kIcon = util::kAssetsProvidersMegaPng;
};

class Mega::CloudProvider
    : public coro::cloudstorage::CloudProvider<Mega, CloudProvider> {
 public:
  CloudProvider(const coro::http::Http* http,
                const coro::util::EventLoop* event_loop,
                util::RandomNumberGenerator* random_number_generator,
                util::ThumbnailGenerator thumbnail_generator,
                Auth::AuthToken auth_token)
      : http_(http),
        event_loop_(event_loop),
        random_number_generator_(random_number_generator),
        thumbnail_generator_(thumbnail_generator),
        auth_token_(std::move(auth_token)) {}

  CloudProvider(CloudProvider&& other) noexcept;
  CloudProvider(const CloudProvider&) = delete;

  CloudProvider& operator=(CloudProvider&& other) noexcept;
  CloudProvider& operator=(const CloudProvider&) = delete;

  ~CloudProvider() { stop_source_.request_stop(); }

  Task<Root> GetRoot(stdx::stop_token stop_token);

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token);

  template <typename DirectoryT,
            typename = std::enable_if_t<std::is_same_v<DirectoryT, Directory> ||
                                        std::is_same_v<DirectoryT, Root> ||
                                        std::is_same_v<DirectoryT, Trash> ||
                                        std::is_same_v<DirectoryT, Inbox>>>
  Task<PageData> ListDirectoryPage(DirectoryT directory,
                                   std::optional<std::string>,
                                   coro::stdx::stop_token stop_token);

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        coro::stdx::stop_token stop_token);

  template <typename ItemT,
            typename = std::enable_if_t<std::is_same_v<ItemT, File> ||
                                        std::is_same_v<ItemT, Directory>>>
  Task<ItemT> RenameItem(ItemT item, std::string new_name,
                         stdx::stop_token stop_token);

  template <typename ItemT,
            typename = std::enable_if_t<std::is_same_v<ItemT, File> ||
                                        std::is_same_v<ItemT, Directory>>>
  Task<> RemoveItem(ItemT item, stdx::stop_token stop_token);

  template <typename ItemT, IsDirectory<CloudProvider> DirectoryT,
            typename = std::enable_if_t<std::is_same_v<ItemT, File> ||
                                        std::is_same_v<ItemT, Directory>>>
  Task<ItemT> MoveItem(ItemT source, DirectoryT destination,
                       stdx::stop_token stop_token);

  template <typename DirectoryT,
            typename = std::enable_if_t<std::is_same_v<DirectoryT, Root> ||
                                        std::is_same_v<DirectoryT, Directory>>>
  Task<Directory> CreateDirectory(DirectoryT parent, std::string name,
                                  stdx::stop_token stop_token);

  Task<Thumbnail> GetItemThumbnail(File item, http::Range range,
                                   stdx::stop_token stop_token);

  template <typename DirectoryT,
            typename = std::enable_if_t<std::is_same_v<DirectoryT, Root> ||
                                        std::is_same_v<DirectoryT, Directory>>>
  Task<File> CreateFile(DirectoryT parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token);

  Task<File> TrySetThumbnail(File file, stdx::stop_token stop_token);

  Task<File> SetThumbnail(File file, std::string thumbnail,
                          stdx::stop_token stop_token);

  Task<Auth::AuthToken> GetSession(Auth::UserCredential credential,
                                   stdx::stop_token stop_token);

 private:
  struct PreloginData {
    int version;
    std::optional<std::string> salt;
  };

  template <typename T, size_t Size>
  std::array<T, Size> GenerateKey() const;

  std::optional<File> FindByName(uint64_t parent, std::string_view name) const;

  std::string GetEncryptedItemKey(std::span<const uint8_t> key) const;

  Task<Thumbnail> GetItemThumbnailImpl(File item, http::Range range,
                                       stdx::stop_token stop_token);

  Task<> LazyInit(stdx::stop_token stop_token);

  Task<PreloginData> Prelogin(std::string_view email,
                              stdx::stop_token stop_token);

  Task<nlohmann::json> DoCommand(nlohmann::json command,
                                 stdx::stop_token stop_token);

  template <typename Request>
  Task<nlohmann::json> FetchJson(Request request, stdx::stop_token stop_token);

  template <typename TaskF>
  auto DoWithBackoff(const TaskF& task, int retry_count,
                     stdx::stop_token stop_token)
      -> Task<typename decltype(task())::type>;

  Task<nlohmann::json> FetchJsonWithBackoff(http::Request<std::string> request,
                                            int retry_count,
                                            stdx::stop_token stop_token);

  Task<nlohmann::json> GetFileSystem(stdx::stop_token stop_token);

  Task<nlohmann::json> NewDownload(uint64_t id, stdx::stop_token stop_token);

  Task<nlohmann::json> GetAttribute(uint64_t id, stdx::stop_token stop_token);

  Task<nlohmann::json> CreateUpload(int64_t size, stdx::stop_token stop_token);

  void AddItem(Item e);

  Task<> PollEvents(std::string ssn, stdx::stop_token stop_token) noexcept;

  const Item* HandleAttributeUpdateEvent(std::string_view attr,
                                         uint64_t handle);

  void HandleAddItemEvent(const nlohmann::json& json);

  void HandleUpdateItemEvent(const nlohmann::json& json);

  void HandleRemoveItemEvent(uint64_t handle);

  struct DoInit {
    Task<> operator()() const;
    CloudProvider* p;
  };

  const coro::http::Http* http_;
  const coro::util::EventLoop* event_loop_;
  util::RandomNumberGenerator* random_number_generator_;
  util::ThumbnailGenerator thumbnail_generator_;
  Auth::AuthToken auth_token_;
  std::optional<SharedPromise<DoInit>> init_;
  int id_ = 0;
  std::unordered_map<std::string, std::string> skmap_;
  std::unordered_map<uint64_t, Item> items_;
  std::unordered_map<uint64_t, std::vector<uint64_t>> file_tree_;
  stdx::stop_source stop_source_;
};

class Mega::Auth::AuthHandler {
 public:
  using CloudProviderT = CloudProvider;

  explicit AuthHandler(CloudProviderT provider)
      : provider_(std::move(provider)) {}

  Task<std::variant<http::Response<>, Auth::AuthToken>> operator()(
      http::Request<> request, stdx::stop_token stop_token);

 private:
  CloudProviderT provider_;
};

namespace util {

template <>
nlohmann::json ToJson<Mega::Auth::AuthToken>(Mega::Auth::AuthToken token);

template <>
Mega::Auth::AuthToken ToAuthToken<Mega::Auth::AuthToken>(
    const nlohmann::json& json);

template <>
Mega::Auth::AuthData GetAuthData<Mega>();

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_FUSE_MEGA_H
