#ifndef CORO_CLOUDSTORAGE_HANDLER_UTILS_H
#define CORO_CLOUDSTORAGE_HANDLER_UTILS_H

#include <span>
#include <sstream>
#include <string_view>
#include <vector>

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/generator.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"

namespace coro::cloudstorage::util {

namespace internal {

Generator<std::string> GetFileContentResponseBody(
    Generator<std::string> content, Generator<std::string>::iterator it);

}  // namespace internal

template <typename T>
bool Equal(std::span<const T> s1, std::span<const T> s2) {
  return std::equal(s1.begin(), s1.end(), s2.begin());
}

template <typename Request>
std::string GetPath(const Request& request) {
  return http::ParseUri(request.url).path.value();
}

std::vector<std::string> GetEffectivePath(std::string_view uri_path);

template <typename Request>
auto ToFileContent(AbstractCloudProvider* p,
                   const AbstractCloudProvider::Directory& parent,
                   Request request) {
  if (!request.body) {
    throw http::HttpException(http::HttpException::kBadRequest);
  }
  AbstractCloudProvider::FileContent content{.data = std::move(*request.body)};
  auto header = http::GetHeader(request.headers, "Content-Length");
  if (p->IsFileContentSizeRequired(parent) && !header) {
    throw http::HttpException(http::HttpException::kBadRequest);
  }
  if (header) {
    content.size = std::stoll(*header);
  }
  return content;
}

template <typename CloudProvider, typename Item>
Task<http::Response<>> GetFileContentResponse(CloudProvider* provider, Item d,
                                              std::optional<http::Range> range,
                                              stdx::stop_token stop_token) {
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Content-Type", d.mime_type},
      {"Content-Disposition", "inline; filename=\"" + d.name + "\""},
      {"Access-Control-Allow-Origin", "*"},
      {"Access-Control-Allow-Headers", "*"}};
  auto size = d.size;
  if (size) {
    http::Range drange = range.value_or(http::Range{});
    if (!drange.end) {
      drange.end = *size - 1;
    }
    headers.emplace_back("Accept-Ranges", "bytes");
    headers.emplace_back("Content-Length",
                         std::to_string(*drange.end - drange.start + 1));
    if (range) {
      std::stringstream stream;
      stream << "bytes " << drange.start << "-" << *drange.end << "/" << *size;
      headers.emplace_back("Content-Range", std::move(stream).str());
    }
  }
  auto content = provider->GetFileContent(
      std::move(d), range.value_or(http::Range{}), std::move(stop_token));
  auto it = co_await content.begin();
  co_return http::Response<>{.status = !range || !size ? 200 : 206,
                             .headers = std::move(headers),
                             .body = internal::GetFileContentResponseBody(
                                 std::move(content), std::move(it))};
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_HANDLER_UTILS_H
