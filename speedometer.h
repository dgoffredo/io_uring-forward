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
  long double sum_of_rates;
  const std::chrono::steady_clock::duration interval;

 public:
  // Create a speedometer that keeps track of the last `interval` worth of data.
  explicit Speedometer(std::chrono::steady_clock::duration interval);

  // Add a data point to the time series.
  void update(Number total_count);
  void update(std::chrono::steady_clock::time_point when, Number total_count);

  // Return the rate of change between approximately `interval` ago and most
  // recently.
  double instant_rate() const;

  // Return the average rate of change starting approximately `interval` ago
  // and ending most recently.
  double average_rate() const;

  // Return the number of measurements in storage.
  std::size_t size() const;
};

template <typename Number>
Speedometer<Number>::Speedometer(std::chrono::steady_clock::duration interval)
    : sum_of_rates(0), interval(interval) {}

template <typename Number>
void Speedometer<Number>::update(Number total_count) {
  update(std::chrono::steady_clock::now(), total_count);
}

template <typename Number>
void Speedometer<Number>::update(std::chrono::steady_clock::time_point when,
                                 Number total_count) {
  measurements.emplace_back(when, total_count);

  if (measurements.size() == 1) {
    return;
  }

  // Steady time doesn't flow backwards.
  assert(when >= measurements[measurements.size() - 2].first);

  // Update are running total of rates with the rate between the point we just
  // inserted and the previous.
  const auto& [t1, v1] = measurements.back();
  const auto& [t2, v2] = *(measurements.end() - 2);
  sum_of_rates += (v2 - v1) / static_cast<long double>((t2 - t1).count());

  // Pop measurements from the past while doing so leaves at least an
  // `interval`'s worth of data. Reduce the running total of rates for each
  // data point we remove.
  const auto by_time = [](const auto& left, const auto& right) {
    return left.first < right;
  };
  auto horizon = std::lower_bound(measurements.begin(), measurements.end(),
                                  when - interval, by_time);
  if (horizon->first > when - interval && horizon != measurements.begin()) {
    // We want the infimum, not the lower bound.
    --horizon;
  }
  for (auto iter = measurements.begin(); iter != horizon; ++iter) {
    assert(iter + 1 != measurements.end());
    const auto& [t1, v1] = *iter;
    const auto& [t2, v2] = *(iter + 1);
    sum_of_rates -= (v2 - v1) / static_cast<long double>((t2 - t1).count());
  }
  measurements.erase(measurements.begin(), horizon);

  assert(measurements.size() > 1);
}

template <typename Number>
double Speedometer<Number>::instant_rate() const {
  switch (measurements.size()) {
    case 0:
      return std::numeric_limits<double>::quiet_NaN();
    case 1:
      return std::numeric_limits<double>::infinity();
  }
  const auto& [past, old_value] = measurements.front();
  const auto& [present, new_value] = measurements.back();
  return (new_value - old_value) /
         (double((present - past).count()) / interval.count());
}

template <typename Number>
double Speedometer<Number>::average_rate() const {
  switch (measurements.size()) {
    case 0:
      return std::numeric_limits<double>::quiet_NaN();
    case 1:
      return std::numeric_limits<double>::infinity();
  }
  return sum_of_rates / measurements.size() * interval.count();
}

template <typename Number>
std::size_t Speedometer<Number>::size() const {
  return measurements.size();
}
