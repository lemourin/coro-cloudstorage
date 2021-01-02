#ifndef CORO_CLOUDSTORAGE_YOUTUBE_H
#define CORO_CLOUDSTORAGE_YOUTUBE_H

#include <coro/cloudstorage/cloud_exception.h>
#include <coro/cloudstorage/providers/google_drive.h>
#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/task.h>
#include <coro/util/lru_cache.h>

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace coro::cloudstorage {

struct YouTube {
  using json = nlohmann::json;
  using Request = http::Request<>;

  static json GetConfig(std::string_view page_data);
  static std::string GetPlayerUrl(std::string_view page_data);
  static std::string GenerateDashManifest(std::string_view path,
                                          const json& stream_data);
  static std::function<std::string(std::string_view cipher)> GetDescrambler(
      std::string_view page);

  template <http::HttpClient Http>
  static Task<std::string> GetVideoPage(const Http& http, std::string video_id,
                                        stdx::stop_token stop_token) {
    auto response = co_await http.Fetch(
        "https://www.youtube.com/watch?v=" + video_id, stop_token);
    co_return co_await http::GetBody(std::move(response.body));
  }

  struct Auth : GoogleDrive::Auth {
    static std::string GetAuthorizationUrl(const AuthData& data) {
      return "https://accounts.google.com/o/oauth2/auth?" +
             http::FormDataToString({{"response_type", "code"},
                                     {"client_id", data.client_id},
                                     {"redirect_uri", data.redirect_uri},
                                     {"scope",
                                      "https://www.googleapis.com/auth/"
                                      "youtube.readonly openid email"},
                                     {"access_type", "offline"},
                                     {"prompt", "consent"},
                                     {"state", data.state}});
    }
  };

  template <typename AuthManager>
  struct CloudProvider;

  enum class Presentation { kDash, kStream };

  struct ItemData {
    std::string id;
    std::string name;
    std::string path;
  };

  struct RootDirectory : ItemData {
    Presentation presentation;
  };

  struct StreamDirectory : ItemData {
    int64_t timestamp;
  };

  struct Playlist : ItemData {
    Presentation presentation;
  };

  struct Stream : ItemData {
    std::string mime_type;
    int64_t timestamp;
    int64_t size;
    int64_t itag;
  };

  struct DashManifest : ItemData {
    static constexpr std::string_view mime_type = "application/dash+xml";
    static constexpr int64_t size = 8096;
    int64_t timestamp;
  };

  struct StreamData {
    json data;
    std::optional<std::function<std::string(std::string_view)>> descrambler;
  };

  using Item = std::variant<DashManifest, RootDirectory, Stream,
                            StreamDirectory, Playlist>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct GeneralData {
    std::string username;
  };

  static constexpr std::string_view kId = "youtube";
};

template <typename AuthManager>
struct YouTube::CloudProvider : YouTube {
  using Request = http::Request<std::string>;

  explicit CloudProvider(AuthManager auth_manager)
      : auth_manager_(std::move(auth_manager)),
        stream_cache_(32, GetStreamData{auth_manager_.GetHttp()}) {}

  Task<RootDirectory> GetRoot(stdx::stop_token) {
    RootDirectory d = {};
    d.id = "root";
    d.path = "/";
    d.presentation = Presentation::kDash;
    co_return d;
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) {
    json json = co_await auth_manager_.FetchJson(
        Request{.url = "https://openidconnect.googleapis.com/v1/userinfo"},
        std::move(stop_token));
    GeneralData result{.username = json["email"]};
    co_return result;
  }

  Task<PageData> ListDirectoryPage(StreamDirectory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
    PageData result;
    StreamData data = co_await stream_cache_.Get(directory.id, stop_token);
    for (const auto& d : data.data) {
      if (!d.contains("contentLength")) {
        continue;
      }
      std::string quality_label =
          (d.contains("qualityLabel") ? d["qualityLabel"] : d["audioQuality"]);
      std::string mime_type = d["mimeType"];
      std::string extension(mime_type.begin() + mime_type.find('/') + 1,
                            mime_type.begin() + mime_type.find(';'));
      Stream file;
      file.id = directory.id;
      file.name = "[" + quality_label + "] stream." + extension;
      file.mime_type = std::move(mime_type);
      file.size = std::stoll(std::string(d["contentLength"]));
      file.path = directory.path + file.name;
      file.itag = d["itag"];
      result.items.emplace_back(std::move(file));
    }
    co_return result;
  }

  Task<PageData> ListDirectoryPage(Playlist directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
    PageData result;
    std::vector<std::pair<std::string, std::string>> headers{
        {"part", "snippet"},
        {"playlistId", directory.id},
        {"maxResults", "50"}};
    if (page_token) {
      headers.emplace_back("pageToken", *page_token);
    }
    Request request = {.url = GetEndpoint("/playlistItems") + "?" +
                              http::FormDataToString(std::move(headers))};
    auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                     std::move(stop_token));
    for (const auto& item : response["items"]) {
      switch (directory.presentation) {
        case Presentation::kStream: {
          StreamDirectory streams;
          streams.id = item["snippet"]["resourceId"]["videoId"];
          streams.timestamp =
              http::ParseTime(std::string(item["snippet"]["publishedAt"]));
          streams.name = std::string(item["snippet"]["title"]);
          streams.path = directory.path + streams.name + "/";
          result.items.emplace_back(std::move(streams));
          break;
        }
        case Presentation::kDash: {
          DashManifest file;
          file.id = item["snippet"]["resourceId"]["videoId"];
          file.timestamp =
              http::ParseTime(std::string(item["snippet"]["publishedAt"]));
          file.name = std::string(item["snippet"]["title"]) + ".mpd";
          file.path = directory.path + file.name;
          result.items.emplace_back(std::move(file));
          break;
        }
      }
    }
    if (response.contains("nextPageToken")) {
      result.next_page_token = response["nextPageToken"];
    }
    co_return result;
  }

  Task<PageData> ListDirectoryPage(RootDirectory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
    PageData result;
    Request request = {
        .url = GetEndpoint("/channels") + "?" +
               http::FormDataToString({{"mine", "true"},
                                       {"part", "contentDetails,snippet"},
                                       {"maxResults", "50"}})};
    auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                     std::move(stop_token));
    for (const auto& [key, value] :
         response["items"][0]["contentDetails"]["relatedPlaylists"].items()) {
      result.items.emplace_back(Playlist{
          {.id = value, .name = key, .path = directory.path + key + "/"},
          directory.presentation});
    }
    if (directory.presentation == Presentation::kDash) {
      result.items.emplace_back(
          RootDirectory{{.id = "root", .name = "streams", .path = "/"},
                        Presentation::kStream});
    }
    co_return result;
  }

  Generator<std::string> GetFileContent(Stream file, http::Range range,
                                        stdx::stop_token stop_token) {
    std::stringstream range_header;
    range_header << "bytes=" << range.start << "-";
    if (range.end) {
      range_header << *range.end;
    }
    std::string video_url =
        co_await GetVideoUrl(file.id, file.itag, stop_token);
    Request request{.url = std::move(video_url),
                    .headers = {{"Range", range_header.str()}}};
    auto response =
        co_await auth_manager_.GetHttp().Fetch(std::move(request), stop_token);
    if (response.status / 100 == 4) {
      stream_cache_.Invalidate(file.id);
      video_url = co_await GetVideoUrl(file.id, file.itag, stop_token);
      Request retry_request{.url = std::move(video_url),
                            .headers = {{"Range", range_header.str()}}};
      response = co_await auth_manager_.GetHttp().Fetch(
          std::move(retry_request), stop_token);
    }

    int max_redirect_count = 8;
    while (response.status == 302 && max_redirect_count-- > 0) {
      auto redirect_request = Request{
          .url = coro::http::GetHeader(response.headers, "Location").value(),
          .headers = {{"Range", std::move(range_header).str()}}};
      response = co_await auth_manager_.GetHttp().Fetch(
          std::move(redirect_request), std::move(stop_token));
    }
    if (response.status / 100 != 2) {
      throw http::HttpException(response.status);
    }

    FOR_CO_AWAIT(std::string & body, response.body) {
      co_yield std::move(body);
    }
  }

  Generator<std::string> GetFileContent(DashManifest file, http::Range range,
                                        stdx::stop_token stop_token) {
    StreamData data =
        co_await stream_cache_.Get(file.id, std::move(stop_token));
    std::string dash_manifest = GenerateDashManifest(
        "../streams" + file.path.substr(0, file.path.size() - 4), data.data);
    if ((range.end && range.end >= kDashManifestSize) ||
        range.start >= kDashManifestSize) {
      throw http::HttpException(http::HttpException::kRangeNotSatisfiable);
    }
    dash_manifest.resize(kDashManifestSize, ' ');
    co_yield std::move(dash_manifest)
        .substr(range.start,
                range.end.value_or(kDashManifestSize - 1) - range.start + 1);
  }

 private:
  static constexpr std::string_view kEndpoint =
      "https://www.googleapis.com/youtube/v3";
  static constexpr int32_t kDashManifestSize = 8096;

  static std::string GetEndpoint(std::string_view path) {
    return std::string(kEndpoint) + std::string(path);
  }

  Task<std::string> GetVideoUrl(std::string video_id, int64_t itag,
                                stdx::stop_token stop_token) const {
    StreamData data = co_await stream_cache_.Get(video_id, stop_token);
    std::optional<std::string> url;
    for (const auto& d : data.data) {
      if (d["itag"] == itag) {
        if (d.contains("url")) {
          url = d["url"];
        } else {
          url = (*data.descrambler)(std::string(d["signatureCipher"]));
        }
      }
    }
    if (!url) {
      throw CloudException(CloudException::Type::kNotFound);
    }
    co_return* url;
  }

  struct GetStreamData {
    Task<StreamData> operator()(std::string video_id,
                                stdx::stop_token stop_token) const {
      std::string page =
          co_await GetVideoPage(http, std::move(video_id), stop_token);
      json config = GetConfig(page);
      StreamData result{
          .data = std::move(config["streamingData"]["adaptiveFormats"])};
      for (const auto& d : result.data) {
        if (!d.contains("url")) {
          auto response = co_await http.Fetch(GetPlayerUrl(page), stop_token);
          result.descrambler =
              GetDescrambler(co_await http::GetBody(std::move(response.body)));
          break;
        }
      }
      co_return result;
    }
    const typename AuthManager::Http& http;
  };

  AuthManager auth_manager_;
  coro::util::LRUCache<std::string, GetStreamData> stream_cache_;
};

namespace util {
template <>
inline YouTube::Auth::AuthData GetAuthData<YouTube>() {
  return {
      .client_id =
          R"(379556609343-0v8r2fpijkjpj707a76no2rve6nto2co.apps.googleusercontent.com)",
      .client_secret = "_VUpM5Pf9_54RIZq2GGUbUMZ"};
}
}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_YOUTUBE_H