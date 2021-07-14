#ifndef CORO_CLOUDSTORAGE_CLOUD_EXCEPTION_H
#define CORO_CLOUDSTORAGE_CLOUD_EXCEPTION_H

#include <string>

namespace coro::cloudstorage {

class CloudException : public std::exception {
 public:
  enum class Type { kNotFound, kUnauthorized, kRetry, kUnknown };

  explicit CloudException(std::string message)
      : type_(Type::kUnknown), message_(std::move(message)) {}

  explicit CloudException(Type type)
      : type_(type),
        message_(std::string("CloudException: ") + TypeToString(type)) {}

  Type type() const { return type_; }
  const char* what() const noexcept final { return message_.c_str(); }

  static const char* TypeToString(Type type) {
    switch (type) {
      case Type::kNotFound:
        return "NotFound";
      case Type::kUnauthorized:
        return "Unauthorized";
      case Type::kRetry:
        return "Retry";
      default:
        return "Unknown";
    }
  }

 private:
  Type type_;
  std::string message_;
};

}  // namespace coro::cloudstorage

#endif  // CORO_CLOUDSTORAGE_CLOUD_EXCEPTION_H
