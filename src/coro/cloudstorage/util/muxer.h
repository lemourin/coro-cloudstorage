#ifndef CORO_CLOUDSTORAGE_FUSE_MUXER_H
#define CORO_CLOUDSTORAGE_FUSE_MUXER_H

#include <coro/cloudstorage/util/avio_context.h>
#include <coro/cloudstorage/util/ffmpeg_utils.h>
#include <coro/cloudstorage/util/file_utils.h>
#include <coro/generator.h>
#include <coro/util/event_loop.h>
#include <coro/util/thread_pool.h>

#include <iostream>

namespace coro::cloudstorage::util {

class MuxerContext {
 public:
  MuxerContext(AVIOContext* video, AVIOContext* audio);

  template <typename ThreadPool>
  Generator<std::string> GetContent(ThreadPool* thread_pool);

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

  std::unique_ptr<std::FILE, FileDeleter> file_;
  std::unique_ptr<AVIOContext, AVIOContextDeleter> io_context_;
  std::unique_ptr<AVFormatContext, AVFormatWriteContextDeleter> format_context_;
  std::vector<Stream> streams_;
};

template <typename EventLoop, typename ThreadPool>
class Muxer {
 public:
  Muxer(EventLoop* event_loop, ThreadPool* thread_pool)
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
      return MuxerContext(video_io_context.get(), audio_io_context.get());
    });
    FOR_CO_AWAIT(std::string & chunk, muxer_context.GetContent(thread_pool_)) {
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
  EventLoop* event_loop_;
  ThreadPool* thread_pool_;
};

template <typename ThreadPool>
Generator<std::string> MuxerContext::GetContent(ThreadPool* thread_pool) {
  while (true) {
    for (auto& stream : streams_) {
      if (!stream.is_eof && !stream.packet) {
        stream.packet = CreatePacket();
        while (true) {
          auto read_packet = co_await thread_pool->Invoke(
              av_read_frame, stream.format_context.get(), stream.packet.get());
          if (read_packet != 0 && read_packet != AVERROR_EOF) {
            CheckAVError(read_packet, "av_read_frame");
          } else {
            if (read_packet == 0 &&
                stream.packet->stream_index != stream.source_stream_index) {
              continue;
            }
            if (read_packet == AVERROR_EOF) {
              stream.is_eof = true;
              stream.packet.reset();
            } else {
              CheckAVError(av_packet_make_writable(stream.packet.get()),
                           "av_packet_make_writable");
              av_packet_rescale_ts(
                  stream.packet.get(),
                  stream.format_context->streams[stream.source_stream_index]
                      ->time_base,
                  stream.stream->time_base);
              stream.packet->stream_index = stream.stream->index;
            }
            break;
          }
        }
      }
    }
    Stream* picked_stream = nullptr;
    for (auto& stream : streams_) {
      if (stream.packet &&
          (!picked_stream ||
           av_compare_ts(stream.packet->dts, stream.stream->time_base,
                         picked_stream->packet->dts,
                         picked_stream->stream->time_base) == -1)) {
        picked_stream = &stream;
      }
    }
    if (!picked_stream) {
      break;
    }
    CheckAVError(
        av_write_frame(format_context_.get(), picked_stream->packet.get()),
        "av_write_frame");
    picked_stream->packet.reset();
  }

  CheckAVError(av_write_frame(format_context_.get(), nullptr),
               "av_write_frame");
  CheckAVError(av_write_trailer(format_context_.get()), "av_write_trailer");

  FOR_CO_AWAIT(std::string & chunk, ReadFile(thread_pool, file_.get())) {
    co_yield std::move(chunk);
  }
}

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_MUXER_H
