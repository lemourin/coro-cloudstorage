#ifndef CORO_CLOUDSTORAGE_MEGA_H
#define CORO_CLOUDSTORAGE_MEGA_H

#include <mega.h>

namespace coro::cloudstorage {

struct Mega {
  struct GeneralData {
    std::string username;
  };

  template <http::HttpClient HttpClient>
  static Task<GeneralData> GetGeneralData(HttpClient& http,
                                          std::string access_token,
                                          stdx::stop_token stop_token) {
    co_return GeneralData{};
  }
};

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_MEGA_H
