#include "coro/cloudstorage/util/account_manager_handler.h"

#include <fmt/core.h>

#include "coro/cloudstorage/util/assets.h"
#include "coro/cloudstorage/util/exception_utils.h"
#include "coro/cloudstorage/util/generator_utils.h"
#include "coro/cloudstorage/util/get_size_handler.h"
#include "coro/cloudstorage/util/item_content_handler.h"
#include "coro/cloudstorage/util/item_thumbnail_handler.h"
#include "coro/cloudstorage/util/list_directory_handler.h"
#include "coro/cloudstorage/util/mux_handler.h"
#include "coro/cloudstorage/util/settings_handler.h"
#include "coro/cloudstorage/util/static_file_handler.h"
#include "coro/cloudstorage/util/theme_handler.h"
#include "coro/cloudstorage/util/webdav_handler.h"
#include "coro/cloudstorage/util/webdav_utils.h"
#include "coro/util/stop_token_or.h"

namespace coro::cloudstorage::util {

namespace {

using ::coro::util::MakeUniqueStopTokenOr;

struct OnAuthTokenChanged {
  void operator()(AbstractCloudProvider::Auth::AuthToken auth_token) {
    d->SaveToken(std::move(auth_token), username);
  }
  SettingsManager* d;
  std::string username;
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

template <typename... Args>
Generator<std::string> Validate(Generator<std::string> body, Args...) {
  std::optional<ErrorMetadata> error_metadata;
  try {
    FOR_CO_AWAIT(std::string & chunk, body) { co_yield std::move(chunk); }
  } catch (...) {
    error_metadata = GetErrorMetadata();
  }
  if (error_metadata) {
    std::stringstream stream;
    stream << "\n\n";
    if (error_metadata->status) {
      stream << "STATUS = " << *error_metadata->status << "\n\n";
    }
    stream << "WHAT = " << error_metadata->what << "\n\n";
    if (error_metadata->source_location) {
      stream << "SOURCE LOCATION = "
             << coro::ToString(*error_metadata->source_location) << "\n\n";
    }
    if (error_metadata->stacktrace) {
      stream << "STACKTRACE = " << coro::ToString(*error_metadata->stacktrace)
             << "\n\n";
    }
    co_yield std::move(stream).str();
  }
}

}  // namespace

class AccountManagerHandler::Impl {
 public:
  using Request = coro::http::Request<>;
  using Response = coro::http::Response<>;

  Impl(const AbstractCloudFactory* factory,
       const ThumbnailGenerator* thumbnail_generator, const Muxer* muxer,
       const Clock* clock, AccountListener account_listener,
       SettingsManager* settings_manager, CacheManager* cache_manager);

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
    CloudProviderAccount account;
  };

  struct Handler {
    std::optional<CloudProviderAccount> account;
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

  void OnCloudProviderCreated(CloudProviderAccount account);

  CloudProviderAccount CreateAccount(
      std::unique_ptr<AbstractCloudProvider> provider, std::string username,
      int64_t version);

  Task<CloudProviderAccount> Create(
      AbstractCloudProvider::Auth::AuthToken auth_token,
      stdx::stop_token stop_token);

  std::optional<Handler> ChooseHandler(std::string_view path);

  Generator<std::string> GetHomePage() const;

  const AbstractCloudFactory* factory_;
  const ThumbnailGenerator* thumbnail_generator_;
  const Muxer* muxer_;
  const Clock* clock_;
  AccountListener account_listener_;
  SettingsManager* settings_manager_;
  CacheManager* cache_manager_;
  std::vector<CloudProviderAccount> accounts_;
  int64_t version_ = 0;
};

AccountManagerHandler::Impl::Impl(const AbstractCloudFactory* factory,
                                  const ThumbnailGenerator* thumbnail_generator,
                                  const Muxer* muxer, const Clock* clock,
                                  AccountListener account_listener,
                                  SettingsManager* settings_manager,
                                  CacheManager* cache_manager)
    : factory_(factory),
      thumbnail_generator_(thumbnail_generator),
      muxer_(muxer),
      clock_(clock),
      account_listener_(std::move(account_listener)),
      settings_manager_(settings_manager),
      cache_manager_(cache_manager) {
  for (auto auth_token : settings_manager_->LoadTokenData()) {
    auto id = std::move(auth_token.id);
    OnCloudProviderCreated(accounts_.emplace_back(CreateAccount(
        factory_->Create(auth_token, OnAuthTokenChanged{settings_manager_, id}),
        id, version_++)));
  }
}

void AccountManagerHandler::Impl::Quit() {
  while (!accounts_.empty()) {
    auto it = accounts_.begin();
    it->stop_source_.request_stop();
    account_listener_.OnDestroy(std::move(*it));
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
    } catch (const CloudException& e) {
      switch (e.type()) {
        case CloudException::Type::kNotFound:
          co_return Response{.status = 404};
        case CloudException::Type::kUnauthorized:
          co_return Response{.status = 401};
        default:
          throw;
      }
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
    if (handler->account) {
      auto stop_token_or = MakeUniqueStopTokenOr(handler->account->stop_token(),
                                                 std::move(stop_token));
      auto response = co_await handler->handler(std::move(request),
                                                stop_token_or->GetToken());
      response.body = Validate(std::move(response.body),
                               std::move(stop_token_or), std::move(handler));
      co_return response;
    } else {
      auto response =
          co_await handler->handler(std::move(request), std::move(stop_token));
      response.body = Validate(std::move(response.body), std::move(handler));
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
        account_type.insert(account.type());
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
        if (account.type() == type) {
          responses.push_back(GetElement(
              ElementData{.path = StrCat("/list/", type, '/',
                                         http::EncodeUri(account.username())),
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
    -> std::optional<Handler> {
  if (path.starts_with("/static/")) {
    return Handler{.handler = StaticFileHandler{factory_}};
  } else if (path.starts_with("/size")) {
    return Handler{.handler = GetSizeHandler{
                       std::span<const CloudProviderAccount>(accounts_)}};
  } else if (path.starts_with("/settings/theme-toggle")) {
    return Handler{.handler = ThemeHandler{}};
  } else if (path.starts_with("/settings")) {
    return Handler{.handler = SettingsHandler(settings_manager_)};
  } else if (path.starts_with("/mux")) {
    return Handler{
        .handler = MuxHandler{
            muxer_, std::span<const CloudProviderAccount>(accounts_)}};
  } else {
    for (AbstractCloudProvider::Type type :
         factory_->GetSupportedCloudProviders()) {
      if (path.starts_with(StrCat("/auth/", factory_->GetAuth(type).GetId()))) {
        return Handler{.handler = AuthHandler{type, this}};
      }
    }

    for (const auto& account : accounts_) {
      auto account_path_prefix = [&](std::string_view prefix) {
        return StrCat(prefix, account.type(), '/',
                      http::EncodeUri(account.username()));
      };
      if (path.starts_with(account_path_prefix("/list/"))) {
        return Handler{
            .account = account,
            .handler = ListDirectoryHandler(
                account,
                [account_id = account.id()](std::string_view item_id) {
                  return StrCat("/list/", account_id.type, '/',
                                http::EncodeUri(account_id.username), '/',
                                http::EncodeUri(item_id));
                },
                [account_id = account.id()](std::string_view item_id) {
                  return StrCat("/thumbnail/", account_id.type, '/',
                                http::EncodeUri(account_id.username), '/',
                                http::EncodeUri(item_id));
                },
                [account_id = account.id()](std::string_view item_id) {
                  return StrCat("/content/", account_id.type, '/',
                                http::EncodeUri(account_id.username), '/',
                                http::EncodeUri(item_id));
                })};
      } else if (path.starts_with(account_path_prefix("/webdav/"))) {
        return Handler{.account = account,
                       .handler = WebDAVHandler(account.provider().get())};
      } else if (path.starts_with(account_path_prefix("/thumbnail/"))) {
        return Handler{.account = account,
                       .handler = ItemThumbnailHandler(account)};
      } else if (path.starts_with(account_path_prefix("/content/"))) {
        return Handler{.account = account,
                       .handler = ItemContentHandler{account}};
      } else if (path.starts_with(account_path_prefix("/remove/"))) {
        return Handler{
            .account = account,
            .handler = OnRemoveHandler{.d = this, .account = account}};
      }
    }
  }
  return std::nullopt;
}

Generator<std::string> AccountManagerHandler::Impl::GetHomePage() const {
  std::stringstream supported_providers;
  for (auto type : factory_->GetSupportedCloudProviders()) {
    supported_providers << GetAuthUrl(type, factory_);
  }
  std::stringstream content_table;
  for (const auto& account : accounts_) {
    auto provider_id = account.type();
    std::string provider_size;
    content_table << fmt::format(
        fmt::runtime(kAccountEntryHtml),
        fmt::arg("provider_icon",
                 util::StrCat("/static/", provider_id, ".png")),
        fmt::arg("provider_url",
                 util::StrCat("/list/", account.type(), "/",
                              http::EncodeUri(account.username()), "/")),
        fmt::arg("provider_name", account.username()),
        fmt::arg("provider_remove_url",
                 util::StrCat("/remove/", account.type(), "/",
                              http::EncodeUri(account.username()))),
        fmt::arg("provider_type", account.type()));
  }
  std::string content = fmt::format(
      fmt::runtime(kHomePageHtml),
      fmt::arg("supported_providers", std::move(supported_providers).str()),
      fmt::arg("content_table", std::move(content_table).str()));
  co_yield std::move(content);
}

void AccountManagerHandler::Impl::OnCloudProviderCreated(
    CloudProviderAccount account) {
  account_listener_.OnCreate(std::move(account));
}

template <typename F>
void AccountManagerHandler::Impl::RemoveCloudProvider(const F& predicate) {
  for (auto it = std::begin(accounts_); it != std::end(accounts_);) {
    if (predicate(*it) && !it->stop_token().stop_requested()) {
      it->stop_source_.request_stop();
      account_listener_.OnDestroy(*it);
      settings_manager_->RemoveToken(it->username(), it->type());
      it = accounts_.erase(it);
    } else {
      it++;
    }
  }
}

CloudProviderAccount AccountManagerHandler::Impl::CreateAccount(
    std::unique_ptr<AbstractCloudProvider> provider, std::string username,
    int64_t version) {
  return {std::move(username), version, std::move(provider),
          cache_manager_,      clock_,  thumbnail_generator_};
}

Task<CloudProviderAccount> AccountManagerHandler::Impl::Create(
    AbstractCloudProvider::Auth::AuthToken auth_token,
    stdx::stop_token stop_token) {
  int64_t version = ++version_;
  auto provider = factory_->Create(auth_token, [](const auto&) {});
  auto general_data = co_await provider->GetGeneralData(std::move(stop_token));
  RemoveCloudProvider([&](const auto& entry) {
    return entry.version_ < version &&
           entry.id() ==
               CloudProviderAccount::Id{.type = std::string(provider->GetId()),
                                        .username = general_data.username};
  });
  provider = factory_->Create(
      auth_token, OnAuthTokenChanged{settings_manager_, general_data.username});
  auto d = accounts_.emplace_back(
      CreateAccount(std::move(provider), general_data.username, version));
  settings_manager_->SaveToken(std::move(auth_token), general_data.username);
  OnCloudProviderCreated(d);
  co_return d;
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
                                   account.type(), account.username())}}};
}

auto AccountManagerHandler::Impl::OnRemoveHandler::operator()(
    Request request, stdx::stop_token stop_token) const -> Task<Response> {
  d->RemoveCloudProvider([account_id = account.id()](const auto& account) {
    return account.id() == account_id;
  });
  co_return Response{.status = 302, .headers = {{"Location", "/"}}};
}

AccountManagerHandler::AccountManagerHandler(
    const AbstractCloudFactory* factory,
    const ThumbnailGenerator* thumbnail_generator, const Muxer* muxer,
    const Clock* clock, AccountListener account_listener,
    SettingsManager* settings_manager, CacheManager* cache_manager)
    : impl_(std::make_unique<Impl>(factory, thumbnail_generator, muxer, clock,
                                   std::move(account_listener),
                                   settings_manager, cache_manager)) {}

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
