#include "coro/cloudstorage/util/settings_manager.h"

#include <sqlite_orm/sqlite_orm.h>

#include "coro/cloudstorage/util/string_utils.h"
#include "coro/http/http_parse.h"

namespace coro::cloudstorage::util {

namespace {

using ::coro::cloudstorage::util::StrCat;
using ::sqlite_orm::and_;
using ::sqlite_orm::c;
using ::sqlite_orm::column;
using ::sqlite_orm::foreign_key;
using ::sqlite_orm::join;
using ::sqlite_orm::make_column;
using ::sqlite_orm::make_storage;
using ::sqlite_orm::make_table;
using ::sqlite_orm::on;
using ::sqlite_orm::order_by;
using ::sqlite_orm::primary_key;
using ::sqlite_orm::select;
using ::sqlite_orm::where;

struct DbAppSettings {
  std::string id;
  std::string value;
};

struct DbAuthToken {
  std::string account_type;
  std::string account_username;
  std::vector<char> auth_token;
};

uint16_t GetPort(std::string_view redirect_uri) {
  return http::ParseUri(redirect_uri).port.value_or(80);
}

auto CreateStorage(std::string path) {
  auto storage = make_storage(
      std::move(path),
      make_table(
          "auth_token", make_column("account_type", &DbAuthToken::account_type),
          make_column("account_username", &DbAuthToken::account_username),
          make_column("auth_token", &DbAuthToken::auth_token),
          primary_key(&DbAuthToken::account_type,
                      &DbAuthToken::account_username)),
      make_table("app_settings", make_column("id", &DbAppSettings::id),
                 make_column("value", &DbAppSettings::value),
                 primary_key(&DbAppSettings::id)));
  storage.sync_schema();
  return storage;
}

std::vector<char> ToCbor(const nlohmann::json& json) {
  std::vector<char> output;
  nlohmann::json::to_cbor(json, output);
  return output;
}

auto& GetDb(std::any& any) {
  return std::any_cast<decltype(CreateStorage(""))&>(any);
}

}  // namespace

SettingsManager::SettingsManager(AbstractCloudFactory* factory,
                                 CloudFactoryConfig config)
    : factory_(factory),
      config_(std::move(config)),
      db_(CreateStorage(config_.config_path)),
      effective_is_public_network_enabled_(IsPublicNetworkEnabled()),
      port_(GetPort(config_.auth_data.redirect_uri())) {}

void SettingsManager::SetEnablePublicNetwork(bool enable) const {
  if (enable) {
    GetDb(db_).replace(DbAppSettings{.id = "public_network", .value = "true"});
  } else {
    GetDb(db_).remove<DbAppSettings>("public_network");
  }
}

bool SettingsManager::IsPublicNetworkEnabled() const {
  std::vector<std::string> values = GetDb(db_).select(
      &DbAppSettings::value, where(c(&DbAppSettings::id) == "public_network"));
  if (values.empty()) {
    return false;
  } else {
    return values[0] == "true";
  }
}

http::HttpServerConfig SettingsManager::GetHttpServerConfig() const {
  return {
      .address = EffectiveIsPublicNetworkEnabled() ? "0.0.0.0" : "127.0.0.1",
      .port = port_};
}

std::string SettingsManager::GetPostAuthRedirectUri(
    std::string_view account_type, std::string_view username) const {
  return config_.post_auth_redirect_uri(account_type, username);
}

auto SettingsManager::LoadTokenData() const -> std::vector<AuthToken> {
  std::vector<AuthToken> result;
  for (const DbAuthToken& db_auth_token : GetDb(db_).get_all<DbAuthToken>()) {
    for (auto type : factory_->GetSupportedCloudProviders()) {
      const auto& auth = factory_->GetAuth(type);
      if (db_auth_token.account_type == auth.GetId()) {
        AuthToken auth_token{auth.ToAuthToken(nlohmann::json::from_cbor(
                                 db_auth_token.auth_token)),
                             db_auth_token.account_username};
        result.emplace_back(std::move(auth_token));
      }
    }
  }
  return result;
}

void SettingsManager::SaveToken(AbstractCloudProvider::Auth::AuthToken token,
                                std::string_view id) const {
  const auto& auth = factory_->GetAuth(token.type);
  GetDb(db_).replace(DbAuthToken{.account_type = std::string(auth.GetId()),
                                 .account_username = std::string(id),
                                 .auth_token = ToCbor(auth.ToJson(token))});
}

void SettingsManager::RemoveToken(std::string_view id,
                                  std::string_view type) const {
  GetDb(db_).remove<DbAuthToken>(type, id);
}

}  // namespace coro::cloudstorage::util