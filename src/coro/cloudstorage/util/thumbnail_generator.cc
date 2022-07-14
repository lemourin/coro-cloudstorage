#include "coro/cloudstorage/util/thumbnail_generator.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "coro/cloudstorage/util/avio_context.h"
#include "coro/cloudstorage/util/ffmpeg_utils.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/util/raii_utils.h"

namespace coro::cloudstorage::util {

namespace {

using ::coro::util::AtScopeExit;

struct ImageSize {
  int width;
  int height;
};

struct AVFrameDeleter {
  void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

struct AVFrameConvertedDeleter {
  void operator()(AVFrame* frame) const {
    av_freep(&frame->data);
    av_frame_free(&frame);
  }
};

struct SwsContextDeleter {
  void operator()(SwsContext* context) const { sws_freeContext(context); }
};

struct AVFilterContextDeleter {
  void operator()(AVFilterContext* filter) const { avfilter_free(filter); }
};

struct AVFilterGraphDeleter {
  void operator()(AVFilterGraph* graph) const { avfilter_graph_free(&graph); }
};

auto DecodeFrame(AVFormatContext* context, AVCodecContext* codec_context,
                 int stream_index, std::atomic_bool* interrupted) {
  std::unique_ptr<AVFrame, AVFrameDeleter> result_frame;
  while (!result_frame) {
    if (*interrupted) {
      throw InterruptedException();
    }
    auto packet = CreatePacket();
    auto read_packet = av_read_frame(context, packet.get());
    if (read_packet != 0 && read_packet != AVERROR_EOF) {
      CheckAVError(read_packet, "av_read_frame");
    } else {
      if (read_packet == 0 && packet->stream_index != stream_index) {
        continue;
      }
      auto send_packet = avcodec_send_packet(
          codec_context, read_packet == AVERROR_EOF ? nullptr : packet.get());
      if (send_packet != AVERROR_EOF) {
        CheckAVError(send_packet, "avcodec_send_packet");
      }
    }
    std::unique_ptr<AVFrame, AVFrameDeleter> frame(av_frame_alloc());
    if (!frame) {
      throw std::runtime_error("av_frame_alloc");
    }
    auto code = avcodec_receive_frame(codec_context, frame.get());
    if (code == 0) {
      result_frame = std::move(frame);
    } else if (code == AVERROR_EOF) {
      break;
    } else if (code != AVERROR(EAGAIN)) {
      CheckAVError(code, "avcodec_receive_frame");
    }
  }
  return result_frame;
}

ImageSize GetThumbnailSize(ImageSize i, int target) {
  if (i.width == 0 || i.height == 0) {
    return {target, target};
  }
  if (i.width > i.height) {
    return {target, i.height * target / i.width};
  } else {
    return {i.width * target / i.height, target};
  }
}

auto CreateSourceFilter(AVFilterGraph* graph, int width, int height, int format,
                        AVRational time_base) {
  std::unique_ptr<AVFilterContext, AVFilterContextDeleter> filter(
      avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"),
                                  nullptr));
  if (!filter) {
    throw std::logic_error("filter buffer unavailable");
  }
  AVDictionary* d = nullptr;
  auto scope_guard = AtScopeExit([&] { av_dict_free(&d); });
  CheckAVError(av_dict_set_int(&d, "width", width, 0), "av_dict_set_int");
  CheckAVError(av_dict_set_int(&d, "height", height, 0), "av_dict_set_int");
  CheckAVError(av_dict_set_int(&d, "pix_fmt", format, 0), "av_dict_set_int");
  CheckAVError(
      av_dict_set(&d, "time_base",
                  StrCat(time_base.num, '/', time_base.den).c_str(), 0),
      "av_dict_set");
  CheckAVError(avfilter_init_dict(filter.get(), &d),
               "avfilter_init_dict source");
  return filter;
}

auto CreateSourceFilter(AVFilterGraph* graph, AVFrame* frame) {
  return CreateSourceFilter(graph, frame->width, frame->height, frame->format,
                            {1, 24});
}

auto CreateSourceFilter(AVFilterGraph* graph, AVFormatContext* format_context,
                        int stream, AVCodecContext* codec_context) {
  return CreateSourceFilter(graph, codec_context->width, codec_context->height,
                            codec_context->pix_fmt,
                            format_context->streams[stream]->time_base);
}

auto CreateFilter(AVFilterGraph* graph, const char* name) {
  std::unique_ptr<AVFilterContext, AVFilterContextDeleter> filter(
      avfilter_graph_alloc_filter(graph, avfilter_get_by_name(name), nullptr));
  if (!filter) {
    throw std::logic_error(StrCat("filter ", name, " unavailable"));
  }
  CheckAVError(avfilter_init_dict(filter.get(), nullptr), "avfilter_init_dict");
  return filter;
}

auto CreateSinkFilter(AVFilterGraph* graph) {
  return CreateFilter(graph, "buffersink");
}

auto CreateThumbnailFilter(AVFilterGraph* graph) {
  return CreateFilter(graph, "thumbnail");
}

auto CreateScaleFilter(AVFilterGraph* graph, ImageSize size) {
  std::unique_ptr<AVFilterContext, AVFilterContextDeleter> filter(
      avfilter_graph_alloc_filter(graph, avfilter_get_by_name("scale"),
                                  nullptr));
  if (!filter) {
    throw std::logic_error("filter scale unavailable");
  }
  AVDictionary* d = nullptr;
  auto scope_guard = AtScopeExit([&] { av_dict_free(&d); });
  CheckAVError(av_dict_set_int(&d, "width", size.width, 0), "av_dict_set_int");
  CheckAVError(av_dict_set_int(&d, "height", size.height, 0),
               "av_dict_set_int");
  CheckAVError(avfilter_init_dict(filter.get(), &d), "avfilter_init_dict");
  return filter;
}

auto CreateTransposeFilter(AVFilterGraph* graph, const char* dir) {
  std::unique_ptr<AVFilterContext, AVFilterContextDeleter> filter(
      avfilter_graph_alloc_filter(graph, avfilter_get_by_name("transpose"),
                                  nullptr));
  if (!filter) {
    throw std::logic_error("filter transpose unavailable");
  }
  AVDictionary* d = nullptr;
  auto scope_guard = AtScopeExit([&] { av_dict_free(&d); });
  CheckAVError(av_dict_set(&d, "dir", dir, 0), "av_dict_set");
  CheckAVError(avfilter_init_dict(filter.get(), &d), "avfilter_init_dict");
  return filter;
}

auto RotateFrame(std::unique_ptr<AVFrame, AVFrameDeleter> frame,
                 int orientation) {
  if (orientation == 1) {
    return frame;
  }

  std::unique_ptr<AVFilterGraph, AVFilterGraphDeleter> graph(
      avfilter_graph_alloc());
  if (!graph) {
    throw std::runtime_error("avfilter_graph_alloc error");
  }

  auto source_filter = CreateSourceFilter(graph.get(), frame.get());
  std::vector<std::unique_ptr<AVFilterContext, AVFilterContextDeleter>> filters;
  AVFilterContext* last_filter = source_filter.get();
  if (orientation >= 5) {
    auto transpose_filter = CreateTransposeFilter(graph.get(), [&] {
      switch (orientation) {
        case 5:
          return "cclock";
        case 6:
          return "cclock_flip";
        case 7:
          return "clock";
        case 8:
          return "clock_flip";
        default:
          throw std::runtime_error("unexpected");
      }
    }());
    CheckAVError(avfilter_link(last_filter, 0, transpose_filter.get(), 0),
                 "avfilter_link");
    last_filter = filters.emplace_back(std::move(transpose_filter)).get();
  }
  if (orientation == 3 || orientation == 4) {
    auto vflip = CreateFilter(graph.get(), "vflip");
    CheckAVError(avfilter_link(last_filter, 0, vflip.get(), 0),
                 "avfilter_link");
    last_filter = filters.emplace_back(std::move(vflip)).get();
  }
  if (orientation == 2 || orientation == 4) {
    auto hflip = CreateFilter(graph.get(), "hflip");
    CheckAVError(avfilter_link(last_filter, 0, hflip.get(), 0),
                 "avfilter_link");
    last_filter = filters.emplace_back(std::move(hflip)).get();
  }
  auto sink_filter = CreateSinkFilter(graph.get());
  CheckAVError(avfilter_link(last_filter, 0, sink_filter.get(), 0),
               "avfilter_link");
  CheckAVError(avfilter_graph_config(graph.get(), nullptr),
               "avfilter_graph_config");

  CheckAVError(av_buffersrc_write_frame(source_filter.get(), frame.get()),
               "av_buffersrc_write_frame");

  std::unique_ptr<AVFrame, AVFrameDeleter> received_frame(av_frame_alloc());
  CheckAVError(av_buffersink_get_frame(sink_filter.get(), received_frame.get()),
               "av_buffersrc_write_frame");

  return received_frame;
}

auto ConvertFrame(std::unique_ptr<AVFrame, AVFrameDeleter> frame,
                  const AVCodec* codec) {
  int orientation = [&] {
    if (AVDictionaryEntry* entry =
            av_dict_get(frame->metadata, "Orientation", nullptr, 0)) {
      int value = std::stoi(entry->value);
      if (value > 1 && value <= 8) {
        return value;
      }
    }
    return 1;
  }();
  frame = RotateFrame(std::move(frame), orientation);

  std::vector<AVPixelFormat> supported;
  for (const auto* p = codec->pix_fmts; p && *p != -1; p++) {
    if (sws_isSupportedOutput(*p)) {
      supported.emplace_back(*p);
    }
  }
  supported.emplace_back(AV_PIX_FMT_NONE);
  AVPixelFormat format = avcodec_find_best_pix_fmt_of_list(
      supported.data(), AVPixelFormat(frame->format), false,
      /*loss_ptr=*/nullptr);
  std::unique_ptr<SwsContext, SwsContextDeleter> sws_context(sws_getContext(
      frame->width, frame->height, AVPixelFormat(frame->format), frame->width,
      frame->height, format, SWS_BICUBIC, nullptr, nullptr, nullptr));
  if (!sws_context) {
    throw std::runtime_error("sws_getContext returned null");
  }
  std::unique_ptr<AVFrame, AVFrameConvertedDeleter> rgb_frame(av_frame_alloc());
  if (!rgb_frame) {
    throw std::runtime_error("av_frame_alloc");
  }
  CheckAVError(av_frame_copy_props(rgb_frame.get(), frame.get()),
               "av_frame_copy_props");
  if (orientation != 1) {
    CheckAVError(av_dict_set_int(&rgb_frame->metadata, "Orientation", 1, 0),
                 "av_dict_set_int");
  }
  rgb_frame->format = format;
  rgb_frame->width = frame->width;
  rgb_frame->height = frame->height;
  CheckAVError(av_image_alloc(rgb_frame->data, rgb_frame->linesize,
                              frame->width, frame->height, format, 32),
               "av_image_alloc");
  CheckAVError(sws_scale(sws_context.get(), frame->data, frame->linesize, 0,
                         frame->height, rgb_frame->data, rgb_frame->linesize),
               "sws_scale");
  return rgb_frame;
}

std::string EncodeFrame(std::unique_ptr<AVFrame, AVFrameDeleter> input_frame,
                        ThumbnailOptions options,
                        std::atomic_bool* interrupted) {
  auto* codec = avcodec_find_encoder(
      options.codec == ThumbnailOptions::Codec::JPEG ? AV_CODEC_ID_MJPEG
                                                     : AV_CODEC_ID_PNG);
  if (!codec) {
    throw std::logic_error("codec not found");
  }
  auto frame = ConvertFrame(std::move(input_frame), codec);
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> context(
      avcodec_alloc_context3(codec));
  if (!context) {
    throw std::runtime_error("avcodec_alloc_context3");
  }
  context->time_base = {1, 24};
  context->pix_fmt = AVPixelFormat(frame->format);
  context->width = frame->width;
  context->height = frame->height;
  context->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
  CheckAVError(avcodec_open2(context.get(), codec, nullptr), "avcodec_open2");
  auto packet = CreatePacket();
  bool frame_sent = false;
  bool flush_sent = false;
  std::string result;
  while (true) {
    if (*interrupted) {
      throw InterruptedException();
    }
    if (!frame_sent) {
      CheckAVError(avcodec_send_frame(context.get(), frame.get()),
                   "avcodec_send_frame");
      frame_sent = true;
    } else if (!flush_sent) {
      CheckAVError(avcodec_send_frame(context.get(), nullptr),
                   "avcodec_send_frame");
      flush_sent = true;
    }
    auto err = avcodec_receive_packet(context.get(), packet.get());
    if (err != 0) {
      if (err == AVERROR_EOF) {
        break;
      } else {
        CheckAVError(err, "avcodec_receive_packet");
      }
    } else {
      result +=
          std::string(reinterpret_cast<char*>(packet->data), packet->size);
    }
  }
  return result;
}

int GetExifOrientation(const int32_t* matrix) {
  double theta = -round(av_display_rotation_get(matrix));
  theta -= 360 * floor(theta / 360 + 0.9 / 360);
  if (fabs(theta - 90.0) < 1.0) {
    return matrix[3] > 0 ? 6 : 7;
  } else if (fabs(theta - 180.0) < 1.0) {
    return matrix[0] < 0 ? 2 : 4;
  } else if (fabs(theta - 270.0) < 1.0) {
    return matrix[3] < 0 ? 8 : 5;
  } else {
    return matrix[4] < 0 ? 3 : 1;
  }
}

auto GetThumbnailFrame(AVIOContext* io_context, ThumbnailOptions options,
                       std::atomic_bool* interrupted) {
  auto context = CreateFormatContext(io_context);
  auto stream = av_find_best_stream(context.get(), AVMEDIA_TYPE_VIDEO, -1, -1,
                                    nullptr, 0);
  CheckAVError(stream, "av_find_best_stream");
  if (context->duration > 0) {
    if (av_seek_frame(context.get(), -1, context->duration / 10, 0) < 0) {
      CheckAVError(av_seek_frame(context.get(), 0, 0,
                                 AVSEEK_FLAG_BYTE | AVSEEK_FLAG_BACKWARD),
                   "av_seek_frame");
    }
  }
  auto codec_context = CreateCodecContext(context.get(), stream);
  auto size = GetThumbnailSize({codec_context->width, codec_context->height},
                               options.size);
  std::unique_ptr<AVFilterGraph, AVFilterGraphDeleter> filter_graph(
      avfilter_graph_alloc());
  if (!filter_graph) {
    throw std::runtime_error("avfilter_graph_alloc error");
  }
  auto source_filter = CreateSourceFilter(filter_graph.get(), context.get(),
                                          stream, codec_context.get());
  auto sink_filter = CreateSinkFilter(filter_graph.get());
  auto thumbnail_filter = CreateThumbnailFilter(filter_graph.get());
  auto scale_filter = CreateScaleFilter(filter_graph.get(), size);
  CheckAVError(avfilter_link(source_filter.get(), 0, scale_filter.get(), 0),
               "avfilter_link");
  CheckAVError(avfilter_link(scale_filter.get(), 0, thumbnail_filter.get(), 0),
               "avfilter_link");
  CheckAVError(avfilter_link(thumbnail_filter.get(), 0, sink_filter.get(), 0),
               "avfilter_link");
  CheckAVError(avfilter_graph_config(filter_graph.get(), nullptr),
               "avfilter_graph_config");
  int stream_orientation = [&] {
    auto* stream_matrix = reinterpret_cast<int32_t*>(av_stream_get_side_data(
        context->streams[stream], AV_PKT_DATA_DISPLAYMATRIX, nullptr));
    if (stream_matrix != nullptr) {
      return GetExifOrientation(stream_matrix);
    } else {
      return 0;
    }
  }();

  while (true) {
    std::unique_ptr<AVFrame, AVFrameDeleter> received_frame(av_frame_alloc());
    int err = av_buffersink_get_frame(sink_filter.get(), received_frame.get());
    if (err == AVERROR(EAGAIN)) {
      auto frame =
          DecodeFrame(context.get(), codec_context.get(), stream, interrupted);
      if (frame) {
        AVFrameSideData* frame_matrix =
            av_frame_get_side_data(frame.get(), AV_FRAME_DATA_DISPLAYMATRIX);
        int orientation =
            frame_matrix ? GetExifOrientation(
                               reinterpret_cast<int32_t*>(frame_matrix->data))
                         : stream_orientation;
        if (orientation != 0) {
          CheckAVError(
              av_dict_set_int(&frame->metadata, "Orientation", orientation, 0),
              "av_dict_set_int");
        }
      }
      CheckAVError(av_buffersrc_write_frame(source_filter.get(), frame.get()),
                   "av_buffersrc_write_frame");
    } else if (err != 0) {
      CheckAVError(err, "av_buffersink_get_frame");
    } else {
      return std::move(received_frame);
    }
  }
}

std::string GenerateThumbnail(AVIOContext* io_context, ThumbnailOptions options,
                              std::atomic_bool* interrupted) {
  return EncodeFrame(GetThumbnailFrame(io_context, options, interrupted),
                     options, interrupted);
}

}  // namespace

Task<std::string> ThumbnailGenerator::operator()(
    AbstractCloudProvider* provider, AbstractCloudProvider::File file,
    ThumbnailOptions options, stdx::stop_token stop_token) const {
  std::unique_ptr<AVIOContext, AVIOContextDeleter> io_context;
  std::atomic_bool interrupted = false;
  stdx::stop_callback cb(stop_token, [&] { interrupted = true; });
  co_return co_await thread_pool_->Do(stop_token, [&] {
    io_context = CreateIOContext(event_loop_, provider, std::move(file),
                                 std::move(stop_token));
    return GenerateThumbnail(io_context.get(), options, &interrupted);
  });
}

}  // namespace coro::cloudstorage::util
