#ifndef CORO_CLOUDSTORAGE_FETCH_JSON_H
#define CORO_CLOUDSTORAGE_FETCH_JSON_H

#include <coro/http/http.h>

#include <nlohmann/json.hpp>

namespace coro::cloudstorage::util {

template <typename RequestType>
static Task<nlohmann::json> FetchJson(http::HttpClient auto& http,
                                      RequestType&& request,
                                      stdx::stop_token stop_token) {
  http::ResponseLike auto response = co_await http.Fetch(
      std::forward<RequestType>(request), std::move(stop_token));
  std::string body = co_await http::GetBody(std::move(response.body));
  if (response.status / 100 != 2) {
    throw coro::http::HttpException(response.status, std::move(body));
  }
  co_return nlohmann::json::parse(std::move(body));
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FETCH_JSON_H
