#ifndef CORO_CLOUDSTORAGE_TEST_MATCHER_H
#define CORO_CLOUDSTORAGE_TEST_MATCHER_H

#include <coro/stdx/any_invocable.h>
#include <coro/stdx/concepts.h>

#include <string>
#include <string_view>
#include <utility>

namespace coro::cloudstorage::test {

template <typename T>
class MatcherBase {
 public:
  template <typename F>
    requires requires(const F& func, const T& d) {
      { func(d) } -> stdx::same_as<bool>;
    }
  MatcherBase(F f) : f_(std::move(f)) {}
  MatcherBase(T data)
      : f_([d = std::move(data)](const T& a) { return d == a; }) {}

  bool Matches(const T& data) const { return f_(data); }

 private:
  stdx::any_invocable<bool(const T&) const> f_;
};

template <typename T>
class Matcher {
 public:
  template <typename... Args>
  Matcher(Args&&... args) : b_(std::forward<Args>(args)...) {}

  bool Matches(const T& data) { return b_.Matches(data); }

 private:
  MatcherBase<T> b_;
};

template <>
class Matcher<std::string> {
 public:
  template <typename... Args>
  Matcher(Args&&... args) : b_(std::forward<Args>(args)...) {}
  Matcher(std::string_view data) : b_(std::string(data)) {}
  Matcher(const char* data) : b_(data) {}
  Matcher(std::string data) : b_(std::move(data)) {}

  bool Matches(std::string_view data) const {
    return b_.Matches(std::string{data});
  }

 private:
  MatcherBase<std::string> b_;
};

}  // namespace coro::cloudstorage::test

#endif  // CORO_CLOUDSTORAGE_TEST_MATCHER_H
