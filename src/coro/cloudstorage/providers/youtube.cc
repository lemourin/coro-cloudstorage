#include "coro/cloudstorage/providers/youtube.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"
#include "coro/cloudstorage/util/evaluate_javascript.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/util/regex.h"
#include "coro/when_all.h"

namespace coro::cloudstorage {

namespace {

enum class TransformType { kReverse, kSplice, kSwap };

using ::coro::cloudstorage::util::CreateAbstractCloudProviderImpl;
using ::coro::cloudstorage::util::MediaContainer;
using ::coro::cloudstorage::util::StrCat;
using ::coro::cloudstorage::util::ThumbnailQuality;
using ::coro::cloudstorage::util::ToStringView;
using ::nlohmann::json;

using JsFunction = coro::cloudstorage::util::js::Function;

namespace re = ::coro::util::re;

constexpr std::string_view kEndpoint = "https://www.googleapis.com/youtube/v3";
constexpr std::string_view kChannelPlayListsPageToken = "CHANNEL_PLAYLISTS";
constexpr std::string_view kUserPlayListsPageToken = "USER_PLAYLISTS";
constexpr int kMaxRedirectCount = 8;

std::string GetEndpoint(std::string_view path) {
  return StrCat(kEndpoint, path);
}

std::string EscapeRegex(std::string_view input) {
  re::regex special_characters{R"([-[\]{}()*+?.,\^$|#\s])"};
  return re::regex_replace(std::string(input), special_characters, R"(\\$&)");
}

std::string XmlAttributes(
    const std::vector<std::pair<std::string, std::string>>& args) {
  std::stringstream stream;
  bool first = true;
  for (const auto& [key, value] : args) {
    if (!first) {
      stream << " ";
    } else {
      first = false;
    }
    stream << key << "=\"" << value << '"';
  }
  return stream.str();
}

std::optional<std::string> Find(std::string_view text,
                                const std::initializer_list<re::regex>& re) {
  re::match_results<std::string_view::iterator> match;
  for (const auto& regex : re) {
    if (re::regex_search(text.begin(), text.end(), match, regex)) {
      return match[static_cast<int>(match.size() - 1)].str();
    }
  }
  return std::nullopt;
}

YouTube::ThumbnailData GetThumbnailData(const nlohmann::json& json) {
  YouTube::ThumbnailData data;
  if (json.contains("default")) {
    data.default_quality_url = json["default"]["url"];
  }
  if (json.contains("maxres")) {
    data.high_quality_url = json["maxres"]["url"];
  } else if (json.contains("standard")) {
    data.high_quality_url = json["standard"]["url"];
  } else if (json.contains("high")) {
    data.high_quality_url = json["high"]["url"];
  } else if (json.contains("medium")) {
    data.high_quality_url = json["medium"]["url"];
  }
  return data;
}

JsFunction GetFunction(std::string_view document,
                       std::string_view function_name) {
  re::match_results<std::string_view::iterator> match;
  if (re::regex_search(
          document.begin(), document.end(), match,
          re::regex{StrCat(
              R"((?:)", EscapeRegex(function_name),
              R"(\s*=\s*function\s*)\(([^\)]*)\)\s*(\{[\s\S]+?return b.join\(\"\"\)\};))")})) {
    auto args = util::SplitString(match[1].str(), ',');
    for (auto& arg : args) {
      arg = http::TrimWhitespace(arg);
    }
    return JsFunction{std::string(function_name), std::move(args),
                      match[2].str()};
  } else {
    throw CloudException(StrCat("function ", function_name, " not found"));
  }
}

std::vector<std::string> Split(std::string_view text, char /*delimiter*/) {
  std::vector<std::string> result;
  std::string current;
  int balance = 0;
  for (char c : text) {
    if (c == '(' || c == '[' || c == '{') {
      balance++;
      current += c;
    } else if (c == ')' || c == ']' || c == '}') {
      balance--;
      current += c;
    } else if (c == ',') {
      if (balance == 0) {
        result.emplace_back(http::TrimWhitespace(current));
        current.clear();
      } else {
        current += c;
      }
    } else {
      current += c;
    }
  }
  if (!current.empty()) {
    result.emplace_back(std::move(current));
  }
  return result;
}

template <typename MuxedStreamT>
MuxedStreamT ToMuxedStream(YouTube::ItemId id, json item) {
  MuxedStreamT stream;
  stream.timestamp =
      http::ParseTime(std::string(item["snippet"]["publishedAt"]));
  stream.name = StrCat(std::string(item["snippet"]["title"]), [] {
    if constexpr (std::is_same_v<MuxedStreamT, YouTube::MuxedStreamMp4>) {
      return ".mp4";
    } else {
      return ".webm";
    }
  }());
  stream.id = std::move(id);
  stream.thumbnail = GetThumbnailData(item["snippet"]["thumbnails"]);
  return stream;
}

Task<std::string> GetVideoPage(const http::Http& http, std::string video_id,
                               stdx::stop_token stop_token) {
  auto response = co_await http.Fetch(
      "https://www.youtube.com/watch?v=" + video_id, stop_token);
  co_return co_await http::GetBody(std::move(response.body));
}

YouTube::Stream ToStream(std::string_view video_id, std::string_view name,
                         json d) {
  std::string mime_type = d["mimeType"];
  std::string extension(
      mime_type.begin() +
          static_cast<std::string::difference_type>(mime_type.find('/') + 1),
      mime_type.begin() +
          static_cast<std::string::difference_type>(mime_type.find(';')));
  YouTube::Stream stream;
  if (d.contains("qualityLabel")) {
    stream.name += StrCat('[', std::string(d["qualityLabel"]), ']');
  }
  if (d.contains("audioQuality")) {
    stream.name += StrCat('[', std::string(d["audioQuality"]), ']');
  }
  stream.name +=
      StrCat('[', static_cast<int>(d["itag"]), "] ", name, '.', extension);
  stream.mime_type = std::move(mime_type);
  stream.size = std::stoll(std::string(d["contentLength"]));
  stream.id = {.type = YouTube::ItemId::Type::kStream,
               .id = std::string(video_id),
               .itag = d["itag"]};
  return stream;
}

nlohmann::json GetConfig(std::string_view page_data) {
  constexpr std::string_view kPattern = "var ytInitialPlayerResponse = ";
  auto it = page_data.find(kPattern);
  if (it != std::string_view::npos) {
    std::stringstream stream;
    stream << std::string(page_data.begin() + it + kPattern.length(),
                          page_data.end());
    json json;
    stream >> json;
    return json;
  } else {
    throw CloudException("ytInitialPlayerResponse not found");
  }
}

std::string GenerateDashManifest(const util::ItemUrlProvider& item_url_provider,
                                 std::string_view video_id,
                                 const json& stream_data) {
  std::stringstream r;
  int64_t duration = 0;
  for (const auto& d : stream_data) {
    duration = std::max<int64_t>(
        duration, std::stoll(std::string(d["approxDurationMs"])));
  }
  r << "<MPD "
    << XmlAttributes(
           {{"xmlns:xsi", "http://www.w3.org/2001/XMLSchema-instance"},
            {"xmlns", "urn:mpeg:dash:schema:mpd:2011"},
            {"xsi:schemaLocation",
             "urn:mpeg:dash:schema:mpd:2011 DASH-MPD.xsd"},
            {"type", "static"},
            {"mediaPresentationDuration",
             StrCat("PT", (duration + 1000 - 1) / 1000, "S")},
            {"minBufferTime", "PT2S"},
            {"profiles", "urn:mpeg:dash:profile:isoff-on-demand:2011"}})
    << ">";
  r << "<Period>";
  std::unordered_map<std::string, std::vector<json>> grouped;
  for (const auto& d : stream_data) {
    std::string mimetype = d["mimeType"];
#ifdef WINRT
    if (mimetype.find("mp4") == std::string::npos) {
      continue;
    }
    if (mimetype.find("av01") != std::string::npos) {
      continue;
    }
#endif
    grouped[mimetype.substr(0, mimetype.find(';'))].emplace_back(d);
  }
  int adaptation_set_index = 0;
  for (std::string mimetype :
       {"video/mp4", "audio/mp4", "video/webm", "audio/webm"}) {
    auto it = grouped.find(mimetype);
    if (it == grouped.end()) {
      continue;
    }
    const auto& streams = it->second;
    int stream_count = 0;
    for (const auto& stream : streams) {
      if (stream.contains("indexRange") && stream.contains("initRange")) {
        stream_count++;
      }
    }
    if (stream_count == 0) {
      continue;
    }
    std::string type = mimetype.substr(0, mimetype.find('/'));
    r << "<AdaptationSet "
      << XmlAttributes({{"id", std::to_string(adaptation_set_index++)},
                        {"mimeType", mimetype},
                        {"contentType", type},
                        {"bitstreamSwitching", "true"},
                        {"segmentAlignment", "true"},
                        {"subsegmentAlignment", "true"},
                        {"subsegmentStartsWithSAP", "1"},
                        {"startWithSAP", "1"}})
      << ">";

    for (const auto& stream : streams) {
      if (!stream.contains("indexRange") || !stream.contains("initRange")) {
        continue;
      }
      std::string full_mimetype = stream["mimeType"];
      std::string codecs = full_mimetype.substr(full_mimetype.find(';') + 2);
      std::string extension(
          full_mimetype.begin() + static_cast<std::string::difference_type>(
                                      full_mimetype.find('/') + 1),
          full_mimetype.begin() + static_cast<std::string::difference_type>(
                                      full_mimetype.find(';')));
      r << "<Representation "
        << XmlAttributes(
               {{"id", std::to_string(int64_t(stream["itag"]))},
                {"bandwidth", std::to_string(int64_t(stream["bitrate"]))}})
        << " " << codecs;
      if (type == "video") {
        r << " "
          << XmlAttributes(
                 {{"width", std::to_string(int64_t(stream["width"]))},
                  {"height", std::to_string(int64_t(stream["height"]))},
                  {"frameRate", std::to_string(int64_t(stream["fps"]))}});
      } else if (type == "audio") {
        r << " "
          << XmlAttributes({{"audioSamplingRate", stream["audioSampleRate"]}});
      }
      r << ">";
      r << "<SegmentBase "
        << XmlAttributes(
               {{"indexRange",
                 StrCat(std::string(stream["indexRange"]["start"]), "-",
                        std::string(stream["indexRange"]["end"]))}})
        << ">";
      r << "<Initialization "
        << XmlAttributes(
               {{"range", StrCat(std::string(stream["initRange"]["start"]), "-",
                                 std::string(stream["initRange"]["end"]))}})
        << "/>";
      r << "</SegmentBase>";
      r << "<BaseURL>"
        << item_url_provider(util::ToString(
               YouTube::ItemId{.type = YouTube::ItemId::Type::kStream,
                               .id = std::string(video_id),
                               .itag = stream["itag"]}))
        << "</BaseURL>";
      r << "</Representation>";
    }
    r << "</AdaptationSet>";
  }

  r << "</Period></MPD>";
  return r.str();
}

std::string GetPlayerUrl(std::string_view page_data) {
  re::match_results<std::string_view::iterator> match;
  if (re::regex_search(page_data.begin(), page_data.end(), match,
                       re::regex(R"re("jsUrl":"([^"]*)")re"))) {
    return StrCat("https://www.youtube.com/", match[1].str());
  } else {
    throw CloudException("jsUrl not found");
  }
}

std::function<std::string(std::string_view)> GetDescrambler(
    std::string_view page_data) {
  auto descrambler =
      Find(
          page_data,
          {re::regex(
               R"re(([a-zA-Z0-9$]+)\s*=\s*function\(\s*a\s*\)\s*\{\s*a\s*=\s*a\.split\(\s*""\s*\))re"),
           re::regex(
               R"re((?:\b|[^a-zA-Z0-9$])([a-zA-Z0-9$]{2})\s*=\s*function\(\s*a\s*\)\s*\{\s*a\s*=\s*a\.split\(\s*""\s*\))re")})
          .value();
  auto rules =
      Find(page_data, {re::regex(StrCat(EscapeRegex(descrambler),
                                        R"(=function[^{]*\{([^}]*)\};)"))})
          .value();
  auto helper = Find(rules, {re::regex(R"(;([^\.]*)\.)")}).value();
  auto transforms =
      Find(page_data,
           {re::regex(StrCat(EscapeRegex(helper), R"(=\{([\s\S]*?)\};)"))})
          .value();
  std::unordered_map<std::string, TransformType> transform_type;
  transform_type[Find(transforms, {re::regex(R"(([^:]{2}):[^:]*reverse)")})
                     .value()] = TransformType::kReverse;
  transform_type[Find(transforms, {re::regex(R"(([^:]{2}):[^:]*splice)")})
                     .value()] = TransformType::kSplice;
  transform_type[Find(transforms, {re::regex(R"(([^:]{2}):[^:]*\[0\])")})
                     .value()] = TransformType::kSwap;

  return [rules, helper, transform_type](std::string_view sig) {
    auto data = http::ParseQuery(sig);
    std::string signature = data["s"];
    size_t it = 0;
    while (it < rules.size()) {
      auto next = rules.find(';', it);
      if (next == std::string_view::npos) {
        break;
      }
      std::string_view transform(rules.data() + it, next - it);
      re::match_results<std::string_view::iterator> match;
      if (re::regex_match(
              transform.begin(), transform.end(), match,
              re::regex(StrCat(EscapeRegex(helper),
                               R"re(\.([^\(]*)\([^,]*,([^\)]*)\))re")))) {
        std::string func = match[1].str();
        int arg = std::stoi(match[2].str());
        switch (transform_type.at(func)) {
          case TransformType::kReverse:
            std::reverse(signature.begin(), signature.end());
            break;
          case TransformType::kSplice:
            signature.erase(0, arg);
            break;
          case TransformType::kSwap:
            std::swap(signature[0], signature[arg % signature.length()]);
            break;
        }
      }
      it = next + 1;
    }
    return StrCat(data["url"], "&", data["sp"], "=", signature);
  };
}

std::optional<std::function<std::string(std::string_view cipher)>>
GetNewDescrambler(std::string_view page_data) {
  re::regex regex(
      R"(\.get\("n"\)\)&&\(b=([a-zA-Z0-9$]{3})(?:\[(\d+)\])?\([a-zA-Z0-9]\))");
  re::match_results<std::string_view::iterator> match;
  if (!re::regex_search(page_data.begin(), page_data.end(), match, regex)) {
    return std::nullopt;
  }
  std::optional<std::string> nsig_function_name =
      [&]() -> std::optional<std::string> {
    std::string_view func = ToStringView(match[1].begin(), match[1].end());
    if (match.size() == 2) {
      return std::string(func);
    }
    re::regex regex(StrCat("var ", func, R"(\s*=\s*\[(.+?)\];)"));
    re::match_results<std::string_view::iterator> func_match;
    if (!re::regex_search(page_data.begin(), page_data.end(), func_match,
                          regex)) {
      return std::nullopt;
    }
    auto func_names =
        Split(ToStringView(func_match[1].begin(), func_match[1].end()), ',');
    int index = std::stoi(match[2].str());
    if (index >= 0 && index < func_names.size()) {
      return func_names[index];
    }
    return std::nullopt;
  }();
  if (!nsig_function_name) {
    return std::nullopt;
  }
  return [nsig_function = GetFunction(page_data, *nsig_function_name)](
             std::string_view nsig) {
    return util::js::EvaluateJavascript(nsig_function,
                                        std::vector{std::string(nsig)});
  };
}

template <typename T>
T ToItem(const nlohmann::json& json) {
  T item;
  item.id.type = static_cast<YouTube::ItemId::Type>(int(json["type"]));
  item.id.id = json["id"];
  item.name = json["name"];
  return item;
}

nlohmann::json ToJson(const YouTube::ThumbnailData& thumbnail) {
  nlohmann::json json;
  if (thumbnail.high_quality_url) {
    json["high_quality_url"] = *thumbnail.high_quality_url;
  }
  if (thumbnail.default_quality_url) {
    json["default_quality_url"] = *thumbnail.default_quality_url;
  }
  return json;
}

YouTube::ThumbnailData ToThumbnailData(const nlohmann::json& json) {
  YouTube::ThumbnailData data;
  if (auto it = json.find("high_quality_url"); it != json.end()) {
    data.high_quality_url = *it;
  }
  if (auto it = json.find("default_quality_url"); it != json.end()) {
    data.default_quality_url = *it;
  }
  return data;
}

std::string GetVideoUrl(const YouTube::StreamData& data, int64_t itag) {
  std::optional<std::string> url;
  for (const auto& formats : {data.adaptive_formats, data.formats}) {
    for (const auto& d : formats) {
      if (d["itag"] == itag) {
        if (d.contains("url")) {
          url = d["url"];
        } else {
          url = (*data.descrambler)(std::string(d["signatureCipher"]));
        }
        auto uri = http::ParseUri(*url);
        auto params = http::ParseQuery(uri.query.value_or(""));
        if (auto it = params.find("n");
            it != params.end() && data.new_descrambler) {
          it->second = (*data.new_descrambler)(it->second);
          url = StrCat(uri.scheme.value_or("https"), "://",
                       uri.host.value_or(""), uri.path.value_or(""), '?',
                       http::FormDataToString(params));
        }
      }
    }
  }
  if (!url) {
    throw CloudException(CloudException::Type::kNotFound);
  }
  return *url;
}

}  // namespace

std::string YouTube::Auth::GetAuthorizationUrl(const AuthData& data) {
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

nlohmann::json YouTube::StreamData::GetBestVideo(
    std::string_view mime_type) const {
  std::optional<json> json;
  for (const auto& d : adaptive_formats) {
    std::string current_mime_type = d["mimeType"];
    if (current_mime_type.find(mime_type) != std::string::npos &&
        (!json || d["bitrate"] > (*json)["bitrate"])) {
      json = d;
    }
  }
  if (!json) {
    throw CloudException("video not found");
  }
  return *json;
}

nlohmann::json YouTube::StreamData::GetBestAudio(
    std::string_view mime_type) const {
  std::optional<json> json;
  for (const auto& d : adaptive_formats) {
    std::string current_mime_type = d["mimeType"];
    if (current_mime_type.find(mime_type) != std::string::npos &&
        (!json || d["bitrate"] > (*json)["bitrate"])) {
      json = d;
    }
  }
  if (!json) {
    throw CloudException("audio not found");
  }
  return *json;
}

auto YouTube::GetRoot(stdx::stop_token) -> Task<RootDirectory> {
  RootDirectory d = {};
  d.id = {.type = ItemId::Type::kRootDirectory,
          .id = "root",
          .presentation = Presentation::kDash};
  co_return d;
}

auto YouTube::GetItem(ItemId id, stdx::stop_token stop_token) -> Task<Item> {
  switch (id.type) {
    case ItemId::Type::kRootDirectory:
      co_return RootDirectory{{.id = id, .name = id.id}};
    case ItemId::Type::kPlaylist: {
      Request request = {
          .url = StrCat(
              GetEndpoint("/playlists"), '?',
              http::FormDataToString({{"part", "snippet"}, {"id", id.id}}))};
      auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                       std::move(stop_token));
      if (response["items"].empty()) {
        throw CloudException(CloudException::Type::kNotFound);
      }
      nlohmann::json entry = response["items"][0];
      Playlist playlist{};
      playlist.id = id;
      playlist.name = entry["snippet"]["title"];
      co_return playlist;
    }
    case ItemId::Type::kStream: {
      StreamData data = co_await stream_cache_.Get(id.id, stop_token);
      for (const auto& formats : {data.adaptive_formats, data.formats}) {
        for (const auto& d : formats) {
          if (d["itag"] == id.itag) {
            co_return ToStream(id.id, data.title, d);
          }
        }
      }
      throw CloudException(CloudException::Type::kNotFound);
    }
    default: {
      Request request = {
          .url = StrCat(
              GetEndpoint("/videos"), '?',
              http::FormDataToString({{"part", "snippet"}, {"id", id.id}}))};
      auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                       std::move(stop_token));
      if (response["items"].empty()) {
        throw CloudException(CloudException::Type::kNotFound);
      }
      nlohmann::json entry = response["items"][0];
      int64_t timestamp =
          http::ParseTime(std::string(entry["snippet"]["publishedAt"]));
      std::string name{entry["snippet"]["title"]};
      if (id.type == ItemId::Type::kDashManifest) {
        DashManifest item{};
        item.id = id;
        item.name = StrCat(name, ".mpd");
        item.timestamp = timestamp;
        item.thumbnail = GetThumbnailData(entry["snippet"]["thumbnails"]);
        co_return item;
      } else if (id.type == ItemId::Type::kStreamDirectory) {
        StreamDirectory item;
        item.id = id;
        item.name = std::move(name);
        item.timestamp = timestamp;
        co_return item;
      } else if (id.type == ItemId::Type::kMuxedStreamMp4) {
        co_return ToMuxedStream<MuxedStreamMp4>(std::move(id),
                                                std::move(entry));
      } else if (id.type == ItemId::Type::kMuxedStreamWebm) {
        co_return ToMuxedStream<MuxedStreamWebm>(std::move(id),
                                                 std::move(entry));
      } else {
        throw CloudException("YouTube::ItemId::Type unsupported.");
      }
    }
  }
}

auto YouTube::GetGeneralData(stdx::stop_token stop_token) -> Task<GeneralData> {
  Request request{.url = "https://openidconnect.googleapis.com/v1/userinfo"};
  json json = co_await auth_manager_.FetchJson(std::move(request),
                                               std::move(stop_token));
  GeneralData result{.username = json["email"]};
  co_return result;
}

auto YouTube::ListDirectoryPage(StreamDirectory directory,
                                std::optional<std::string> page_token,
                                stdx::stop_token stop_token) -> Task<PageData> {
  PageData result;
  StreamData data = co_await stream_cache_.Get(directory.id.id, stop_token);
  for (const auto& formats : {data.adaptive_formats, data.formats}) {
    for (const auto& d : formats) {
      if (!d.contains("contentLength")) {
        continue;
      }
      result.items.emplace_back(ToStream(directory.id.id, directory.name, d));
    }
  }
  co_return result;
}

auto YouTube::ListDirectoryPage(Playlist directory,
                                std::optional<std::string> page_token,
                                stdx::stop_token stop_token) -> Task<PageData> {
  PageData result;
  std::vector<std::pair<std::string, std::string>> params{
      {"part", "snippet,contentDetails,status"},
      {"playlistId", directory.id.id},
      {"maxResults", "50"}};
  if (page_token) {
    params.emplace_back("pageToken", *page_token);
  }
  Request request = {.url = StrCat(GetEndpoint("/playlistItems"), '?',
                                   http::FormDataToString(params))};
  auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                   std::move(stop_token));
  for (const auto& item : response["items"]) {
    auto content_details = item["contentDetails"];
    auto video_published_at = content_details.find("videoPublishedAt");
    if (video_published_at == content_details.end()) {
      continue;
    }
    if (item["status"]["privacyStatus"] == "private") {
      continue;
    }
    switch (directory.id.presentation) {
      case Presentation::kMuxedStreamMp4: {
        result.items.emplace_back(ToMuxedStream<MuxedStreamMp4>(
            {.type = ItemId::Type::kMuxedStreamMp4,
             .id = item["snippet"]["resourceId"]["videoId"]},
            item));
        break;
      }
      case Presentation::kMuxedStreamWebm: {
        result.items.emplace_back(ToMuxedStream<MuxedStreamWebm>(
            {.type = ItemId::Type::kMuxedStreamWebm,
             .id = item["snippet"]["resourceId"]["videoId"]},
            item));
        break;
      }
      case Presentation::kStream: {
        StreamDirectory streams;
        streams.timestamp = http::ParseTime(std::string(*video_published_at));
        streams.name = std::string(item["snippet"]["title"]);
        streams.id = {.type = ItemId::Type::kStreamDirectory,
                      .id = item["snippet"]["resourceId"]["videoId"]};
        result.items.emplace_back(std::move(streams));
        break;
      }
      case Presentation::kDash: {
        DashManifest file;
        file.timestamp = http::ParseTime(std::string(*video_published_at));
        file.name = StrCat(std::string(item["snippet"]["title"]), ".mpd");
        file.id = {.type = ItemId::Type::kDashManifest,
                   .id = item["snippet"]["resourceId"]["videoId"]};
        file.thumbnail = GetThumbnailData(item["snippet"]["thumbnails"]);
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

auto YouTube::ListDirectoryPage(RootDirectory directory,
                                std::optional<std::string> page_token,
                                stdx::stop_token stop_token) -> Task<PageData> {
  PageData result;
  if (!page_token && directory.id.presentation == Presentation::kDash) {
    result.items.emplace_back(
        RootDirectory{{.id = {.type = ItemId::Type::kRootDirectory,
                              .id = "streams",
                              .presentation = Presentation::kStream},
                       .name = "streams"}});
    result.items.emplace_back(
        RootDirectory{{.id = {.type = ItemId::Type::kRootDirectory,
                              .id = "muxed-webm",
                              .presentation = Presentation::kMuxedStreamWebm},
                       .name = "muxed-webm"}});
    result.items.emplace_back(
        RootDirectory{{.id = {.type = ItemId::Type::kRootDirectory,
                              .id = "muxed-mp4",
                              .presentation = Presentation::kMuxedStreamMp4},
                       .name = "muxed-mp4"}});
    result.next_page_token = kChannelPlayListsPageToken;
  } else if (!page_token || page_token == kChannelPlayListsPageToken) {
    Request request = {
        .url = StrCat(GetEndpoint("/channels"), '?',
                      http::FormDataToString({{"mine", "true"},
                                              {"part", "contentDetails"},
                                              {"maxResults", "50"}}))};
    auto response =
        co_await auth_manager_.FetchJson(std::move(request), stop_token);
    std::string ids;
    for (const auto& [_, playlist_id] :
         response["items"][0]["contentDetails"]["relatedPlaylists"].items()) {
      if (!ids.empty()) {
        ids += ',';
      }
      ids += playlist_id;
    }
    {
      Request request = {
          .url = StrCat(
              GetEndpoint("/playlists"), '?',
              http::FormDataToString({{"part", "snippet"}, {"id", ids}}))};
      auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                       std::move(stop_token));
      for (const auto& item : response["items"]) {
        result.items.emplace_back(
            Playlist{{.id = {.type = ItemId::Type::kPlaylist,
                             .id = item["id"],
                             .presentation = directory.id.presentation},
                      .name = item["snippet"]["title"]}});
      }
    }
    result.next_page_token = kUserPlayListsPageToken;
  } else if (page_token == kUserPlayListsPageToken) {
    std::vector<std::pair<std::string, std::string>> params = {
        {"part", "snippet"}, {"mine", "true"}};
    if (page_token != kUserPlayListsPageToken) {
      params.emplace_back("pageToken", *page_token);
    }
    Request request = {.url = StrCat(GetEndpoint("/playlists"), '?',
                                     http::FormDataToString(params))};
    auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                     std::move(stop_token));
    for (const auto& item : response["items"]) {
      result.items.emplace_back(
          Playlist{{.id = {.type = ItemId::Type::kPlaylist,
                           .id = item["id"],
                           .presentation = directory.id.presentation},
                    .name = item["snippet"]["title"]}});
    }
    if (auto it = response.find("nextPageToken"); it != response.end()) {
      result.next_page_token = *it;
    }
  }
  co_return result;
}

Generator<std::string> YouTube::GetFileContent(Stream file, http::Range range,
                                               stdx::stop_token stop_token) {
  if (!range.end) {
    range.end = file.size - 1;
  }
  const auto kChunkSize = 10'000'000;
  for (int64_t i = range.start; i <= *range.end; i += kChunkSize) {
    http::Range subrange{
        .start = i, .end = std::min<int64_t>(i + kChunkSize - 1, *range.end)};
    FOR_CO_AWAIT(std::string & chunk,
                 GetFileContentImpl(file, subrange, stop_token)) {
      co_yield std::move(chunk);
    }
  }
}

Generator<std::string> YouTube::GetFileContent(MuxedStreamWebm file,
                                               http::Range range,
                                               stdx::stop_token stop_token) {
  return GetMuxedFileContent(std::move(file), range, "webm",
                             std::move(stop_token));
}

Generator<std::string> YouTube::GetFileContent(MuxedStreamMp4 file,
                                               http::Range range,
                                               stdx::stop_token stop_token) {
  return GetMuxedFileContent(std::move(file), range, "mp4",
                             std::move(stop_token));
}

Generator<std::string> YouTube::GetFileContent(DashManifest file,
                                               http::Range range,
                                               stdx::stop_token stop_token) {
  StreamData data =
      co_await stream_cache_.Get(file.id.id, std::move(stop_token));
  auto strip_extension = [](std::string_view str) {
    return std::string(str.substr(0, str.size() - 4));
  };
  std::string dash_manifest = GenerateDashManifest(
      item_url_provider_, file.id.id, data.adaptive_formats);
  if ((range.end && range.end >= DashManifest::size) ||
      range.start >= DashManifest::size) {
    throw http::HttpException(http::HttpException::kRangeNotSatisfiable);
  }
  dash_manifest.resize(DashManifest::size, ' ');
  co_yield std::move(dash_manifest)
      .substr(static_cast<size_t>(range.start),
              static_cast<size_t>(range.end.value_or(DashManifest::size - 1) -
                                  range.start + 1));
}

auto YouTube::GetItemThumbnail(DashManifest item, ThumbnailQuality quality,
                               http::Range range, stdx::stop_token stop_token)
    -> Task<Thumbnail> {
  return GetItemThumbnailImpl(std::move(item), quality, range,
                              std::move(stop_token));
}

auto YouTube::GetItemThumbnail(MuxedStreamMp4 item, ThumbnailQuality quality,
                               http::Range range, stdx::stop_token stop_token)
    -> Task<Thumbnail> {
  return GetItemThumbnailImpl(std::move(item), quality, range,
                              std::move(stop_token));
}

auto YouTube::GetItemThumbnail(MuxedStreamWebm item, ThumbnailQuality quality,
                               http::Range range, stdx::stop_token stop_token)
    -> Task<Thumbnail> {
  return GetItemThumbnailImpl(std::move(item), quality, range,
                              std::move(stop_token));
}

template <typename MuxedStream>
Generator<std::string> YouTube::GetMuxedFileContent(
    MuxedStream file, http::Range range, std::string_view type,
    stdx::stop_token stop_token) {
  if (range.start != 0 || range.end) {
    throw CloudException("partial read unsupported");
  }
  StreamData data = co_await stream_cache_.Get(file.id.id, stop_token);
  Stream video_stream{};
  auto best_video = data.GetBestVideo(StrCat("video/", type));
  video_stream.id.id = file.id.id;
  video_stream.id.itag = best_video["itag"];
  video_stream.size = std::stoll(std::string(best_video["contentLength"]));
  Stream audio_stream{};
  audio_stream.id.id = std::move(file.id.id);
  auto best_audio = data.GetBestAudio(StrCat("audio/", type));
  audio_stream.id.itag = best_audio["itag"];
  audio_stream.size = std::stoll(std::string(best_audio["contentLength"]));
  auto impl = CreateAbstractCloudProviderImpl(this);
  FOR_CO_AWAIT(std::string & chunk,
               (*muxer_)(&impl, impl.Convert(std::move(video_stream)), &impl,
                         impl.Convert(std::move(audio_stream)),
                         util::MuxerOptions{
                             .container = type == "webm" ? MediaContainer::kWebm
                                                         : MediaContainer::kMp4,
                             .buffered = type != "mp4"},
                         std::move(stop_token))) {
    co_yield std::move(chunk);
  }
}

Generator<std::string> YouTube::GetFileContentImpl(
    Stream file, http::Range range, stdx::stop_token stop_token) const {
  auto stream_data = co_await stream_cache_.Get(file.id.id, stop_token);
  std::string video_url = GetVideoUrl(stream_data, file.id.itag);
  Request request{.url = std::move(video_url),
                  .headers = {http::ToRangeHeader(range)}};
  auto response = co_await http_->Fetch(std::move(request), stop_token);
  if (response.status / 100 == 4) {
    stream_cache_.Invalidate(file.id.id);
    video_url = GetVideoUrl(co_await stream_cache_.Get(file.id.id, stop_token),
                            file.id.itag);
    Request retry_request{.url = std::move(video_url),
                          .headers = {http::ToRangeHeader(range)}};
    response = co_await http_->Fetch(std::move(retry_request), stop_token);
  }

  int max_redirect_count = kMaxRedirectCount;
  while (response.status == 302 && max_redirect_count-- > 0) {
    auto redirect_request = Request{
        .url = coro::http::GetHeader(response.headers, "Location").value(),
        .headers = {http::ToRangeHeader(range)}};
    response = co_await http_->Fetch(std::move(redirect_request), stop_token);
  }
  if (response.status / 100 != 2) {
    throw http::HttpException(response.status);
  }
  FOR_CO_AWAIT(std::string & chunk, response.body) {
    co_yield std::move(chunk);
  }
}

template <typename ItemT>
auto YouTube::GetItemThumbnailImpl(ItemT item, ThumbnailQuality quality,
                                   http::Range range,
                                   stdx::stop_token stop_token)
    -> Task<Thumbnail> {
  std::optional<std::string> url = [&]() -> std::optional<std::string> {
    switch (quality) {
      case ThumbnailQuality::kLow:
        return std::move(item.thumbnail.default_quality_url);
      case ThumbnailQuality::kHigh:
        return std::move(item.thumbnail.high_quality_url
                             ? item.thumbnail.high_quality_url
                             : item.thumbnail.default_quality_url);
      default:
        throw RuntimeError("Unexpected quality.");
    }
  }();
  if (!url) {
    throw CloudException(CloudException::Type::kNotFound);
  }
  Request request{.url = std::move(*url), .headers = {ToRangeHeader(range)}};
  auto response =
      co_await auth_manager_.Fetch(std::move(request), std::move(stop_token));
  Thumbnail result;
  result.mime_type = http::GetHeader(response.headers, "Content-Type").value();
  result.size =
      std::stoll(http::GetHeader(response.headers, "Content-Length").value());
  result.data = std::move(response.body);
  co_return result;
}

auto YouTube::GetStreamData::operator()(std::string video_id,
                                        stdx::stop_token stop_token) const
    -> Task<StreamData> {
  std::string page =
      co_await GetVideoPage(http, std::move(video_id), stop_token);
  json config = GetConfig(page);
  if (!config.contains("videoDetails")) {
    throw CloudException("GetStreamData error.");
  }
  StreamData result{
      .title = config["videoDetails"]["title"],
      .adaptive_formats = config["streamingData"]["adaptiveFormats"],
      .formats = config["streamingData"]["formats"]};
  auto response = co_await http.Fetch(GetPlayerUrl(page), stop_token);
  auto player_content = co_await http::GetBody(std::move(response.body));
  result.new_descrambler = GetNewDescrambler(player_content);
  for (const auto& formats : {result.adaptive_formats, result.formats}) {
    if (result.descrambler) {
      break;
    }
    for (const auto& d : formats) {
      if (!d.contains("url")) {
        result.descrambler = GetDescrambler(player_content);
        break;
      }
    }
  }
  std::vector<Task<>> tasks;
  for (auto& format : result.formats) {
    if (format.contains("contentLength")) {
      continue;
    }
    tasks.emplace_back([](const http::Http& http, const StreamData& stream_data,
                          nlohmann::json& format,
                          stdx::stop_token stop_token) -> Task<> {
      auto request =
          http::Request<>{.url = GetVideoUrl(stream_data, format["itag"]),
                          .method = http::Method::kHead};
      auto response = co_await http.Fetch(std::move(request), stop_token);
      int max_redirect_count = kMaxRedirectCount;
      while (response.status == 302 && max_redirect_count-- > 0) {
        auto redirect_request = Request{
            .url = coro::http::GetHeader(response.headers, "Location").value(),
            .method = http::Method::kHead};
        response = co_await http.Fetch(std::move(redirect_request), stop_token);
      }
      if (response.status / 100 != 2) {
        throw http::HttpException(response.status);
      }
      if (auto content_length =
              http::GetHeader(response.headers, "Content-Length")) {
        format["contentLength"] = *content_length;
      }
    }(http, result, format, stop_token));
  }

  co_await WhenAll(std::move(tasks));

  co_return result;
}

auto YouTube::ToItem(const nlohmann::json& json) -> Item {
  switch (static_cast<ItemId::Type>(int(json["type"]))) {
    case ItemId::Type::kDashManifest: {
      auto item = ::coro::cloudstorage::ToItem<DashManifest>(json);
      item.timestamp = json["timestamp"];
      item.thumbnail = ToThumbnailData(json["thumbnail"]);
      return item;
    }
    case ItemId::Type::kRootDirectory: {
      auto item = ::coro::cloudstorage::ToItem<RootDirectory>(json);
      item.id.presentation =
          static_cast<Presentation>(int(json["presentation"]));
      return item;
    }
    case ItemId::Type::kStream: {
      auto item = ::coro::cloudstorage::ToItem<Stream>(json);
      item.id.itag = json["itag"];
      item.mime_type = json["mime_type"];
      item.size = json["size"];
      return item;
    }
    case ItemId::Type::kMuxedStreamWebm: {
      auto item = ::coro::cloudstorage::ToItem<MuxedStreamWebm>(json);
      item.timestamp = json["timestamp"];
      item.thumbnail = ToThumbnailData(json["thumbnail"]);
      return item;
    }
    case ItemId::Type::kMuxedStreamMp4: {
      auto item = ::coro::cloudstorage::ToItem<MuxedStreamWebm>(json);
      item.timestamp = json["timestamp"];
      item.thumbnail = ToThumbnailData(json["thumbnail"]);
      return item;
    }
    case ItemId::Type::kStreamDirectory: {
      auto item = ::coro::cloudstorage::ToItem<StreamDirectory>(json);
      item.timestamp = json["timestamp"];
      return item;
    }
    case ItemId::Type::kPlaylist: {
      auto item = ::coro::cloudstorage::ToItem<Playlist>(json);
      item.id.presentation =
          static_cast<Presentation>(int(json["presentation"]));
      return item;
    }
    default:
      throw CloudException("Unrecognized YouTube::Item type.");
  }
}

nlohmann::json YouTube::ToJson(const Item& item) {
  return std::visit(
      []<typename T>(const T& d) {
        nlohmann::json json;
        json["type"] = d.id.type;
        json["id"] = d.id.id;
        json["name"] = d.name;
        if constexpr (std::is_same_v<T, DashManifest>) {
          json["timestamp"] = d.timestamp;
          json["thumbnail"] = ::coro::cloudstorage::ToJson(d.thumbnail);
        } else if constexpr (std::is_same_v<T, RootDirectory>) {
          json["presentation"] = static_cast<int>(d.id.presentation);
        } else if constexpr (std::is_same_v<T, Stream>) {
          json["itag"] = d.id.itag;
          json["mime_type"] = d.mime_type;
          json["size"] = d.size;
        } else if constexpr (std::is_same_v<T, MuxedStreamWebm>) {
          json["timestamp"] = d.timestamp;
          json["thumbnail"] = ::coro::cloudstorage::ToJson(d.thumbnail);
        } else if constexpr (std::is_same_v<T, MuxedStreamMp4>) {
          json["timestamp"] = d.timestamp;
          json["thumbnail"] = ::coro::cloudstorage::ToJson(d.thumbnail);
        } else if constexpr (std::is_same_v<T, StreamDirectory>) {
          json["timestamp"] = d.timestamp;
        } else if constexpr (std::is_same_v<T, Playlist>) {
          json["presentation"] = static_cast<int>(d.id.presentation);
        }
        return json;
      },
      item);
}

namespace util {

template <>
YouTube::Auth::AuthData GetAuthData<YouTube>(const nlohmann::json& json) {
  return GetAuthData<GoogleDrive>(json);
}

template <>
auto AbstractCloudProvider::Create<YouTube>(YouTube p)
    -> std::unique_ptr<AbstractCloudProvider> {
  return CreateAbstractCloudProvider(std::move(p));
}

YouTube::ItemId FromStringT<YouTube::ItemId>::operator()(
    std::string item_id) const {
  nlohmann::json json = nlohmann::json::from_cbor(http::FromBase64(item_id));
  YouTube::ItemId id;
  id.type = static_cast<YouTube::ItemId::Type>(int(json["type"]));
  id.id = json["id"];
  if (id.type == YouTube::ItemId::Type::kStream) {
    id.itag = json["itag"];
  }
  if (id.type == YouTube::ItemId::Type::kRootDirectory ||
      id.type == YouTube::ItemId::Type::kPlaylist) {
    id.presentation = json["presentation"];
  }
  return id;
}

std::string ToString(const YouTube::ItemId& id) {
  std::string output;
  nlohmann::json json;
  json["type"] = id.type;
  json["id"] = id.id;
  if (id.type == YouTube::ItemId::Type::kStream) {
    json["itag"] = id.itag;
  }
  if (id.type == YouTube::ItemId::Type::kRootDirectory ||
      id.type == YouTube::ItemId::Type::kPlaylist) {
    json["presentation"] = id.presentation;
  }
  nlohmann::json::to_cbor(json, output);
  return http::ToBase64(output);
}

}  // namespace util

}  // namespace coro::cloudstorage
