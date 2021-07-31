#ifndef CORO_CLOUDSTORAGE_HANDLER_UTILS_H
#define CORO_CLOUDSTORAGE_HANDLER_UTILS_H

#include <span>
#include <sstream>
#include <string_view>
#include <vector>

#include "coro/http/http.h"
#include "coro/http/http_parse.h"

namespace coro::cloudstorage::util {

template <typename T>
bool Equal(std::span<const T> s1, std::span<const T> s2) {
  return std::equal(s1.begin(), s1.end(), s2.begin());
}

template <typename Request>
std::string GetPath(const Request& request) {
  return http::ParseUri(request.url).path.value();
}

std::vector<std::string> GetEffectivePath(std::string_view uri_path);

template <typename CloudProvider, typename Request>
auto ToFileContent(Request request) {
  if (!request.body) {
    throw http::HttpException(http::HttpException::kBadRequest);
  }
  typename CloudProvider::FileContent content{.data = std::move(*request.body)};
  auto header = http::GetHeader(request.headers, "Content-Length");
  if (std::is_convertible_v<decltype(content.size), int64_t> && !header) {
    throw http::HttpException(http::HttpException::kBadRequest);
  }
  if (header) {
    content.size = std::stoll(*header);
  }
  return content;
}

template <typename CloudProvider, typename Item>
http::Response<> GetFileContentResponse(CloudProvider* provider, Item d,
                                        std::optional<http::Range> range,
                                        stdx::stop_token stop_token) {
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Content-Type", CloudProvider::GetMimeType(d)},
      {"Content-Disposition", "inline; filename=\"" + d.name + "\""},
      {"Access-Control-Allow-Origin", "*"},
      {"Access-Control-Allow-Headers", "*"}};
  auto size = CloudProvider::GetSize(d);
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
  return http::Response<>{
      .status = !range || !size ? 200 : 206,
      .headers = std::move(headers),
      .body = provider->GetFileContent(d, range.value_or(http::Range{}),
                                       std::move(stop_token))};
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_HANDLER_UTILS_H
