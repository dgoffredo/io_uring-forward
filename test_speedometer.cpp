#include <chrono>
#include <iostream>
#include <random>

#include "speedometer.h"

int main() {
  Speedometer<int> rate{std::chrono::seconds(1)};
  std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();

  std::mt19937 generator;
  std::normal_distribution<double> normal{100, 10};
  const auto random = [&]() -> int { return std::lround(normal(generator)); };

  for (int i = 0;; ++i, now += std::chrono::milliseconds(random())) {
    std::cout << "After i=" << i << ": " << rate.update(now, i) << '\n';
  }
}
