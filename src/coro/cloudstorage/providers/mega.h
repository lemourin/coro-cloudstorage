#ifndef CORO_CLOUDSTORAGE_FUSE_MEGA_H
#define CORO_CLOUDSTORAGE_FUSE_MEGA_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/assets.h>
#include <coro/cloudstorage/util/auth_data.h>
#include <coro/cloudstorage/util/auth_handler.h>
#include <coro/cloudstorage/util/fetch_json.h>
#include <coro/cloudstorage/util/serialize_utils.h>
#include <coro/cloudstorage/util/theme_handler.h>
#include <coro/cloudstorage/util/thumbnail_generator.h>
#include <coro/cloudstorage/util/thumbnail_options.h>
#include <coro/http/http_parse.h>
#include <coro/stdx/stop_token.h>
#include <coro/util/event_loop.h>
#include <coro/util/function_traits.h>
#include <coro/util/raii_utils.h>
#include <coro/util/type_list.h>
#include <fmt/format.h>

#include <array>

namespace coro::cloudstorage {

struct Mega {
  struct Auth {
    struct AuthToken {
      std::string email;
      std::string session;
      std::vector<uint8_t> pkey;
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

    template <typename Http = class HttpT,
              typename EventLoop = class EventLoopT,
              typename RandomNumberGenerator = class RandomNumberGeneratorT,
              typename ThumbnailGenerator = class ThumbnailGeneratorT>
    class AuthHandler;

    static std::vector<uint8_t> GetPasswordKey(std::string_view password);
    static std::string GetHash(std::string_view text,
                               const std::vector<uint8_t>& key);

    struct SessionData {
      std::vector<uint8_t> pkey;
      std::string session_id;
    };

    static SessionData DecryptSessionId(const std::vector<uint8_t>& passkey,
                                        std::string_view key,
                                        std::string_view privk,
                                        std::string_view csid);

    struct LoginWithSaltData {
      std::string handle;
      std::vector<uint8_t> password_key;
    };
    static LoginWithSaltData GetLoginWithSaltData(std::string_view password,
                                                  std::string_view salt);
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
    std::vector<uint8_t> key;
    std::vector<uint8_t> compkey;
  };

  struct File : ItemData {
    uint64_t parent;
    int64_t size;
    std::string name;
    std::string user;
    nlohmann::json attr;
    std::vector<uint8_t> key;
    std::vector<uint8_t> compkey;
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

  template <typename Http = class HttpT, typename EventLoop = class EventLoopT,
            typename RandomNumberGenerator = class RandomNumberGeneratorT,
            typename ThumbnailGenerator = class ThumbnailGeneratorT>
  class CloudProvider;

  static std::string ToBase64(std::string_view);
  static std::string FromBase64(std::string_view);
  static Item ToItem(const nlohmann::json&,
                     std::span<const uint8_t> master_key);

  static std::optional<std::string_view> GetAttribute(std::string_view attr,
                                                      int index);

  static std::string DecodeChunk(std::span<const uint8_t> key,
                                 std::span<const uint8_t> compkey,
                                 int64_t position, std::string_view encoded);
  static nlohmann::json DecryptAttribute(std::span<const uint8_t> key,
                                         std::string_view input);
  static std::string DecodeAttributeContent(std::span<const uint8_t> key,
                                            std::string_view encoded);

  static std::string EncodeChunk(std::span<const uint8_t> key,
                                 std::span<const uint8_t> compkey,
                                 int64_t position, std::string_view text);
  static std::string EncryptAttribute(std::span<const uint8_t> key,
                                      const nlohmann::json& json);
  static std::string EncodeAttributeContent(std::span<const uint8_t> key,
                                            std::string_view content);

  static std::vector<uint8_t> ToFileKey(std::span<const uint8_t> compkey);

  static std::string ToHandle(uint64_t id);
  static std::string ToAttributeHandle(uint64_t id);
  static uint64_t DecodeHandle(std::string_view b64);

  static std::string BlockEncrypt(std::span<const uint8_t> key,
                                  std::string_view message);

  static std::vector<uint8_t> ToBytes(std::span<const uint32_t> span);
  static std::span<const uint8_t> ToBytes(std::string_view d);
  static std::string_view ToStringView(std::span<const uint8_t> d);
  static std::vector<uint32_t> ToA32(std::span<const uint8_t> bytes);

  static Generator<std::string> GetEncodedStream(
      std::span<const uint8_t> key, std::span<const uint8_t> compkey,
      Generator<std::string> decoded, std::vector<uint32_t>& cbc_mac);

  static constexpr std::string_view kId = "mega";
  static inline const auto& kIcon = util::kAssetsProvidersMegaPng;
};

template <typename Http, typename EventLoop, typename RandomNumberGenerator,
          typename ThumbnailGenerator>
class Mega::CloudProvider
    : public coro::cloudstorage::CloudProvider<
          Mega, CloudProvider<Http, EventLoop, RandomNumberGenerator,
                              ThumbnailGenerator>> {
 public:
  CloudProvider(const Http* http, const EventLoop* event_loop,
                RandomNumberGenerator* random_number_generator,
                const ThumbnailGenerator* thumbnail_generator,
                Auth::AuthToken auth_token)
      : http_(http),
        event_loop_(event_loop),
        random_number_generator_(random_number_generator),
        thumbnail_generator_(thumbnail_generator),
        auth_token_(std::move(auth_token)) {}

  CloudProvider(CloudProvider&& other) noexcept
      : http_(other.http_),
        event_loop_(other.event_loop_),
        random_number_generator_(other.random_number_generator_),
        thumbnail_generator_(other.thumbnail_generator_),
        auth_token_(std::move(other.auth_token_)),
        id_(other.id_),
        skmap_(std::move(other.skmap_)),
        items_(std::move(other.items_)),
        file_tree_(std::move(other.file_tree_)),
        stop_source_(std::move(other.stop_source_)) {}

  CloudProvider& operator=(CloudProvider&& other) noexcept {
    http_ = other.http_;
    event_loop_ = other.event_loop_;
    random_number_generator_ = other.random_number_generator_;
    thumbnail_generator_ = other.thumbnail_generator_;
    auth_token_ = std::move(other.auth_token_);
    id_ = other.id_;
    skmap_ = std::move(other.skmap_);
    items_ = std::move(other.items_);
    file_tree_ = std::move(other.file_tree_);
    stop_source_ = std::move(other.stop_source_);
    return *this;
  }

  ~CloudProvider() { stop_source_.request_stop(); }

  Task<Root> GetRoot(stdx::stop_token stop_token) {
    co_await LazyInit(std::move(stop_token));
    for (const auto& [key, value] : items_) {
      if (std::holds_alternative<Root>(value)) {
        co_return std::get<Root>(value);
      }
    }
    throw CloudException(CloudException::Type::kNotFound);
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) {
    nlohmann::json command;
    command["a"] = "uq";
    command["xfer"] = 1;
    command["strg"] = 1;
    auto response =
        co_await DoCommand(std::move(command), std::move(stop_token));
    co_return GeneralData{.username = auth_token_.email,
                          .space_used = response["cstrg"],
                          .space_total = response["mstrg"]};
  }

  template <typename DirectoryT,
            typename = std::enable_if_t<std::is_same_v<DirectoryT, Directory> ||
                                        std::is_same_v<DirectoryT, Root> ||
                                        std::is_same_v<DirectoryT, Trash> ||
                                        std::is_same_v<DirectoryT, Inbox>>>
  Task<PageData> ListDirectoryPage(DirectoryT directory,
                                   std::optional<std::string>,
                                   coro::stdx::stop_token stop_token) {
    co_await LazyInit(std::move(stop_token));
    if (!items_.contains(directory.id)) {
      throw CloudException(CloudException::Type::kNotFound);
    }
    auto it = file_tree_.find(directory.id);
    if (it == file_tree_.end()) {
      co_return PageData{};
    }
    PageData page_data;
    for (uint64_t id : it->second) {
      page_data.items.emplace_back(items_[id]);
    }
    co_return page_data;
  }

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        coro::stdx::stop_token stop_token) {
    if (range.start >= file.size || (range.end && *range.end >= file.size)) {
      throw http::HttpException(http::HttpException::kRangeNotSatisfiable);
    }
    int64_t position = range.start;
    int64_t size = range.end.value_or(file.size - 1) - range.start + 1;
    co_await LazyInit(stop_token);
    auto json = co_await NewDownload(file.id, stop_token);
    DecryptAttribute(file.key, FromBase64(std::string(json["at"])));
    std::string url = json["g"];
    auto chunk_url = util::StrCat(url, "/", position, "-", position + size - 1);
    auto chunk_response = co_await http_->Fetch(chunk_url, stop_token);
    if (chunk_response.status / 100 != 2) {
      throw http::HttpException(chunk_response.status);
    }
    FOR_CO_AWAIT(std::string_view chunk, chunk_response.body) {
      co_yield DecodeChunk(file.key, file.compkey, position, chunk);
      position += static_cast<int64_t>(chunk.size());
    }
  }

  template <typename ItemT,
            typename = std::enable_if_t<std::is_same_v<ItemT, File> ||
                                        std::is_same_v<ItemT, Directory>>>
  Task<ItemT> RenameItem(ItemT item, std::string new_name,
                         stdx::stop_token stop_token) {
    nlohmann::json command;
    command["a"] = "a";
    item.name = new_name;
    item.attr["n"] = new_name;
    command["attr"] = ToBase64(EncryptAttribute(item.key, item.attr));
    command["n"] = ToHandle(item.id);
    command["key"] = GetEncryptedItemKey(item.compkey);
    co_await DoCommand(std::move(command), std::move(stop_token));
    co_return std::get<ItemT>(items_[item.id] = std::move(item));
  }

  template <typename ItemT,
            typename = std::enable_if_t<std::is_same_v<ItemT, File> ||
                                        std::is_same_v<ItemT, Directory>>>
  Task<> RemoveItem(ItemT item, stdx::stop_token stop_token) {
    nlohmann::json command;
    command["a"] = "d";
    command["n"] = ToHandle(item.id);
    co_await DoCommand(std::move(command), std::move(stop_token));
    HandleRemoveItemEvent(item.id);
  }

  template <typename ItemT, IsDirectory<CloudProvider> DirectoryT,
            typename = std::enable_if_t<std::is_same_v<ItemT, File> ||
                                        std::is_same_v<ItemT, Directory>>>
  Task<ItemT> MoveItem(ItemT source, DirectoryT destination,
                       stdx::stop_token stop_token) {
    nlohmann::json command;
    command["a"] = "m";
    command["n"] = ToHandle(source.id);
    command["t"] = ToHandle(destination.id);
    co_await DoCommand(std::move(command), std::move(stop_token));
    HandleRemoveItemEvent(source.id);
    source.parent = destination.id;
    AddItem(source);
    co_return source;
  }

  template <typename DirectoryT,
            typename = std::enable_if_t<std::is_same_v<DirectoryT, Root> ||
                                        std::is_same_v<DirectoryT, Directory>>>
  Task<Directory> CreateDirectory(DirectoryT parent, std::string name,
                                  stdx::stop_token stop_token) {
    std::vector<uint8_t> compkey = GenerateKey<uint8_t>(16);
    Directory directory{};
    directory.compkey = std::move(compkey);
    directory.parent = parent.id;
    nlohmann::json command;
    command["a"] = "p";
    command["t"] = ToHandle(parent.id);
    nlohmann::json entry;
    entry["h"] = "xxxxxxxx";
    entry["t"] = 1;
    entry["k"] = GetEncryptedItemKey(directory.compkey);
    nlohmann::json attr;
    attr["n"] = std::move(name);
    entry["a"] = ToBase64(EncryptAttribute(directory.compkey, attr));
    command["n"].emplace_back(std::move(entry));

    auto response =
        co_await DoCommand(std::move(command), std::move(stop_token));
    auto item = ToItem(response["f"].at(0), auth_token_.pkey);
    AddItem(item);
    co_return std::get<Directory>(item);
  }

  Task<Thumbnail> GetItemThumbnail(File item, http::Range range,
                                   stdx::stop_token stop_token) {
    if (!item.thumbnail_id) {
      throw CloudException(CloudException::Type::kNotFound);
    }
    auto response = co_await GetAttribute(*item.thumbnail_id, stop_token);
    std::string input = FromBase64(ToAttributeHandle(*item.thumbnail_id));
    if (input.size() % 8 != 0) {
      input.resize(input.size() + 8 - input.size() % 8);
    }
    http::Request<> request = {
        .url = response["p"],
        .method = http::Method::kPost,
        .headers = {{"Content-Type", "application/octet-stream"},
                    {"Content-Length", "8"}},
        .body = http::CreateBody(std::move(input))};
    auto thumbnail_response =
        co_await http_->Fetch(std::move(request), stop_token);
    auto content = co_await http::GetBody(std::move(thumbnail_response.body));
    content = content.substr(12);
    Thumbnail thumbnail{.size = static_cast<int64_t>(content.size())};
    auto decoded = DecodeAttributeContent(
        std::span<const uint8_t>(item.key).subspan(0, 16), content);
    int64_t end = range.end.value_or(decoded.size() - 1);
    if (end >= static_cast<int64_t>(decoded.size())) {
      throw http::HttpException(http::HttpException::kRangeNotSatisfiable);
    }
    std::string output(end - range.start + 1, 0);
    memcpy(output.data(), decoded.data() + range.start, end - range.start + 1);
    thumbnail.data = http::CreateBody(std::move(output));
    co_return thumbnail;
  }

  template <typename DirectoryT,
            typename = std::enable_if_t<std::is_same_v<DirectoryT, Root> ||
                                        std::is_same_v<DirectoryT, Directory>>>
  Task<File> CreateFile(DirectoryT parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token) {
    nlohmann::json upload_response =
        co_await CreateUpload(content.size, stop_token);
    std::string upload_url = upload_response["p"];
    std::vector<uint32_t> compkey = GenerateKey<uint32_t>(6);
    std::span<const uint32_t> key(compkey.data(), 4);
    std::vector<uint32_t> cbc_mac(4, 0);
    auto response = co_await http_->Fetch(
        http::Request<>{
            .url = util::StrCat(upload_url, "/0"),
            .method = http::Method::kPost,
            .headers = {{"Content-Length", std::to_string(content.size)}},
            .body = GetEncodedStream(ToBytes(key), ToBytes(compkey),
                                     std::move(content.data), cbc_mac)},
        stop_token);
    if (response.status / 100 != 2) {
      throw http::HttpException(response.status);
    }

    std::array<uint32_t, 2> meta_mac{cbc_mac[0] ^ cbc_mac[1],
                                     cbc_mac[2] ^ cbc_mac[3]};
    std::array<uint32_t, 16> item_key = {compkey[0] ^ compkey[4],
                                         compkey[1] ^ compkey[5],
                                         compkey[2] ^ meta_mac[0],
                                         compkey[3] ^ meta_mac[1],
                                         compkey[4],
                                         compkey[5],
                                         meta_mac[0],
                                         meta_mac[1]};

    std::string item_key_bytes(ToStringView(ToBytes(item_key)));
    std::string encoded_key = util::StrCat(
        EncodeAttributeContent(auth_token_.pkey, item_key_bytes.substr(0, 16)),
        EncodeAttributeContent(auth_token_.pkey,
                               item_key_bytes.substr(16, 16)));

    std::string completion_handle =
        co_await http::GetBody(std::move(response.body));
    nlohmann::json commit_command;
    commit_command["a"] = "p";
    commit_command["t"] = ToHandle(parent.id);
    nlohmann::json entry;
    entry["h"] = std::move(completion_handle);
    entry["t"] = 0;
    nlohmann::json attr;
    attr["n"] = name;
    entry["a"] = ToBase64(EncryptAttribute(ToBytes(key), attr));
    entry["k"] = ToBase64(encoded_key);
    commit_command["n"].emplace_back(std::move(entry));

    std::optional<File> previous_file = FindByName(parent.id, name);
    nlohmann::json commit_command_response =
        co_await DoCommand(std::move(commit_command), stop_token);
    auto new_item = ToItem(commit_command_response["f"][0], auth_token_.pkey);
    AddItem(new_item);
    if (previous_file) {
      co_await RemoveItem(std::move(*previous_file), stop_token);
    }
    co_return co_await TrySetThumbnail(std::get<File>(new_item),
                                       std::move(stop_token));
  }

  Task<File> TrySetThumbnail(File file, stdx::stop_token stop_token) {
    switch (this->GetFileType(file)) {
      case FileType::kImage:
      case FileType::kVideo: {
        try {
          auto thumbnail = co_await(*thumbnail_generator_)(
              this, file,
              util::ThumbnailOptions{
                  .size = 120, .codec = util::ThumbnailOptions::Codec::JPEG},
              stop_token);
          co_return co_await SetThumbnail(std::move(file), std::move(thumbnail),
                                          std::move(stop_token));
        } catch (...) {
        }
        break;
      }
      default:
        break;
    }
    co_return file;
  }

  Task<File> SetThumbnail(File file, std::string thumbnail,
                          stdx::stop_token stop_token) {
    std::string encoded = EncodeAttributeContent(file.key, thumbnail);
    nlohmann::json command;
    command["a"] = "ufa";
    command["s"] = encoded.size();
    command["h"] = ToHandle(file.id);
    nlohmann::json url_response =
        co_await DoCommand(std::move(command), stop_token);
    auto response = co_await http_->Fetch(
        http::Request<std::string>{.url = url_response["p"],
                                   .method = http::Method::kPost,
                                   .body = std::move(encoded)},
        stop_token);
    if (response.status / 100 != 2) {
      throw http::HttpException(response.status);
    }
    std::string thumbnail_id_bytes =
        co_await http::GetBody(std::move(response.body));
    uint64_t thumbnail_id = DecodeHandle(ToBase64(thumbnail_id_bytes));
    nlohmann::json update_attributes;
    update_attributes["a"] = "pfa";
    update_attributes["n"] = ToHandle(file.id);
    update_attributes["fa"] =
        util::StrCat("0*", ToAttributeHandle(thumbnail_id));
    nlohmann::json attribute =
        co_await DoCommand(update_attributes, std::move(stop_token));
    if (auto* new_item =
            HandleAttributeUpdateEvent(std::string(attribute), file.id)) {
      co_return std::get<File>(*new_item);
    } else {
      throw CloudException(CloudException::Type::kNotFound);
    }
  }

  Task<Auth::AuthToken> GetSession(Auth::UserCredential credential,
                                   stdx::stop_token stop_token) {
    auto prelogin_data = co_await Prelogin(credential.email, stop_token);
    nlohmann::json command;
    command["a"] = "us";
    command["user"] = http::ToLowerCase(std::string(credential.email));
    if (credential.twofactor) {
      command["mfa"] = std::move(*credential.twofactor);
    }
    std::vector<uint8_t> password_key;
    if (prelogin_data.version == 1) {
      password_key = Auth::GetPasswordKey(credential.password);
      command["uh"] = Auth::GetHash(credential.email, password_key);
    } else if (prelogin_data.version == 2 && prelogin_data.salt) {
      auto data =
          Auth::GetLoginWithSaltData(credential.password, *prelogin_data.salt);
      password_key = std::move(data.password_key);
      command["uh"] = std::move(data.handle);
      command["sek"] = ToBase64(ToStringView(GenerateKey<uint8_t>(16)));
    } else {
      throw CloudException("not supported account version");
    }
    auto response = co_await DoCommand(std::move(command), stop_token);
    auto session_data = Auth::DecryptSessionId(
        password_key, FromBase64(std::string(response["k"])),
        FromBase64(std::string(response["privk"])),
        FromBase64(std::string(response["csid"])));
    co_return Auth::AuthToken{credential.email,
                              std::move(session_data.session_id),
                              std::move(session_data.pkey)};
  }

 private:
  static constexpr std::string_view kApiEndpoint = "https://g.api.mega.co.nz";
  static constexpr int kRetryCount = 7;

  struct PreloginData {
    int version;
    std::optional<std::string> salt;
  };

  template <typename T>
  std::vector<T> GenerateKey(int length) const {
    std::vector<T> key(length, 0);
    for (T& c : key) {
      c = random_number_generator_->template Get<T>();
    }
    return key;
  }

  std::optional<File> FindByName(uint64_t parent, std::string_view name) const {
    auto nodes = file_tree_.find(parent);
    if (nodes == file_tree_.end()) {
      return std::nullopt;
    }
    for (uint64_t handle : nodes->second) {
      auto it = items_.find(handle);
      if (it != items_.end()) {
        const File* file = std::get_if<File>(&it->second);
        if (file && file->name == name) {
          return *file;
        }
      }
    }
    return std::nullopt;
  }

  std::string GetEncryptedItemKey(std::span<const uint8_t> key) const {
    return ToBase64(BlockEncrypt(auth_token_.pkey, ToStringView(key)));
  }

  Task<> LazyInit(stdx::stop_token stop_token) {
    if (!init_) {
      init_.emplace(DoInit{this});
      co_await init_->Get(std::move(stop_token));
      co_return;
    }
    std::exception_ptr exception;
    try {
      co_await init_->Get(std::move(stop_token));
      co_return;
    } catch (const CloudException&) {
    } catch (const http::HttpException&) {
    }
    init_.emplace(DoInit{this});
    co_await init_->Get(std::move(stop_token));
  }

  Task<PreloginData> Prelogin(std::string_view email,
                              stdx::stop_token stop_token) {
    nlohmann::json command;
    command["a"] = "us0";
    command["user"] = http::ToLowerCase(std::string(email));
    auto response =
        co_await DoCommand(std::move(command), std::move(stop_token));
    PreloginData data{.version = response.at("v")};
    if (response.contains("s")) {
      data.salt = FromBase64(std::string(response["s"]));
    }
    co_return data;
  }

  Task<nlohmann::json> DoCommand(nlohmann::json command,
                                 stdx::stop_token stop_token) {
    nlohmann::json body;
    body.emplace_back(std::move(command));
    nlohmann::json response = co_await FetchJsonWithBackoff(
        http::Request<std::string>{.url = util::StrCat(kApiEndpoint, "/cs"),
                                   .method = http::Method ::kPost,
                                   .body = body.dump()},
        kRetryCount, std::move(stop_token));
    co_return response.at(0);
  }

  static CloudException ToException(int status) {
    if (status == -3) {
      return CloudException(CloudException::Type::kRetry);
    } else {
      return CloudException(util::StrCat("mega error ", status));
    }
  }

  template <typename Request>
  Task<nlohmann::json> FetchJson(Request request, stdx::stop_token stop_token) {
    std::vector<std::pair<std::string, std::string>> params = {
        {"id", std::to_string(id_++)}};
    if (!auth_token_.session.empty()) {
      params.emplace_back("sid", auth_token_.session);
    }
    http::Uri uri = coro::http::ParseUri(request.url);
    uri.query = util::StrCat(uri.query ? util::StrCat(*uri.query, "&") : "",
                             http::FormDataToString(params));
    request.url =
        util::StrCat(*uri.scheme, "://", *uri.host, *uri.path, "?", *uri.query);
    nlohmann::json response = co_await util::FetchJson(
        *http_, std::move(request), std::move(stop_token));
    if (response.is_number() && response != 0) {
      throw ToException(response);
    }
    if (response.is_array()) {
      for (const nlohmann::json& entry : response) {
        if (entry.is_number() && entry != 0) {
          throw ToException(entry);
        }
      }
    }
    co_return response;
  }

  template <typename TaskF>
  auto DoWithBackoff(const TaskF& task, int retry_count,
                     stdx::stop_token stop_token)
      -> Task<typename decltype(task())::type> {
    int backoff_ms = 0;
    while (true) {
      try {
        if (backoff_ms > 0) {
          co_await event_loop_->Wait(backoff_ms, stop_token);
        }
        co_return co_await task();
      } catch (const CloudException& e) {
        if (e.type() == CloudException::Type::kRetry) {
          backoff_ms = std::max<int>(backoff_ms * 2, 100);
          retry_count--;
          if (retry_count == 0) {
            throw;
          }
        } else {
          throw;
        }
      } catch (const http::HttpException&) {
        backoff_ms = std::max<int>(backoff_ms * 2, 100);
        retry_count--;
        if (retry_count == 0) {
          throw;
        }
      }
    }
  }

  Task<nlohmann::json> FetchJsonWithBackoff(http::Request<std::string> request,
                                            int retry_count,
                                            stdx::stop_token stop_token) {
    co_return co_await DoWithBackoff(
        [&]() -> Task<nlohmann::json> {
          co_return co_await FetchJson(request, stop_token);
        },
        retry_count, stop_token);
  }

  Task<nlohmann::json> GetFileSystem(stdx::stop_token stop_token) {
    nlohmann::json command;
    command["a"] = "f";
    command["c"] = 1;
    co_return co_await DoCommand(std::move(command), std::move(stop_token));
  }

  Task<nlohmann::json> NewDownload(uint64_t id, stdx::stop_token stop_token) {
    nlohmann::json command;
    command["a"] = "g";
    command["g"] = 1;
    command["n"] = ToHandle(id);
    co_return co_await DoCommand(std::move(command), std::move(stop_token));
  }

  Task<nlohmann::json> GetAttribute(uint64_t id, stdx::stop_token stop_token) {
    nlohmann::json command;
    command["a"] = "ufa";
    command["r"] = 1;
    command["fah"] = ToAttributeHandle(id);
    co_return co_await DoCommand(std::move(command), std::move(stop_token));
  }

  Task<nlohmann::json> CreateUpload(int64_t size, stdx::stop_token stop_token) {
    nlohmann::json command;
    command["a"] = "u";
    command["s"] = size;
    co_return co_await DoCommand(std::move(command), std::move(stop_token));
  }

  void AddItem(Item e) {
    std::visit(
        [&]<typename T>(T&& item) {
          auto id = item.id;
          if constexpr (std::is_same_v<T, File> ||
                        std::is_same_v<T, Directory>) {
            auto& tree = file_tree_[item.parent];
            if (auto it = std::find(tree.begin(), tree.end(), item.id);
                it == tree.end()) {
              tree.emplace_back(item.id);
            }
          }
          items_.emplace(id, std::forward<T>(item));
        },
        std::move(e));
  }

  Task<> PollEvents(std::string ssn, stdx::stop_token stop_token) noexcept {
    int backoff_ms = 0;
    while (!stop_token.stop_requested()) {
      try {
        if (backoff_ms > 0) {
          co_await event_loop_->Wait(backoff_ms, stop_token);
        }
        nlohmann::json json = co_await FetchJsonWithBackoff(
            http::Request<std::string>{
                .url = util::StrCat(kApiEndpoint, "/sc", "?",
                                    http::FormDataToString({{"sn", ssn}})),
                .method = http::Method::kPost},
            kRetryCount, stop_token);
        if (json.contains("w")) {
          co_await http_->Fetch(std::string(json["w"]), stop_token);
          continue;
        }
        for (const nlohmann::json& event : json["a"]) {
          std::string type = event["a"];
          if (type == "t") {
            HandleAddItemEvent(event);
          } else if (type == "u") {
            HandleUpdateItemEvent(event);
          } else if (type == "d") {
            HandleRemoveItemEvent(DecodeHandle(std::string(event["n"])));
          } else if (type == "fa") {
            HandleAttributeUpdateEvent(std::string(event["fa"]),
                                       DecodeHandle(std::string(event["n"])));
          }
        }
        ssn = json["sn"];
        backoff_ms = 0;
      } catch (const CloudException&) {
        backoff_ms = std::max<int>(backoff_ms * 2, 100);
      } catch (const http::HttpException&) {
        backoff_ms = std::max<int>(backoff_ms * 2, 100);
      }
    }
  }

  const Item* HandleAttributeUpdateEvent(std::string_view attr,
                                         uint64_t handle) {
    if (auto it = items_.find(handle); it != items_.end()) {
      if (auto* file = std::get_if<File>(&it->second)) {
        if (auto thumbnail_attr = Mega::GetAttribute(attr, 0)) {
          file->thumbnail_id = DecodeHandle(*thumbnail_attr);
          return &it->second;
        }
      }
    }
    return nullptr;
  }

  void HandleAddItemEvent(const nlohmann::json& json) {
    for (const nlohmann::json& item : json["t"]["f"]) {
      AddItem(ToItem(item, auth_token_.pkey));
    }
  }

  void HandleUpdateItemEvent(const nlohmann::json& json) {
    uint64_t handle = DecodeHandle(std::string(json["n"]));
    if (auto it = items_.find(handle); it != items_.end()) {
      std::visit(
          [&]<typename T>(T& item) {
            if constexpr (std::is_same_v<T, File> ||
                          std::is_same_v<T, Directory>) {
              try {
                item.name = DecryptAttribute(
                                item.key, FromBase64(std::string(json["at"])))
                                .at("n");
              } catch (const nlohmann::json::exception&) {
                item.name = "MALFORMED ATTRIBUTES";
              } catch (const CloudException&) {
                item.name = "MALFORMED ATTRIBUTES";
              }
              item.timestamp = json["ts"];
            }
          },
          it->second);
    }
  }

  void HandleRemoveItemEvent(uint64_t handle) {
    if (auto it = items_.find(handle); it != items_.end()) {
      std::visit(
          [&]<typename T>(const T& d) {
            if constexpr (std::is_same_v<T, File> ||
                          std::is_same_v<T, Directory>) {
              auto& children = file_tree_[d.parent];
              if (auto it = std::find(children.begin(), children.end(), handle);
                  it != children.end()) {
                children.erase(it);
              }
            }
          },
          it->second);
      items_.erase(it);
      file_tree_.erase(handle);
    }
  }

  struct DoInit {
    Task<> operator()() const {
      auto stop_token = p->stop_source_.get_token();
      auto json = co_await p->GetFileSystem(stop_token);
      if (stop_token.stop_requested()) {
        throw InterruptedException();
      }
      for (const auto& entry : json["ok"]) {
        p->skmap_[entry["h"]] = entry["k"];
      }
      for (const auto& entry : json["f"]) {
        p->AddItem(ToItem(entry, p->auth_token_.pkey));
      }
      RunTask(p->PollEvents(json["sn"], std::move(stop_token)));
    }

    CloudProvider* p;
  };

  const Http* http_;
  const EventLoop* event_loop_;
  RandomNumberGenerator* random_number_generator_;
  const ThumbnailGenerator* thumbnail_generator_;
  Auth::AuthToken auth_token_;
  std::optional<SharedPromise<DoInit>> init_;
  int id_ = 0;
  std::unordered_map<std::string, std::string> skmap_;
  std::unordered_map<uint64_t, Item> items_;
  std::unordered_map<uint64_t, std::vector<uint64_t>> file_tree_;
  stdx::stop_source stop_source_;
};

template <typename Http, typename EventLoop, typename RandomNumberGenerator,
          typename ThumbnailGenerator>
class Mega::Auth::AuthHandler {
 public:
  using CloudProviderT =
      CloudProvider<Http, EventLoop, RandomNumberGenerator, ThumbnailGenerator>;

  explicit AuthHandler(CloudProviderT provider)
      : provider_(std::move(provider)) {}

  Task<std::variant<http::Response<>, Auth::AuthToken>> operator()(
      http::Request<> request, stdx::stop_token stop_token) {
    if (request.method == http::Method::kPost) {
      auto query =
          http::ParseQuery(co_await http::GetBody(std::move(*request.body)));
      auto it1 = query.find("email");
      auto it2 = query.find("password");
      if (it1 != std::end(query) && it2 != std::end(query)) {
        auto it3 = query.find("twofactor");
        Auth::UserCredential credential = {
            .email = it1->second,
            .password = it2->second,
            .twofactor = it3 != std::end(query)
                             ? std::make_optional(it3->second)
                             : std::nullopt};
        co_return co_await provider_.GetSession(std::move(credential),
                                                stop_token);
      } else {
        throw http::HttpException(http::HttpException::kBadRequest);
      }
    } else {
      co_return http::Response<>{
          .status = 200,
          .body = http::CreateBody(fmt::format(
              fmt::runtime(util::kAssetsHtmlMegaLoginHtml),
              fmt::arg("theme",
                       util::ToString(util::GetTheme(request.headers)))))};
    }
  }

 private:
  CloudProviderT provider_;
};

namespace util {

template <>
inline nlohmann::json ToJson<Mega::Auth::AuthToken>(
    Mega::Auth::AuthToken token) {
  nlohmann::json json;
  json["email"] = std::move(token.email);
  json["session"] = std::move(token.session);
  for (uint8_t c : token.pkey) {
    json["pkey"].emplace_back(c);
  }
  return json;
}

template <>
inline Mega::Auth::AuthToken ToAuthToken<Mega::Auth::AuthToken>(
    const nlohmann::json& json) {
  Mega::Auth::AuthToken auth_token = {
      .email = json.at("email"), .session = std::string(json.at("session"))};
  for (uint8_t c : json.at("pkey")) {
    auth_token.pkey.emplace_back(c);
  }
  return auth_token;
}

template <>
inline Mega::Auth::AuthData GetAuthData<Mega>() {
  return {.api_key = "ZVhB0Czb", .app_name = "coro-cloudstorage"};
}

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_FUSE_MEGA_H
