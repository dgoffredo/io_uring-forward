#pragma once

#include <cassert>
#include <chrono>
#include <deque>
#include <limits>
#include <utility>

template <typename Number>
class Speedometer {
  std::deque<std::pair<std::chrono::steady_clock::time_point, Number>>
      measurements;
  const std::chrono::steady_clock::duration interval;

 public:
  // Create a speedometer that keeps track of the last `interval` worth of data.
  explicit Speedometer(std::chrono::steady_clock::duration interval);

  // Add a data point to the time series. Return the running average rate of
  // change over approximately the last `interval`, in units of per
  // `interval`.
  double update(Number total_count);
  double update(std::chrono::steady_clock::time_point when, Number total_count);
};

template <typename Number>
Speedometer<Number>::Speedometer(std::chrono::steady_clock::duration interval)
    : interval(interval) {}

template <typename Number>
double Speedometer<Number>::update(Number total_count) {
  return update(std::chrono::steady_clock::now(), total_count);
}

template <typename Number>
double Speedometer<Number>::update(std::chrono::steady_clock::time_point when,
                                   Number total_count) {
  measurements.emplace_back(when, total_count);

  if (measurements.size() == 1) {
    return std::numeric_limits<double>::infinity();
  }

  // Steady time doesn't flow backwards.
  assert(when >= measurements[measurements.size() - 2].first);

  // Pop measurements from the past while doing so leaves at least an
  // `interval`'s worth of data.
  const auto by_time = [](const auto& left, const auto& right) {
    return left.first < right;
  };
  const auto horizon = std::lower_bound(
      measurements.begin(), measurements.end(), when - interval, by_time);
  measurements.erase(measurements.begin(), horizon);

  // That `erase` will leave behind at least what we just inserted and one
  // element before it.
  assert(measurements.size() > 1);
  const auto& [before, oldest] = measurements.front();
  const auto& [after, newest] = measurements.back();
  // The difference in measured value, divided by the time elapsed in units
  // of `interval`.
  return (newest - oldest) /
         (double((after - before).count()) / interval.count());
}
