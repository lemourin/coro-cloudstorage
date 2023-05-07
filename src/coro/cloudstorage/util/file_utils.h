#ifndef CORO_CLOUDSTORAGE_FUSE_FILE_UTILS_H
#define CORO_CLOUDSTORAGE_FUSE_FILE_UTILS_H

#include <cstdio>
#include <span>
#include <string>

#include "coro/generator.h"
#include "coro/util/thread_pool.h"

namespace coro::cloudstorage::util {

extern const char kPathSeparator;

struct FileDeleter {
  void operator()(std::FILE* file) const {
    if (file) {
      fclose(file);
    }
  }
};

std::string GetCacheDirectory();
std::string GetConfigDirectory();
void CreateDirectory(std::string_view path);
void RemoveDirectory(std::string_view path);
bool IsPathSeparator(char);

int64_t Ftell(std::FILE* file);
int Fseek(std::FILE* file, int64_t offset, int origin);
std::unique_ptr<std::FILE, FileDeleter> CreateTmpFile();
std::string GetFileName(std::string path);
std::string GetDirectoryPath(std::string path);
std::span<const std::string> GetDirectoryPath(
    std::span<const std::string> path);

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
