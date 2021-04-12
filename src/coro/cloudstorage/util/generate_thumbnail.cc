#include "generate_thumbnail.h"

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

namespace coro::cloudstorage::util {

namespace {

struct AVCodecContextDeleter {
  void operator()(AVCodecContext* context) {
    if (context) {
      avcodec_free_context(&context);
    }
  }
};

std::string av_error(int err) {
  char buffer[AV_ERROR_MAX_STRING_SIZE + 1] = {};
  if (av_strerror(err, buffer, AV_ERROR_MAX_STRING_SIZE) < 0)
    return "invalid error";
  else
    return buffer;
}

void Check(int code, const std::string& call) {
  if (code < 0) {
    throw std::logic_error(call + " (" + av_error(code) + ")");
  }
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
  Check(avcodec_parameters_to_context(codec_context.get(),
                                      context->streams[stream_index]->codecpar),
        "avcodec_parameters_to_context");
  Check(avcodec_open2(codec_context.get(), codec, nullptr), "avcodec_open2");
  return codec_context;
}

}  // namespace

}  // namespace coro::cloudstorage::util