#ifndef CORO_CLOUDSTORAGE_UTIL_AVIO_CONTEXT_H
#define CORO_CLOUDSTORAGE_UTIL_AVIO_CONTEXT_H

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/util/event_loop.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace coro::cloudstorage::util {

struct AVIOContextDeleter {
  void operator()(AVIOContext* context);
};

std::unique_ptr<AVIOContext, AVIOContextDeleter> CreateIOContext(
    const coro::util::EventLoop* event_loop,
    const AbstractCloudProvider* provider, AbstractCloudProvider::File file,
    stdx::stop_token stop_token);

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_UTIL_AVIO_CONTEXT_H
