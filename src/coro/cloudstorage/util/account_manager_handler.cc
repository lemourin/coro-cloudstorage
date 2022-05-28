#include "coro/cloudstorage/util/account_manager_handler.h"

namespace coro::cloudstorage::util {

namespace {

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
  std::string id(factory->CreateAuth(type)->GetId());
  std::string url = factory->CreateAuth(type)->GetAuthorizationUrl().value_or(
      util::StrCat("/auth/", id));
  stream << fmt::format(
      fmt::runtime(kAssetsHtmlProviderEntryHtml), fmt::arg("provider_url", url),
      fmt::arg("image_url", util::StrCat("/static/", id, ".png")));
}

}  // namespace

Task<> AccountManagerHandler::Quit() {
  std::vector<Task<>> tasks;
  for (auto& account : accounts_) {
    if (!account.stop_token().stop_requested()) {
      account.stop_source_.request_stop();
      tasks.emplace_back(account_listener_->OnDestroy(&account));
    }
  }
  co_await WhenAll(std::move(tasks));
  accounts_.clear();
}

auto AccountManagerHandler::operator()(Request request,
                                       coro::stdx::stop_token stop_token)
    -> Task<Response> {
  auto response =
      co_await HandleRequest(std::move(request), std::move(stop_token));
  response.headers.emplace_back("Accept-CH", "Sec-CH-Prefers-Color-Scheme");
  response.headers.emplace_back("Vary", "Sec-CH-Prefers-Color-Scheme");
  response.headers.emplace_back("Critical-CH", "Sec-CH-Prefers-Color-Scheme");
  co_return response;
}

auto AccountManagerHandler::HandleRequest(Request request,
                                          coro::stdx::stop_token stop_token)
    -> Task<Response> {
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
  auto path_opt = http::ParseUri(request.url).path;
  if (!path_opt) {
    co_return Response{.status = 400};
  }
  auto path = http::DecodeUri(std::move(*path_opt));
  if (auto* handler = ChooseHandler(path)) {
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
  if (path.empty() || path == "/") {
    if (request.method == coro::http::Method::kPropfind) {
      co_return GetWebDAVRootResponse(request.headers);
    } else {
      co_return Response{.status = 200, .body = GetHomePage()};
    }
  } else {
    co_return Response{.status = 302, .headers = {{"Location", "/"}}};
  }
}

auto AccountManagerHandler::GetWebDAVRootResponse(
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

void AccountManagerHandler::RemoveHandler(std::string_view account_id) {
  for (auto it = std::begin(handlers_); it != std::end(handlers_);) {
    if (it->id == account_id) {
      it = handlers_.erase(it);
    } else {
      it++;
    }
  }
}

auto AccountManagerHandler::ChooseHandler(std::string_view path) -> Handler* {
  Handler* best = nullptr;
  for (auto& handler : handlers_) {
    if (path.starts_with(handler.prefix) &&
        (!best || handler.prefix.length() > best->prefix.length())) {
      best = &handler;
    }
  }
  return best;
}

Generator<std::string> AccountManagerHandler::GetHomePage() const {
  std::stringstream supported_providers;
  for (auto type : factory2_->GetSupportedCloudProviders()) {
    AppendAuthUrl(type, factory2_, supported_providers);
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
                 util::StrCat("/", http::EncodeUri(account.id()), "/")),
        fmt::arg("provider_name", account.username()),
        fmt::arg("provider_remove_url",
                 util::StrCat("/remove/", http::EncodeUri(account.id()))),
        fmt::arg("provider_id", http::EncodeUri(account.id())));
  }
  co_yield fmt::format(
      fmt::runtime(kAssetsHtmlHomePageHtml),
      fmt::arg("supported_providers", std::move(supported_providers).str()),
      fmt::arg("content_table", std::move(content_table).str()));
}

void AccountManagerHandler::OnCloudProviderCreated(
    CloudProviderAccount* account) {
  std::string account_id = std::string(account->id());
  try {
    handlers_.emplace_back(Handler{
        .id = std::string(account_id),
        .prefix = StrCat("/remove/", account_id),
        .handler = OnRemoveHandler{.d = this, .account_id = account_id}});

    auto& provider = account->provider();
    handlers_.emplace_back(
        Handler{.id = std::string(account_id),
                .prefix = StrCat("/", account_id),
                .handler = CloudProviderHandler(&provider, thumbnail_generator_,
                                                &settings_manager_)});

    account_listener_->OnCreate(account);
  } catch (...) {
    RemoveHandler(account_id);
    throw;
  }
}

template <typename F>
Task<> AccountManagerHandler::RemoveCloudProvider(const F& predicate) {
  for (auto it = std::begin(accounts_); it != std::end(accounts_);) {
    if (predicate(*it) && !it->stop_token().stop_requested()) {
      it->stop_source_.request_stop();
      co_await account_listener_->OnDestroy(&*it);
      settings_manager_.RemoveToken(it->username(), it->type());
      RemoveHandler(it->id());
      it = accounts_.erase(it);
    } else {
      it++;
    }
  }
}

CloudProviderAccount* AccountManagerHandler::CreateAccount2(
    AbstractCloudProvider::Auth::AuthToken auth_token,
    std::shared_ptr<std::optional<std::string>> username) {
  return &accounts_.emplace_back(
      username->value_or(""), version_++,
      factory2_->Create(
          std::move(auth_token),
          internal::OnAuthTokenChanged2{&settings_manager_, username}));
}

Task<CloudProviderAccount*> AccountManagerHandler::Create2(
    AbstractCloudProvider::Auth::AuthToken auth_token,
    stdx::stop_token stop_token) {
  auto username = std::make_shared<std::optional<std::string>>(std::nullopt);
  auto* account = CreateAccount2(auth_token, username);
  auto version = account->version_;
  auto& provider = account->provider();
  bool on_create_called = false;
  std::exception_ptr exception;
  try {
    auto general_data = co_await provider.GetGeneralData(std::move(stop_token));
    *username = std::move(general_data.username);
    account->username_ = **username;
    co_await RemoveCloudProvider([&](const auto& entry) {
      return entry.version_ < version &&
             entry.id() == GetAccountId(provider.GetId(), **username);
    });
    for (const auto& entry : accounts_) {
      if (entry.version_ == version) {
        OnCloudProviderCreated(account);
        on_create_called = true;
        settings_manager_.SaveToken2(std::move(auth_token), **username);
        break;
      }
    }
    co_return account;
  } catch (...) {
    exception = std::current_exception();
  }
  co_await RemoveCloudProvider(
      [&](const auto& entry) { return entry.version_ == version; });
  std::rethrow_exception(exception);
}

auto AccountManagerHandler::OnRemoveHandler::operator()(
    Request request, stdx::stop_token stop_token) const -> Task<Response> {
  co_await d->RemoveCloudProvider(
      [&](const auto& account) { return account.id() == account_id; });
  co_return Response{.status = 302, .headers = {{"Location", "/"}}};
}

}  // namespace coro::cloudstorage::util