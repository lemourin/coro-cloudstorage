#include "webdav.h"

#include <iomanip>
#include <regex>

namespace coro::cloudstorage {

namespace {

int64_t ParseTime(std::string str) {
  std::stringstream stream(std::move(str));
  std::tm time;
  stream >> std::get_time(&time, "%a, %d %b %Y %T GMT");
  if (!stream.fail()) {
    return http::timegm(time);
  } else {
    throw CloudException("invalid timestamp");
  }
}

Generator<std::string> GenerateLoginPage() { co_yield std::string(util::kAssetsHtmlWebdavLoginHtml); }

}  // namespace

namespace util {

template <>
nlohmann::json ToJson<WebDAV::Auth::AuthToken>(WebDAV::Auth::AuthToken token) {
  nlohmann::json json;
  json["endpoint"] = std::move(token.endpoint);
  if (token.credential) {
    json["access_token"] = WebDAV::Auth::ToAccessToken(*token.credential);
  }
  return json;
}

template <>
WebDAV::Auth::AuthToken ToAuthToken<WebDAV::Auth::AuthToken>(
    const nlohmann::json& json) {
  WebDAV::Auth::AuthToken auth_token;
  auth_token.endpoint = json.at("endpoint");
  if (json.contains("access_token")) {
    std::string access_token =
        http::FromBase64(std::string(json["access_token"]));
    std::regex regex(R"(([^\:]+):(.*))");
    std::smatch match;
    if (std::regex_match(access_token, match, regex)) {
      auth_token.credential = WebDAV::Auth::Credential{
          .username = match[1].str(), .password = match[2].str()};
    } else {
      throw std::invalid_argument("invalid access_token");
    }
  }
  return auth_token;
}

template <>
WebDAV::Auth::AuthData GetAuthData<WebDAV>() {
  return {};
}

Task<std::variant<http::Response<>, WebDAV::Auth::AuthToken>>
WebDAVAuthHandler::operator()(http::Request<> request, stdx::stop_token) const {
  if (request.method == http::Method::kPost) {
    auto query =
        http::ParseQuery(co_await http::GetBody(std::move(*request.body)));
    WebDAV::Auth::AuthToken auth_token{};
    if (auto it = query.find("endpoint");
        it != query.end() && !it->second.empty()) {
      auth_token.endpoint = std::move(it->second);
    } else {
      throw http::HttpException(http::HttpException::kBadRequest,
                                "endpoint not set");
    }
    auto it1 = query.find("username");
    auto it2 = query.find("password");
    if (it1 != query.end() && it2 != query.end() && !it1->second.empty() &&
        !it2->second.empty()) {
      auth_token.credential =
          WebDAV::Auth::Credential{.username = std::move(it1->second),
                                   .password = std::move(it2->second)};
    }
    co_return auth_token;
  } else {
    co_return http::Response<>{.status = 501, .body = GenerateLoginPage()};
  }
}

}  // namespace util

std::optional<std::string> WebDAV::GetNamespace(const pugi::xml_node& node) {
  pugi::xml_attribute attr =
      node.find_attribute([](const pugi::xml_attribute& attr) {
        return std::string_view(attr.as_string()) == "DAV:";
      });
  if (std::string_view(attr.name()) == "xmlns") {
    return std::nullopt;
  } else if (std::cmatch match; std::regex_match(
                 attr.name(), match, std::regex(R"(xmlns\:(\S+))"))) {
    return match[1];
  } else {
    throw CloudException("invalid xml");
  }
}

template <typename T>
T WebDAV::ToItemImpl(const WebDAV::XmlNode<pugi::xml_node>& node) {
  T item{};
  auto props = node.child("propstat").child("prop");
  item.id = node.child("href").text().as_string();
  item.name = http::DecodeUri(props.child("displayname").text().as_string());
  if (auto timestamp = props.child("getlastmodified").text()) {
    item.timestamp = ParseTime(timestamp.as_string());
  }
  if constexpr (std::is_same_v<T, WebDAV::File>) {
    if (auto size = props.child("getcontentlength").text()) {
      item.size = std::stoll(size.as_string());
    }
    if (auto mime_type = props.child("getcontenttype").text()) {
      item.mime_type = mime_type.as_string();
    }
  }
  return item;
}

WebDAV::Item WebDAV::ToItem(const XmlNode<pugi::xml_node>& node) {
  if (node.child("propstat")
          .child("prop")
          .child("resourcetype")
          .child("collection")) {
    return ToItemImpl<Directory>(node);
  } else {
    return ToItemImpl<File>(node);
  }
}

}  // namespace coro::cloudstorage