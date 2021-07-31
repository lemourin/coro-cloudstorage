#ifndef CORO_CLOUDSTORAGE_FUSE_TIMING_OUT_STOP_TOKEN_H
#define CORO_CLOUDSTORAGE_FUSE_TIMING_OUT_STOP_TOKEN_H

#include <string>

#include "coro/stdx/stop_token.h"
#include "coro/util/event_loop.h"

namespace coro::cloudstorage::util {

class TimingOutStopToken {
 public:
  TimingOutStopToken(const coro::util::EventLoop& event_loop,
                     std::string action, int timeout_ms);
  ~TimingOutStopToken();

  stdx::stop_token GetToken() const { return stop_source_.get_token(); }

 private:
  stdx::stop_source stop_source_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_TIMING_OUT_STOP_TOKEN_H
