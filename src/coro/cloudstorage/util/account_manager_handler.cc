#include "coro/cloudstorage/util/account_manager_handler.h"

#include <fmt/core.h>

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/cloud_provider_handler.h"
#include "coro/cloudstorage/util/exception_utils.h"
#include "coro/cloudstorage/util/generator_utils.h"
#include "coro/cloudstorage/util/get_size_handler.h"
#include "coro/cloudstorage/util/mux_handler.h"
#include "coro/cloudstorage/util/settings_handler.h"
#include "coro/cloudstorage/util/static_file_handler.h"
#include "coro/cloudstorage/util/theme_handler.h"
#include "coro/cloudstorage/util/webdav_utils.h"
#include "coro/util/stop_token_or.h"

namespace coro::cloudstorage::util {

namespace {

using ::coro::util::MakeUniqueStopTokenOr;

struct OnAuthTokenChanged {
  void operator()(AbstractCloudProvider::Auth::AuthToken auth_token) {
    if (*account_id) {
      d->SaveToken(std::move(auth_token), **account_id);
    }
  }
  SettingsManager* d;
  std::shared_ptr<std::optional<std::string>> account_id;
};

std::string GetAuthUrl(AbstractCloudProvider::Type type,
                       const AbstractCloudFactory* factory) {
  std::string id(factory->GetAuth(type).GetId());
  std::string url = factory->GetAuth(type).GetAuthorizationUrl().value_or(
      util::StrCat("/auth/", id));
  return fmt::format(
      fmt::runtime(kProviderEntryHtml), fmt::arg("provider_url", url),
      fmt::arg("image_url", util::StrCat("/static/", id, ".png")));
}

std::string GetHtmlStacktrace(const stdx::stacktrace& stacktrace) {
  std::stringstream stream;
  stream << "<tr><td><br><br>Stacktrace: <br>"
         << coro::GetHtmlStacktrace(stacktrace) << "</td></tr>";
  return std::move(stream).str();
}

http::Response<> GetErrorResponse(ErrorMetadata error) {
  std::string content = fmt::format(
      fmt::runtime(kErrorPageHtml), fmt::arg("error_message", error.what),
      fmt::arg(
          "source_location",
          error.source_location
              ? StrCat("<tr><td>Source location: ",
                       coro::ToString(*error.source_location), "</td></tr>")
              : ""),
      fmt::arg("stacktrace",
               error.stacktrace ? GetHtmlStacktrace(*error.stacktrace) : ""));
  auto length = content.size();
  return http::Response<>{
      .status = error.status.value_or(500),
      .headers = {{"Content-Type", "text/html; charset=UTF-8"},
                  {"Content-Length", std::to_string(length)}},
      .body = ToGenerator(std::move(content))};
}

}  // namespace

class AccountManagerHandler::Impl {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;

  Impl(const AbstractCloudFactory* factory,
       const ThumbnailGenerator* thumbnail_generator, const Muxer* muxer,
       AccountListener account_listener, SettingsManager* settings_manager);

  ~Impl() { Quit(); }

  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  Task<Response> operator()(Request request, coro::stdx::stop_token stop_token);

  void Quit();

 private:
  struct AuthHandler {
    Task<Response> operator()(Request request,
                              stdx::stop_token stop_token) const;

    AbstractCloudProvider::Type type;
    Impl* d;
  };

  struct OnRemoveHandler {
    Task<Response> operator()(Request request,
                              stdx::stop_token stop_token) const;
    Impl* d;
    CloudProviderAccount* account;
  };

  struct Handler {
    std::shared_ptr<CloudProviderAccount> account;
    std::string prefix;
    stdx::any_invocable<Task<http::Response<>>(http::Request<>,
                                               stdx::stop_token)>
        handler;
  };

  Task<Response> HandleRequest(Request request,
                               coro::stdx::stop_token stop_token);

  Response GetWebDAVResponse(
      std::string_view path,
      std::span<const std::pair<std::string, std::string>> headers) const;

  template <typename F>
  void RemoveCloudProvider(const F& predicate);

  void OnCloudProviderCreated(std::shared_ptr<CloudProviderAccount> account);

  CloudProviderAccount CreateAccount(
      AbstractCloudProvider::Auth::AuthToken auth_token,
      std::shared_ptr<std::optional<std::string>> username);

  Task<std::shared_ptr<CloudProviderAccount>> Create(
      AbstractCloudProvider::Auth::AuthToken auth_token,
      stdx::stop_token stop_token);

  std::unique_ptr<Handler> ChooseHandler(std::string_view path);

  Generator<std::string> GetHomePage() const;

  const AbstractCloudFactory* factory_;
  const ThumbnailGenerator* thumbnail_generator_;
  const Muxer* muxer_;
  AccountListener account_listener_;
  SettingsManager* settings_manager_;
  std::vector<std::shared_ptr<CloudProviderAccount>> accounts_;
  int64_t version_ = 0;
};

AccountManagerHandler::Impl::Impl(const AbstractCloudFactory* factory,
                                  const ThumbnailGenerator* thumbnail_generator,
                                  const Muxer* muxer,
                                  AccountListener account_listener,
                                  SettingsManager* settings_manager)
    : factory_(factory),
      thumbnail_generator_(thumbnail_generator),
      muxer_(muxer),
      account_listener_(std::move(account_listener)),
      settings_manager_(settings_manager) {
  for (auto auth_token : settings_manager_->LoadTokenData()) {
    auto id = std::move(auth_token.id);
    auto account =
        accounts_.emplace_back(std::make_shared<CloudProviderAccount>(
            CreateAccount(std::move(auth_token),
                          std::make_shared<std::optional<std::string>>(id))));
    OnCloudProviderCreated(std::move(account));
  }
}

void AccountManagerHandler::Impl::Quit() {
  while (!accounts_.empty()) {
    auto it = accounts_.begin();
    (*it)->stop_source_.request_stop();
    account_listener_.OnDestroy(*it);
    accounts_.erase(it);
  }
}

auto AccountManagerHandler::Impl::operator()(Request request,
                                             coro::stdx::stop_token stop_token)
    -> Task<Response> {
  auto response = co_await [&]() -> Task<Response> {
    try {
      co_return co_await HandleRequest(std::move(request),
                                       std::move(stop_token));
    } catch (...) {
      co_return GetErrorResponse(GetErrorMetadata());
    }
  }();
  response.headers.emplace_back("Accept-CH", "Sec-CH-Prefers-Color-Scheme");
  response.headers.emplace_back("Vary", "Sec-CH-Prefers-Color-Scheme");
  response.headers.emplace_back("Critical-CH", "Sec-CH-Prefers-Color-Scheme");
  co_return response;
}

auto AccountManagerHandler::Impl::HandleRequest(
    Request request, coro::stdx::stop_token stop_token) -> Task<Response> {
  if (request.method == coro::http::Method::kOptions) {
    co_return Response{
        .status = 204,
        .headers = {{"Allow",
                     "OPTIONS, GET, HEAD, POST, PUT, DELETE, MOVE, "
                     "MKCOL, PROPFIND, PATCH, PROPPATCH"},
                    {"DAV", "1"},
                    {"Access-Control-Allow-Origin", "*"},
                    {"Access-Control-Allow-Headers", "*"}}};
  }
  auto path = http::ParseUri(request.url).path;
  if (!path) {
    co_return Response{.status = 400};
  }
  if (auto handler = ChooseHandler(*path)) {
    if (handler->account != nullptr) {
      auto stop_token_or = MakeUniqueStopTokenOr(handler->account->stop_token(),
                                                 std::move(stop_token));
      auto response = co_await handler->handler(std::move(request),
                                                stop_token_or->GetToken());
      response.body = Forward(std::move(response.body),
                              std::move(stop_token_or), std::move(handler));
      co_return response;
    } else {
      auto response =
          co_await handler->handler(std::move(request), std::move(stop_token));
      response.body = Forward(std::move(response.body), std::move(handler));
      co_return response;
    }
  } else if (*path == "/" || *path == "") {
    co_return Response{.status = 200, .body = GetHomePage()};
  }
  if (path->starts_with("/list") && request.method == http::Method::kPropfind) {
    co_return GetWebDAVResponse(*path, request.headers);
  }
  co_return Response{.status = 302, .headers = {{"Location", "/"}}};
}

auto AccountManagerHandler::Impl::GetWebDAVResponse(
    std::string_view path,
    std::span<const std::pair<std::string, std::string>> headers) const
    -> Response {
  auto decomposed = SplitString(std::string(path), '/');
  if (decomposed.empty()) {
    return Response{.status = 404};
  }
  std::vector<std::string> responses = {
      GetElement(ElementData{.path = std::string(path),
                             .name = GetFileName(std::string(path)),
                             .is_directory = true})};
  if (coro::http::GetHeader(headers, "Depth") == "1") {
    if (decomposed.size() == 1) {
      std::set<std::string_view> account_type;
      for (const auto& account : accounts_) {
        account_type.insert(account->type());
      }
      for (std::string_view type : account_type) {
        responses.push_back(
            GetElement(ElementData{.path = StrCat("/list/", type, '/'),
                                   .name = std::string(type),
                                   .is_directory = true}));
      }
    } else if (decomposed.size() == 2) {
      std::string type = http::DecodeUri(decomposed[1]);
      for (const auto& account : accounts_) {
        if (account->type() == type) {
          responses.push_back(GetElement(
              ElementData{.path = StrCat("/list/", type, '/',
                                         http::EncodeUri(account->username())),
                          .name = std::string(type),
                          .is_directory = true}));
        }
      }
    } else {
      return Response{.status = 404};
    }
  }
  return Response{.status = 207,
                  .headers = {{"Content-Type", "text/xml"}},
                  .body = http::CreateBody(GetMultiStatusResponse(responses))};
}

auto AccountManagerHandler::Impl::ChooseHandler(std::string_view path)
    -> std::unique_ptr<Handler> {
  std::vector<Handler> handlers;
  handlers.emplace_back(
      Handler{.prefix = "/static/", .handler = StaticFileHandler{factory_}});
  handlers.emplace_back(Handler{
      .prefix = "/size",
      .handler = GetSizeHandler{
          std::span<const std::shared_ptr<CloudProviderAccount>>(accounts_)}});
  handlers.emplace_back(Handler{.prefix = "/settings",
                                .handler = SettingsHandler(settings_manager_)});
  handlers.emplace_back(
      Handler{.prefix = "/settings/theme-toggle", .handler = ThemeHandler{}});
  handlers.emplace_back(Handler{
      .prefix = "/mux/",
      .handler = MuxHandler{
          muxer_,
          std::span<const std::shared_ptr<CloudProviderAccount>>(accounts_)}});

  for (AbstractCloudProvider::Type type :
       factory_->GetSupportedCloudProviders()) {
    handlers.emplace_back(
        Handler{.prefix = util::StrCat(
                    "/auth/", http::EncodeUri(factory_->GetAuth(type).GetId())),
                .handler = AuthHandler{type, this}});
  }

  for (const auto& account : accounts_) {
    handlers.emplace_back(Handler{
        .account = account,
        .prefix = StrCat("/list/", account->type(), '/',
                         http::EncodeUri(account->username())),
        .handler = CloudProviderHandler(
            &*account->provider(), thumbnail_generator_, settings_manager_)});
    handlers.emplace_back(
        Handler{.account = account,
                .prefix = StrCat("/remove/", account->type(), '/',
                                 http::EncodeUri(account->username())),
                .handler = OnRemoveHandler{.d = this, .account = &*account}});
  }

  std::optional<Handler> best = std::nullopt;
  for (auto& handler : handlers) {
    if (path.starts_with(handler.prefix) &&
        (!best || handler.prefix.length() > best->prefix.length())) {
      best = std::move(handler);
    }
  }
  return best ? std::make_unique<Handler>(*std::move(best)) : nullptr;
}

Generator<std::string> AccountManagerHandler::Impl::GetHomePage() const {
  std::stringstream supported_providers;
  for (auto type : factory_->GetSupportedCloudProviders()) {
    supported_providers << GetAuthUrl(type, factory_);
  }
  std::stringstream content_table;
  for (const auto& account : accounts_) {
    auto provider_id = account->type();
    std::string provider_size;
    content_table << fmt::format(
        fmt::runtime(kAccountEntryHtml),
        fmt::arg("provider_icon",
                 util::StrCat("/static/", provider_id, ".png")),
        fmt::arg("provider_url",
                 util::StrCat("/list/", account->type(), "/",
                              http::EncodeUri(account->username()), "/")),
        fmt::arg("provider_name", account->username()),
        fmt::arg("provider_remove_url",
                 util::StrCat("/remove/", account->type(), "/",
                              http::EncodeUri(account->username()))),
        fmt::arg("provider_type", account->type()));
  }
  std::string content = fmt::format(
      fmt::runtime(kHomePageHtml),
      fmt::arg("supported_providers", std::move(supported_providers).str()),
      fmt::arg("content_table", std::move(content_table).str()));
  co_yield std::move(content);
}

void AccountManagerHandler::Impl::OnCloudProviderCreated(
    std::shared_ptr<CloudProviderAccount> account) {
  account_listener_.OnCreate(std::move(account));
}

template <typename F>
void AccountManagerHandler::Impl::RemoveCloudProvider(const F& predicate) {
  for (auto it = std::begin(accounts_); it != std::end(accounts_);) {
    if (predicate(*it) && !(*it)->stop_token().stop_requested()) {
      (*it)->stop_source_.request_stop();
      account_listener_.OnDestroy(*it);
      settings_manager_->RemoveToken((*it)->username(), (*it)->type());
      *(*it)->username_ = std::nullopt;
      it = accounts_.erase(it);
    } else {
      it++;
    }
  }
}

CloudProviderAccount AccountManagerHandler::Impl::CreateAccount(
    AbstractCloudProvider::Auth::AuthToken auth_token,
    std::shared_ptr<std::optional<std::string>> username) {
  return CloudProviderAccount(
      username, version_++,
      factory_->Create(std::move(auth_token),
                       OnAuthTokenChanged{settings_manager_, username}));
}

Task<std::shared_ptr<CloudProviderAccount>> AccountManagerHandler::Impl::Create(
    AbstractCloudProvider::Auth::AuthToken auth_token,
    stdx::stop_token stop_token) {
  auto username = std::make_shared<std::optional<std::string>>(std::nullopt);
  auto account = CreateAccount(auth_token, username);
  auto version = account.version_;
  auto& provider = account.provider();
  std::exception_ptr exception;
  try {
    auto general_data =
        co_await provider->GetGeneralData(std::move(stop_token));
    *username = std::move(general_data.username);
    RemoveCloudProvider(
        [version, account_id = account.id()](const auto& entry) {
          return entry->version_ < version && entry->id() == account_id;
        });
    auto d = accounts_.emplace_back(
        std::make_shared<CloudProviderAccount>(std::move(account)));
    OnCloudProviderCreated(d);
    settings_manager_->SaveToken(std::move(auth_token), **username);
    co_return d;
  } catch (...) {
    exception = std::current_exception();
  }
  RemoveCloudProvider(
      [version](const auto& entry) { return entry->version_ == version; });
  std::rethrow_exception(exception);
}

auto AccountManagerHandler::Impl::AuthHandler::operator()(
    Request request, stdx::stop_token stop_token) const -> Task<Response> {
  auto result =
      co_await d->factory_->GetAuth(type).CreateAuthHandler()->OnRequest(
          std::move(request), stop_token);
  if (std::holds_alternative<Response>(result)) {
    co_return std::move(std::get<Response>(result));
  }
  auto account = co_await d->Create(
      std::get<AbstractCloudProvider::Auth::AuthToken>(std::move(result)),
      std::move(stop_token));
  co_return Response{
      .status = 302,
      .headers = {{"Location", d->settings_manager_->GetPostAuthRedirectUri(
                                   account->type(), account->username())}}};
}

auto AccountManagerHandler::Impl::OnRemoveHandler::operator()(
    Request request, stdx::stop_token stop_token) const -> Task<Response> {
  d->RemoveCloudProvider([account_id = account->id()](const auto& account) {
    return account->id() == account_id;
  });
  co_return Response{.status = 302, .headers = {{"Location", "/"}}};
}

AccountManagerHandler::AccountManagerHandler(
    const AbstractCloudFactory* factory,
    const ThumbnailGenerator* thumbnail_generator, const Muxer* muxer,
    AccountListener account_listener, SettingsManager* settings_manager)
    : impl_(std::make_unique<Impl>(factory, thumbnail_generator, muxer,
                                   std::move(account_listener),
                                   settings_manager)) {}

AccountManagerHandler::AccountManagerHandler(AccountManagerHandler&&) noexcept =
    default;

AccountManagerHandler::~AccountManagerHandler() = default;

AccountManagerHandler& AccountManagerHandler::operator=(
    AccountManagerHandler&&) noexcept = default;

Task<http::Response<>> AccountManagerHandler::operator()(
    http::Request<> request, coro::stdx::stop_token stop_token) {
  return (*impl_)(std::move(request), std::move(stop_token));
}

void AccountManagerHandler::Quit() { impl_->Quit(); }

}  // namespace coro::cloudstorage::util
