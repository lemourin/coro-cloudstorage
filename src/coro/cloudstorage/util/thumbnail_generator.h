#ifndef CORO_CLOUDSTORAGE_UTIL_GENERATE_THUMBNAIL_H
#define CORO_CLOUDSTORAGE_UTIL_GENERATE_THUMBNAIL_H

#include <coro/cloudstorage/cloud_provider.h>
#include <coro/cloudstorage/util/generator_utils.h>
#include <coro/task.h>
#include <coro/util/thread_pool.h>

#include <future>

extern "C" {
#include <libavformat/avformat.h>
};

namespace coro::cloudstorage::util {

struct ThumbnailOptions {
  int size = 256;
  enum class Codec { PNG, JPEG } codec;
};

std::string GenerateThumbnail(AVIOContext* io_context,
                              ThumbnailOptions options);

class ThumbnailGenerator {
 public:
  ThumbnailGenerator(::coro::util::ThreadPool* thread_pool,
                     ::coro::util::EventLoop* event_loop)
      : thread_pool_(thread_pool), event_loop_(event_loop) {}

  template <typename CloudProvider, IsFile<CloudProvider> File>
  Task<std::string> operator()(CloudProvider* provider, File file,
                               ThumbnailOptions options,
                               stdx::stop_token stop_token) const {
    auto io_context =
        CreateIOContext(provider, std::move(file), std::move(stop_token));
    co_return co_await thread_pool_->Invoke(
        [&] { return GenerateThumbnail(io_context.get(), options); });
  }

 private:
  template <typename CloudProvider, IsFile<CloudProvider> File>
  auto CreateIOContext(CloudProvider* provider, File file,
                       stdx::stop_token stop_token) const {
    struct Context {
      ::coro::util::EventLoop* event_loop;
      CloudProvider* provider;
      File file;
      int64_t offset;
      stdx::stop_token stop_token;
      std::optional<Generator<std::string>> generator;
      std::optional<Generator<std::string>::iterator> it;
    };
    struct AVIOContextDeleter {
      void operator()(AVIOContext* context) {
        delete reinterpret_cast<Context*>(context->opaque);
        av_free(context->buffer);
        avio_context_free(&context);
      }
    };
    const int kBufferSize = 4 * 1024;
    auto buffer = static_cast<uint8_t*>(av_malloc(kBufferSize));
    return std::unique_ptr<AVIOContext, AVIOContextDeleter>(avio_alloc_context(
        buffer, kBufferSize, /*write_flag=*/0,
        new Context{event_loop_, provider, std::move(file), 0,
                    std::move(stop_token)},
        [](void* opaque, uint8_t* buf, int buf_size) -> int {
          auto data = reinterpret_cast<Context*>(opaque);
          std::promise<int> promise;
          data->event_loop->RunOnEventLoop([&]() -> Task<> {
            try {
              if (!data->generator) {
                data->generator = data->provider->GetFileContent(
                    data->file, http::Range{.start = data->offset},
                    data->stop_token);
                data->it = co_await data->generator->begin();
              }
              auto buffer = co_await http::GetBody(
                  util::Take(*data->generator, *data->it, buf_size));
              data->offset += buffer.size();
              memcpy(buf, buffer.data(), buffer.size());
              promise.set_value(buffer.size());
            } catch (...) {
              promise.set_value(-1);
            }
          });
          return promise.get_future().get();
        },
        /*write_packet=*/nullptr,
        [](void* opaque, int64_t offset, int whence) -> int64_t {
          auto data = reinterpret_cast<Context*>(opaque);
          whence &= ~AVSEEK_FORCE;
          if (whence == AVSEEK_SIZE) {
            return CloudProvider::GetSize(data->file).value_or(-1);
          }
          if (whence == SEEK_SET) {
            data->offset = offset;
          } else if (whence == SEEK_CUR) {
            data->offset += offset;
          } else if (whence == SEEK_END) {
            auto size = CloudProvider::GetSize(data->file);
            if (!size) {
              return -1;
            }
            data->offset = *size + offset;
          } else {
            return -1;
          }
          std::promise<int64_t> promise;
          data->event_loop->RunOnEventLoop([&]() -> Task<> {
            try {
              data->generator = data->provider->GetFileContent(
                  data->file, http::Range{.start = data->offset},
                  data->stop_token);
              data->it = co_await data->generator->begin();
              promise.set_value(data->offset);
            } catch (...) {
              promise.set_value(-1);
            }
          });
          return promise.get_future().get();
        }));
  }

  ::coro::util::ThreadPool* thread_pool_;
  ::coro::util::EventLoop* event_loop_;
};

}  // namespace coro::cloudstorage::util

#endif  //  CORO_CLOUDSTORAGE_UTIL_GENERATE_THUMBNAIL_H