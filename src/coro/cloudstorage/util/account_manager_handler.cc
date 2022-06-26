#include "coro/cloudstorage/util/account_manager_handler.h"

#include <fmt/core.h>

#include <list>

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/cloud_provider_handler.h"
#include "coro/cloudstorage/util/get_size_handler.h"
#include "coro/cloudstorage/util/settings_handler.h"
#include "coro/cloudstorage/util/static_file_handler.h"
#include "coro/cloudstorage/util/theme_handler.h"
#include "coro/cloudstorage/util/webdav_utils.h"
#include "coro/util/stop_token_or.h"

namespace coro::cloudstorage::util {

namespace {

struct OnAuthTokenChanged {
  void operator()(AbstractCloudProvider::Auth::AuthToken auth_token) {
    if (*account_id) {
      d->SaveToken(std::move(auth_token), **account_id);
    }
  }
  SettingsManager* d;
  std::shared_ptr<std::optional<std::string>> account_id;
};

Generator<std::string> GetResponse(Generator<std::string> body,
                                   stdx::stop_source stop_source,
                                   stdx::stop_token account_token,
                                   stdx::stop_token request_token) {
  coro::util::StopTokenOr stop_token_or(std::move(stop_source),
                                        std::move(account_token),
                                        std::move(request_token));
  FOR_CO_AWAIT(std::string & chunk, body) { co_yield std::move(chunk); }
}

void AppendAuthUrl(AbstractCloudProvider::Type type,
                   const AbstractCloudFactory* factory,
                   std::stringstream& stream) {
  std::string id(factory->GetAuth(type).GetId());
  std::string url = factory->GetAuth(type).GetAuthorizationUrl().value_or(
      util::StrCat("/auth/", id));
  stream << fmt::format(
      fmt::runtime(kAssetsHtmlProviderEntryHtml), fmt::arg("provider_url", url),
      fmt::arg("image_url", util::StrCat("/static/", id, ".png")));
}

}  // namespace

class AccountManagerHandler::Impl {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;

  Impl(const AbstractCloudFactory* factory,
       const ThumbnailGenerator* thumbnail_generator,
       AccountListener account_listener, SettingsManager settings_manager);

  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  Task<Response> operator()(Request request, coro::stdx::stop_token stop_token);

  Task<> Quit();

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
    std::string account_id;
  };

  struct Handler {
    std::string id;
    std::string prefix;
    stdx::any_invocable<Task<http::Response<>>(http::Request<>,
                                               stdx::stop_token)>
        handler;
  };

  Task<Response> HandleRequest(Request request,
                               coro::stdx::stop_token stop_token);

  Response GetWebDAVRootResponse(
      std::span<const std::pair<std::string, std::string>> headers) const;

  void RemoveHandler(std::string_view account_id);

  template <typename F>
  Task<> RemoveCloudProvider(const F& predicate);

  void OnCloudProviderCreated(CloudProviderAccount* account);

  CloudProviderAccount CreateAccount(
      AbstractCloudProvider::Auth::AuthToken auth_token,
      std::shared_ptr<std::optional<std::string>> username);

  Task<CloudProviderAccount*> Create(
      AbstractCloudProvider::Auth::AuthToken auth_token,
      stdx::stop_token stop_token);

  Handler* ChooseHandler(std::string_view path);

  Generator<std::string> GetHomePage() const;

  const AbstractCloudFactory* factory_;
  const ThumbnailGenerator* thumbnail_generator_;
  std::vector<Handler> handlers_;
  AccountListener account_listener_;
  SettingsManager settings_manager_;
  std::list<CloudProviderAccount> accounts_;
  int64_t version_ = 0;
};

AccountManagerHandler::Impl::Impl(const AbstractCloudFactory* factory,
                                  const ThumbnailGenerator* thumbnail_generator,
                                  AccountListener account_listener,
                                  SettingsManager settings_manager)
    : factory_(factory),
      thumbnail_generator_(thumbnail_generator),
      account_listener_(std::move(account_listener)),
      settings_manager_(std::move(settings_manager)) {
  handlers_.emplace_back(
      Handler{.prefix = "/static/", .handler = StaticFileHandler{factory_}});
  handlers_.emplace_back(
      Handler{.prefix = "/size", .handler = GetSizeHandler{&accounts_}});
  handlers_.emplace_back(Handler{
      .prefix = "/settings", .handler = SettingsHandler(&settings_manager_)});
  handlers_.emplace_back(
      Handler{.prefix = "/settings/theme-toggle", .handler = ThemeHandler{}});

  for (AbstractCloudProvider::Type type :
       factory->GetSupportedCloudProviders()) {
    handlers_.emplace_back(
        Handler{.prefix = util::StrCat(
                    "/auth/", http::EncodeUri(factory->GetAuth(type).GetId())),
                .handler = AuthHandler{type, this}});
  }

  for (auto auth_token : settings_manager_.LoadTokenData()) {
    auto id = std::move(auth_token.id);
    auto& account = accounts_.emplace_back(
        CreateAccount(std::move(auth_token),
                      std::make_shared<std::optional<std::string>>(id)));
    OnCloudProviderCreated(&account);
  }
}

Task<> AccountManagerHandler::Impl::Quit() {
  std::vector<Task<>> tasks;
  for (auto& account : accounts_) {
    if (!account.stop_token().stop_requested()) {
      account.stop_source_.request_stop();
      tasks.emplace_back(account_listener_.OnDestroy(&account));
    }
  }
  co_await WhenAll(std::move(tasks));
  accounts_.clear();
}

auto AccountManagerHandler::Impl::operator()(Request request,
                                             coro::stdx::stop_token stop_token)
    -> Task<Response> {
  auto response =
      co_await HandleRequest(std::move(request), std::move(stop_token));
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
  if ((*path == "/list/" || *path == "/list") &&
      request.method == http::Method::kPropfind) {
    co_return GetWebDAVRootResponse(request.headers);
  }
  if (*path == "/" || *path == "") {
    co_return Response{.status = 200, .body = GetHomePage()};
  }
  if (auto* handler = ChooseHandler(*path)) {
    if (auto account_it = std::find_if(
            accounts_.begin(), accounts_.end(),
            [&](const auto& account) { return account.id() == handler->id; });
        account_it != accounts_.end()) {
      stdx::stop_source stop_source;
      stdx::stop_token account_token = account_it->stop_token();
      coro::util::StopTokenOr stop_token_or(stop_source, account_token,
                                            stop_token);
      auto response = co_await handler->handler(std::move(request),
                                                stop_token_or.GetToken());
      co_return Response{
          .status = response.status,
          .headers = std::move(response.headers),
          .body = GetResponse(std::move(response.body), std::move(stop_source),
                              std::move(account_token), std::move(stop_token))};
    } else {
      co_return co_await handler->handler(std::move(request),
                                          std::move(stop_token));
    }
  }
  co_return Response{.status = 302, .headers = {{"Location", "/"}}};
}

auto AccountManagerHandler::Impl::GetWebDAVRootResponse(
    std::span<const std::pair<std::string, std::string>> headers) const
    -> Response {
  std::vector<std::string> responses = {GetElement(
      ElementData{.path = "/", .name = "root", .is_directory = true})};
  if (coro::http::GetHeader(headers, "Depth") == "1") {
    for (const auto& account : accounts_) {
      responses.push_back(
          GetElement(ElementData{.path = StrCat("/", account.id(), "/"),
                                 .name = std::string(account.id()),
                                 .is_directory = true}));
    }
  }
  return Response{.status = 207,
                  .headers = {{"Content-Type", "text/xml"}},
                  .body = http::CreateBody(GetMultiStatusResponse(responses))};
}

void AccountManagerHandler::Impl::RemoveHandler(std::string_view account_id) {
  for (auto it = std::begin(handlers_); it != std::end(handlers_);) {
    if (it->id == account_id) {
      it = handlers_.erase(it);
    } else {
      it++;
    }
  }
}

auto AccountManagerHandler::Impl::ChooseHandler(std::string_view path)
    -> Handler* {
  Handler* best = nullptr;
  for (auto& handler : handlers_) {
    if (path.starts_with(handler.prefix) &&
        (!best || handler.prefix.length() > best->prefix.length())) {
      best = &handler;
    }
  }
  return best;
}

Generator<std::string> AccountManagerHandler::Impl::GetHomePage() const {
  std::stringstream supported_providers;
  for (auto type : factory_->GetSupportedCloudProviders()) {
    AppendAuthUrl(type, factory_, supported_providers);
  }
  std::stringstream content_table;
  for (const auto& account : accounts_) {
    auto provider_id = account.type();
    std::string provider_size;
    content_table << fmt::format(
        fmt::runtime(kAssetsHtmlAccountEntryHtml),
        fmt::arg("provider_icon",
                 util::StrCat("/static/", provider_id, ".png")),
        fmt::arg("provider_url",
                 util::StrCat("/list/", http::EncodeUri(account.id()), "/")),
        fmt::arg("provider_name", account.username()),
        fmt::arg("provider_remove_url",
                 util::StrCat("/remove/", http::EncodeUri(account.id()))),
        fmt::arg("provider_id", http::EncodeUri(account.id())));
  }
  std::string content = fmt::format(
      fmt::runtime(kAssetsHtmlHomePageHtml),
      fmt::arg("supported_providers", std::move(supported_providers).str()),
      fmt::arg("content_table", std::move(content_table).str()));
  co_yield std::move(content);
}

void AccountManagerHandler::Impl::OnCloudProviderCreated(
    CloudProviderAccount* account) {
  std::string account_id = std::string(account->id());
  try {
    handlers_.emplace_back(Handler{
        .id = std::string(account_id),
        .prefix = StrCat("/remove/", http::EncodeUri(account_id)),
        .handler = OnRemoveHandler{.d = this, .account_id = account_id}});

    auto& provider = account->provider();
    handlers_.emplace_back(
        Handler{.id = std::string(account_id),
                .prefix = StrCat("/list/", http::EncodeUri(account_id)),
                .handler = CloudProviderHandler(&provider, thumbnail_generator_,
                                                &settings_manager_)});
    account_listener_.OnCreate(account);
  } catch (...) {
    RemoveHandler(account_id);
    throw;
  }
}

template <typename F>
Task<> AccountManagerHandler::Impl::RemoveCloudProvider(const F& predicate) {
  for (auto it = std::begin(accounts_); it != std::end(accounts_);) {
    if (predicate(*it) && !it->stop_token().stop_requested()) {
      it->stop_source_.request_stop();
      co_await account_listener_.OnDestroy(&*it);
      settings_manager_.RemoveToken(it->username(), it->type());
      RemoveHandler(it->id());
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
      username->value_or(""), version_++,
      factory_->Create(std::move(auth_token),
                       OnAuthTokenChanged{&settings_manager_, username}));
}

Task<CloudProviderAccount*> AccountManagerHandler::Impl::Create(
    AbstractCloudProvider::Auth::AuthToken auth_token,
    stdx::stop_token stop_token) {
  auto username = std::make_shared<std::optional<std::string>>(std::nullopt);
  auto account = CreateAccount(auth_token, username);
  auto version = account.version_;
  auto& provider = account.provider();
  std::exception_ptr exception;
  try {
    auto general_data = co_await provider.GetGeneralData(std::move(stop_token));
    *username = std::move(general_data.username);
    account.username_ = **username;
    account.id_ = GetAccountId(account.type(), account.username());
    co_await RemoveCloudProvider([&](const auto& entry) {
      return entry.version_ < version && entry.id() == account.id();
    });
    auto& d = accounts_.emplace_back(std::move(account));
    OnCloudProviderCreated(&d);
    settings_manager_.SaveToken(std::move(auth_token), **username);
    co_return &d;
  } catch (...) {
    exception = std::current_exception();
  }
  co_await RemoveCloudProvider(
      [&](const auto& entry) { return entry.version_ == version; });
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
  auto* account = co_await d->Create(
      std::get<AbstractCloudProvider::Auth::AuthToken>(std::move(result)),
      std::move(stop_token));
  co_return Response{
      .status = 302,
      .headers = {{"Location",
                   util::StrCat("/list/", http::EncodeUri(account->id()))}}};
}

auto AccountManagerHandler::Impl::OnRemoveHandler::operator()(
    Request request, stdx::stop_token stop_token) const -> Task<Response> {
  co_await d->RemoveCloudProvider(
      [&](const auto& account) { return account.id() == account_id; });
  co_return Response{.status = 302, .headers = {{"Location", "/"}}};
}

AccountManagerHandler::AccountManagerHandler(
    const AbstractCloudFactory* factory,
    const ThumbnailGenerator* thumbnail_generator,
    AccountListener account_listener, SettingsManager settings_manager)
    : impl_(std::make_unique<Impl>(factory, thumbnail_generator,
                                   std::move(account_listener),
                                   std::move(settings_manager))) {}

AccountManagerHandler::AccountManagerHandler(AccountManagerHandler&&) noexcept =
    default;

AccountManagerHandler::~AccountManagerHandler() = default;

AccountManagerHandler& AccountManagerHandler::operator=(
    AccountManagerHandler&&) noexcept = default;

Task<http::Response<>> AccountManagerHandler::operator()(
    http::Request<> request, coro::stdx::stop_token stop_token) {
  return (*impl_)(std::move(request), std::move(stop_token));
}

Task<> AccountManagerHandler::Quit() { return impl_->Quit(); }

}  // namespace coro::cloudstorage::util