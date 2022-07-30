#include "coro/cloudstorage/util/ffmpeg_utils.h"

#include <memory>
#include <stdexcept>
#include <string>

#include "coro/exception.h"

namespace coro::cloudstorage::util {

namespace {

std::string GetAvError(int err) {
  std::string buffer(AV_ERROR_MAX_STRING_SIZE, 0);
  if (av_strerror(err, buffer.data(), AV_ERROR_MAX_STRING_SIZE) < 0) {
    return "invalid error";
  } else {
    return std::string(buffer.c_str(), strlen(buffer.c_str()));
  }
}

}  // namespace

void CheckAVError(int code, std::string_view call) {
  if (code < 0) {
    throw RuntimeError(std::string(call) + " (" + GetAvError(code) + ")");
  }
}

std::unique_ptr<AVFormatContext, AVFormatContextDeleter> CreateFormatContext(
    AVIOContext* io_context) {
  auto* context = avformat_alloc_context();
  if (!context) {
    throw RuntimeError("avformat_alloc_context");
  }
  context->interrupt_callback.opaque = nullptr;
  context->interrupt_callback.callback = [](void*) -> int { return 0; };
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
  auto* codec =
      avcodec_find_decoder(context->streams[stream_index]->codecpar->codec_id);
  if (!codec) {
    throw LogicError("decoder not found");
  }
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> codec_context(
      avcodec_alloc_context3(codec));
  if (!codec_context) {
    throw RuntimeError("avcodec_alloc_context3");
  }
  CheckAVError(
      avcodec_parameters_to_context(codec_context.get(),
                                    context->streams[stream_index]->codecpar),
      "avcodec_parameters_to_context");
  CheckAVError(avcodec_open2(codec_context.get(), codec, nullptr),
               "avcodec_open2");
  return codec_context;
}

std::unique_ptr<AVPacket, AVPacketDeleter> CreatePacket() {
  std::unique_ptr<AVPacket, AVPacketDeleter> packet(av_packet_alloc());
  if (!packet) {
    throw RuntimeError("av_packet_alloc");
  }
  return packet;
}

}  // namespace coro::cloudstorage::util
