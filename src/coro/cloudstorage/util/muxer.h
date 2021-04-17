#ifndef CORO_CLOUDSTORAGE_FUSE_MUXER_H
#define CORO_CLOUDSTORAGE_FUSE_MUXER_H

#include <coro/cloudstorage/util/avio_context.h>
#include <coro/cloudstorage/util/ffmpeg_utils.h>
#include <coro/generator.h>
#include <coro/util/event_loop.h>
#include <coro/util/thread_pool.h>

#include <iostream>

namespace coro::cloudstorage::util {

class MuxerContext {
 public:
  MuxerContext(::coro::util::ThreadPool* thread_pool, AVIOContext* video,
               AVIOContext* audio);

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

  Stream CreateStream(AVIOContext* video, AVMediaType type) const;

  coro::util::ThreadPool* thread_pool_;
  std::unique_ptr<std::string> buffer_;
  std::unique_ptr<AVIOContext, AVIOContextDeleter> io_context_;
  std::unique_ptr<AVFormatContext, AVFormatWriteContextDeleter> format_context_;
  std::vector<Stream> streams_;
};

class Muxer {
 public:
  Muxer(::coro::util::EventLoop* event_loop,
        ::coro::util::ThreadPool* thread_pool)
      : event_loop_(event_loop), thread_pool_(thread_pool) {}

  template <typename VideoCloudProvider, typename Video,
            typename AudioCloudProvider, typename Audio>
  Generator<std::string> operator()(VideoCloudProvider* video_cloud_provider,
                                    Video video_track,
                                    AudioCloudProvider* audio_cloud_provider,
                                    Audio audio_track,
                                    stdx::stop_token stop_token) const {
    auto video_io_context = CreateIOContext(video_cloud_provider,
                                            std::move(video_track), stop_token);
    auto audio_io_context = CreateIOContext(audio_cloud_provider,
                                            std::move(audio_track), stop_token);
    auto muxer_context = co_await thread_pool_->Invoke([&] {
      return MuxerContext(thread_pool_, video_io_context.get(),
                          audio_io_context.get());
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
    return ::coro::cloudstorage::util::CreateIOContext(
        event_loop_, provider, std::move(item), std::move(stop_token));
  }
  ::coro::util::EventLoop* event_loop_;
  ::coro::util::ThreadPool* thread_pool_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_MUXER_H
