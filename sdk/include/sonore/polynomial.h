// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a polynomial as a value: evaluate it, add it, multiply it.
//
// The polynomials that run per sample in this SDK are written inline where
// they are used -- the sine in fast_math.h, the BLEP residual, the Lagrange
// weights -- and each is tested against the function it approximates. That
// stays: a fixed polynomial is fastest as a fixed expression.
//
// This is for the other job, where the polynomial is BUILT: multiplying out
// (s - p1)(s - p2)... from a filter's poles, the transfer function of a
// cascade as one numerator and one denominator, a waveshaper curve fitted at
// prepare(), a Chebyshev polynomial for harmonic shaping. Doing that by hand
// is a nested loop with an off-by-one in it, which is what this replaces.
//
// Coefficients are in ASCENDING order -- c[0] + c[1] x + c[2] x^2 ... --
// the order the reference uses, and the one where coefficient i is the
// power i, so nobody has to remember which end is which.
//
// The coefficients live in a std::vector, so CONSTRUCTING one allocates, and
// so do the operations that return a new polynomial. EVALUATING does not:
// build at prepare(), evaluate on the audio thread.
#pragma once

#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace sonore {

/**
 * A polynomial held as its coefficients, lowest power first, as the
 * reference's dsp::Polynomial orders them: evaluate it, add, multiply,
 * differentiate and integrate it.
 *
 * A value type that owns a `std::vector` and is never empty (the zero
 * polynomial has one coefficient, 0). Building one, and every operation
 * that returns a new one, allocates: do that in prepare() or on a message
 * thread. Evaluating with operator() does not, and is what the audio thread
 * calls. Nothing trims trailing zeros, so getOrder() is the storage's order.
 */
template <typename T>
class Polynomial {
public:
  /** The zero polynomial. Never empty: one coefficient, 0, so the order and
   *  the evaluation are defined for every polynomial there is. */
  Polynomial() : c_(1, T(0)) {}

  /** From `count` coefficients, lowest power first. */
  Polynomial(const T* coefficients, int count) {
    if (coefficients && count > 0) c_.assign(coefficients, coefficients + count);
    if (c_.empty()) c_.assign(1, T(0));
  }
  /** From a list, lowest power first: `{1, 0, -2}` is 1 - 2x^2. An empty
   *  list gives the zero polynomial. Allocates. */
  Polynomial(std::initializer_list<T> coefficients) : c_(coefficients) {
    if (c_.empty()) c_.assign(1, T(0));
  }
  /** Takes over the vector, lowest power first; an empty one gives the zero
   *  polynomial. */
  explicit Polynomial(std::vector<T> coefficients) : c_(std::move(coefficients)) {
    if (c_.empty()) c_.assign(1, T(0));
  }

  /** The coefficient of x^index. Out of range reads as 0 -- a polynomial of
   *  order 2 has a zero x^5 coefficient, it does not have an error there. */
  T operator[](int index) const {
    return index >= 0 && (size_t) index < c_.size() ? c_[(size_t) index] : T(0);
  }
  /** Writable coefficient; the index must be within getOrder(). */
  T& operator[](int index) { return c_[(size_t) index]; }

  /** The highest power stored -- which can carry a zero coefficient, since
   *  nothing here trims: the product of an order-2 and an order-3 is order 5
   *  even when it cancels, so a caller sizing a buffer by it is never short. */
  int getOrder() const { return (int) c_.size() - 1; }

  /** Every stored coefficient, lowest power first; never empty. Valid until
   *  this polynomial is changed or destroyed. */
  const std::vector<T>& getCoefficients() const { return c_; }

  /**
   * The value at x, by Horner's rule: one multiply and one add per
   * coefficient, and far better conditioned than summing c[i] * pow(x, i),
   * which builds large powers only to cancel them. No allocation.
   */
  T operator()(T x) const {
    T r = c_.back();
    for (size_t i = c_.size() - 1; i-- > 0;) r = r * x + c_[i];
    return r;
  }

  /** Every coefficient scaled: g * p(x). */
  Polynomial withGain(T gain) const {
    Polynomial out(*this);
    for (T& v : out.c_) v *= gain;
    return out;
  }

  /** p(x) + q(x). The result is as long as the longer of the two. */
  Polynomial getSumWith(const Polynomial& other) const {
    const size_t n = c_.size() > other.c_.size() ? c_.size() : other.c_.size();
    std::vector<T> out(n, T(0));
    for (size_t i = 0; i < c_.size(); ++i) out[i] += c_[i];
    for (size_t i = 0; i < other.c_.size(); ++i) out[i] += other.c_[i];
    return Polynomial(std::move(out));
  }

  /** p(x) * q(x): the coefficients convolved. Order m times order n is
   *  order m + n. */
  Polynomial getProductWith(const Polynomial& other) const {
    std::vector<T> out(c_.size() + other.c_.size() - 1, T(0));
    for (size_t i = 0; i < c_.size(); ++i)
      for (size_t j = 0; j < other.c_.size(); ++j) out[i + j] += c_[i] * other.c_[j];
    return Polynomial(std::move(out));
  }

  /** d/dx. A constant's derivative is the zero polynomial, not an empty one. */
  Polynomial getDerivative() const {
    if (c_.size() < 2) return Polynomial();
    std::vector<T> out(c_.size() - 1);
    for (size_t i = 1; i < c_.size(); ++i) out[i - 1] = c_[i] * (T) i;
    return Polynomial(std::move(out));
  }

  /** The antiderivative whose value at 0 is `constant`. */
  Polynomial getIntegral(T constant = T(0)) const {
    std::vector<T> out(c_.size() + 1);
    out[0] = constant;
    for (size_t i = 0; i < c_.size(); ++i) out[i + 1] = c_[i] / (T) (i + 1);
    return Polynomial(std::move(out));
  }

  /** "1 + 2x + 3x^2", for a log line or a failing test. */
  std::string toString() const {
    std::string s;
    char buf[64];
    for (size_t i = 0; i < c_.size(); ++i) {
      if (i > 0) s += " + ";
      if (i == 0) std::snprintf(buf, sizeof buf, "%g", (double) c_[i]);
      else if (i == 1) std::snprintf(buf, sizeof buf, "%gx", (double) c_[i]);
      else std::snprintf(buf, sizeof buf, "%gx^%u", (double) c_[i], (unsigned) i);
      s += buf;
    }
    return s;
  }

private:
  std::vector<T> c_;
};

} // namespace sonore
