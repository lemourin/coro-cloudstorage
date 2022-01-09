#include "coro/cloudstorage/util/net_utils.h"

#ifdef HAVE_IFADDRS_H
#include <ifaddrs.h>
#include <netinet/in.h>
#endif

#include <event2/event.h>

#include "coro/http/http.h"
#include "coro/util/raii_utils.h"

namespace coro::cloudstorage::util {

using ::coro::util::AtScopeExit;

std::vector<std::string> GetHostAddresses() {
#ifdef HAVE_IFADDRS_H
  ifaddrs* addrs;
  if (int err = getifaddrs(&addrs); err != 0) {
    throw http::HttpException(err, "getifaddrs");
  }
  auto scope_guard = AtScopeExit([&] { freeifaddrs(addrs); });
  std::vector<std::string> result;
  ifaddrs* current = addrs;

  while (current) {
    if (current->ifa_addr && current->ifa_addr->sa_family == AF_INET) {
      char buffer[INET_ADDRSTRLEN] = {};
      auto address = reinterpret_cast<sockaddr_in*>(current->ifa_addr);
      if (evutil_inet_ntop(AF_INET, &address->sin_addr, buffer,
                           sizeof(buffer)) == nullptr) {
        throw http::HttpException(errno, "inet_ntop");
      }
      result.emplace_back(buffer);
    }
    current = current->ifa_next;
  }
  return result;
#else
  return {};
#endif
}

}  // namespace coro::cloudstorage::util