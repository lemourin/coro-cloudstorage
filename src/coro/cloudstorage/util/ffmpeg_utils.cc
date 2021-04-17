#include "ffmpeg_utils.h"

#include <string>

namespace coro::cloudstorage::util {

void CheckAVError(int code, std::string_view call) {
  if (code < 0) {
    throw std::runtime_error(std::string(call) + " (" + av_err2str(code) + ")");
  }
}

std::unique_ptr<AVFormatContext, AVFormatContextDeleter> CreateFormatContext(
    AVIOContext* io_context) {
  auto context = avformat_alloc_context();
  context->interrupt_callback.opaque = nullptr;
  context->interrupt_callback.callback = [](void* t) -> int { return 0; };
  context->pb = io_context;
  int e = 0;
  if ((e = avformat_open_input(&context, nullptr, nullptr, nullptr)) < 0) {
    avformat_free_context(context);
    CheckAVError(e, "avformat_open_input");
  } else if ((e = avformat_find_stream_info(context, nullptr)) < 0) {
    avformat_close_input(&context);
    CheckAVError(e, "avformat_find_stream_info");
  }
  return std::unique_ptr<AVFormatContext, AVFormatContextDeleter>(context);
}

std::unique_ptr<AVCodecContext, AVCodecContextDeleter> CreateCodecContext(
    AVFormatContext* context, int stream_index) {
  auto codec =
      avcodec_find_decoder(context->streams[stream_index]->codecpar->codec_id);
  if (!codec) {
    throw std::logic_error("decoder not found");
  }
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> codec_context(
      avcodec_alloc_context3(codec));
  CheckAVError(
      avcodec_parameters_to_context(codec_context.get(),
                                    context->streams[stream_index]->codecpar),
      "avcodec_parameters_to_context");
  CheckAVError(avcodec_open2(codec_context.get(), codec, nullptr),
               "avcodec_open2");
  return codec_context;
}

std::unique_ptr<AVPacket, AVPacketDeleter> CreatePacket() {
  return std::unique_ptr<AVPacket, AVPacketDeleter>(av_packet_alloc());
}

}  // namespace coro::cloudstorage::util