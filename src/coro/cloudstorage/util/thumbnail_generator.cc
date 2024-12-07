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

  AVRational GetSinkTimeBase() const {
    return av_buffersink_get_time_base(sink());
  }

  int GetSinkWidth() const { return av_buffersink_get_w(sink()); }

  int GetSinkHeight() const { return av_buffersink_get_h(sink()); }

  int GetSinkColorSpace() const { return av_buffersink_get_colorspace(sink()); }

  int GetSinkColorRange() const {
    return av_buffersink_get_color_range(sink());
  }

  AVPixelFormat GetSinkFormat() const {
    return AVPixelFormat(av_buffersink_get_format(sink()));
  }

  void WriteFrame(const AVFrame* frame) {
    CheckAVError(av_buffersrc_write_frame(source(), frame),
                 "av_buffersrc_write_frame");
  }

  std::optional<std::unique_ptr<AVFrame, AVFrameDeleter>> PullFrame() {
    std::unique_ptr<AVFrame, AVFrameDeleter> received_frame(av_frame_alloc());
    if (!received_frame) {
      throw RuntimeError("av_frame_alloc error");
    }
    int err = av_buffersink_get_frame(sink(), received_frame.get());
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
  AVFilterContext* source() const { return filters_.front().get(); }

  AVFilterContext* sink() const { return filters_.back().get(); }

  std::unique_ptr<AVFilterGraph, AVFilterGraphDeleter> graph_;
  std::vector<std::unique_ptr<AVFilterContext, AVFilterContextDeleter>>
      filters_;
};

class GraphBuilder {
 public:
  GraphBuilder(int width, int height, int format, int color_space,
               int color_range, AVRational time_base) {
    AddFilter("buffer",
              {{"width", std::to_string(width)},
               {"height", std::to_string(height)},
               {"pix_fmt", std::to_string(format)},
               {"colorspace", std::to_string(color_space)},
               {"range", std::to_string(color_range)},
               {"time_base", StrCat(time_base.num, '/', time_base.den)}});
  }

  GraphBuilder(const Graph& input)
      : GraphBuilder(input.GetSinkWidth(), input.GetSinkHeight(),
                     input.GetSinkFormat(), input.GetSinkColorSpace(),
                     input.GetSinkColorRange(), input.GetSinkTimeBase()) {}

  explicit GraphBuilder(const AVFrame* frame)
      : GraphBuilder(frame->width, frame->height, frame->format,
                     frame->colorspace, frame->color_range, {1, 24}) {}

  GraphBuilder(const AVFormatContext* format_context, int stream,
               const AVCodecContext* codec_context)
      : GraphBuilder(codec_context->width, codec_context->height,
                     codec_context->pix_fmt, codec_context->colorspace,
                     codec_context->color_range,
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

auto ConvertFrame(const AVFrame* frame, AVPixelFormat format) {
  std::unique_ptr<SwsContext, SwsContextDeleter> sws_context(sws_getContext(
      frame->width, frame->height, AVPixelFormat(frame->format), frame->width,
      frame->height, format, SWS_BICUBIC,
      /*srcFilter=*/nullptr, /*dstFilter=*/nullptr, /*param=*/nullptr));
  if (!sws_context) {
    throw RuntimeError("sws_getContext returned null");
  }
  std::unique_ptr<AVFrame, AVFrameConvertedDeleter> target_frame(
      av_frame_alloc());
  if (!target_frame) {
    throw RuntimeError("av_frame_alloc");
  }
  CheckAVError(av_frame_copy_props(target_frame.get(), frame),
               "av_frame_copy_props");
  target_frame->format = format;
  target_frame->width = frame->width;
  target_frame->height = frame->height;
  CheckAVError(
      av_image_alloc(target_frame->data, target_frame->linesize, frame->width,
                     frame->height, format, /*align=*/32),
      "av_image_alloc");
  CheckAVError(
      sws_scale(sws_context.get(), frame->data, frame->linesize, 0,
                frame->height, target_frame->data, target_frame->linesize),
      "sws_scale");
  return target_frame;
}

auto ConvertFrame(const AVFrame* frame, const AVCodec* codec) {
  const AVPixelFormat* pix_fmts = nullptr;
  CheckAVError(avcodec_get_supported_config(
                   /*avctx=*/nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT,
                   /*flags=*/0, reinterpret_cast<const void**>(&pix_fmts),
                   /*out_num_configs=*/nullptr),
               "avcodec_get_supported_config");
  std::vector<AVPixelFormat> supported;
  for (const AVPixelFormat* pix_fmt = pix_fmts;
       pix_fmt && *pix_fmt != AV_PIX_FMT_NONE; pix_fmt++) {
    if (sws_isSupportedOutput(*pix_fmt)) {
      supported.push_back(*pix_fmt);
    }
  }
  supported.emplace_back(AV_PIX_FMT_NONE);
  AVPixelFormat format = avcodec_find_best_pix_fmt_of_list(
      supported.data(), AVPixelFormat(frame->format), /*has_alpha=*/false,
      /*loss_ptr=*/nullptr);
  return ConvertFrame(frame, format);
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
  int orientation = [&] {
    if (AVDictionaryEntry* entry =
            av_dict_get(input_frame->metadata, "Orientation", nullptr, 0)) {
      int value = std::stoi(entry->value);
      if (value > 1 && value <= 8) {
        return value;
      }
    }
    return 1;
  }();
  if (orientation != 1) {
    input_frame = RotateFrame(std::move(input_frame), orientation);
    CheckAVError(av_dict_set_int(&input_frame->metadata, "Orientation", 1, 0),
                 "av_dict_set_int");
  }
  auto frame = ConvertFrame(input_frame.get(), codec);
  std::unique_ptr<AVCodecContext, AVCodecContextDeleter> context(
      avcodec_alloc_context3(codec));
  if (!context) {
    throw RuntimeError("avcodec_alloc_context3");
  }
  context->time_base = {1, 24};
  context->pix_fmt = AVPixelFormat(frame->format);
  context->width = frame->width;
  context->height = frame->height;
  context->strict_std_compliance = FF_COMPLIANCE_NORMAL;
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
          std::string_view(reinterpret_cast<char*>(packet->data), packet->size);
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

bool IsFrameBlackImpl(const AVFrame* frame) {
  int count = 0;
  const uint8_t* p = frame->data[0];
  for (int i = 0; i < frame->height; i++) {
    for (int j = 0; j < frame->width; j++) {
      count += p[j] < 32;
    }
    p += frame->linesize[0];
  }
  return count * 100 / (frame->width * frame->height) >= 95;
}

bool IsFrameBlack(const AVFrame* input) {
  static const enum AVPixelFormat kPixelFormats[] = {
      AV_PIX_FMT_YUV410P, AV_PIX_FMT_YUV420P, AV_PIX_FMT_GRAY8,
      AV_PIX_FMT_NV12,    AV_PIX_FMT_NV21,    AV_PIX_FMT_YUV444P,
      AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV411P, AV_PIX_FMT_NONE};
  AVPixelFormat format = avcodec_find_best_pix_fmt_of_list(
      kPixelFormats, AVPixelFormat(input->format), /*alpha=*/false,
      /*loss_ptr=*/nullptr);
  return IsFrameBlackImpl(
      input->format == format ? input : ConvertFrame(input, format).get());
}

auto GetThumbnailFrame(AVIOContext* io_context, ThumbnailOptions options,
                       std::atomic_bool* interrupted) {
  auto context = CreateFormatContext(io_context);
  auto stream = av_find_best_stream(context.get(), AVMEDIA_TYPE_VIDEO, -1, -1,
                                    nullptr, 0);
  CheckAVError(stream, "av_find_best_stream");
  if (context->duration > 0) {
    if (int err = av_seek_frame(context.get(), -1, context->duration / 10, 0);
        err < 0) {
      if (err != AVERROR(EPERM)) {
        CheckAVError(av_seek_frame(context.get(), 0, 0,
                                   AVSEEK_FLAG_BYTE | AVSEEK_FLAG_BACKWARD),
                     "av_seek_frame");
      }
    }
  }
  auto codec_context = CreateCodecContext(context.get(), stream);
  auto size = GetThumbnailSize({codec_context->width, codec_context->height},
                               options.size);
  Graph read_graph =
      std::move(
          GraphBuilder(context.get(), stream, codec_context.get())
              .AddFilter("scale", {{"width", std::to_string(size.width)},
                                   {"height", std::to_string(size.height)}}))
          .Build();
  Graph thumbnail_graph =
      std::move(GraphBuilder(read_graph).AddFilter("thumbnail", {})).Build();

  int stream_orientation = [&] {
    const AVPacketSideData* stream_matrix = av_packet_side_data_get(
        context->streams[stream]->codecpar->coded_side_data,
        context->streams[stream]->codecpar->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX);
    if (stream_matrix != nullptr) {
      return GetExifOrientation(
          reinterpret_cast<const int32_t*>(stream_matrix->data));
    } else {
      return 0;
    }
  }();

  int read_frame_count = 0;
  int written_frame_count = 0;
  while (true) {
    auto received_frame = thumbnail_graph.PullFrame();
    if (received_frame) {
      if (*received_frame == nullptr) {
        throw LogicError("Couldn't extract any frame.");
      }
      return std::move(*received_frame);
    }
    received_frame = read_graph.PullFrame();
    if (received_frame) {
      if (!received_frame->get() || written_frame_count == 0 ||
          !IsFrameBlack(received_frame->get())) {
        written_frame_count++;
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
    read_frame_count++;
    read_graph.WriteFrame(read_frame_count < 200 ? frame.get() : nullptr);
  }
}

std::string GenerateThumbnail(AVIOContext* io_context, ThumbnailOptions options,
                              std::atomic_bool* interrupted) {
  return EncodeFrame(GetThumbnailFrame(io_context, options, interrupted),
                     options, interrupted);
}

}  // namespace

Task<std::string> ThumbnailGenerator::operator()(
    const AbstractCloudProvider* provider, AbstractCloudProvider::File file,
    ThumbnailOptions options, stdx::stop_token stop_token) const {
  std::unique_ptr<AVIOContext, AVIOContextDeleter> io_context;
  std::atomic_bool interrupted = false;
  stdx::stop_callback cb(stop_token, [&] { interrupted = true; });
  co_return co_await thread_pool_->Do(stop_token, [&] {
    try {
      io_context = CreateIOContext(event_loop_, provider, std::move(file),
                                   std::move(stop_token));
      return GenerateThumbnail(io_context.get(), options, &interrupted);
    } catch (const std::exception& e) {
      throw ThumbnailGeneratorException(e.what());
    }
  });
}

}  // namespace coro::cloudstorage::util
