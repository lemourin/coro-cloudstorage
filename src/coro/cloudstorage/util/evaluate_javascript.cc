#include "coro/cloudstorage/util/evaluate_javascript.h"

#include <duktape.h>

#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace coro::cloudstorage::util::js {

namespace {

class JsException : public std::exception {
 public:
  explicit JsException(std::string what) : what_(std::move(what)) {}

  const char* what() const noexcept override { return what_.c_str(); }

 private:
  std::string what_;
};

struct DukHeapDeleter {
  void operator()(duk_context* ctx) const { duk_destroy_heap(ctx); }
};

}  // namespace

std::string EvaluateJavascript(const Function& function,
                               std::span<const std::string> arguments) {
  std::unique_ptr<duk_context, DukHeapDeleter> ctx(duk_create_heap_default());
  if (!ctx) {
    throw JsException("Can't alloc duk_context.");
  }
  std::stringstream source_code;
  source_code << "{";
  for (size_t i = 0; i < function.args.size(); i++) {
    source_code << "var " << function.args[i] << "=\"" << arguments[i]
                << "\";\n";
  }
  source_code << "function decode()" << function.source << "};decode()";
  if (duk_peval_string(ctx.get(), std::move(source_code).str().c_str()) != 0) {
    throw JsException(duk_safe_to_string(ctx.get(), -1));
  }
  if (const char* str = duk_get_string(ctx.get(), -1)) {
    return str;
  } else {
    throw JsException("Last evaluated value is not a string.");
  }
}

}  // namespace coro::cloudstorage::util::js
