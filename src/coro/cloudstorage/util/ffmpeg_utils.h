#ifndef CORO_CLOUDSTORAGE_FUSE_FFMPEG_UTILS_H
#define CORO_CLOUDSTORAGE_FUSE_FFMPEG_UTILS_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <memory>
#include <string_view>

namespace coro::cloudstorage::util {

struct AVCodecContextDeleter {
  void operator()(AVCodecContext* context) const {
    avcodec_free_context(&context);
  }
};

struct AVFormatContextDeleter {
  void operator()(AVFormatContext* context) const {
    avformat_close_input(&context);
  }
};

struct AVPacketDeleter {
  void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

void CheckAVError(int code, std::string_view call);
std::unique_ptr<AVFormatContext, AVFormatContextDeleter> CreateFormatContext(
    AVIOContext* io_context);
std::unique_ptr<AVCodecContext, AVCodecContextDeleter> CreateCodecContext(
    AVFormatContext* context, int stream_index);
std::unique_ptr<AVPacket, AVPacketDeleter> CreatePacket();

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_FFMPEG_UTILS_H
