#ifndef CORO_CLOUDSTORAGE_RANDOM_NUMBER_GENERATOR_H
#define CORO_CLOUDSTORAGE_RANDOM_NUMBER_GENERATOR_H

#include <coro/stdx/any_invocable.h>

#include <random>

namespace coro::cloudstorage::util {

class RandomNumberGenerator {
 private:
  using result_type = std::uint_fast32_t;

 public:
  explicit RandomNumberGenerator(stdx::any_invocable<result_type()> generator)
      : generator_(std::move(generator)) {}

  template <typename T>
  T Get() {
    std::uniform_int_distribution<std::conditional_t<
        std::is_same_v<uint8_t, T>, result_type,
        std::conditional_t<std::is_same_v<int8_t, T>,
                           std::make_signed_t<result_type>, T>>>
        dist;
    return T(dist(generator_));
  }

 private:
  class Generator {
   public:
    using result_type = RandomNumberGenerator::result_type;

    explicit Generator(stdx::any_invocable<result_type()> generator)
        : generator_(std::move(generator)) {}

    result_type operator()() { return generator_(); }

    constexpr static result_type(min)() { return 0; }
    constexpr static result_type(max)() { return UINT_FAST32_MAX; }

   private:
    stdx::any_invocable<result_type()> generator_;
  } generator_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_RANDOM_NUMBER_GENERATOR_H
