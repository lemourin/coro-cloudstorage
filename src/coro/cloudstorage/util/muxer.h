#ifndef CORO_CLOUDSTORAGE_FUSE_MUXER_H
#define CORO_CLOUDSTORAGE_FUSE_MUXER_H

#include "coro/cloudstorage/util/avio_context.h"
#include "coro/cloudstorage/util/ffmpeg_utils.h"
#include "coro/cloudstorage/util/file_utils.h"
#include "coro/generator.h"
#include "coro/util/event_loop.h"
#include "coro/util/thread_pool.h"
#include "coro/when_all.h"

namespace coro::cloudstorage::util {

enum class MediaContainer { kMp4, kWebm };

class MuxerContext {
 public:
  MuxerContext(coro::util::ThreadPool* thread_pool, AVIOContext* video,
               AVIOContext* audio, MediaContainer container);

  Generator<std::string> GetContent();

 private:
  struct AVIOContextDeleter {
    void operator()(AVIOContext* context) {
      av_free(context->buffer);
      avio_context_free(&context);
    }
  };
  struct AVFormatWriteContextDeleter {
    void operator()(AVFormatContext* context) {
      avformat_free_context(context);
    }
  };

  struct Stream {
    std::unique_ptr<AVFormatContext, AVFormatContextDeleter> format_context;
    std::unique_ptr<AVCodecContext, AVCodecContextDeleter> codec_context;
    int source_stream_index;
    AVStream* stream;
    std::unique_ptr<AVPacket, AVPacketDeleter> packet;
    bool is_eof;
  };

  Stream CreateStream(AVIOContext* io_context, AVMediaType type) const;

  coro::util::ThreadPool* thread_pool_;
  std::unique_ptr<std::FILE, FileDeleter> file_;
  std::unique_ptr<AVIOContext, AVIOContextDeleter> io_context_;
  std::unique_ptr<AVFormatContext, AVFormatWriteContextDeleter> format_context_;
  std::vector<Stream> streams_;
};

class Muxer {
 public:
  Muxer(const coro::util::EventLoop* event_loop,
        coro::util::ThreadPool* thread_pool)
      : event_loop_(event_loop), thread_pool_(thread_pool) {}

  template <typename VideoCloudProvider, typename Video,
            typename AudioCloudProvider, typename Audio>
  Generator<std::string> operator()(VideoCloudProvider* video_cloud_provider,
                                    Video video_track,
                                    AudioCloudProvider* audio_cloud_provider,
                                    Audio audio_track, MediaContainer container,
                                    stdx::stop_token stop_token) const {
    decltype(CreateIOContext(video_cloud_provider, video_track,
                             stop_token)) video_io_context;
    decltype(CreateIOContext(audio_cloud_provider, audio_track,
                             stop_token)) audio_io_context;
    auto muxer_context = co_await thread_pool_->Do([&] {
      std::tie(video_io_context, audio_io_context) = InParallel(
          [&] {
            return CreateIOContext(video_cloud_provider, std::move(video_track),
                                   stop_token);
          },
          [&] {
            return CreateIOContext(audio_cloud_provider, std::move(audio_track),
                                   stop_token);
          });
      return MuxerContext(thread_pool_, video_io_context.get(),
                          audio_io_context.get(), container);
    });
    FOR_CO_AWAIT(std::string & chunk, muxer_context.GetContent()) {
      if (!chunk.empty()) {
        co_yield std::move(chunk);
      }
    }
  }

 private:
  template <typename CloudProvider, typename Item>
  auto CreateIOContext(CloudProvider* provider, Item item,
                       stdx::stop_token stop_token) const {
    return coro::cloudstorage::util::CreateIOContext(
        event_loop_, provider, std::move(item), std::move(stop_token));
  }
  template <typename F1, typename F2>
  auto InParallel(F1&& f1, F2&& f2) const {
    std::promise<std::tuple<decltype(f1()), decltype(f2())>> promise;
    coro::RunTask([&]() -> Task<> {
      try {
        promise.set_value(
            co_await WhenAll(thread_pool_->Do(std::forward<F1>(f1)),
                             thread_pool_->Do(std::forward<F2>(f2))));
      } catch (...) {
        promise.set_exception(std::current_exception());
      }
    });
    return promise.get_future().get();
  }

  const coro::util::EventLoop* event_loop_;
  coro::util::ThreadPool* thread_pool_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_MUXER_H
