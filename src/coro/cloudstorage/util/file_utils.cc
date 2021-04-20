#include "file_utils.h"

#include <cstdlib>

namespace coro::cloudstorage::util {

namespace {

using ::coro::util::ThreadPool;

#ifdef __ANDROID__
std::string gAndroidTempDirectory;
#endif

}  // namespace

#ifdef __ANDROID__
void SetAndroidTempDirectory(std::string path) {
  gAndroidTempDirectory = std::move(path);
}
#endif

int64_t Ftell(std::FILE* file) {
#ifdef WIN32
  return _ftelli64(file);
#else
  return ftello(file);
#endif
}

int Fseek(std::FILE* file, int64_t offset, int origin) {
#ifdef WIN32
  return _fseeki64(file, offset, origin);
#else
  return fseeko(file, offset, origin);
#endif
}

std::unique_ptr<std::FILE, FileDeleter> CreateTmpFile() {
  return std::unique_ptr<std::FILE, FileDeleter>([] {
#if defined(__ANDROID__)
    std::string name = gAndroidTempDirectory + "/tmp.XXXXXX";
    int fno = mkstemp(name.data());
    if (name.empty()) {
      throw std::runtime_error("couldn't create tmpfile");
    }
    std::remove(name.c_str());
    return fdopen(fno, "w+");
#elif defined(_MSC_VER)
    std::FILE* file;
    if (tmpfile_s(&file) != 0) {
      throw std::runtime_error("couldn't create tmpfile");
    }
    return file;
#else
    std::FILE* file = std::tmpfile();
    if (!file) {
      throw std::runtime_error("couldn't create tmpfile");
    }
    return file;
#endif
  }());
}

Task<int64_t> GetFileSize(ThreadPool* thread_pool, std::FILE* file) {
  return thread_pool->Invoke([=] {
    if (Fseek(file, 0, SEEK_END) != 0) {
      throw std::runtime_error("fseek failed");
    }
    return Ftell(file);
  });
}

Generator<std::string> ReadFile(ThreadPool* thread_pool, std::FILE* file) {
  const int kBufferSize = 4096;
  char buffer[kBufferSize];
  if (co_await thread_pool->Invoke(Fseek, file, 0, SEEK_SET) != 0) {
    throw std::runtime_error("fseek failed");
  }
  while (feof(file) == 0) {
    size_t size =
        co_await thread_pool->Invoke(fread, &buffer, 1, kBufferSize, file);
    if (ferror(file) != 0) {
      throw std::runtime_error("read error");
    }
    co_yield std::string(buffer, size);
  }
}

Task<std::string> ReadFile(ThreadPool* thread_pool, std::FILE* file,
                           int64_t offset, size_t size) {
  return thread_pool->Invoke([=] {
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

Task<> WriteFile(ThreadPool* thread_pool, std::FILE* file, int64_t offset,
                 std::string_view data) {
  return thread_pool->Invoke([=] {
    if (Fseek(file, offset, SEEK_SET) != 0) {
      throw std::runtime_error("fseek failed " + std::to_string(offset) + " " +
                               std::to_string(errno));
    }
    if (fwrite(data.data(), 1, data.size(), file) != data.size()) {
      throw std::runtime_error("fwrite failed");
    }
  });
}

}  // namespace coro::cloudstorage::util