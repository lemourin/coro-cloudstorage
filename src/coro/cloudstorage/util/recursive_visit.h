#ifndef CORO_CLOUDSTORAGE_RECURSIVE_VISIT_H
#define CORO_CLOUDSTORAGE_RECURSIVE_VISIT_H

#include <vector>

#include "coro/cloudstorage/cloud_provider.h"
#include "coro/when_all.h"

namespace coro::cloudstorage::util {

template <typename CloudProvider, typename Item, typename F>
Task<> RecursiveVisit(CloudProvider* provider, Item item, const F& func,
                      stdx::stop_token stop_token) {
  if constexpr (IsDirectory<Item, CloudProvider>) {
    std::vector<Task<>> tasks;
    FOR_CO_AWAIT(auto& page, provider->ListDirectory(item, stop_token)) {
      for (const auto& entry : page.items) {
        tasks.emplace_back(std::visit(
            [&](auto& entry) {
              return RecursiveVisit(provider, entry, func, stop_token);
            },
            entry));
      }
    }
    tasks.emplace_back(func(item));
    co_await WhenAll(std::move(tasks));
  } else {
    co_await func(item);
  }
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_RECURSIVE_VISIT_H