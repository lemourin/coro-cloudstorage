#include "coro/cloudstorage/util/cloud_provider_handler.h"

#include "coro/cloudstorage/util/net_utils.h"

namespace coro::cloudstorage::util {

std::string GetItemPathPrefix(
    std::span<const std::pair<std::string, std::string>> headers) {
  namespace re = coro::util::re;
  std::optional<std::vector<std::string>> host_addresses;
  auto host = [&]() -> std::string {
    auto host = http::GetCookie(headers, "host");
    if (!host || host->empty()) {
      return "";
    }
    host_addresses = GetHostAddresses();
    if (std::find(host_addresses->begin(), host_addresses->end(), *host) ==
        host_addresses->end()) {
      return "";
    }
    return *host;
  }();
  if (host.empty()) {
    if (!host_addresses) {
      host_addresses = GetHostAddresses();
    }
    std::optional<int> idx;
    bool ambiguous = false;
    for (size_t i = 0; i < host_addresses->size(); i++) {
      if ((*host_addresses)[i] != "127.0.0.1") {
        if (idx) {
          ambiguous = true;
          break;
        }
        idx = i;
      }
    }
    if (!ambiguous && idx) {
      host = std::move((*host_addresses)[*idx]);
    }
  }
  if (host.empty()) {
    return "";
  }
  auto port = [&]() -> std::string_view {
    re::regex regex(R"((\:\d{1,5})$)");
    re::match_results<std::string::const_iterator> match;
    std::string_view port = "";
    if (re::regex_search(http::GetHeader(headers, "host").value(), match,
                         regex)) {
      return std::string_view(&*match[1].begin(), match[1].length());
    } else {
      return "";
    }
  }();
  return StrCat("http://", host, port);
}

}  // namespace coro::cloudstorage::util