#include "muxer.h"

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
        auto file = reinterpret_cast<std::FILE*>(opaque);
        whence &= ~AVSEEK_FORCE;
        return Fseek(file, offset, whence);
      });
  if (!io_context) {
    throw std::runtime_error("avio_alloc_context");
  }
  return io_context;
}

}  // namespace

MuxerContext::MuxerContext(AVIOContext* video, AVIOContext* audio,
                           MediaContainer container)
    : file_(CreateTmpFile()),
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

}  // namespace coro::cloudstorage::util