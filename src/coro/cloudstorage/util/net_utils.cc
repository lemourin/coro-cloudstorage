#include "coro/cloudstorage/util/net_utils.h"

#ifdef HAVE_IFADDRS_H
#include <ifaddrs.h>
#include <netinet/in.h>
#endif

#ifdef WIN32
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
// clang-format on
#endif

#include <event2/event.h>

#include "coro/http/http.h"
#include "coro/util/raii_utils.h"

namespace coro::cloudstorage::util {

using ::coro::util::AtScopeExit;

std::vector<std::string> GetHostAddresses() {
#if defined(HAVE_IFADDRS_H)
  ifaddrs* addrs;
  if (int err = getifaddrs(&addrs); err != 0) {
    throw http::HttpException(err, "getifaddrs");
  }
  auto scope_guard = AtScopeExit([&] { freeifaddrs(addrs); });
  std::vector<std::string> result;
  ifaddrs* current = addrs;

  while (current) {
    if (current->ifa_addr && current->ifa_addr->sa_family == AF_INET) {
      std::array<char, INET_ADDRSTRLEN> buffer = {};
      const auto* address = reinterpret_cast<sockaddr_in*>(current->ifa_addr);
      if (evutil_inet_ntop(AF_INET, &address->sin_addr, buffer.data(),
                           sizeof(buffer)) == nullptr) {
        throw http::HttpException(errno, "inet_ntop");
      }
      result.emplace_back(buffer.data());
    }
    current = current->ifa_next;
  }
  return result;
#elif defined(WIN32)
  ULONG size = 15000;
  std::vector<char> buffer;
  ULONG status;
  int retry = 0;
  do {
    buffer.resize(size);
    status = GetAdaptersAddresses(
        AF_INET,
        GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
            GAA_FLAG_SKIP_FRIENDLY_NAME,
        nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
  } while (status == ERROR_BUFFER_OVERFLOW && retry++ < 3);
  if (status == ERROR_NO_DATA || status == ERROR_ADDRESS_NOT_ASSOCIATED) {
    return {};
  }
  if (status != S_OK) {
    throw http::HttpException(status, "GetAdaptersAddresses");
  }

  std::vector<std::string> result;
  const auto* adapter_address =
      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
  while (adapter_address != nullptr) {
    for (const auto* addr = adapter_address->FirstUnicastAddress; addr;
         addr = addr->Next) {
      char buffer[INET_ADDRSTRLEN] = {};
      const auto* address =
          reinterpret_cast<sockaddr_in*>(addr->Address.lpSockaddr);
      if (evutil_inet_ntop(AF_INET, &address->sin_addr, buffer,
                           sizeof(buffer)) == nullptr) {
        throw http::HttpException(errno, "inet_ntop");
      }
      std::string entry = buffer;
      if (!entry.starts_with("169.254")) {
        result.emplace_back(std::move(entry));
      }
    }
    adapter_address = adapter_address->Next;
  }

  return result;
#else
#error "GetHostAddresses unavailable."
#endif
}

}  // namespace coro::cloudstorage::util