#ifndef CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
#define CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H

#include <coro/http/http.h>

namespace coro::cloudstorage {

template <typename T>
concept CloudProviderImpl = requires(const T i, http::HttpStub& http,
                                     std::string_view string_view,
                                     stdx::stop_token stop_token) {
  { i.ExchangeAuthorizationCode(http, string_view, stop_token) }
  ->Awaitable;
};

template <CloudProviderImpl Impl>
class CloudProvider : public Impl {
 public:
  using Impl::Impl;

  template <http::HttpClient HttpClient>
  auto GetGeneralData(HttpClient& http, std::string_view access_token,
                      stdx::stop_token stop_token = stdx::stop_token()) {
    return Impl::GetGeneralData(http, access_token, std::move(stop_token));
  }

  template <http::HttpClient HttpClient>
  Generator<typename Impl::PageData> ListDirectory(
      HttpClient& http, std::string_view access_token,
      const typename Impl::Directory& directory,
      stdx::stop_token stop_token = stdx::stop_token()) {
    std::optional<std::string> current_page_token;
    do {
      auto page_data = co_await Impl::ListDirectoryPage(
          http, access_token, directory, std::move(current_page_token),
          stop_token);
      co_yield page_data;
      current_page_token = page_data.next_page_token;
    } while (current_page_token);
  }

  template <http::HttpClient HttpClient>
  auto ListDirectoryPage(
      HttpClient& http, std::string_view access_token,
      const typename Impl::Directory& directory,
      std::optional<std::string_view> page_token = std::nullopt,
      stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::ListDirectoryPage(http, access_token, directory, page_token,
                                   std::move(stop_token));
  }

  template <http::HttpClient HttpClient>
  auto ExchangeAuthorizationCode(
      HttpClient& http, std::string_view code,
      stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::ExchangeAuthorizationCode(http, code, std::move(stop_token));
  }

  template <http::HttpClient HttpClient>
  auto RefreshAccessToken(
      HttpClient& http, std::string_view refresh_token,
      stdx::stop_token stop_token = stdx::stop_token()) const {
    return Impl::RefreshAccessToken(http, refresh_token, std::move(stop_token));
  }

 private:
};

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_PROVIDER_H
