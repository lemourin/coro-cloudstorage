#include "muxer.h"

namespace coro::cloudstorage::util {

namespace {

auto CreateMuxerIOContext(std::FILE* file) {
  const int kBufferSize = 4 * 1024;
  auto buffer = static_cast<uint8_t*>(av_malloc(kBufferSize));
  return avio_alloc_context(
      buffer, kBufferSize, /*write_flag=*/1, file,
      /*read_packet=*/nullptr,
      /*write_packet=*/
      [](void* opaque, uint8_t* buf, int buf_size) -> int {
        return static_cast<int>(fwrite(buf, 1, static_cast<size_t>(buf_size),
                                       reinterpret_cast<std::FILE*>(opaque)));
      },
      [](void* opaque, int64_t offset, int whence) -> int64_t {
        auto file = reinterpret_cast<std::FILE*>(opaque);
        whence &= ~AVSEEK_FORCE;
        return Fseek(file, offset, whence);
      });
}

}  // namespace

MuxerContext::MuxerContext(::coro::util::ThreadPool* thread_pool,
                           AVIOContext* video, AVIOContext* audio)
    : thread_pool_(thread_pool),
      file_(CreateTmpFile()),
      io_context_(CreateMuxerIOContext(file_.get())),
      format_context_([&] {
        AVFormatContext* format_context;
        CheckAVError(avformat_alloc_output_context2(&format_context,
                                                    /*oformat=*/nullptr, "webm",
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
  while (true) {
    for (auto& stream : streams_) {
      if (!stream.is_eof && !stream.packet) {
        stream.packet = CreatePacket();
        while (true) {
          auto read_packet = co_await thread_pool_->Invoke(
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

  FOR_CO_AWAIT(std::string & chunk, ReadFile(thread_pool_, file_.get())) {
    co_yield std::move(chunk);
  }
}

}  // namespace coro::cloudstorage::util