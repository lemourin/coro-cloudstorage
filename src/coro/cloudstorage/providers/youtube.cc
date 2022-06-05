#include "coro/cloudstorage/providers/youtube.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/util/regex.h"

namespace coro::cloudstorage {

namespace {

enum class TransformType { kReverse, kSplice, kSwap };

using ::coro::cloudstorage::util::CreateAbstractCloudProviderImpl;
using ::coro::cloudstorage::util::MediaContainer;
using ::coro::cloudstorage::util::StrCat;
using ::nlohmann::json;

using StreamDirectory = YouTube::StreamDirectory;

namespace re = ::coro::util::re;

constexpr std::string_view kEndpoint = "https://www.googleapis.com/youtube/v3";

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
      return match[match.size() - 1].str();
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

namespace js {

struct Function {
  std::string name;
  std::vector<std::string> args;
  std::string source;
};

Function GetFunction(std::string_view document,
                     std::string_view function_name) {
  re::match_results<std::string_view::iterator> match;
  if (re::regex_search(
          document.begin(), document.end(), match,
          re::regex{StrCat(
              R"((?:)", EscapeRegex(function_name),
              R"(\s*=\s*function\s*)\(([^\)]*)\)\s*(\{(?!\};)[\s\S]+?\};))")})) {
    auto args = util::SplitString(match[1].str(), ',');
    for (auto& arg : args) {
      arg = util::TrimWhitespace(arg);
    }
    return Function{std::string(function_name), std::move(args),
                    match[2].str()};
  } else {
    throw CloudException(StrCat("function ", function_name, " not found"));
  }
}

std::vector<std::string> Split(std::string_view text, char delimiter) {
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
        result.emplace_back(util::TrimWhitespace(current));
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

}  // namespace js

int ToInt(const std::string& str) { return std::lround(std::stod(str)); }

template <typename Container>
void CircularShift(Container& container, int shift) {
  int size = container.size();
  int m = size - (((shift % size) + size) % size);
  std::reverse(container.begin(), container.begin() + m);
  std::reverse(container.begin() + m, container.end());
  std::reverse(container.begin(), container.end());
}

template <typename Container>
void SwapElement(Container& container, int shift) {
  int size = container.size();
  int m = ((shift % size) + size) % size;
  std::swap(container[0], container[m]);
}

template <typename Container>
void RemoveElement(Container& container, int shift) {
  int size = container.size();
  int m = ((shift % size) + size) % size;
  container.erase(container.begin() + m);
}

std::string Decrypt(std::string input, std::string key,
                    std::string_view cipher_chars) {
  int h = cipher_chars.length();
  for (int i = 0; i < input.size(); i++) {
    int i1 = cipher_chars.find(input[i]);
    int i2 = cipher_chars.find(key[i]);
    input[i] = cipher_chars[(i1 - i2 + i + h--) % cipher_chars.length()];
    key.push_back(input[i]);
  }
  return input;
}

std::string GetNewCipher(const js::Function& function, std::string nsig) {
  auto input = js::Split(
      Find(function.source, {re::regex(R"(\w\s*=\s*\[([\s\S]*)\];)")}).value(),
      ',');
  for (std::string_view command : js::Split(
           Find(function.source, {re::regex(R"(try\s*\{([\s\S]*)\}\s*catch)")})
               .value(),
           ',')) {
    re::match_results<std::string_view::iterator> match;
    if (re::regex_search(
            command.begin(), command.end(), match,
            re::regex(R"(\w+\[(\d+)\]\(\w+\[(\d+)\],\s*\w+\[(\d+)\]\))"))) {
      int a = ToInt(match[1].str());
      int b = ToInt(match[2].str());
      int c = ToInt(match[3].str());
      auto do_operation = [&]<typename T>(T& i) {
        std::string_view source = input.at(a);
        if (source.find("for") != std::string::npos) {
          CircularShift(i, ToInt(input.at(c)));
        } else if (source.find("d.splice(e,1)") != std::string::npos) {
          RemoveElement(i, ToInt(input.at(c)));
        } else if (source.find("push") != std::string::npos) {
          if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            i.push_back(input.at(c));
          } else {
            throw CloudException("unexpected push");
          }
        } else {
          SwapElement(i, ToInt(input.at(c)));
        }
      };
      std::string_view nd_argument = input.at(b);
      if (nd_argument == "null") {
        do_operation(input);
      } else if (nd_argument == "b") {
        do_operation(nsig);
      } else {
        throw CloudException(StrCat("unexpected ", nd_argument));
      }
    } else if (re::regex_search(command.begin(), command.end(), match,
                                re::regex(R"(\w+\[(\d+)\]\(\w+\[(\d+)\]\))"))) {
      int a = ToInt(match[1].str());
      int b = ToInt(match[2].str());
      std::string_view nd_argument = input.at(b);
      if (nd_argument == "null") {
        std::reverse(input.begin(), input.end());
      } else if (nd_argument == "b") {
        std::reverse(nsig.begin(), nsig.end());
      } else {
        throw CloudException(StrCat("unexpected ", nd_argument));
      }
    } else if (
        re::regex_search(
            command.begin(), command.end(), match,
            re::regex(
                R"(\w+\[(\d+)\]\(\w+\[(\d+)\],\s*\w+\[(\d+)\],\s*\w+\[(\d+)\]\(\)\))"))) {
      int a = ToInt(match[1].str());
      int b = ToInt(match[2].str());
      int c = ToInt(match[3].str());
      int d = ToInt(match[4].str());
      const std::string& key = input.at(c);
      nsig = Decrypt(std::move(nsig), key.substr(1, key.size() - 2), [&] {
        const std::string& cipher_source = input.at(d);
        if (cipher_source.find("-=58") != std::string::npos ||
            cipher_source.find("-=18") != std::string::npos) {
          return R"(0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_)";
        } else {
          return R"(ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_)";
        }
      }());
    } else {
      throw CloudException(StrCat("unexpected command ", command));
    }
  }
  return nsig;
}

template <typename MuxedStreamT>
MuxedStreamT ToMuxedStream(std::string_view directory_id, json item) {
  MuxedStreamT stream;
  stream.video_id = item["snippet"]["resourceId"]["videoId"];
  stream.timestamp =
      http::ParseTime(std::string(item["snippet"]["publishedAt"]));
  stream.name = StrCat(std::string(item["snippet"]["title"]), [] {
    if constexpr (std::is_same_v<MuxedStreamT, YouTube::MuxedStreamMp4>) {
      return ".mp4";
    } else {
      return ".webm";
    }
  }());
  stream.id = StrCat(directory_id, http::EncodeUri(stream.name));
  stream.thumbnail = GetThumbnailData(item["snippet"]["thumbnails"]);
  return stream;
}

Task<std::string> GetVideoPage(const http::Http& http, std::string video_id,
                               stdx::stop_token stop_token) {
  auto response = co_await http.Fetch(
      "https://www.youtube.com/watch?v=" + video_id, stop_token);
  co_return co_await http::GetBody(std::move(response.body));
}

YouTube::Stream ToStream(const StreamDirectory& directory, json d) {
  std::string mime_type = d["mimeType"];
  std::string extension(
      mime_type.begin() +
          static_cast<std::string::difference_type>(mime_type.find('/') + 1),
      mime_type.begin() +
          static_cast<std::string::difference_type>(mime_type.find(';')));
  YouTube::Stream stream;
  stream.video_id = directory.video_id;
  if (d.contains("qualityLabel")) {
    stream.name += StrCat("[", std::string(d["qualityLabel"]), "]");
  }
  if (d.contains("audioQuality")) {
    stream.name += StrCat("[", std::string(d["audioQuality"]), "]");
  }
  stream.name += StrCat("[", static_cast<int>(d["itag"]), "] ", directory.name,
                        ".", extension);
  stream.mime_type = std::move(mime_type);
  stream.size = std::stoll(std::string(d["contentLength"]));
  stream.id = directory.id + stream.name;
  stream.itag = d["itag"];
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

std::string GenerateDashManifest(std::string_view path, std::string_view name,
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
            {"mediaPresentationDuration", StrCat("PT", duration / 1000, "S")},
            {"minBufferTime", "PT2S"},
            {"profiles", "urn:mpeg:dash:profile:isoff-main:2011"}})
    << ">";
  r << "<Period>";
  std::unordered_map<std::string, std::vector<json>> grouped;
  for (const auto& d : stream_data) {
    std::string mimetype = d["mimeType"];
    grouped[mimetype.substr(0, mimetype.find(';'))].emplace_back(d);
  }
  for (const auto& [mimetype, streams] : grouped) {
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
      << XmlAttributes({{"mimeType", mimetype},
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
      std::string quality_label =
          (stream.contains("qualityLabel") ? stream["qualityLabel"]
                                           : stream["audioQuality"]);
      std::string extension(
          full_mimetype.begin() + static_cast<std::string::difference_type>(
                                      full_mimetype.find('/') + 1),
          full_mimetype.begin() + static_cast<std::string::difference_type>(
                                      full_mimetype.find(';')));
      r << "<Representation "
        << XmlAttributes(
               {{"id", quality_label},
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
        << util::StrCat(
               path, http::EncodeUri(
                         ToStream(StreamDirectory{{.name = std::string(name)}},
                                  stream)
                             .name))
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
    return "https://www.youtube.com/" + match[1].str();
  } else {
    throw CloudException("jsUrl not found");
  }
  return "";
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
  auto nsig_function_name = Find(
      page_data,
      {re::regex(R"(\.get\("n"\)\)&&\(b=([a-zA-Z0-9$]{3})\([a-zA-Z0-9]\))")});
  if (!nsig_function_name) {
    return std::nullopt;
  }
  return [nsig_function = js::GetFunction(page_data, *nsig_function_name)](
             std::string_view nsig) {
    return GetNewCipher(nsig_function, std::string(nsig));
  };
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

auto YouTube::CloudProvider::GetRoot(stdx::stop_token) -> Task<RootDirectory> {
  RootDirectory d = {};
  d.id = "/";
  d.presentation = Presentation::kDash;
  co_return d;
}

auto YouTube::CloudProvider::GetGeneralData(stdx::stop_token stop_token)
    -> Task<GeneralData> {
  json json = co_await auth_manager_.FetchJson(
      Request{.url = "https://openidconnect.googleapis.com/v1/userinfo"},
      std::move(stop_token));
  GeneralData result{.username = json["email"]};
  co_return result;
}

auto YouTube::CloudProvider::ListDirectoryPage(
    StreamDirectory directory, std::optional<std::string> page_token,
    stdx::stop_token stop_token) -> Task<PageData> {
  PageData result;
  StreamData data = co_await stream_cache_.Get(directory.video_id, stop_token);
  for (const auto& formats : {data.adaptive_formats, data.formats}) {
    for (const auto& d : formats) {
      if (!d.contains("contentLength")) {
        continue;
      }
      result.items.emplace_back(ToStream(directory, d));
    }
  }
  co_return result;
}

auto YouTube::CloudProvider::ListDirectoryPage(
    Playlist directory, std::optional<std::string> page_token,
    stdx::stop_token stop_token) -> Task<PageData> {
  PageData result;
  std::vector<std::pair<std::string, std::string>> headers{
      {"part", "snippet"},
      {"playlistId", directory.playlist_id},
      {"maxResults", "50"}};
  if (page_token) {
    headers.emplace_back("pageToken", *page_token);
  }
  Request request = {.url = GetEndpoint("/playlistItems") + "?" +
                            http::FormDataToString(headers)};
  auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                   std::move(stop_token));
  for (const auto& item : response["items"]) {
    switch (directory.presentation) {
      case Presentation::kMuxedStreamMp4: {
        result.items.emplace_back(
            ToMuxedStream<MuxedStreamMp4>(directory.id, item));
        break;
      }
      case Presentation::kMuxedStreamWebm: {
        result.items.emplace_back(
            ToMuxedStream<MuxedStreamWebm>(directory.id, item));
        break;
      }
      case Presentation::kStream: {
        StreamDirectory streams;
        streams.video_id = item["snippet"]["resourceId"]["videoId"];
        streams.timestamp =
            http::ParseTime(std::string(item["snippet"]["publishedAt"]));
        streams.name = std::string(item["snippet"]["title"]);
        streams.id = directory.id + http::EncodeUri(streams.name) + "/";
        result.items.emplace_back(std::move(streams));
        break;
      }
      case Presentation::kDash: {
        DashManifest file;
        file.video_id = item["snippet"]["resourceId"]["videoId"];
        file.timestamp =
            http::ParseTime(std::string(item["snippet"]["publishedAt"]));
        file.name = std::string(item["snippet"]["title"]) + ".mpd";
        file.id = directory.id + http::EncodeUri(file.name);
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

auto YouTube::CloudProvider::ListDirectoryPage(
    RootDirectory directory, std::optional<std::string> page_token,
    stdx::stop_token stop_token) -> Task<PageData> {
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
    result.items.emplace_back(
        Playlist{{.id = directory.id + key + "/", .name = key},
                 value,
                 directory.presentation});
  }
  if (directory.presentation == Presentation::kDash) {
    result.items.emplace_back(RootDirectory{
        {.id = "/streams/", .name = "streams"}, Presentation::kStream});
    result.items.emplace_back(
        RootDirectory{{.id = "/muxed-webm/", .name = "muxed-webm"},
                      Presentation::kMuxedStreamWebm});
    result.items.emplace_back(
        RootDirectory{{.id = "/muxed-mp4/", .name = "muxed-mp4"},
                      Presentation::kMuxedStreamMp4});
  }
  co_return result;
}

Generator<std::string> YouTube::CloudProvider::GetFileContent(
    Stream file, http::Range range, stdx::stop_token stop_token) {
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

Generator<std::string> YouTube::CloudProvider::GetFileContent(
    MuxedStreamWebm file, http::Range range, stdx::stop_token stop_token) {
  return GetMuxedFileContent(std::move(file), range, "webm",
                             std::move(stop_token));
}

Generator<std::string> YouTube::CloudProvider::GetFileContent(
    MuxedStreamMp4 file, http::Range range, stdx::stop_token stop_token) {
  return GetMuxedFileContent(std::move(file), range, "mp4",
                             std::move(stop_token));
}

Generator<std::string> YouTube::CloudProvider::GetFileContent(
    DashManifest file, http::Range range, stdx::stop_token stop_token) {
  StreamData data =
      co_await stream_cache_.Get(file.video_id, std::move(stop_token));
  auto strip_extension = [](std::string_view str) {
    return std::string(str.substr(0, str.size() - 4));
  };
  std::string dash_manifest = GenerateDashManifest(
      util::StrCat("../streams", strip_extension(file.id), "/"),
      strip_extension(file.name), data.adaptive_formats);
  if ((range.end && range.end >= kDashManifestSize) ||
      range.start >= kDashManifestSize) {
    throw http::HttpException(http::HttpException::kRangeNotSatisfiable);
  }
  dash_manifest.resize(kDashManifestSize, ' ');
  co_yield std::move(dash_manifest)
      .substr(static_cast<size_t>(range.start),
              static_cast<size_t>(range.end.value_or(kDashManifestSize - 1) -
                                  range.start + 1));
}

auto YouTube::CloudProvider::GetItemThumbnail(DashManifest item,
                                              ThumbnailQuality quality,
                                              http::Range range,
                                              stdx::stop_token stop_token)
    -> Task<Thumbnail> {
  return GetItemThumbnailImpl(std::move(item), quality, range,
                              std::move(stop_token));
}

auto YouTube::CloudProvider::GetItemThumbnail(MuxedStreamMp4 item,
                                              ThumbnailQuality quality,
                                              http::Range range,
                                              stdx::stop_token stop_token)
    -> Task<Thumbnail> {
  return GetItemThumbnailImpl(std::move(item), quality, range,
                              std::move(stop_token));
}

auto YouTube::CloudProvider::GetItemThumbnail(MuxedStreamWebm item,
                                              ThumbnailQuality quality,
                                              http::Range range,
                                              stdx::stop_token stop_token)
    -> Task<Thumbnail> {
  return GetItemThumbnailImpl(std::move(item), quality, range,
                              std::move(stop_token));
}

template <typename MuxedStream>
Generator<std::string> YouTube::CloudProvider::GetMuxedFileContent(
    MuxedStream file, http::Range range, std::string_view type,
    stdx::stop_token stop_token) {
  if (range.start != 0 || range.end) {
    throw CloudException("partial read unsupported");
  }
  StreamData data = co_await stream_cache_.Get(file.video_id, stop_token);
  Stream video_stream{};
  video_stream.video_id = file.video_id;
  auto best_video = data.GetBestVideo(StrCat("video/", type));
  video_stream.itag = best_video["itag"];
  video_stream.size = std::stoll(std::string(best_video["contentLength"]));
  Stream audio_stream{};
  audio_stream.video_id = std::move(file.video_id);
  auto best_audio = data.GetBestAudio(StrCat("audio/", type));
  audio_stream.itag = best_audio["itag"];
  audio_stream.size = std::stoll(std::string(best_audio["contentLength"]));
  auto impl = CreateAbstractCloudProviderImpl(this);
  FOR_CO_AWAIT(
      std::string & chunk,
      (*muxer_)(&impl, impl.Convert(std::move(video_stream)), &impl,
                impl.Convert(std::move(audio_stream)),
                type == "webm" ? MediaContainer::kWebm : MediaContainer::kMp4,
                std::move(stop_token))) {
    co_yield std::move(chunk);
  }
}

Generator<std::string> YouTube::CloudProvider::GetFileContentImpl(
    Stream file, http::Range range, stdx::stop_token stop_token) {
  std::string video_url =
      co_await GetVideoUrl(file.video_id, file.itag, stop_token);
  Request request{.url = std::move(video_url),
                  .headers = {http::ToRangeHeader(range)}};
  auto response = co_await http_->Fetch(std::move(request), stop_token);
  if (response.status / 100 == 4) {
    stream_cache_.Invalidate(file.video_id);
    video_url = co_await GetVideoUrl(file.video_id, file.itag, stop_token);
    Request retry_request{.url = std::move(video_url),
                          .headers = {http::ToRangeHeader(range)}};
    response = co_await http_->Fetch(std::move(retry_request), stop_token);
  }

  int max_redirect_count = 8;
  while (response.status == 302 && max_redirect_count-- > 0) {
    auto redirect_request = Request{
        .url = coro::http::GetHeader(response.headers, "Location").value(),
        .headers = {http::ToRangeHeader(range)}};
    response = co_await http_->Fetch(std::move(redirect_request), stop_token);
  }
  if (response.status / 100 != 2) {
    throw http::HttpException(response.status);
  }

  FOR_CO_AWAIT(std::string & body, response.body) { co_yield std::move(body); }
}

template <typename ItemT>
auto YouTube::CloudProvider::GetItemThumbnailImpl(ItemT item,
                                                  ThumbnailQuality quality,
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

Task<std::string> YouTube::CloudProvider::GetVideoUrl(
    std::string video_id, int64_t itag, stdx::stop_token stop_token) const {
  StreamData data = co_await stream_cache_.Get(video_id, stop_token);
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
          url = util::StrCat(uri.scheme.value_or("https"), "://",
                             uri.host.value_or(""), uri.path.value_or(""), "?",
                             http::FormDataToString(params));
        }
      }
    }
  }
  if (!url) {
    throw CloudException(CloudException::Type::kNotFound);
  }
  co_return *url;
}

auto YouTube::CloudProvider::GetStreamData::operator()(
    std::string video_id, stdx::stop_token stop_token) const
    -> Task<StreamData> {
  std::string page =
      co_await GetVideoPage(http, std::move(video_id), stop_token);
  json config = GetConfig(page);
  StreamData result{
      .adaptive_formats = config["streamingData"]["adaptiveFormats"],
      .formats = config["streamingData"]["formats"]};
  auto response = co_await http.Fetch(GetPlayerUrl(page), stop_token);
  auto player_content = co_await http::GetBody(std::move(response.body));
  result.new_descrambler = GetNewDescrambler(player_content);
  for (const auto& formats : {result.adaptive_formats, result.formats}) {
    for (const auto& d : formats) {
      if (!d.contains("url")) {
        result.descrambler = GetDescrambler(player_content);
        co_return result;
      }
    }
  }
  co_return result;
}

namespace util {

template <>
YouTube::Auth::AuthData GetAuthData<YouTube>() {
  return {.client_id = YOUTUBE_CLIENT_ID,
          .client_secret = YOUTUBE_CLIENT_SECRET};
}

template <>
auto AbstractCloudProvider::Create<YouTube::CloudProvider>(
    YouTube::CloudProvider p) -> std::unique_ptr<CloudProvider> {
  return CreateAbstractCloudProvider(std::move(p));
}

}  // namespace util

}  // namespace coro::cloudstorage
