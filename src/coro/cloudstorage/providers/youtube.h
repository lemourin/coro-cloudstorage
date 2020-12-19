#ifndef CORO_CLOUDSTORAGE_YOUTUBE_H
#define CORO_CLOUDSTORAGE_YOUTUBE_H

#include <coro/cloudstorage/cloud_exception.h>
#include <coro/cloudstorage/providers/google_drive.h>
#include <coro/http/http.h>
#include <coro/http/http_parse.h>
#include <coro/task.h>

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace coro::cloudstorage {

template <typename Auth>
struct YouTubeImpl;

struct YouTube {
  using json = nlohmann::json;

  struct Auth : GoogleDrive::Auth {
    static std::string GetAuthorizationUrl(const AuthData& data) {
      return "https://accounts.google.com/o/oauth2/auth?" +
             http::FormDataToString({{"response_type", "code"},
                                     {"client_id", data.client_id},
                                     {"redirect_uri", data.redirect_uri},
                                     {"scope",
                                      "https://www.googleapis.com/auth/"
                                      "youtube.readonly openid email"},
                                     {"access_type", "offline"},
                                     {"prompt", "consent"},
                                     {"state", data.state}});
    }
  };

  template <typename AuthManager>
  using Impl = YouTubeImpl<AuthManager>;

  struct Directory {
    std::string id;
    std::string name;
  };

  struct File : Directory {
    std::optional<std::string> mime_type;
    std::optional<int64_t> size;
  };

  using Item = std::variant<File, Directory>;

  struct PageData {
    std::vector<Item> items;
    std::optional<std::string> next_page_token;
  };

  static constexpr std::string_view kId = "youtube";
};

template <typename AuthManager>
struct YouTubeImpl : YouTube {
  using Request = http::Request<std::string>;

  explicit YouTubeImpl(AuthManager auth_manager)
      : auth_manager_(std::move(auth_manager)) {}

  static Task<Directory> GetRoot(stdx::stop_token) {
    Directory d;
    d.id = "root";
    co_return d;
  }

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token) {
    PageData result;
    if (directory.id == "root") {
      Request request = {
          .url = GetEndpoint("/channels") + "?" +
                 http::FormDataToString({{"mine", "true"},
                                         {"part", "contentDetails,snippet"},
                                         {"maxResults", "50"}})};
      auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                       std::move(stop_token));
      for (const auto& [key, value] :
           response["items"][0]["contentDetails"]["relatedPlaylists"].items()) {
        result.items.emplace_back(Directory{.id = value, .name = key});
      }
      co_return result;
    } else {
      Request request = {
          .url = GetEndpoint("/playlistItems") + "?" +
                 http::FormDataToString({{"part", "snippet"},
                                         {"playlistId", directory.id},
                                         {"maxResults", "50"}})};
      auto response = co_await auth_manager_.FetchJson(std::move(request),
                                                       std::move(stop_token));
      for (const auto& item : response["items"]) {
        File file;
        file.id = item["id"];
        file.name = item["snippet"]["title"];
        result.items.emplace_back(std::move(file));
      }
      co_return result;
    };
  }

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token) {
    co_yield "TODO";
  }

 private:
  static constexpr std::string_view kEndpoint =
      "https://www.googleapis.com/youtube/v3";

  static std::string GetEndpoint(std::string_view path) {
    return std::string(kEndpoint) + std::string(path);
  }

  AuthManager auth_manager_;
};  // namespace coro::cloudstorage

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_YOUTUBE_H