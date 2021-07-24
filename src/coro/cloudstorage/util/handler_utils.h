#ifndef CORO_CLOUDSTORAGE_HANDLER_UTILS_H
#define CORO_CLOUDSTORAGE_HANDLER_UTILS_H

#include <coro/http/http.h>
#include <coro/http/http_parse.h>

#include <span>
#include <string_view>
#include <vector>

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
static auto ToFileContent(Request request) {
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

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_HANDLER_UTILS_H
