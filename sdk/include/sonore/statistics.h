// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: running mean, variance, minimum, maximum and RMS of a stream
// of values.
//
// ── Why Welford, not sums ───────────────────────────────────────────────────
//
// The obvious version keeps a sum and a sum of squares and computes the
// variance as E[x^2] - E[x]^2 at the end. That subtraction is catastrophic
// cancellation: for values near 1e9 with a spread of 1, both terms are about
// 1e18 and their difference is below a double's resolution, so the variance
// comes out as zero, or negative, or noise. A DC offset with a small ripple
// on it -- exactly what a meter measures -- is that case.
//
// Welford's update keeps the running mean and the sum of squared deviations
// FROM it, so nothing large is ever subtracted from anything large: one
// division per value, and the variance is as accurate as a two-pass
// computation over the stored data. The RMS comes out of the same two numbers
// (mean squared plus variance), and two accumulators merge exactly (Chan et
// al.), so per-block statistics can be summed into per-second ones.
//
// float input is accumulated in double: a float running mean over a few
// million values drifts visibly, and the extra precision costs nothing that
// shows. Every member is allocation-free and noexcept -- usable on the audio
// thread.
#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace sonore {

/**
 * Running statistics of the values added to it.
 *
 * With no values, the mean, variance and RMS are 0, the minimum is +infinity
 * and the maximum -infinity (which is what makes merging an empty accumulator
 * a no-op). A NaN added poisons the mean, variance and RMS -- the honest
 * answer to "what is the average of these" when one of them is not a number
 * -- but is skipped by the minimum and maximum.
 */
template <typename FloatType>
class StatisticsAccumulator {
  static_assert(std::is_floating_point<FloatType>::value,
                "StatisticsAccumulator is for float, double or long double");
  using Accumulator =
      typename std::conditional<(sizeof(FloatType) < sizeof(double)), double, FloatType>::type;

public:
  /** [audio thread] Adds one value. */
  void addValue(FloatType value) noexcept {
    const Accumulator x = (Accumulator) value;
    ++count_;
    const Accumulator delta = x - mean_;
    mean_ += delta / (Accumulator) count_;
    m2_ += delta * (x - mean_);
    if (x < min_) min_ = x;
    if (x > max_) max_ = x;
  }

  /** [audio thread] Adds `count` values. */
  void addValues(const FloatType* values, size_t count) noexcept {
    for (size_t i = 0; i < count; ++i) addValue(values[i]);
  }

  /** [audio thread] Back to no values. */
  void reset() noexcept { *this = StatisticsAccumulator(); }

  /** [audio thread] Merges another accumulator's values into this one, as
   *  if every value it saw had been added here. */
  StatisticsAccumulator& operator+=(const StatisticsAccumulator& other) noexcept {
    if (other.count_ == 0) return *this;
    if (count_ == 0) {
      *this = other;
      return *this;
    }
    const Accumulator na = (Accumulator) count_, nb = (Accumulator) other.count_;
    const Accumulator n = na + nb;
    const Accumulator delta = other.mean_ - mean_;
    mean_ += delta * (nb / n);
    m2_ += other.m2_ + delta * delta * (na * nb / n);
    count_ += other.count_;
    if (other.min_ < min_) min_ = other.min_;
    if (other.max_ > max_) max_ = other.max_;
    return *this;
  }

  /** How many values were added, NaNs included. */
  size_t getCount() const noexcept { return count_; }

  /** The mean; 0 with no values, NaN once a NaN was added. */
  FloatType getAverage() const noexcept { return count_ > 0 ? (FloatType) mean_ : FloatType(0); }

  /** The POPULATION variance: the mean squared deviation, divided by n. */
  FloatType getVariance() const noexcept {
    return count_ > 0 ? (FloatType) nonNegative(m2_ / (Accumulator) count_) : FloatType(0);
  }
  /** The SAMPLE variance, divided by n - 1: the unbiased estimate when the
   *  values are a sample of something larger. 0 for fewer than two values. */
  FloatType getSampleVariance() const noexcept {
    return count_ > 1 ? (FloatType) nonNegative(m2_ / (Accumulator) (count_ - 1)) : FloatType(0);
  }

  /** The square root of getVariance(). */
  FloatType getStandardDeviation() const noexcept { return (FloatType) std::sqrt(getVariance()); }
  /** The square root of getSampleVariance(); 0 for fewer than two values. */
  FloatType getSampleStandardDeviation() const noexcept {
    return (FloatType) std::sqrt(getSampleVariance());
  }

  /** Root mean square: sqrt(mean^2 + variance), which is the mean of the
   *  squares without ever having summed them. */
  FloatType getRMS() const noexcept {
    if (count_ == 0) return FloatType(0);
    return (FloatType) std::sqrt(mean_ * mean_ + nonNegative(m2_ / (Accumulator) count_));
  }

  /** The smallest value added, NaNs skipped; +infinity with none. */
  FloatType getMinValue() const noexcept { return (FloatType) min_; }
  /** The largest value added, NaNs skipped; -infinity with none. */
  FloatType getMaxValue() const noexcept { return (FloatType) max_; }

private:
  // m2 is a sum of non-negative terms in exact arithmetic; rounding can leave
  // it a hair below zero, and a negative variance has no square root.
  static Accumulator nonNegative(Accumulator v) noexcept { return v < Accumulator(0) ? Accumulator(0) : v; }

  size_t count_ = 0;
  Accumulator mean_ = 0;
  Accumulator m2_ = 0;
  Accumulator min_ = std::numeric_limits<Accumulator>::infinity();
  Accumulator max_ = -std::numeric_limits<Accumulator>::infinity();
};

} // namespace sonore
