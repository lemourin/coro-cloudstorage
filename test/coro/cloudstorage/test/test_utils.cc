#include "coro/cloudstorage/test/test_utils.h"

#include <fmt/format.h>

#include <fstream>

#include "coro/cloudstorage/util/file_utils.h"
#include "coro/cloudstorage/util/string_utils.h"
#include "coro/util/raii_utils.h"
#include "coro/util/regex.h"

#ifdef _WIN32
#include <windows.h>
#endif

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
}

namespace coro::cloudstorage::test {

namespace {

namespace re = coro::util::re;

using ::coro::util::AtScopeExit;
using ::coro::cloudstorage::util::StrCat;
using ::coro::cloudstorage::util::FileDeleter;

struct AVFilterGraphDeleter {
  void operator()(AVFilterGraph* graph) const { avfilter_graph_free(&graph); }
};

struct AVFrameDeleter {
  void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

std::string EscapePath(std::string_view path) {
  return re::regex_replace(
      re::regex_replace(std::string(path), re::regex("\\\\"), "/"),
      re::regex("\\:"), R"(\\\\$&)");
}

class TemporaryFile {
 public:
  TemporaryFile() {
#ifdef _WIN32
    std::string path(MAX_PATH, 0);
    if (GetTempFileNameA(kTestRunDirectory.data(), "tmp", 0, path.data()) ==
        0) {
      throw RuntimeError("GetTempFileNameA error");
    }
    path.resize(strlen(path.c_str()));
    path_ = std::move(path);
    file_.reset(std::fopen(path_.c_str(), "wb+"));
#else
    std::string tmpl = StrCat(kTestRunDirectory, "/tmp.XXXXXX");
    int fd = mkstemp(tmpl.data());
    if (fd < 0) {
      throw RuntimeError("mkstemp error");
    }
    path_ = std::move(tmpl);
    file_.reset(fdopen(fd, "wb+"));
#endif
  }

  TemporaryFile(const TemporaryFile&) = delete;
  TemporaryFile(TemporaryFile&&) = default;
  TemporaryFile& operator=(const TemporaryFile&) = delete;
  TemporaryFile& operator=(TemporaryFile&&) = delete;

  ~TemporaryFile() {
    if (file_) {
      std::remove(path_.c_str());
    }
  }

  std::FILE* stream() const { return file_.get(); }
  std::string_view path() const { return path_; }

 private:
  std::string path_;
  std::unique_ptr<std::FILE, FileDeleter> file_;
};

std::string GetFileContent(std::string_view path) {
  std::ifstream stream(std::string(path), std::fstream::binary);
  std::string data;
  std::string buffer(4096, 0);
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    data += std::string_view(buffer.data(), stream.gcount());
  }
  return data;
}

void WriteFileContent(std::string_view path, std::string_view content) {
  std::ofstream stream(std::string(path), std::fstream::binary);
  if (!stream) {
    throw std::runtime_error("File not writeable.");
  }
  stream << content;
}

bool AreVideosEquivImpl(std::string_view path1, std::string_view path2,
                        std::string_view format) {
  std::unique_ptr<AVFilterGraph, AVFilterGraphDeleter> graph{
      avfilter_graph_alloc()};
  if (!graph) {
    throw RuntimeError("avfilter_graph_alloc");
  }
  std::string graph_str =
      format == "png" || format == "mjpeg"
          ? fmt::format(
                "movie=filename={}:f={format}:dec_threads=1 [v1];"
                "movie=filename={}:f={format}:dec_threads=1 [v2];"
                "[v1][v2] msad [vout];"
                "[vout] buffersink@output;",
                EscapePath(path1), EscapePath(path2),
                fmt::arg("format", format))
          : fmt::format(
                "movie=filename={}:f={format}:dec_threads=1:s=dv+da [v1][a1];"
                "movie=filename={}:f={format}:dec_threads=1:s=dv+da [v2][a2];"
                "[v1][v2] msad [vout];"
                "[vout] buffersink@output;"
                "[a1] abuffersink@output1;"
                "[a2] abuffersink@output2;",
                EscapePath(path1), EscapePath(path2),
                fmt::arg("format", format));
  if (avfilter_graph_parse(graph.get(), graph_str.c_str(), nullptr, nullptr,
                           nullptr) != 0) {
    throw RuntimeError("avfilter_graph_parse2 error");
  }

  if (avfilter_graph_config(graph.get(), nullptr) != 0) {
    throw RuntimeError("avfilter_graph_config error");
  }

  AVFilterContext* sink =
      avfilter_graph_get_filter(graph.get(), "buffersink@output");
  AVFilterContext* asink1 =
      avfilter_graph_get_filter(graph.get(), "abuffersink@output1");
  AVFilterContext* asink2 =
      avfilter_graph_get_filter(graph.get(), "abuffersink@output2");
  bool video_drained = false;
  bool audio_drained = !asink1 && !asink2;
  while (!video_drained || !audio_drained) {
    std::unique_ptr<AVFrame, AVFrameDeleter> frame;
    if (!video_drained) {
      frame.reset(av_frame_alloc());
      if (!frame) {
        throw RuntimeError("av_frame_alloc");
      }
      int err = av_buffersink_get_frame(sink, frame.get());
      if (err == AVERROR_EOF) {
        video_drained = true;
      } else if (err != 0) {
        throw RuntimeError("av_buffersink_get_frame");
      } else {
        const AVDictionaryEntry* entry =
            av_dict_get(frame->metadata, "lavfi.msad.msad_avg", nullptr, 0);
        if (entry == nullptr) {
          throw RuntimeError("lavfi.msad.msad_avg attribute missing");
        }
        if (std::abs(std::stod(entry->value)) > 0.01) {
          return false;
        }
      }
    }
    while (!audio_drained) {
      std::unique_ptr<AVFrame, AVFrameDeleter> frame1{av_frame_alloc()};
      std::unique_ptr<AVFrame, AVFrameDeleter> frame2{av_frame_alloc()};
      if (!frame1 || !frame2) {
        throw RuntimeError("av_frame_alloc");
      }
      int err1 = av_buffersink_get_frame(asink1, frame1.get());
      int err2 = av_buffersink_get_frame(asink2, frame2.get());
      if (err1 == AVERROR_EOF && err2 == AVERROR_EOF) {
        audio_drained = true;
      } else if (err1 != 0 || err2 != 0) {
        throw RuntimeError("av_buffersink_get_frame");
      } else {
        if (frame1->linesize[0] != frame2->linesize[0]) {
          return false;
        }
        if (memcmp(frame1->data[0], frame2->data[0], frame1->linesize[0]) !=
            0) {
          return false;
        }
        if (frame &&
            av_compare_ts(frame->pts, av_buffersink_get_time_base(sink),
                          frame1->pts,
                          av_buffersink_get_time_base(asink1)) < 0) {
          break;
        }
      }
    }
  }

  return true;
}

void WriteFileContent(std::FILE* file, std::string_view content) {
  if (std::fwrite(content.data(), 1, content.size(), file) != content.size()) {
    throw RuntimeError("fwrite error");
  }
  std::fflush(file);
}

}  // namespace

std::string GetTestFileContent(std::string_view filename) {
  return GetFileContent(StrCat(kTestDataDirectory, '/', filename));
}

void WriteTestFileContent(std::string_view filename, std::string_view content) {
  WriteFileContent(StrCat(kTestDataDirectory, '/', filename), content);
}

bool AreVideosEquiv(std::string_view video1, std::string_view video2,
                    std::string_view format) {
  if (video1 == video2) {
    return true;
  }
  TemporaryFile f1;
  TemporaryFile f2;
  WriteFileContent(f1.stream(), video1);
  WriteFileContent(f2.stream(), video2);
  return AreVideosEquivImpl(f1.path(), f2.path(), format);
}

}  // namespace coro::cloudstorage::test