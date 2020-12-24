#ifndef CORO_CLOUDSTORAGE_FETCH_JSON_H
#define CORO_CLOUDSTORAGE_FETCH_JSON_H

#include <coro/http/http.h>
#include <coro/http/http_parse.h>

#include <nlohmann/json.hpp>

namespace coro::cloudstorage::util {

template <typename RequestType>
Task<nlohmann::json> FetchJson(const http::HttpClient auto& http,
                               RequestType request,
                               stdx::stop_token stop_token) {
  if (!http::HasHeader(request.headers, "Allow", "application/json")) {
    request.headers.emplace_back("Allow", "application/json");
  }
  http::ResponseLike auto response =
      co_await http.Fetch(std::move(request), std::move(stop_token));
  std::string body = co_await http::GetBody(std::move(response.body));
  if (response.status / 100 != 2) {
    throw coro::http::HttpException(response.status, std::move(body));
  }
  co_return nlohmann::json::parse(std::move(body));
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FETCH_JSON_H
