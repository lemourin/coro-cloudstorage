#include "coro/cloudstorage/providers/webdav.h"

#include <iomanip>
#include <pugixml.hpp>
#include <string>
#include <utility>

#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/util/regex.h"

#ifdef WIN32
#undef CreateDirectory
#undef CreateFile
#endif

namespace coro::cloudstorage {

namespace {

namespace re = ::coro::util::re;

using ::coro::cloudstorage::util::SplitString;

std::string ToAccessToken(const WebDAV::Auth::Credential& credential) {
  return http::ToBase64(credential.username + ":" + credential.password);
}

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

Generator<std::string> GenerateLoginPage() {
  co_yield std::string(util::kAssetsHtmlWebdavLoginHtml);
}

std::string Concat(std::string parent, std::string_view name) {
  if (parent.empty()) {
    throw CloudException("invalid path");
  }
  if (parent.back() != '/') {
    parent += '/';
  }
  parent += http::EncodeUri(name);
  return parent;
}

std::optional<std::string> GetNamespace(const pugi::xml_node& node) {
  pugi::xml_attribute attr =
      node.find_attribute([](const pugi::xml_attribute& attr) {
        return std::string_view(attr.as_string()) == "DAV:";
      });
  if (std::string_view(attr.name()) == "xmlns") {
    return std::nullopt;
  } else if (re::cmatch match; re::regex_match(attr.name(), match,
                                               re::regex(R"(xmlns\:(\S+))"))) {
    return match[1];
  } else {
    throw CloudException("invalid xml");
  }
}

template <typename T>
class XmlNode : public T {
 public:
  using T::T;

  explicit XmlNode(T node) : T(std::move(node)) {}
  XmlNode(T node, std::optional<std::string> ns)
      : T(std::move(node)), namespace_(std::move(ns)) {}

  auto child(std::string name) const {
    if (namespace_) {
      name = *namespace_ + ":" + std::move(name);
    }
    return XmlNode<pugi::xml_node>(T::child(name.c_str()), namespace_);
  }

  auto document_element() const {
    return XmlNode<pugi::xml_node>(T::document_element(), namespace_);
  }

  std::optional<std::string> ns() const { return namespace_; }

 protected:
  std::optional<std::string> namespace_;
};

class XmlDocument : public XmlNode<pugi::xml_document> {
 public:
  explicit XmlDocument(std::string data) {
    std::stringstream stream(std::move(data));
    auto result = load(stream);
    if (!result) {
      throw CloudException(result.description());
    }
    namespace_ = GetNamespace(document_element());
  }
};

template <typename T>
T ToItemImpl(const XmlNode<pugi::xml_node>& node) {
  T item{};
  auto props = node.child("propstat").child("prop");
  item.id = node.child("href").text().as_string();
  if (auto name = props.child("displayname").text()) {
    item.name = http::DecodeUri(name.as_string());
  } else if (auto components = SplitString(item.id, '/'); !components.empty()) {
    item.name = http::DecodeUri(components.back());
  }
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

WebDAV::Item ToItem(const XmlNode<pugi::xml_node>& node) {
  if (node.child("propstat")
          .child("prop")
          .child("resourcetype")
          .child("collection")) {
    return ToItemImpl<WebDAV::Directory>(node);
  } else {
    return ToItemImpl<WebDAV::File>(node);
  }
}

template <typename RequestT>
Task<http::Response<>> Fetch(
    const coro::http::Http& http,
    const std::optional<WebDAV::Auth::Credential>& credential, RequestT request,
    stdx::stop_token stop_token) {
  if (credential) {
    request.headers.emplace_back("Authorization",
                                 "Basic " + ToAccessToken(*credential));
  }
  co_return co_await http.FetchOk(std::move(request), std::move(stop_token));
}

template <typename RequestT>
auto FetchXml(const coro::http::Http& http,
              const std::optional<WebDAV::Auth::Credential>& credential,
              RequestT request, stdx::stop_token stop_token)
    -> Task<XmlDocument> {
  if (request.body) {
    request.headers.emplace_back("Content-Type", "application/xml");
  }
  request.headers.emplace_back("Accept", "application/xml");
  auto response = co_await Fetch(http, credential, std::move(request),
                                 std::move(stop_token));
  co_return XmlDocument(co_await http::GetBody(std::move(response.body)));
}

}  // namespace

namespace util {

template <>
nlohmann::json ToJson<WebDAV::Auth::AuthToken>(WebDAV::Auth::AuthToken token) {
  nlohmann::json json;
  json["endpoint"] = std::move(token.endpoint);
  if (token.credential) {
    json["access_token"] = ToAccessToken(*token.credential);
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
    re::regex regex(R"(([^\:]+):(.*))");
    re::smatch match;
    if (re::regex_match(access_token, match, regex)) {
      auth_token.credential = WebDAV::Auth::Credential{
          .username = match[1].str(), .password = match[2].str()};
    } else {
      throw std::invalid_argument("invalid access_token");
    }
  }
  return auth_token;
}

}  // namespace util

Task<std::variant<http::Response<>, WebDAV::Auth::AuthToken>>
WebDAV::Auth::AuthHandler::operator()(http::Request<> request,
                                      stdx::stop_token) const {
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
    co_return http::Response<>{.status = 200, .body = GenerateLoginPage()};
  }
}

auto WebDAV::GetRoot(stdx::stop_token) const -> Task<Directory> {
  Directory d{{.id = auth_token_.endpoint}};
  co_return d;
}

auto WebDAV::GetGeneralData(stdx::stop_token stop_token) const
    -> Task<GeneralData> {
  std::string username;
  auto uri = http::ParseUri(auth_token_.endpoint);
  if (auth_token_.credential) {
    username += auth_token_.credential->username + "@";
  }
  username += std::move(uri.host).value();
  if (uri.port) {
    username += ":" + std::to_string(*uri.port);
  }
  Request request{.url = auth_token_.endpoint,
                  .method = http::Method::kPropfind,
                  .headers = {{"Depth", "0"}},
                  .body = R"(
                                 <D:propfind xmlns:D="DAV:">
                                   <D:prop>
                                     <D:quota-available-bytes/>
                                     <D:quota-used-bytes/>
                                   </D:prop>
                                 </D:propfind>
                            )"};
  auto response = co_await FetchXml(*http_, auth_token_.credential,
                                    std::move(request), std::move(stop_token));
  auto stats = response.document_element()
                   .child("response")
                   .child("propstat")
                   .child("prop");
  GeneralData result{.username = std::move(username)};
  if (auto text = stats.child("quota-used-bytes").text()) {
    result.space_used = text.as_llong();
  }
  if (auto text = stats.child("quota-available-bytes").text();
      text && result.space_used && text.as_llong() >= 0) {
    result.space_total = text.as_llong() + *result.space_used;
  }
  co_return result;
}

auto WebDAV::ListDirectoryPage(Directory directory,
                               std::optional<std::string> page_token,
                               stdx::stop_token stop_token) const
    -> Task<PageData> {
  Request request{.url = GetEndpoint(directory.id),
                  .method = http::Method::kPropfind,
                  .headers = {{"Depth", "1"}}};
  auto response = co_await FetchXml(*http_, auth_token_.credential,
                                    std::move(request), std::move(stop_token));
  auto root = response.document_element();
  PageData page;
  for (auto node = root.first_child().next_sibling(); node;
       node = node.next_sibling()) {
    page.items.emplace_back(
        ToItem(XmlNode<pugi::xml_node>(node, response.ns())));
  }
  co_return page;
}

Generator<std::string> WebDAV::GetFileContent(
    File file, http::Range range, stdx::stop_token stop_token) const {
  auto request = Request{.url = GetEndpoint(file.id),
                         .headers = {http::ToRangeHeader(range)}};
  if (auth_token_.credential) {
    request.headers.emplace_back(
        "Authorization", "Basic " + ToAccessToken(*auth_token_.credential));
  }
  auto response =
      co_await http_->Fetch(std::move(request), std::move(stop_token));
  FOR_CO_AWAIT(std::string & body, response.body) { co_yield std::move(body); }
}

template <typename ItemT>
Task<ItemT> WebDAV::RenameItem(ItemT item, std::string new_name,
                               stdx::stop_token stop_token) const {
  std::string destination = item.id;
  if (destination.empty()) {
    throw CloudException("invalid path");
  }
  if (destination.back() == '/') {
    destination.pop_back();
  }
  auto it = destination.find_last_of('/');
  if (it == std::string::npos) {
    throw CloudException("invalid path");
  }
  destination.resize(it);
  destination += '/' + http::EncodeUri(new_name);
  co_return co_await Move(std::move(item), std::move(destination),
                          std::move(stop_token));
}

auto WebDAV::CreateDirectory(Directory parent, std::string name,
                             stdx::stop_token stop_token) const
    -> Task<Directory> {
  auto endpoint = GetEndpoint(Concat(parent.id, name));
  Request request{.url = endpoint, .method = http::Method::kMkcol};
  co_await Fetch(*http_, auth_token_.credential, std::move(request),
                 stop_token);
  request = {.url = std::move(endpoint),
             .method = http::Method::kPropfind,
             .headers = {{"Depth", "0"}}};
  auto response = co_await FetchXml(*http_, auth_token_.credential,
                                    std::move(request), std::move(stop_token));
  co_return std::get<Directory>(ToItem(XmlNode<pugi::xml_node>(
      response.document_element().first_child(), response.ns())));
}

template <typename ItemT>
Task<> WebDAV::RemoveItem(ItemT item, stdx::stop_token stop_token) const {
  Request request{.url = GetEndpoint(item.id), .method = http::Method::kDelete};
  co_await Fetch(*http_, auth_token_.credential, std::move(request),
                 std::move(stop_token));
}

template <typename ItemT>
Task<ItemT> WebDAV::MoveItem(ItemT source, Directory destination,
                             stdx::stop_token stop_token) const {
  co_return co_await Move(std::move(source),
                          Concat(destination.id, source.name),
                          std::move(stop_token));
}

auto WebDAV::CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token) const
    -> Task<File> {
  auto endpoint = GetEndpoint(Concat(parent.id, name));
  http::Request<> upload_request = {
      .url = endpoint,
      .method = http::Method::kPut,
      .headers = {{"Content-Type", "application/octet-stream"}},
      .body = std::move(content.data)};
  if (content.size) {
    upload_request.headers.emplace_back("Content-Length",
                                        std::to_string(*content.size));
  }
  co_await Fetch(*http_, auth_token_.credential, std::move(upload_request),
                 stop_token);
  Request request{.url = std::move(endpoint),
                  .method = http::Method::kPropfind,
                  .headers = {{"Depth", "0"}}};
  auto response = co_await FetchXml(*http_, auth_token_.credential,
                                    std::move(request), std::move(stop_token));
  co_return std::get<File>(ToItem(XmlNode<pugi::xml_node>(
      response.document_element().first_child(), response.ns())));
}

template <typename T>
Task<T> WebDAV::Move(T item, std::string destination,
                     stdx::stop_token stop_token) const {
  Request request{.url = GetEndpoint(item.id),
                  .method = http::Method::kMove,
                  .headers = {{"Destination", destination}}};
  co_await Fetch(*http_, auth_token_.credential, std::move(request),
                 stop_token);
  request = {.url = GetEndpoint(destination),
             .method = http::Method::kPropfind,
             .headers = {{"Depth", "0"}}};
  auto response = co_await FetchXml(*http_, auth_token_.credential,
                                    std::move(request), std::move(stop_token));
  co_return std::get<T>(ToItem(XmlNode<pugi::xml_node>(
      response.document_element().first_child(), response.ns())));
}

std::string WebDAV::GetEndpoint(std::string_view href) const {
  auto uri = http::ParseUri(href);
  if (uri.host) {
    return std::string(href);
  } else {
    auto endpoint = http::ParseUri(auth_token_.endpoint);
    std::string uri;
    if (endpoint.scheme) {
      uri += *endpoint.scheme + "://";
    }
    uri += endpoint.host.value();
    if (endpoint.port) {
      uri += ":" + std::to_string(*endpoint.port);
    }
    uri += std::string(href);
    return uri;
  }
}

namespace util {

template <>
auto AbstractCloudProvider::Create<WebDAV>(WebDAV p)
    -> std::unique_ptr<AbstractCloudProvider> {
  return CreateAbstractCloudProvider(std::move(p));
}

}  // namespace util

template auto WebDAV::RenameItem(File item, std::string new_name,
                                 stdx::stop_token stop_token) const
    -> Task<File>;

template auto WebDAV::RenameItem(Directory item, std::string new_name,
                                 stdx::stop_token stop_token) const
    -> Task<Directory>;

template auto WebDAV::MoveItem(File, Directory, stdx::stop_token) const
    -> Task<File>;

template auto WebDAV::MoveItem(Directory, Directory, stdx::stop_token) const
    -> Task<Directory>;

template auto WebDAV::RemoveItem(File, stdx::stop_token) const -> Task<>;

template auto WebDAV::RemoveItem(Directory, stdx::stop_token) const -> Task<>;

}  // namespace coro::cloudstorage
