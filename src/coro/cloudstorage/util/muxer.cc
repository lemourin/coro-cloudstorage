#include "coro/cloudstorage/util/muxer.h"

#include <iostream>

#include "coro/cloudstorage/util/avio_context.h"
#include "coro/cloudstorage/util/ffmpeg_utils.h"
#include "coro/cloudstorage/util/file_utils.h"
#include "coro/generator.h"
#include "coro/util/event_loop.h"
#include "coro/util/thread_pool.h"
#include "coro/when_all.h"

namespace coro::cloudstorage::util {

namespace {

auto CreateMuxerIOContext(std::FILE* file) {
  const int kBufferSize = 4 * 1024;
  auto* buffer = static_cast<uint8_t*>(av_malloc(kBufferSize));
  auto* io_context = avio_alloc_context(
      buffer, kBufferSize, /*write_flag=*/1, file,
      /*read_packet=*/nullptr,
      /*write_packet=*/
      [](void* opaque, uint8_t* buf, int buf_size) -> int {
        return static_cast<int>(fwrite(buf, 1, static_cast<size_t>(buf_size),
                                       reinterpret_cast<std::FILE*>(opaque)));
      },
      [](void* opaque, int64_t offset, int whence) -> int64_t {
        auto* file = reinterpret_cast<std::FILE*>(opaque);
        whence &= ~AVSEEK_FORCE;
        return Fseek(file, offset, whence);
      });
  if (!io_context) {
    throw std::runtime_error("avio_alloc_context");
  }
  return io_context;
}

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

MuxerContext::MuxerContext(coro::util::ThreadPool* thread_pool,
                           AVIOContext* video, AVIOContext* audio,
                           MediaContainer container)
    : thread_pool_(thread_pool),
      file_(CreateTmpFile()),
      io_context_(CreateMuxerIOContext(file_.get())),
      format_context_([&] {
        AVFormatContext* format_context;
        CheckAVError(avformat_alloc_output_context2(
                         &format_context,
                         /*oformat=*/nullptr,
                         [&] {
                           switch (container) {
                             case MediaContainer::kMp4:
                               return "mp4";
                             case MediaContainer::kWebm:
                               return "webm";
                             default:
                               throw std::runtime_error("invalid container");
                           }
                         }(),
                         /*filename=*/nullptr),
                     "avformat_alloc_output_context");
        format_context->pb = io_context_.get();
        return format_context;
      }()) {
  streams_.emplace_back(CreateStream(video, AVMEDIA_TYPE_VIDEO));
  streams_.emplace_back(CreateStream(audio, AVMEDIA_TYPE_AUDIO));
  CheckAVError(
      avformat_write_header(format_context_.get(), /*options=*/nullptr),
      "avformat_write_header");
}

MuxerContext::Stream MuxerContext::CreateStream(AVIOContext* io_context,
                                                AVMediaType type) const {
  Stream stream{};
  stream.format_context = CreateFormatContext(io_context);
  stream.source_stream_index = av_find_best_stream(stream.format_context.get(),
                                                   type, -1, -1, nullptr, 0);
  stream.codec_context = CreateCodecContext(stream.format_context.get(),
                                            stream.source_stream_index);
  stream.stream =
      avformat_new_stream(format_context_.get(), stream.codec_context->codec);
  if (!stream.stream) {
    throw std::runtime_error("couldn't add stream");
  }
  CheckAVError(avcodec_parameters_from_context(stream.stream->codecpar,
                                               stream.codec_context.get()),
               "avcodec_parameters_from_context");
  stream.stream->time_base =
      stream.format_context->streams[stream.source_stream_index]->time_base;
  stream.stream->duration =
      stream.format_context->streams[stream.source_stream_index]->duration;
  return stream;
}

Generator<std::string> MuxerContext::GetContent() {
  int previous_progress = 0;
  while (true) {
    for (auto& stream : streams_) {
      if (!stream.is_eof && !stream.packet) {
        stream.packet = CreatePacket();
        while (true) {
          auto read_packet = co_await thread_pool_->Do(
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
    int current_progress = static_cast<int>(100 * picked_stream->packet->pts /
                                            picked_stream->stream->duration);
    if (current_progress > previous_progress) {
      previous_progress = current_progress;
      std::cerr << "TRANSCODE PROGRESS " << current_progress << "%\n";
    }
    CheckAVError(
        av_write_frame(format_context_.get(), picked_stream->packet.get()),
        "av_write_frame");
    picked_stream->packet.reset();
  }

  CheckAVError(av_write_frame(format_context_.get(), nullptr),
               "av_write_frame");
  CheckAVError(av_write_trailer(format_context_.get()), "av_write_trailer");

  std::cerr << "TRANSCODE DONE\n";

  FOR_CO_AWAIT(std::string & chunk, ReadFile(thread_pool_, file_.get())) {
    co_yield std::move(chunk);
  }
}

}  // namespace

template <typename F1, typename F2>
auto Muxer::InParallel(F1&& f1, F2&& f2) const
    -> std::tuple<decltype(f1()), decltype(f2())> {
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

Generator<std::string> Muxer::operator()(
    AbstractCloudProvider* video_cloud_provider,
    AbstractCloudProvider::File video_track,
    AbstractCloudProvider* audio_cloud_provider,
    AbstractCloudProvider::File audio_track, MediaContainer container,
    stdx::stop_token stop_token) const {
  std::unique_ptr<AVIOContext, AVIOContextDeleter> video_io_context;
  std::unique_ptr<AVIOContext, AVIOContextDeleter> audio_io_context;
  auto muxer_context = co_await thread_pool_->Do([&] {
    std::tie(video_io_context, audio_io_context) = InParallel(
        [&] {
          return CreateIOContext(event_loop_, video_cloud_provider,
                                 std::move(video_track), stop_token);
        },
        [&] {
          return CreateIOContext(event_loop_, audio_cloud_provider,
                                 std::move(audio_track), stop_token);
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

}  // namespace coro::cloudstorage::util
