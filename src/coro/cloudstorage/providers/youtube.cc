#include "youtube.h"

#include <coro/cloudstorage/util/string_utils.h>

#include <regex>
#include <sstream>

namespace coro::cloudstorage {

namespace {

enum class TransformType { kReverse, kSplice, kSwap };

using ::coro::cloudstorage::util::StrCat;
using ::coro::cloudstorage::util::ToString;

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

std::string Find(std::string_view text,
                 const std::initializer_list<std::regex>& re) {
  std::match_results<std::string_view::iterator> match;
  for (const auto& regex : re) {
    if (std::regex_search(text.begin(), text.end(), match, regex)) {
      return match[match.size() - 1].str();
    }
  }
  throw CloudException("pattern not found");
}

}  // namespace

nlohmann::json YouTube::StreamData::GetBestVideo() const {
  std::optional<json> json;
  for (const auto& d : adaptive_formats) {
    std::string mime_type = d["mimeType"];
    if (mime_type.find("video/webm") != std::string::npos &&
        (!json || d["bitrate"] > (*json)["bitrate"])) {
      json = d;
    }
  }
  if (!json) {
    throw CloudException("video not found");
  }
  return *json;
}

nlohmann::json YouTube::StreamData::GetBestAudio() const {
  std::optional<json> json;
  for (const auto& d : adaptive_formats) {
    std::string mime_type = d["mimeType"];
    if (mime_type.find("audio/webm") != std::string::npos &&
        (!json || d["bitrate"] > (*json)["bitrate"])) {
      json = d;
    }
  }
  if (!json) {
    throw CloudException("audio not found");
  }
  return *json;
}

YouTube::Stream YouTube::ToStream(const StreamDirectory& directory, json d) {
  std::string mime_type = d["mimeType"];
  std::string extension(mime_type.begin() + mime_type.find('/') + 1,
                        mime_type.begin() + mime_type.find(';'));
  Stream stream;
  stream.video_id = directory.video_id;
  if (d.contains("qualityLabel")) {
    stream.name += "[" + std::string(d["qualityLabel"]) + "]";
  }
  if (d.contains("audioQuality")) {
    stream.name += "[" + std::string(d["audioQuality"]) + "]";
  }
  stream.name += "[" + std::to_string(int(d["itag"])) + "] " + directory.name +
                 "." + extension;
  stream.mime_type = std::move(mime_type);
  stream.size = std::stoll(std::string(d["contentLength"]));
  stream.id = directory.id + stream.name;
  stream.itag = d["itag"];
  return stream;
}

nlohmann::json YouTube::GetConfig(std::string_view page_data) {
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

std::string YouTube::GenerateDashManifest(std::string_view path,
                                          std::string_view name,
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
    if (mimetype == "video/mp4" || mimetype == "audio/mp4") {
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
      std::string full_mimetype = stream["mimeType"];
      std::string codecs = full_mimetype.substr(full_mimetype.find(';') + 2);
      std::string quality_label =
          (stream.contains("qualityLabel") ? stream["qualityLabel"]
                                           : stream["audioQuality"]);
      std::string extension(full_mimetype.begin() + full_mimetype.find('/') + 1,
                            full_mimetype.begin() + full_mimetype.find(';'));
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

std::string YouTube::GetPlayerUrl(std::string_view page_data) {
  std::match_results<std::string_view::iterator> match;
  if (std::regex_search(page_data.begin(), page_data.end(), match,
                        std::regex(R"re("jsUrl":"([^"]*)")re"))) {
    return "https://www.youtube.com/" + match[1].str();
  } else {
    throw CloudException("jsUrl not found");
  }
  return "";
}

std::function<std::string(std::string_view)> YouTube::GetDescrambler(
    std::string_view page_data) {
  auto descrambler = Find(
      page_data,
      {std::regex(
          R"re((?:\b|[^a-zA-Z0-9$])([a-zA-Z0-9$]{2})\s*=\s*function\(\s*a\s*\)\s*\{\s*a\s*=\s*a\.split\(\s*""\s*\))re")});
  auto rules = Find(
      page_data, {std::regex(descrambler + R"(=function[^{]*\{([^}]*)\};)")});
  auto helper = Find(rules, {std::regex(R"(;([^\.]*)\.)")});
  auto transforms =
      Find(page_data, {std::regex(helper + R"(=\{([\s\S]*?)\};)")});
  std::unordered_map<std::string, TransformType> transform_type;
  transform_type[Find(transforms, {std::regex(R"(([^:]{2}):.*reverse)")})] =
      TransformType::kReverse;
  transform_type[Find(transforms, {std::regex(R"(([^:]{2}):.*splice)")})] =
      TransformType::kSplice;
  transform_type[Find(transforms, {std::regex(R"(([^:]{2}):.*\[0\])")})] =
      TransformType::kSwap;

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
      std::match_results<std::string_view::iterator> match;
      if (std::regex_match(
              transform.begin(), transform.end(), match,
              std::regex(helper + R"re(\.([^\(]*)\([^,]*,([^\)]*)\))re"))) {
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
    return data["url"] + "&" + data["sp"] + "=" + signature;
  };
}

}  // namespace coro::cloudstorage