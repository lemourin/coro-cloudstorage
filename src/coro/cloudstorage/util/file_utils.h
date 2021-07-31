#ifndef CORO_CLOUDSTORAGE_FUSE_FILE_UTILS_H
#define CORO_CLOUDSTORAGE_FUSE_FILE_UTILS_H

#include <cstdio>
#include <span>
#include <string>

#include "coro/generator.h"
#include "coro/util/thread_pool.h"

namespace coro::cloudstorage::util {

struct FileDeleter {
  void operator()(std::FILE* file) const {
    if (file) {
      fclose(file);
    }
  }
};

int64_t Ftell(std::FILE* file);
int Fseek(std::FILE* file, int64_t offset, int origin);
std::unique_ptr<std::FILE, FileDeleter> CreateTmpFile();
std::string GetFileName(std::string path);
std::string GetDirectoryPath(std::string path);
std::span<const std::string> GetDirectoryPath(
    std::span<const std::string> path);

template <typename ThreadPool>
Task<int64_t> GetFileSize(ThreadPool* thread_pool, std::FILE* file) {
  co_return co_await thread_pool->Do([=] {
    if (Fseek(file, 0, SEEK_END) != 0) {
      throw std::runtime_error("fseek failed");
    }
    return Ftell(file);
  });
}

template <typename ThreadPool>
Task<> WriteFile(ThreadPool* thread_pool, std::FILE* file, int64_t offset,
                 std::string_view data) {
  co_return co_await thread_pool->Do([=] {
    if (Fseek(file, offset, SEEK_SET) != 0) {
      throw std::runtime_error("fseek failed " + std::to_string(offset) + " " +
                               std::to_string(errno));
    }
    if (fwrite(data.data(), 1, data.size(), file) != data.size()) {
      throw std::runtime_error("fwrite failed");
    }
  });
}

template <typename ThreadPool>
Generator<std::string> ReadFile(ThreadPool* thread_pool, std::FILE* file) {
  const int kBufferSize = 4096;
  char buffer[kBufferSize];
  if (co_await thread_pool->Do(Fseek, file, 0, SEEK_SET) != 0) {
    throw std::runtime_error("fseek failed");
  }
  while (feof(file) == 0) {
    size_t size =
        co_await thread_pool->Do(fread, &buffer, 1, kBufferSize, file);
    if (ferror(file) != 0) {
      throw std::runtime_error("read error");
    }
    co_yield std::string(buffer, size);
  }
}

template <typename ThreadPool>
Task<std::string> ReadFile(ThreadPool* thread_pool, std::FILE* file,
                           int64_t offset, size_t size) {
  co_return co_await thread_pool->Do([=] {
    if (Fseek(file, offset, SEEK_SET) != 0) {
      throw std::runtime_error("fseek failed " + std::to_string(offset));
    }
    std::string buffer(size, 0);
    if (fread(buffer.data(), 1, size, file) != size) {
      throw std::runtime_error("fread failed");
    }
    return buffer;
  });
}

#ifdef __ANDROID__
void SetAndroidTempDirectory(std::string path);
#endif

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_FILE_UTILS_H
