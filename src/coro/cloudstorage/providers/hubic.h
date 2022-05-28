#ifndef CORO_CLOUDSTORAGE_HUBIC_H
#define CORO_CLOUDSTORAGE_HUBIC_H

#include <string_view>

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/cloudstorage/providers/open_stack.h"
#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/auth_data.h"
#include "coro/cloudstorage/util/auth_manager.h"
#include "coro/cloudstorage/util/fetch_json.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http.h"
#include "coro/http/http_parse.h"

namespace coro::cloudstorage {

class HubiC : public OpenStack {
 public:
  struct GeneralData {
    std::string username;
    int64_t space_used;
    int64_t space_total;
  };

  struct Auth {
    struct AuthData {
      std::string client_id;
      std::string client_secret;
      std::string redirect_uri;
      std::string state;
    };

    struct AuthToken {
      std::string access_token;
      std::string refresh_token;
      OpenStack::Auth::AuthToken openstack_auth_token;
    };

    static std::string GetAuthorizationUrl(const AuthData& data);

    static Task<AuthToken> ExchangeAuthorizationCode(
        const coro::http::Http& http, AuthData auth_data, std::string code,
        stdx::stop_token stop_token);
  };

  class CloudProvider;

  static constexpr std::string_view kId = "hubic";
  static inline constexpr auto& kIcon = util::kAssetsProvidersHubicPng;
};

class HubiC::CloudProvider
    : public coro::cloudstorage::CloudProvider<HubiC, CloudProvider> {
 public:
  CloudProvider(const coro::http::Http* http, Auth::AuthToken auth_token,
                Auth::AuthData auth_data,
                OnAuthTokenUpdated<Auth::AuthToken> on_auth_token_updated,
                util::AuthorizeRequest<Auth> authorizer_request);

  CloudProvider(CloudProvider&& other) noexcept;
  CloudProvider(const CloudProvider&) = delete;

  CloudProvider& operator=(CloudProvider&& other) noexcept;
  CloudProvider& operator=(const CloudProvider&) = delete;

  Task<Directory> GetRoot(stdx::stop_token stop_token) const;

  Task<PageData> ListDirectoryPage(Directory directory,
                                   std::optional<std::string> page_token,
                                   stdx::stop_token stop_token);

  Generator<std::string> GetFileContent(File file, http::Range range,
                                        stdx::stop_token stop_token);

  Task<Directory> CreateDirectory(Directory parent, std::string_view name,
                                  stdx::stop_token stop_token);

  template <typename Item>
  Task<> RemoveItem(Item item, stdx::stop_token stop_token);

  template <typename Item>
  Task<Item> MoveItem(Item source, Directory destination,
                      stdx::stop_token stop_token);

  template <typename Item>
  Task<Item> RenameItem(Item item, std::string new_name,
                        stdx::stop_token stop_token);

  Task<File> CreateFile(Directory parent, std::string_view name,
                        FileContent content, stdx::stop_token stop_token);

  Task<GeneralData> GetGeneralData(stdx::stop_token stop_token);

 private:
  OpenStack::CloudProvider CreateOpenStackProvider();

  const coro::http::Http* http_;
  std::unique_ptr<const OpenStack::Auth::AuthToken*> current_openstack_token_ =
      std::make_unique<const OpenStack::Auth::AuthToken*>(nullptr);
  util::AuthManager<Auth> auth_manager_;
  OpenStack::CloudProvider provider_;
};

namespace util {

template <>
nlohmann::json ToJson<HubiC::Auth::AuthToken>(HubiC::Auth::AuthToken token);

template <>
HubiC::Auth::AuthToken ToAuthToken<HubiC::Auth::AuthToken>(
    const nlohmann::json& json);

template <>
HubiC::Auth::AuthData GetAuthData<HubiC>();

}  // namespace util

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_HUBIC_H