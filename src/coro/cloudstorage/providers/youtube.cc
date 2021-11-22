#include "coro/cloudstorage/providers/youtube.h"

#include <algorithm>
#include <regex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include "coro/cloudstorage/util/string_utils.h"

namespace coro::cloudstorage {

namespace {

enum class TransformType { kReverse, kSplice, kSwap };

using ::coro::cloudstorage::util::StrCat;

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
                                const std::initializer_list<std::regex>& re) {
  std::match_results<std::string_view::iterator> match;
  for (const auto& regex : re) {
    if (std::regex_search(text.begin(), text.end(), match, regex)) {
      return match[match.size() - 1].str();
    }
  }
  return std::nullopt;
}

namespace js {

struct Function {
  std::string name;
  std::vector<std::string> args;
  std::string source;
};

Function GetFunction(std::string_view document,
                     std::string_view function_name) {
  std::match_results<std::string_view::iterator> match;
  if (std::regex_search(
          document.begin(), document.end(), match,
          std::regex{StrCat(
              R"((?:)", function_name,
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
      Find(function.source, {std::regex(R"(\w\s*=\s*\[([\s\S]*)\];)")}).value(),
      ',');
  for (std::string_view command : js::Split(
           Find(function.source, {std::regex(R"(try\s*\{([\s\S]*)\}\s*catch)")})
               .value(),
           ',')) {
    std::match_results<std::string_view::iterator> match;
    if (std::regex_search(
            command.begin(), command.end(), match,
            std::regex(R"(\w+\[(\d+)\]\(\w+\[(\d+)\],\s*\w+\[(\d+)\]\))"))) {
      int a = std::stoi(match[1].str());
      int b = std::stoi(match[2].str());
      int c = std::stoi(match[3].str());
      auto do_operation = [&]<typename T>(T& i) {
        std::string_view source = input.at(a);
        if (source.find("for") != std::string::npos) {
          CircularShift(i, std::stoi(input.at(c)));
        } else if (source.find("d.splice(e,1)") != std::string::npos) {
          RemoveElement(i, std::stoi(input.at(c)));
        } else if (source.find("push") != std::string::npos) {
          if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            i.push_back(input.at(c));
          } else {
            throw CloudException("unexpected push");
          }
        } else {
          SwapElement(i, std::stoi(input.at(c)));
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
    } else if (std::regex_search(
                   command.begin(), command.end(), match,
                   std::regex(R"(\w+\[(\d+)\]\(\w+\[(\d+)\]\))"))) {
      int a = std::stoi(match[1].str());
      int b = std::stoi(match[2].str());
      std::string_view nd_argument = input.at(b);
      if (nd_argument == "null") {
        std::reverse(input.begin(), input.end());
      } else if (nd_argument == "b") {
        std::reverse(nsig.begin(), nsig.end());
      } else {
        throw CloudException(StrCat("unexpected ", nd_argument));
      }
    } else if (
        std::regex_search(
            command.begin(), command.end(), match,
            std::regex(
                R"(\w+\[(\d+)\]\(\w+\[(\d+)\],\s*\w+\[(\d+)\],\s*\w+\[(\d+)\]\(\)\))"))) {
      int a = std::stoi(match[1].str());
      int b = std::stoi(match[2].str());
      int c = std::stoi(match[3].str());
      int d = std::stoi(match[4].str());
      const std::string& key = input.at(c);
      nsig = Decrypt(std::move(nsig), key.substr(1, key.size() - 2), [&] {
        const std::string& cipher_source = input.at(d);
        if (cipher_source.find("-=58") != std::string::npos) {
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

}  // namespace

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

YouTube::Stream YouTube::ToStream(const StreamDirectory& directory, json d) {
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
  auto descrambler =
      Find(
          page_data,
          {std::regex(
               R"re(([a-zA-Z0-9$]+)\s*=\s*function\(\s*a\s*\)\s*\{\s*a\s*=\s*a\.split\(\s*""\s*\))re"),
           std::regex(
               R"re((?:\b|[^a-zA-Z0-9$])([a-zA-Z0-9$]{2})\s*=\s*function\(\s*a\s*\)\s*\{\s*a\s*=\s*a\.split\(\s*""\s*\))re")})
          .value();
  auto rules = Find(page_data,
                    {std::regex(descrambler + R"(=function[^{]*\{([^}]*)\};)")})
                   .value();
  auto helper = Find(rules, {std::regex(R"(;([a-zA-Z0-9]*)\.)")}).value();
  auto transforms =
      Find(page_data, {std::regex(helper + R"(=\{([\s\S]*?)\};)")}).value();
  std::unordered_map<std::string, TransformType> transform_type;
  transform_type[Find(transforms, {std::regex(R"(([^:]{2}):.*reverse)")})
                     .value()] = TransformType::kReverse;
  transform_type[Find(transforms, {std::regex(R"(([^:]{2}):.*splice)")})
                     .value()] = TransformType::kSplice;
  transform_type[Find(transforms, {std::regex(R"(([^:]{2}):.*\[0\])")})
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
    return StrCat(data["url"], "&", data["sp"], "=", signature);
  };
}

std::optional<std::function<std::string(std::string_view cipher)>>
YouTube::GetNewDescrambler(std::string_view page_data) {
  auto nsig_function_name = Find(
      page_data,
      {std::regex(R"(\.get\("n"\)\)&&\(b=([a-zA-Z0-9$]{3})\([a-zA-Z0-9]\))")});
  if (!nsig_function_name) {
    return std::nullopt;
  }
  return [nsig_function = js::GetFunction(page_data, *nsig_function_name)](
             std::string_view nsig) {
    return GetNewCipher(nsig_function, std::string(nsig));
  };
}

}  // namespace coro::cloudstorage
