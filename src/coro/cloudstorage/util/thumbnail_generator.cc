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

class Graph {
 public:
  Graph(std::unique_ptr<AVFilterGraph, AVFilterGraphDeleter> graph,
        std::vector<std::unique_ptr<AVFilterContext, AVFilterContextDeleter>>
            filters)
      : graph_(std::move(graph)), filters_(std::move(filters)) {}

  void WriteFrame(AVFrame* frame) {
    CheckAVError(av_buffersrc_write_frame(filters_.front().get(), frame),
                 "av_buffersrc_write_frame");
  }

  std::optional<std::unique_ptr<AVFrame, AVFrameDeleter>> PullFrame() {
    std::unique_ptr<AVFrame, AVFrameDeleter> received_frame(av_frame_alloc());
    if (!received_frame) {
      throw RuntimeError("av_frame_alloc error");
    }
    int err =
        av_buffersink_get_frame(filters_.back().get(), received_frame.get());
    if (err == AVERROR(EAGAIN)) {
      return std::nullopt;
    }
    if (err == AVERROR_EOF) {
      return nullptr;
    }
    CheckAVError(err, "av_buffersink_get_frame2");
    return received_frame;
  }

 private:
  std::unique_ptr<AVFilterGraph, AVFilterGraphDeleter> graph_;
  std::vector<std::unique_ptr<AVFilterContext, AVFilterContextDeleter>>
      filters_;
};

class GraphBuilder {
 public:
  GraphBuilder(int width, int height, int format, AVRational time_base) {
    AddFilter("buffer",
              {{"width", std::to_string(width)},
               {"height", std::to_string(height)},
               {"pix_fmt", std::to_string(format)},
               {"time_base", StrCat(time_base.num, '/', time_base.den)}});
  }

  explicit GraphBuilder(AVFrame* frame)
      : GraphBuilder(frame->width, frame->height, frame->format, {1, 24}) {}

  GraphBuilder(AVFormatContext* format_context, int stream,
               AVCodecContext* codec_context)
      : GraphBuilder(codec_context->width, codec_context->height,
                     codec_context->pix_fmt,
                     format_context->streams[stream]->time_base) {}

  GraphBuilder& AddFilter(
      const char* name,
      std::initializer_list<std::pair<const char*, std::string>> args) {
    std::unique_ptr<AVFilterContext, AVFilterContextDeleter> filter(
        avfilter_graph_alloc_filter(graph_.get(), avfilter_get_by_name(name),
                                    nullptr));
    if (!filter) {
      throw LogicError(StrCat("filter ", name, " unavailable"));
    }
    AVDictionary* d = nullptr;
    auto scope_guard = AtScopeExit([&] { av_dict_free(&d); });
    for (const auto& [key, value] : args) {
      CheckAVError(av_dict_set(&d, key, value.c_str(), 0), "av_dict_set");
    }
    CheckAVError(avfilter_init_dict(filter.get(), &d), "avfilter_init_dict");
    filters_.emplace_back(std::move(filter));
    return *this;
  }

  Graph Build() && {
    AddFilter("buffersink", {});
    for (size_t i = 0; i + 1 < filters_.size(); i++) {
      CheckAVError(
          avfilter_link(filters_[i].get(), 0, filters_[i + 1].get(), 0),
          "avfilter_link");
    }
    CheckAVError(avfilter_graph_config(graph_.get(), nullptr),
                 "avfilter_graph_config");
    return Graph(std::move(graph_), std::move(filters_));
  }

 private:
  std::unique_ptr<AVFilterGraph, AVFilterGraphDeleter> graph_{[] {
    auto* graph = avfilter_graph_alloc();
    if (!graph) {
      throw RuntimeError("avfilter_graph_alloc error");
    }
    return graph;
  }()};
  std::vector<std::unique_ptr<AVFilterContext, AVFilterContextDeleter>>
      filters_;
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
      throw RuntimeError("av_frame_alloc");
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

auto RotateFrame(std::unique_ptr<AVFrame, AVFrameDeleter> frame,
                 int orientation) {
  if (orientation == 1) {
    return frame;
  }

  GraphBuilder graph_builder{frame.get()};
  if (orientation >= 5) {
    graph_builder.AddFilter("transpose",
                            {{"dir", [&] {
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
                                    throw RuntimeError("unexpected");
                                }
                              }()}});
  }
  if (orientation == 3 || orientation == 4) {
    graph_builder.AddFilter("vflip", {});
  }
  if (orientation == 2 || orientation == 4) {
    graph_builder.AddFilter("hflip", {});
  }

  Graph graph = std::move(graph_builder).Build();
  graph.WriteFrame(frame.get());
  return graph.PullFrame().value();
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
    throw RuntimeError("sws_getContext returned null");
  }
  std::unique_ptr<AVFrame, AVFrameConvertedDeleter> rgb_frame(av_frame_alloc());
  if (!rgb_frame) {
    throw RuntimeError("av_frame_alloc");
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
    throw LogicError("codec not found");
  }
  auto frame = ConvertFrame(std::move(input_frame), codec);
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> context(
      avcodec_alloc_context3(codec));
  if (!context) {
    throw RuntimeError("avcodec_alloc_context3");
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

bool IsFrameBlack(const AVFrame* frame) {
  int count = 0;
  for (int i = 0; i < frame->height; i++) {
    for (int j = 0; j < frame->width; j++) {
      int value =
          *std::max(frame->data[0] + i * frame->linesize[0] + 3 * j,
                    frame->data[0] + i * frame->linesize[0] + 3 * (j + 1));
      if (value < 32) {
        count++;
      }
    }
  }
  return count >= 0.95 * frame->width * frame->height;
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
  Graph read_graph =
      std::move(
          GraphBuilder(context.get(), stream, codec_context.get())
              .AddFilter("scale", {{"width", std::to_string(size.width)},
                                   {"height", std::to_string(size.height)}})
              .AddFilter("format", {{"pix_fmts", "rgb24"}}))
          .Build();
  Graph thumbnail_graph =
      std::move(GraphBuilder(size.width, size.height, AV_PIX_FMT_RGB24, {1, 24})
                    .AddFilter("thumbnail", {}))
          .Build();

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
    auto received_frame = thumbnail_graph.PullFrame();
    if (received_frame) {
      return std::move(*received_frame);
    }
    received_frame = read_graph.PullFrame();
    if (received_frame) {
      if (!received_frame->get() || !IsFrameBlack(received_frame->get())) {
        thumbnail_graph.WriteFrame(received_frame->get());
      }
      continue;
    }
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
    read_graph.WriteFrame(frame.get());
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
