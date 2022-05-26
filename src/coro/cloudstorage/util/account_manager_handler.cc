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
  append_auth_urls_(factory_, supported_providers);
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

}  // namespace coro::cloudstorage::util