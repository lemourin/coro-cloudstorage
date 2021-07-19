#ifndef CORO_CLOUDSTORAGE_RANDOM_NUMBER_GENERATOR_H
#define CORO_CLOUDSTORAGE_RANDOM_NUMBER_GENERATOR_H

#include <random>

namespace coro::cloudstorage::util {

template <typename RandomEngine>
class RandomNumberGenerator {
 public:
  explicit RandomNumberGenerator(RandomEngine* engine) : engine_(engine) {}

  template <typename T>
  T Get() {
    std::uniform_int_distribution<std::conditional_t<
        std::is_same_v<uint8_t, T>, uint32_t,
        std::conditional_t<std::is_same_v<int8_t, T>, int32_t, T>>>
        dist;
    return T(dist(*engine_));
  }

 private:
  RandomEngine* engine_;
};

}  // namespace coro::cloudstorage::util

#endif  // CORO_CLOUDSTORAGE_RANDOM_NUMBER_GENERATOR_H
