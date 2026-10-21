// SPDX-License-Identifier: Apache-2.0
// The showcase's transfer curve: text a user typed, turned into a table the
// audio thread can read.
//
// ── Why a table ─────────────────────────────────────────────────────────────
//
// sonore::Expression parses "x - pow(x, 3) / 3" and evaluates it, and its own
// header says where: not on the audio thread, because evaluating allocates its
// working stacks. And even if it did not, walking a tree per sample, four
// times over at 4x oversampling, is a lot of work for a curve that changes only
// when somebody types. So the curve is SAMPLED once, on the message thread,
// into 2049 points over -8..8, and the audio thread does what it would do for
// any curve: a lookup and a linear interpolation.
//
// ── Why three tables for one curve ──────────────────────────────────────────
//
// The audio thread is reading the table while the user types the next one.
// Writing into the table it reads tears the curve mid-block; a lock would make
// the audio thread wait on a keystroke. CurveExchange is a triple buffer: the
// writer fills a table nobody reads, publishes it with one atomic exchange,
// and the reader picks the newest finished one up at the start of a block --
// also with one exchange. Neither ever waits, and no table is ever written
// while it is read.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

#include <sonore/expression.h>

namespace showcase {

/** The curve, sampled. y = f(x) for x from -kRange to +kRange. */
struct CurveTable {
  static constexpr int kPoints = 2049;
  static constexpr float kRange = 8.0f;
  float y[kPoints];

  /** [audio thread] f(x) by linear interpolation. Beyond the range the curve
   *  holds its end values -- a driven signal past +-8 is clipped by the table,
   *  which is the honest thing a sampled curve can do there. */
  float lookup(float x) const {
    const float scaled = (x + kRange) * ((float) (kPoints - 1) / (2.0f * kRange));
    if (!(scaled > 0.0f)) return y[0]; // also catches NaN
    if (scaled >= (float) (kPoints - 1)) return y[kPoints - 1];
    const int i = (int) scaled;
    const float frac = scaled - (float) i;
    return y[i] + (y[i + 1] - y[i]) * frac;
  }
};

/**
 * The names a curve may use: `x`, the driven input, and `pi`; and on top of
 * Expression's standard functions (sin, abs, pow, min, max...), the three a
 * waveshaper wants and the standard set lacks -- tanh, sgn and clamp.
 */
class CurveScope : public sonore::Expression::Scope {
public:
  double x = 0.0;

  bool getSymbolValue(const std::string& symbol, sonore::Expression& value) const override {
    if (symbol == "x") value = sonore::Expression(x);
    else if (symbol == "pi") value = sonore::Expression(3.14159265358979323846);
    else return false;
    return true;
  }

  bool evaluateFunction(const std::string& name, const double* p, int count, double& result,
                        std::string& error) const override {
    if (name == "tanh" && count == 1) result = std::tanh(p[0]);
    else if (name == "sgn" && count == 1) result = (p[0] > 0.0) - (p[0] < 0.0);
    else if (name == "clamp" && count == 3) result = p[0] < p[1] ? p[1] : (p[0] > p[2] ? p[2] : p[0]);
    else return Scope::evaluateFunction(name, p, count, result, error);
    return true;
  }
};

/**
 * [message thread] Parse `text` and sample it into `out`. On failure `out` is
 * untouched and `error` says why ("column 7: expected ')'", "unknown symbol:
 * y") -- so a half-typed expression leaves the last good curve playing.
 *
 * What comes out is made safe to put in front of a speaker, because nothing
 * stops a user typing pow(x, 9): every point is finite and inside +-4. A curve that
 * is NaN or infinite somewhere -- log(x) at 0 -- is refused rather than
 * clipped, since that is almost always a typo the user wants to hear about.
 */
inline bool compileCurve(const std::string& text, CurveTable& out, std::string& error) {
  error.clear();
  const sonore::Expression parsed = sonore::Expression::parse(text, &error);
  if (!error.empty()) return false;
  CurveScope scope;
  // Sampled into a table of its own, so a failure half way leaves `out` as it
  // was. On the heap: 8 kB is no stack's business.
  std::unique_ptr<CurveTable> scratch(new CurveTable);
  for (int i = 0; i < CurveTable::kPoints; ++i) {
    scope.x = -CurveTable::kRange + 2.0 * CurveTable::kRange * i / (CurveTable::kPoints - 1);
    const double v = parsed.evaluate(scope, error);
    if (!error.empty()) return false;
    if (!std::isfinite(v)) {
      char at[32];
      std::snprintf(at, sizeof(at), "%.2f", scope.x);
      error = std::string("not a number at x = ") + at;
      return false;
    }
    scratch->y[i] = (float) (v > 4.0 ? 4.0 : (v < -4.0 ? -4.0 : v));
  }
  out = *scratch;
  return true;
}

/** Hands a CurveTable from the message thread to the audio thread: see the
 *  file comment. Every slot starts as `initial`, so the reader always has a
 *  whole curve, even before anything was published. */
class CurveExchange {
public:
  explicit CurveExchange(const CurveTable& initial) {
    for (CurveTable& t : slots_) t = initial;
  }
  CurveExchange(const CurveExchange&) = delete;
  CurveExchange& operator=(const CurveExchange&) = delete;

  /** [message thread] The table to fill; nobody reads it until publish(). */
  CurveTable& back() { return slots_[back_]; }

  /** [message thread] Make back() the newest curve; back() is another table now. */
  void publish() { back_ = shared_.exchange(back_ | kFresh, std::memory_order_acq_rel) & kIndex; }

  /** [audio thread] The newest published curve, picked up if there is one. One
   *  load, and an exchange only when something new arrived: no wait, no lock. */
  const CurveTable& acquire() {
    if (shared_.load(std::memory_order_acquire) & kFresh)
      front_ = shared_.exchange(front_, std::memory_order_acq_rel) & kIndex;
    return slots_[front_];
  }

private:
  static constexpr int kIndex = 3, kFresh = 4;
  CurveTable slots_[3];
  std::atomic<int> shared_{1}; // the slot in the middle, and whether it is new
  int back_ = 2;               // the writer's
  int front_ = 0;              // the reader's
};

} // namespace showcase
