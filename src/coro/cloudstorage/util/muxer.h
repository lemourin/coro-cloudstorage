#ifndef CORO_CLOUDSTORAGE_FUSE_MUXER_H
#define CORO_CLOUDSTORAGE_FUSE_MUXER_H

#include "coro/cloudstorage/util/abstract_cloud_provider.h"
#include "coro/util/thread_pool.h"

namespace coro::cloudstorage::util {

enum class MediaContainer { kMp4, kWebm };

class Muxer {
 public:
  Muxer(const coro::util::EventLoop* event_loop,
        coro::util::ThreadPool* thread_pool)
      : event_loop_(event_loop), thread_pool_(thread_pool) {}

  Generator<std::string> operator()(
      AbstractCloudProvider::CloudProvider* video_cloud_provider,
      AbstractCloudProvider::File video_track,
      AbstractCloudProvider::CloudProvider* audio_cloud_provider,
      AbstractCloudProvider::File audio_track, MediaContainer container,
      stdx::stop_token stop_token) const;

 private:
  template <typename F1, typename F2>
  auto InParallel(F1&& f1, F2&& f2) const
      -> std::tuple<decltype(f1()), decltype(f2())>;

  const coro::util::EventLoop* event_loop_;
  coro::util::ThreadPool* thread_pool_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_MUXER_H
