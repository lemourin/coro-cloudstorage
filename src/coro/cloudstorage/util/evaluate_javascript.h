#ifndef CORO_CLOUDSTORAGE_UTIL_EVALUATE_JAVASCRIPT_H
#define CORO_CLOUDSTORAGE_UTIL_EVALUATE_JAVASCRIPT_H

#include <span>
#include <string>
#include <vector>

namespace coro::cloudstorage::util::js {

struct Function {
  std::string name;
  std::vector<std::string> args;
  std::string source;
};

std::string EvaluateJavascript(const Function& function,
                               std::span<const std::string> arguments);

}  // namespace coro::cloudstorage::util::js

#endif  // CORO_CLOUDSTORAGE_UTIL_EVALUATE_JAVASCRIPT_H