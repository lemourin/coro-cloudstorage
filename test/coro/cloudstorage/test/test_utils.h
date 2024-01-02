#ifndef CORO_CLOUDSTORAGE_TEST_TEST_UTILS_H
#define CORO_CLOUDSTORAGE_TEST_TEST_UTILS_H

#include <string>
#include <string_view>

namespace coro::cloudstorage::test {

extern const std::string_view kTestDataDirectory;
extern const std::string_view kTestRunDirectory;

class TestDataScope {
 public:
  TestDataScope();
  TestDataScope(TestDataScope&&) = default;
  TestDataScope(const TestDataScope&) = delete;
  TestDataScope& operator=(TestDataScope&&) = default;
  TestDataScope& operator=(const TestDataScope&) = delete;
  ~TestDataScope();
};

std::string GetTestFileContent(std::string_view filename);

void WriteTestFileContent(std::string_view filename, std::string_view content);

bool AreVideosEquiv(std::string_view video1, std::string_view video2,
                    std::string_view format);

}  // namespace coro::cloudstorage::test

#endif  // CORO_CLOUDSTORAGE_TEST_TEST_UTILS_H
