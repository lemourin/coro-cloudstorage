#include "coro/cloudstorage/util/list_directory_handler.h"

#include <fmt/format.h>

#include "coro/cloudstorage/util/cloud_provider_utils.h"
#include "coro/cloudstorage/util/serialize_utils.h"
#include "coro/util/regex.h"

namespace coro::cloudstorage::util {

namespace {

namespace re = coro::util::re;

std::string RewriteThumbnailUrl(std::string_view host, std::string url) {
  auto host_uri = http::ParseUri(StrCat("//", host));
  if (!host_uri.host->ends_with(".localhost")) {
    return url;
  }
  auto uri = http::ParseUri(url);
  uri.host = StrCat("img.", *host_uri.host);
  uri.port = host_uri.port;
  std::string rewritten = http::ToString(uri);
  return rewritten;
}

std::string GetItemEntry(
    std::string_view host, const AbstractCloudProvider::Item& item,
    const stdx::any_invocable<std::string(std::string_view item_id) const>&
        list_url_generator,
    const stdx::any_invocable<std::string(std::string_view item_id) const>&
        thumbnail_url_generator,
    const stdx::any_invocable<std::string(const AbstractCloudProvider::File&)
                                  const>& content_url_generator) {
  return std::visit(
      [&]<typename Item>(const Item& d) {
        return fmt::format(
            fmt::runtime(kItemEntryHtml), fmt::arg("name", d.name),
            fmt::arg("size", SizeToString(d.size)),
            fmt::arg("timestamp", TimeStampToString(d.timestamp)),
            fmt::arg(
                "url",
                [&] {
                  if constexpr (std::is_same_v<
                                    Item, AbstractCloudProvider::Directory>) {
                    return list_url_generator(d.id);
                  } else {
                    return content_url_generator(d);
                  }
                }()),
            fmt::arg("thumbnail_url",
                     RewriteThumbnailUrl(host, thumbnail_url_generator(d.id))));
      },
      item);
}

}  // namespace

auto ListDirectoryHandler::operator()(http::Request<> request,
                                      stdx::stop_token stop_token)
    -> Task<http::Response<>> {
  auto uri = http::ParseUri(request.url);
  re::smatch results;
  if (!re::regex_match(uri.path.value(), results,
                       re::regex(R"(\/list\/[^\/]+\/[^\/]+\/(.*)$)"))) {
    co_return http::Response<>{.status = 400};
  }
  std::string item_id =
      http::DecodeUri(ToStringView(results[1].begin(), results[1].end()));
  auto item = co_await account_.GetItemById(item_id, stop_token);
  auto* directory = std::get_if<AbstractCloudProvider::Directory>(&item.item);
  if (!directory) {
    co_return http::Response<>{.status = 400};
  }
  auto versioned = co_await account_.ListDirectory(*directory, stop_token);
  co_return http::Response<>{
      .status = 200,
      .headers = {{"Content-Type", "text/html"}},
      .body = GetDirectoryContent(
          http::GetHeader(request.headers, "Host").value(),
          std::move(*directory), std::move(versioned.content), stop_token)};
}

Generator<std::string> ListDirectoryHandler::GetDirectoryContent(
    std::string host, AbstractCloudProvider::Directory parent,
    Generator<AbstractCloudProvider::PageData> page_data,
    stdx::stop_token stop_token) const {
  co_yield "<!DOCTYPE html>"
      "<html lang='en-us'>"
      "<head>"
      "  <title>coro-cloudstorage</title>"
      "  <meta charset='UTF-8'>"
      "  <meta name='viewport' "
      "        content='width=device-width, initial-scale=1'>"
      "  <link rel=stylesheet href='/static/layout.css'>"
      "  <link rel=stylesheet href='/static/colors.css'>"
      "  <link rel='icon' type='image/x-icon' href='/static/favicon.ico'>"
      "</head>"
      "<body class='root-container'>"
      "<table class='content-table'>";
  std::string parent_entry =
      fmt::format(fmt::runtime(kItemEntryHtml), fmt::arg("name", ".."),
                  fmt::arg("size", ""), fmt::arg("timestamp", ""),
                  fmt::arg("url", "javascript: history.go(-1)"),
                  fmt::arg("thumbnail_url",
                           RewriteThumbnailUrl(host, "/static/folder.svg")));
  co_yield std::move(parent_entry);
  FOR_CO_AWAIT(const auto& page, page_data) {
    for (const auto& item : page.items) {
      co_yield GetItemEntry(host, item, list_url_generator_,
                            thumbnail_url_generator_, content_url_generator_);
    }
  }
  co_yield "</table>"
      "</body>"
      "</html>";
}

}  // namespace coro::cloudstorage::util
