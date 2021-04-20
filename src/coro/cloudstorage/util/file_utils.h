#ifndef CORO_CLOUDSTORAGE_FUSE_FILE_UTILS_H
#define CORO_CLOUDSTORAGE_FUSE_FILE_UTILS_H

#include <coro/generator.h>
#include <coro/util/thread_pool.h>

#include <cstdio>
#include <string>

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
Task<int64_t> GetFileSize(coro::util::ThreadPool* thread_pool, std::FILE* file);
Task<> WriteFile(coro::util::ThreadPool* thread_pool, std::FILE* file,
                 int64_t offset, std::string_view data);
Generator<std::string> ReadFile(coro::util::ThreadPool* thread_pool,
                                std::FILE* file);
Task<std::string> ReadFile(coro::util::ThreadPool* thread_pool, std::FILE* file,
                           int64_t offset, size_t size);

#ifdef __ANDROID__
void SetAndroidTempDirectory(std::string path);
#endif

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_FUSE_FILE_UTILS_H
