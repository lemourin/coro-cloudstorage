#ifndef CORO_CLOUDSTORAGE_TEST_TEST_UTILS_H
#define CORO_CLOUDSTORAGE_TEST_TEST_UTILS_H

#include <string>
#include <string_view>

#include "coro/cloudstorage/util/file_utils.h"

namespace coro::cloudstorage::test {

extern const std::string_view kTestDataDirectory;
extern const std::string_view kTestRunDirectory;

class TemporaryFile {
 public:
  TemporaryFile();
  TemporaryFile(const TemporaryFile&) = delete;
  TemporaryFile(TemporaryFile&&) = default;
  TemporaryFile& operator=(const TemporaryFile&) = delete;
  TemporaryFile& operator=(TemporaryFile&&) = delete;

  ~TemporaryFile();

  std::FILE* stream() const { return file_.get(); }
  std::string_view path() const { return path_; }

 private:
  std::string path_;
  std::unique_ptr<std::FILE, coro::cloudstorage::util::FileDeleter> file_;
};

std::string GetTestFileContent(std::string_view filename);

void WriteTestFileContent(std::string_view filename, std::string_view content);

bool AreVideosEquiv(std::string_view video1, std::string_view video2,
                    std::string_view format);

}  // namespace coro::cloudstorage::test

#endif  // CORO_CLOUDSTORAGE_TEST_TEST_UTILS_H
