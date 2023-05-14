#include "coro/cloudstorage/util/avio_context.h"

#include "coro/cloudstorage/util/generator_utils.h"
#include "coro/http/http.h"

namespace coro::cloudstorage::util {

namespace {

struct Context {
  const coro::util::EventLoop* event_loop;
  const AbstractCloudProvider* provider;
  AbstractCloudProvider::File file;
  int64_t offset;
  stdx::stop_token stop_token;
  std::optional<Generator<std::string>> generator;
  std::optional<Generator<std::string>::iterator> it;
};

}  // namespace

void AVIOContextDeleter::operator()(AVIOContext* context) {
  delete reinterpret_cast<Context*>(context->opaque);
  av_free(context->buffer);
  avio_context_free(&context);
}

std::unique_ptr<AVIOContext, AVIOContextDeleter> CreateIOContext(
    const coro::util::EventLoop* event_loop,
    const AbstractCloudProvider* provider, AbstractCloudProvider::File file,
    stdx::stop_token stop_token) {
  const int kBufferSize = 32 * 1024;
  auto buffer = static_cast<uint8_t*>(av_malloc(kBufferSize));
  std::unique_ptr<AVIOContext, AVIOContextDeleter> context(avio_alloc_context(
      buffer, kBufferSize, /*write_flag=*/0,
      new Context{event_loop, provider, std::move(file), 0,
                  std::move(stop_token)},
      [](void* opaque, uint8_t* buf, int buf_size) -> int {
        auto data = reinterpret_cast<Context*>(opaque);
        return data->event_loop->Do([&]() -> Task<int> {
          try {
            if (data->offset == -1) {
              co_return AVERROR(EIO);
            }
            if (data->offset == data->file.size) {
              co_return AVERROR_EOF;
            }
            if (data->stop_token.stop_requested()) {
              co_return AVERROR(EINTR);
            }
            if (!data->generator) {
              data->generator = data->provider->GetFileContent(
                  data->file, http::Range{.start = data->offset},
                  data->stop_token);
              data->it = co_await data->generator->begin();
            }
            auto buffer = co_await http::GetBody(util::Take(
                *data->generator, *data->it, static_cast<size_t>(buf_size)));
            data->offset += buffer.size();
            memcpy(buf, buffer.data(), buffer.size());
            if (buffer.size() == 0) {
              co_return AVERROR_EOF;
            }
            co_return static_cast<int>(buffer.size());
          } catch (...) {
            data->generator.reset();
            data->offset = -1;
            co_return AVERROR(EIO);
          }
        });
      },
      /*write_packet=*/nullptr,
      [](void* opaque, int64_t offset, int whence) -> int64_t {
        auto data = reinterpret_cast<Context*>(opaque);
        whence &= ~AVSEEK_FORCE;
        if (whence == AVSEEK_SIZE) {
          return data->file.size.value_or(AVERROR(ENOSYS));
        }
        int64_t new_offset = -1;
        if (whence == SEEK_SET) {
          new_offset = offset;
        } else if (whence == SEEK_CUR) {
          new_offset = data->offset + offset;
        } else if (whence == SEEK_END) {
          auto size = data->file.size;
          if (!size) {
            return AVERROR(ENOSYS);
          }
          new_offset = *size + offset;
        } else {
          return AVERROR(EINVAL);
        }
        if (data->offset == new_offset) {
          return new_offset;
        }
        return data->event_loop->Do([&]() -> Task<int64_t> {
          try {
            if (data->stop_token.stop_requested()) {
              data->offset = -1;
              co_return AVERROR(EINTR);
            }
            data->generator = data->provider->GetFileContent(
                data->file, http::Range{.start = new_offset}, data->stop_token);
            data->it = co_await data->generator->begin();
            co_return data->offset = new_offset;
          } catch (...) {
            data->generator.reset();
            data->offset = -1;
            co_return AVERROR(EIO);
          }
        });
      }));
  if (!context) {
    throw RuntimeError("avio_alloc_context");
  }
  return context;
}

}  // namespace coro::cloudstorage::util