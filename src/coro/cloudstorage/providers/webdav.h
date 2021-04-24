#ifndef CORO_CLOUDSTORAGE_WEBDAV_H
#define CORO_CLOUDSTORAGE_WEBDAV_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/assets.h>
#include <coro/cloudstorage/util/auth_data.h>
#include <coro/cloudstorage/util/auth_handler.h>
#include <coro/cloudstorage/util/auth_token_manager.h>
#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/when_all.h>

#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <sstream>

namespace coro::cloudstorage {

class WebDAV {
 public:
  struct GeneralData {
    std::string username;
    std::optional<int64_t> space_used;
    std::optional<int64_t> space_total;
  };

  struct ItemData {
    std::string id;
    std::string name;
    std::optional<int64_t> timestamp;
  };

  struct Directory : ItemData {};

  struct File : ItemData {
    std::optional<int64_t> size;
    std::optional<std::string> mime_type;
  };

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  struct Auth {
    struct Credential {
      std::string username;
      std::string password;
    };
    struct AuthToken {
      std::string endpoint;
      std::optional<Credential> credential;
    };
    struct AuthData {};

    static std::string ToAccessToken(const Credential& credential) {
      return http::ToBase64(credential.username + ":" + credential.password);
    }
  };

  struct FileContent {
    Generator<std::string> data;
    std::optional<int64_t> size;
  };

  template <http::HttpClient Http>
  class CloudProvider;

  static constexpr std::string_view kId = "webdav";
  static inline constexpr auto& kIcon = util::kAssetsProvidersWebdavPng;

 private:
  static std::optional<std::string> GetNamespace(const pugi::xml_node& node);

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

  static Item ToItem(const XmlNode<pugi::xml_node>&);

  template <typename T>
  static T ToItemImpl(const WebDAV::XmlNode<pugi::xml_node>& node);
};

template <http::HttpClient Http>
class WebDAV::CloudProvider
    : public coro::cloudstorage::CloudProvider<WebDAV, CloudProvider<Http>> {
 public:
  using Request = http::Request<std::string>;

  CloudProvider(const Http& http, WebDAV::Auth::AuthToken auth_token)
      : http_(&http), auth_token_(std::move(auth_token)) {}

  Task<Directory> GetRoot(stdx::stop_token) const {
    Directory d{{.id = "/"}};
    co_return d;
  }

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token) const {
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
    auto response =
        co_await FetchXml(std::move(request), std::move(stop_token));
    auto stats = response.document_element()
                     .child("response")
                     .child("propstat")
                     .child("prop");
    GeneralData result{.username = std::move(username)};
    if (auto text = stats.child("quota-used-bytes").text()) {
      result.space_used = text.as_llong();
    }
    if (auto text = stats.child("quota-available-bytes").text();
        text && result.space_used) {
      result.space_total = text.as_llong() + *result.space_used;
    }
    co_return result;
  }

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) const {
    Request request{.url = GetEndpoint(directory.id),
                    .method = http::Method::kPropfind,
                    .headers = {{"Depth", "1"}}};
    auto response =
        co_await FetchXml(std::move(request), std::move(stop_token));
    auto root = response.document_element();
    PageData page;
    for (auto node = root.first_child().next_sibling(); node;
         node = node.next_sibling()) {
      page.items.emplace_back(
          ToItem(XmlNode<pugi::xml_node>(node, response.ns())));
    }
    co_return page;
  }

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) const {
    std::stringstream range_header;
    range_header << "bytes=" << range.start << "-";
    if (range.end) {
      range_header << *range.end;
    }
    auto request =
        Request{.url = GetEndpoint(file.id),
                .headers = {{"Range", std::move(range_header).str()}}};
    if (auth_token_.credential) {
      request.headers.emplace_back(
          "Authorization",
          "Basic " + Auth::ToAccessToken(*auth_token_.credential));
    }
    auto response =
        co_await http_->Fetch(std::move(request), std::move(stop_token));
    FOR_CO_AWAIT(std::string & body, response.body) {
      co_yield std::move(body);
    }
  }

  template <typename Item>
  Task<Item> RenameItem(Item item, std::string new_name,
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

  Task<Directory> CreateDirectory(Directory parent, std::string name,
                                  stdx::stop_token stop_token) const {
    auto endpoint = GetEndpoint(Concat(parent.id, name));
    Request request{.url = endpoint, .method = http::Method::kMkcol};
    co_await Fetch(std::move(request), stop_token);
    request = {.url = std::move(endpoint),
               .method = http::Method::kPropfind,
               .headers = {{"Depth", "0"}}};
    auto response =
        co_await FetchXml(std::move(request), std::move(stop_token));
    co_return std::get<Directory>(ToItem(XmlNode<pugi::xml_node>(
        response.document_element().first_child(), response.ns())));
  }

  template <typename Item>
  Task<> RemoveItem(Item item, stdx::stop_token stop_token) const {
    Request request{.url = GetEndpoint(item.id),
                    .method = http::Method::kDelete};
    co_await Fetch(std::move(request), std::move(stop_token));
  }

  template <typename Item>
  Task<Item> MoveItem(Item source, Directory destination,
                      stdx::stop_token stop_token) const {
    co_return co_await Move(std::move(source),
                            Concat(destination.id, source.name),
                            std::move(stop_token));
  }

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content,
                        stdx::stop_token stop_token) const {
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
    co_await Fetch(std::move(upload_request), stop_token);
    Request request{.url = std::move(endpoint),
                    .method = http::Method::kPropfind,
                    .headers = {{"Depth", "0"}}};
    auto response =
        co_await FetchXml(std::move(request), std::move(stop_token));
    co_return std::get<File>(ToItem(XmlNode<pugi::xml_node>(
        response.document_element().first_child(), response.ns())));
  }

 private:
  static std::string Concat(std::string parent, std::string_view name) {
    if (parent.empty()) {
      throw CloudException("invalid path");
    }
    if (parent.back() != '/') {
      parent += '/';
    }
    parent += http::EncodeUri(name);
    return parent;
  }

  template <typename T>
  Task<T> Move(T item, std::string destination,
               stdx::stop_token stop_token) const {
    Request request{.url = GetEndpoint(item.id),
                    .method = http::Method::kMove,
                    .headers = {{"Destination", destination}}};
    co_await Fetch(std::move(request), stop_token);
    request = {.url = GetEndpoint(destination),
               .method = http::Method::kPropfind,
               .headers = {{"Depth", "0"}}};
    auto response =
        co_await FetchXml(std::move(request), std::move(stop_token));
    co_return std::get<T>(ToItem(XmlNode<pugi::xml_node>(
        response.document_element().first_child(), response.ns())));
  }

  std::string GetEndpoint(std::string_view href) const {
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

  template <typename RequestT>
  Task<typename Http::ResponseType> Fetch(RequestT request,
                                          stdx::stop_token stop_token) const {
    if (auth_token_.credential) {
      request.headers.emplace_back(
          "Authorization",
          "Basic " + Auth::ToAccessToken(*auth_token_.credential));
    }
    http::ResponseLike auto response =
        co_await http_->Fetch(std::move(request), std::move(stop_token));
    if (response.status / 100 != 2) {
      throw coro::http::HttpException(
          response.status, co_await http::GetBody(std::move(response.body)));
    }
    co_return response;
  }

  template <typename RequestT>
  Task<XmlDocument> FetchXml(RequestT request,
                             stdx::stop_token stop_token) const {
    if (request.body) {
      request.headers.emplace_back("Content-Type", "application/xml");
    }
    request.headers.emplace_back("Accept", "application/xml");
    http::ResponseLike auto response =
        co_await Fetch(std::move(request), std::move(stop_token));
    co_return XmlDocument(co_await http::GetBody(std::move(response.body)));
  }

  const Http* http_;
  WebDAV::Auth::AuthToken auth_token_;
};

namespace util {

template <>
nlohmann::json ToJson<WebDAV::Auth::AuthToken>(WebDAV::Auth::AuthToken token);

template <>
WebDAV::Auth::AuthToken ToAuthToken<WebDAV::Auth::AuthToken>(
    const nlohmann::json& json);

template <>
WebDAV::Auth::AuthData GetAuthData<WebDAV>();

class WebDAVAuthHandler {
 public:
  Task<std::variant<http::Response<>, WebDAV::Auth::AuthToken>> operator()(
      http::Request<> request, stdx::stop_token) const;
};

template <>
struct CreateAuthHandler<WebDAV> {
  template <typename CloudFactory>
  auto operator()(const CloudFactory& cloud_factory,
                  WebDAV::Auth::AuthData auth_data) const {
    return WebDAVAuthHandler();
  }
};

}  // namespace util

template <>
struct CreateCloudProvider<WebDAV> {
  template <typename CloudFactory, typename... Args>
  auto operator()(const CloudFactory& factory,
                  WebDAV::Auth::AuthToken auth_token, Args&&...) const {
    return WebDAV::CloudProvider(*factory.http_, std::move(auth_token));
  }
};

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_DROPBOX_H
