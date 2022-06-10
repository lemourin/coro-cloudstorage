#ifndef CORO_CLOUDSTORAGE_YOUTUBE_H
#define CORO_CLOUDSTORAGE_YOUTUBE_H

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "coro/cloudstorage/cloud_exception.h"
#include "coro/cloudstorage/providers/google_drive.h"
#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/avio_context.h"
#include "coro/cloudstorage/util/muxer.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"
#include "coro/task.h"
#include "coro/util/lru_cache.h"

namespace coro::cloudstorage {

class YouTube {
 public:
  using json = nlohmann::json;
  using Request = http::Request<std::string>;

  struct Auth : GoogleDrive::Auth {
    static std::string GetAuthorizationUrl(const AuthData& data);
  };

  using AuthManager = util::AuthManager<Auth>;

  enum class Presentation { kDash, kStream, kMuxedStreamWebm, kMuxedStreamMp4 };

  struct ItemData {
    std::string id;
    std::string name;
  };

  struct ThumbnailData {
    std::optional<std::string> default_quality_url;
    std::optional<std::string> high_quality_url;
  };

  struct RootDirectory : ItemData {
    Presentation presentation;
  };

  struct StreamDirectory : ItemData {
    std::string video_id;
    int64_t timestamp;
  };

  struct Playlist : ItemData {
    std::string playlist_id;
    Presentation presentation;
  };

  struct MuxedStreamWebm : ItemData {
    static constexpr std::string_view mime_type = "application/octet-stream";
    std::string video_id;
    int64_t timestamp;
    ThumbnailData thumbnail;
  };

  struct MuxedStreamMp4 : MuxedStreamWebm {};

  struct Stream : ItemData {
    std::string video_id;
    std::string mime_type;
    int64_t size;
    int64_t itag;
  };

  struct DashManifest : ItemData {
    static constexpr std::string_view mime_type = "application/dash+xml";
    static constexpr int64_t size = 32384;
    std::string video_id;
    int64_t timestamp;
    ThumbnailData thumbnail;
  };

  struct StreamData {
    json adaptive_formats;
    json formats;
    std::optional<std::function<std::string(std::string_view)>> descrambler;
    std::optional<std::function<std::string(std::string_view)>> new_descrambler;

    json GetBestVideo(std::string_view mime_type) const;
    json GetBestAudio(std::string_view mime_type) const;
  };

  using Item =
      std::variant<DashManifest, RootDirectory, Stream, MuxedStreamWebm,
                   MuxedStreamMp4, StreamDirectory, Playlist>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct GeneralData {
    std::string username;
  };

  struct FileContent {
    Generator<std::string> data;
    int64_t size;
  };

  struct Thumbnail {
    Generator<std::string> data;
    int64_t size;
    std::string mime_type;
  };

  static constexpr std::string_view kId = "youtube";
  static inline constexpr auto& kIcon = util::kAssetsProvidersYoutubePng;

  YouTube(AuthManager auth_manager, const http::Http* http,
          const util::Muxer* muxer)
      : auth_manager_(std::move(auth_manager)),
        http_(http),
        muxer_(muxer),
        stream_cache_(32, GetStreamData{*http}) {}

  Task<RootDirectory> GetRoot(stdx::stop_token);

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token);

  Task<PageData> ListDirectoryPage(StreamDirectory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token);

  Task<PageData> ListDirectoryPage(Playlist directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token);

  Task<PageData> ListDirectoryPage(RootDirectory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token);

  Generator<std::string> GetFileContent(Stream file, http::Range range,
                                        stdx::stop_token stop_token);

  Generator<std::string> GetFileContent(MuxedStreamWebm file, http::Range range,
                                        stdx::stop_token stop_token);

  Generator<std::string> GetFileContent(MuxedStreamMp4 file, http::Range range,
                                        stdx::stop_token stop_token);

  Generator<std::string> GetFileContent(DashManifest file, http::Range range,
                                        stdx::stop_token stop_token);

  Task<Thumbnail> GetItemThumbnail(DashManifest item, util::ThumbnailQuality,
                                   http::Range range,
                                   stdx::stop_token stop_token);

  Task<Thumbnail> GetItemThumbnail(MuxedStreamMp4 item, util::ThumbnailQuality,
                                   http::Range range,
                                   stdx::stop_token stop_token);

  Task<Thumbnail> GetItemThumbnail(MuxedStreamWebm item, util::ThumbnailQuality,
                                   http::Range range,
                                   stdx::stop_token stop_token);

 private:
  template <typename MuxedStream>
  Generator<std::string> GetMuxedFileContent(MuxedStream file,
                                             http::Range range,
                                             std::string_view type,
                                             stdx::stop_token stop_token);

  Generator<std::string> GetFileContentImpl(Stream file, http::Range range,
                                            stdx::stop_token stop_token);

  template <typename Item>
  Task<Thumbnail> GetItemThumbnailImpl(Item item,
                                       util::ThumbnailQuality quality,
                                       http::Range range,
                                       stdx::stop_token stop_token);

  Task<std::string> GetVideoUrl(std::string video_id, int64_t itag,
                                stdx::stop_token stop_token) const;

  struct GetStreamData {
    Task<StreamData> operator()(std::string video_id,
                                stdx::stop_token stop_token) const;
    const http::Http& http;
  };

  AuthManager auth_manager_;
  const http::Http* http_;
  const util::Muxer* muxer_;
  mutable coro::util::LRUCache<std::string, GetStreamData> stream_cache_;
};

namespace util {
template <>
YouTube::Auth::AuthData GetAuthData<YouTube>();
}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_YOUTUBE_H