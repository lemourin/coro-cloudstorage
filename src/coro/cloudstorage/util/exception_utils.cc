#include "coro/cloudstorage/util/exception_utils.h"

#include "coro/http/http_exception.h"

namespace coro::cloudstorage::util {

ErrorMetadata GetErrorMetadata(std::exception_ptr exception) {
  try {
    std::rethrow_exception(exception);
  } catch (const http::HttpException& e) {
    return ErrorMetadata{
        .status = e.status(),
        .what = e.what(),
        .source_location = e.source_location(),
        .stacktrace = e.stacktrace().empty()
                          ? std::nullopt
                          : std::make_optional(e.stacktrace())};
  } catch (const Exception& e) {
    return ErrorMetadata{
        .what = e.what(),
        .source_location = e.source_location(),
        .stacktrace = e.stacktrace().empty()
                          ? std::nullopt
                          : std::make_optional(e.stacktrace())};
  } catch (const std::exception& e) {
    return ErrorMetadata{.what = e.what()};
  }
}

}  // namespace coro::cloudstorage::util