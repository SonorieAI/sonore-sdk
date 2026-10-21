// SPDX-License-Identifier: Apache-2.0
//
// Bounds that move over time rather than jumping.
//
// ── Why a plugin wants this ─────────────────────────────────────────────────
//
// A panel that appears instantly reads as a redraw glitch: the eye cannot tell
// "this is new" from "this was drawn wrong". Two hundred milliseconds of travel
// is the difference between a plugin that feels built and one that feels like a
// prototype, and it costs a lerp per frame.
//
// ── Why the clock is passed in ──────────────────────────────────────────────
//
// The same reason TooltipManager takes one: an animation is defined entirely by
// time, and reading a clock inside would make every rule below untestable
// except by sleeping. So `now` is an argument, the peer passes its own, and a
// test passes whatever it likes and gets exact numbers.
//
// ── What it does not do ─────────────────────────────────────────────────────
//
// No keyframes, and no property animation beyond bounds and numbers. A plugin
// animates a panel sliding, a meter's ballistics (which belong in the meter,
// not here) and little else. Component animators elsewhere are the same shape
// for the same reason. Springs are here as an easing CURVE
// (SpringEasingOptions) on a fixed duration, which is how a duration-based
// animator takes one; a free-running integrator with momentum is
// AnimatedPosition, in animated_position.h.
#pragma once

#include <cmath>
#include <algorithm>
#include <vector>

#include <functional>
#include <memory>

#include "component.h"

namespace sonore {
namespace gfx {

/** How the value moves between its ends. */
enum class Easing {
  Linear,  ///< constant speed
  /** Slow to start. For something leaving: it looks like it is being let go. */
  In,
  /** Slow to stop. For something ARRIVING, which is most things -- an object
   *  that decelerates into place reads as having weight. */
  Out,
  InOut,  ///< slow at both ends: for something moving from one place to another
  /** Overshoots and comes back. For something appearing that should feel
   *  eager -- a popover, a badge. Wrong for anything the user is watching a
   *  value on, because the overshoot LIES about the value for a moment. */
  Back,
  /** Overshoots and oscillates in. A toy, and occasionally exactly right for a
   *  toggle that wants to feel physical. */
  Elastic,
  /** Falls and bounces. Same caveat as Back, more so. */
  Bounce,
};

/** The curve every animator here follows, by name: 0 at t = 0, 1 at t = 1,
 *  and between them whatever the name says -- Back and Elastic overshoot,
 *  Bounce lands in steps. One function, so the bounds animator and the value
 *  animator cannot disagree about what a name means; the bounds animator had
 *  its own copy that knew four of the seven and ran the rest linearly. */
inline double easeCurve(double t, Easing easing) {
  if (t <= 0.0) return 0.0;
  if (t >= 1.0) return 1.0;
  switch (easing) {
    case Easing::Linear: return t;
    case Easing::In: return t * t;
    case Easing::Out: return 1.0 - (1.0 - t) * (1.0 - t);
    case Easing::InOut: return t < 0.5 ? 2.0 * t * t : 1.0 - 2.0 * (1.0 - t) * (1.0 - t);
    case Easing::Back: {
      // 1.70158 is the constant that gives roughly a ten percent overshoot,
      // which is the one every animation library uses because it reads as
      // deliberate rather than as a bug.
      const double c = 1.70158;
      const double u = t - 1.0;
      return u * u * ((c + 1.0) * u + c) + 1.0;
    }
    case Easing::Elastic: {
      // The phase is in t itself. Written with t - 1 inside the sine, as the
      // ease-IN form has it, the oscillation started 1/0.3 periods out of
      // step and the curve leapt to 1.5 in its first instant.
      const double period = 0.3;
      return std::pow(2.0, -10.0 * t) * std::sin((t - period / 4.0) * 6.28318530718 / period) +
             1.0;
    }
    case Easing::Bounce: {
      // The textbook form writes `7.5625 * (u -= a) * u`, which modifies u
      // and reads it in the same unsequenced expression -- undefined
      // behaviour that gcc flags, and that an optimiser is free to evaluate
      // in either order. The shift is a statement of its own here.
      double u = t;
      if (u < 1.0 / 2.75) return 7.5625 * u * u;
      if (u < 2.0 / 2.75) {
        u -= 1.5 / 2.75;
        return 7.5625 * u * u + 0.75;
      }
      if (u < 2.5 / 2.75) {
        u -= 2.25 / 2.75;
        return 7.5625 * u * u + 0.9375;
      }
      u -= 2.625 / 2.75;
      return 7.5625 * u * u + 0.984375;
    }
  }
  return t;
}

/**
 * A cubic Bézier easing, in CSS's parameterisation.
 *
 * cubic-bezier(x1, y1, x2, y2) with the two endpoints fixed at (0,0) and
 * (1,1), which is what a designer hands over -- every design tool exports
 * exactly this, and the named curves above are a handful of points on the same
 * family.
 *
 * The awkward part, and the reason this is not one line: the curve is
 * parameterised by an internal t, and what is wanted is y as a function of
 * X -- so x(t) = the input has to be SOLVED before y can be read. Newton, with
 * a bisection fallback for the near-vertical curves where Newton wanders.
 */
class CubicBezierEasing {
public:
  /** CSS's cubic-bezier(x1, y1, x2, y2). The x values are clamped to 0..1, which
   *  keeps the curve a function of time; the y values are not, so a curve may
   *  overshoot. */
  CubicBezierEasing(double x1, double y1, double x2, double y2)
      : x1_(clamp01(x1)), y1_(y1), x2_(clamp01(x2)), y2_(y2) {}

  /** The eased value at progress `x`: 0 at or below 0, 1 at or above 1. */
  double operator()(double x) const {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    return curveY(solveForT(x));
  }

private:
  static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }

  double curveX(double t) const {
    const double u = 1.0 - t;
    return 3.0 * u * u * t * x1_ + 3.0 * u * t * t * x2_ + t * t * t;
  }
  double curveY(double t) const {
    const double u = 1.0 - t;
    return 3.0 * u * u * t * y1_ + 3.0 * u * t * t * y2_ + t * t * t;
  }
  double slopeX(double t) const {
    const double u = 1.0 - t;
    return 3.0 * u * u * x1_ + 6.0 * u * t * (x2_ - x1_) + 3.0 * t * t * (1.0 - x2_);
  }

  double solveForT(double x) const {
    // Newton first: eight iterations is far more than enough for any curve
    // whose slope is not near zero.
    double t = x;
    for (int i = 0; i < 8; ++i) {
      const double error = curveX(t) - x;
      if (error > -1e-7 && error < 1e-7) return t;
      const double slope = slopeX(t);
      // A near-flat slope means Newton would take a huge step to nowhere.
      // Bisection cannot diverge, and this is the case it exists for --
      // cubic-bezier(1, 0, 0, 1) is a real curve people use.
      if (slope < 1e-6 && slope > -1e-6) break;
      t -= error / slope;
    }

    double low = 0.0, high = 1.0;
    t = x;
    for (int i = 0; i < 32; ++i) {
      const double current = curveX(t);
      if (current > x - 1e-7 && current < x + 1e-7) break;
      if (current > x) high = t;
      else low = t;
      t = (low + high) * 0.5;
    }
    return t;
  }

  double x1_, y1_, x2_, y2_;
};

/**
 * A spring, as an easing curve.
 *
 * ── Why a spring when Elastic and Back already overshoot ──
 *
 * Those are fixed shapes; a spring is a family with physical meaning, and it
 * is what a designer specifying motion in a modern tool hands over -- "a
 * response of 0.4 s with 20 % bounce", or a mass, a stiffness and a damping.
 * The curve is the exact step response of a damped harmonic oscillator that
 * starts at rest: 0 at the start, zero initial velocity, and for an
 * underdamped spring a first overshoot of exp(-pi * zeta / sqrt(1 - zeta^2)),
 * the textbook figure, which is what a test can hold it to.
 *
 * ── The reference's options, and the physical ones ──
 *
 * withFrequency, withAttenuation and withExtraAttenuationRange are the
 * reference's parameterisation, with its defaults (3, 3, 0.25): FREQUENCY is
 * how many oscillations fit in the animation's duration, ATTENUATION the
 * exponential decay rate of their envelope over that duration. The last
 * fraction of the duration -- the extra attenuation range -- is blended onto
 * 1, so the animation ENDS on its target however lively the spring: a
 * duration-based animator has to land when its time is up.
 *
 * fromPhysical(mass, stiffness, damping, seconds) and fromResponse(response,
 * bounce, seconds) map a physical spring onto the same curve for an animation
 * of the given length; settlingTime() says how long that spring actually
 * takes, which is the duration to give it if it should not be cut short.
 * Over- and critically-damped springs are handled exactly too -- they just
 * never overshoot.
 */
class SpringEasingOptions {
public:
  /** The reference's defaults: frequency 3, attenuation 3, extra attenuation
   *  range 0.25. */
  SpringEasingOptions() { setFromReference(); }

  /** A copy with `cyclesPerAnimation` oscillations inside the animation's
   *  duration; a negative count is 0. */
  SpringEasingOptions withFrequency(float cyclesPerAnimation) const {
    SpringEasingOptions o = *this;
    o.frequency_ = cyclesPerAnimation > 0.0f ? cyclesPerAnimation : 0.0f;
    o.setFromReference();
    return o;
  }
  /** A copy whose oscillations decay at `decayPerAnimation`, the exponential
   *  rate over the whole duration; a negative rate is 0. */
  SpringEasingOptions withAttenuation(float decayPerAnimation) const {
    SpringEasingOptions o = *this;
    o.attenuation_ = decayPerAnimation > 0.0f ? decayPerAnimation : 0.0f;
    o.setFromReference();
    return o;
  }
  /** Clamped to 0.05 .. 0.98, as the reference clamps it. */
  SpringEasingOptions withExtraAttenuationRange(float range) const {
    SpringEasingOptions o = *this;
    o.extraRange_ = range < 0.05f ? 0.05f : (range > 0.98f ? 0.98f : range);
    return o;
  }

  /** Oscillations per animation. */
  float getFrequency() const { return frequency_; }
  /** The envelope's decay rate per animation. */
  float getAttenuation() const { return attenuation_; }
  /** The closing fraction of the duration blended onto the target, 0.05..0.98. */
  float getExtraAttenuationRange() const { return extraRange_; }

  /** The damping ratio: below 1 it overshoots, 1 is critical, above is
   *  sluggish. */
  double dampingRatio() const { return zeta_; }

  /**
   * A mass on a spring with a damper, run for `seconds`.
   *
   * zeta = damping / (2 sqrt(stiffness * mass)), omega0 = sqrt(stiffness /
   * mass); time is normalised by `seconds`, so the same spring given a longer
   * animation simply has more of its motion inside it.
   */
  static SpringEasingOptions fromPhysical(double mass, double stiffness, double damping,
                                          double seconds) {
    SpringEasingOptions o;
    if (!(mass > 0.0) || !(stiffness > 0.0) || !(seconds > 0.0)) return o;
    const double omega0 = std::sqrt(stiffness / mass);
    o.setFromPhysics(omega0 * seconds, (damping > 0.0 ? damping : 0.0) / (2.0 * std::sqrt(stiffness * mass)));
    return o;
  }

  /**
   * The designer's parameterisation: `response` is the undamped period in
   * seconds, `bounce` 0 for no overshoot up to nearly 1 for a lot (negative
   * is over-damped) -- zeta = 1 - bounce.
   */
  static SpringEasingOptions fromResponse(double response, double bounce, double seconds) {
    SpringEasingOptions o;
    if (!(response > 0.0) || !(seconds > 0.0)) return o;
    const double b = bounce < -1.0 ? -1.0 : (bounce > 0.99 ? 0.99 : bounce);
    o.setFromPhysics(6.283185307179586 / response * seconds, 1.0 - b);
    return o;
  }

  /** How long a physical spring takes to stay within `tolerance` of its
   *  target -- the duration that falls out of the physics. */
  static double settlingTime(double mass, double stiffness, double damping,
                             double tolerance = 0.001) {
    if (!(mass > 0.0) || !(stiffness > 0.0) || !(damping > 0.0)) return 0.0;
    const double omega0 = std::sqrt(stiffness / mass);
    const double zeta = damping / (2.0 * std::sqrt(stiffness * mass));
    const double tol = tolerance > 1e-12 ? tolerance : 1e-12;
    // The slower of the decay rates bounds it: the envelope for an
    // underdamped spring, the slow real root for an overdamped one.
    const double rate =
        zeta < 1.0 ? zeta * omega0 : omega0 * (zeta - std::sqrt(zeta * zeta - 1.0));
    return std::log(1.0 / tol) / rate;
  }

  /** For an underdamped spring, how far past the target the first swing goes,
   *  as a fraction of the distance; 0 for one that does not overshoot. Ignores
   *  the extra attenuation range, which only touches the end. */
  double overshoot() const {
    if (!(zeta_ < 1.0)) return 0.0;
    return std::exp(-3.141592653589793 * zeta_ / std::sqrt(1.0 - zeta_ * zeta_));
  }

  /** Where the first overshoot peaks, in normalised time; -1 if none. */
  double peakTime() const {
    if (!(zeta_ < 1.0)) return -1.0;
    return 3.141592653589793 / (omega0_ * std::sqrt(1.0 - zeta_ * zeta_));
  }

  /** The curve: 0 at 0, exactly 1 at 1. */
  double operator()(double t) const {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    const double raw = physical(t);
    const double from = 1.0 - (double) extraRange_;
    if (t <= from) return raw;
    // Smoothstep onto the target over the last stretch: continuous in value
    // and slope where it starts, and exactly 1 where it ends.
    const double u = (t - from) / (double) extraRange_;
    const double s = u * u * (3.0 - 2.0 * u);
    return raw + (1.0 - raw) * s;
  }

private:
  /** frequency (cycles per duration) and attenuation (decay per duration)
   *  as an oscillator: omega_d = 2 pi f, zeta omega0 = a. */
  void setFromReference() {
    const double wd = 6.283185307179586 * (double) frequency_;
    const double a = (double) attenuation_;
    omega0_ = std::sqrt(wd * wd + a * a);
    zeta_ = omega0_ > 0.0 ? a / omega0_ : 1.0;
  }

  /** And the other way, so the getters describe a physical spring too. */
  void setFromPhysics(double omega0, double zeta) {
    omega0_ = omega0 > 0.0 ? omega0 : 0.0;
    zeta_ = zeta > 0.0 ? zeta : 0.0;
    attenuation_ = (float) (zeta_ * omega0_);
    frequency_ = zeta_ < 1.0
                     ? (float) (omega0_ * std::sqrt(1.0 - zeta_ * zeta_) / 6.283185307179586)
                     : 0.0f;
  }

  /** The step response of x'' + 2 zeta w0 x' + w0^2 x = w0^2, from rest. */
  double physical(double t) const {
    const double w0 = omega0_, z = zeta_;
    if (!(w0 > 0.0)) return t; // no spring at all: linear rather than nothing
    if (z < 1.0 - 1e-9) {
      const double wd = w0 * std::sqrt(1.0 - z * z);
      const double decay = std::exp(-z * w0 * t);
      return 1.0 - decay * (std::cos(wd * t) + (z * w0 / wd) * std::sin(wd * t));
    }
    if (z <= 1.0 + 1e-9) return 1.0 - std::exp(-w0 * t) * (1.0 + w0 * t);
    const double root = std::sqrt(z * z - 1.0);
    const double r1 = w0 * (-z + root), r2 = w0 * (-z - root);
    return 1.0 + (r2 * std::exp(r1 * t) - r1 * std::exp(r2 * t)) / (r1 - r2);
  }

  float frequency_ = 3.0f;
  float attenuation_ = 3.0f;
  float extraRange_ = 0.25f;
  double omega0_ = 0.0;
  double zeta_ = 1.0;
};

/**
 * Moves components' bounds over time.
 *
 * Does not own anything, like every other container here. A component
 * destroyed mid-flight is dropped at the next tick: each animation watches its
 * component (detail::DeletionWatch). It used to be the caller's job to call
 * cancel() first -- the contract MouseRouter::forget once had, and as easy to
 * miss: a notice closed while it slid in, a panel deleted as it animated out,
 * and the next tick set the bounds of freed memory and called its resized().
 */
class Animator {
public:
  /**
   * Move `component` to `target` over `seconds`, starting at `now`.
   *
   * Re-animating something already in flight REPLACES the old animation from
   * wherever it currently is, rather than queueing or fighting. A panel told to
   * go left and then right while still moving should end up on the right, and
   * should not visibly stutter getting there.
   */
  void animateBounds(Component* component, const Rect& target, double seconds, double now,
                     Easing easing = Easing::Out) {
    if (!component) return;
    cancel(component, /*moveToEnd=*/false);
    if (seconds <= 0.0) {
      component->setBounds(target);
      return;
    }
    Animation a;
    a.component = component;
    a.alive = std::make_shared<detail::DeletionWatch>(component);
    a.from = component->bounds();
    a.to = target;
    a.startTime = now;
    a.duration = seconds;
    a.easing = easing;
    animations_.push_back(a);
  }

  /** The same, moved by a spring. */
  void animateBounds(Component* component, const Rect& target, double seconds, double now,
                     const SpringEasingOptions& spring) {
    animateBounds(component, target, seconds, now, Easing::Linear);
    for (Animation& a : animations_)
      if (a.component == component) {
        a.spring = spring;
        a.hasSpring = true;
      }
  }

  /** Whether `component` has an animation in flight. */
  bool isAnimating(const Component* component) const {
    for (const Animation& a : animations_)
      if (a.component == component) return true;
    return false;
  }

  /** Whether anything is in flight. */
  bool isAnimating() const { return !animations_.empty(); }
  /** How many components are in flight; one animation each at most. */
  int count() const { return (int) animations_.size(); }

  /**
   * Stop animating `component`.
   *
   * `moveToEnd` decides whether it lands where it was going or stays where it
   * is. Landing is right when a layout is being replaced -- half-moved
   * components in a finished layout are worse than none. Staying is right when
   * something new is taking over the same component.
   */
  void cancel(Component* component, bool moveToEnd) {
    // The entry is removed BEFORE setBounds runs, because setBounds calls
    // resized() -- user code -- which may cancel or start animations on this
    // same object. Holding animations_[i] across that call, as this once did,
    // dangles the reference and desyncs the index. Off the list first, then
    // the callback on a local copy.
    for (size_t i = 0; i < animations_.size();) {
      if (animations_[i].component != component) {
        ++i;
        continue;
      }
      const Rect to = animations_[i].to;
      Component* c = animations_[i].component;
      const bool gone = animations_[i].gone();
      animations_.erase(animations_.begin() + (long) i);
      if (moveToEnd && c && !gone) c->setBounds(to);
    }
  }

  /** Stop every animation: each component lands on its target if `moveToEnd`,
   *  else stays where it is. A component destroyed in flight is not touched. */
  void cancelAll(bool moveToEnd) {
    // Off the list first, as in cancel(): each setBounds runs resized(),
    // which may start animations and reallocate the vector being walked.
    // Whatever those callbacks start is theirs, and stays.
    std::vector<Animation> ending;
    ending.swap(animations_);
    if (moveToEnd)
      for (const Animation& a : ending)
        if (a.component && !a.gone()) a.component->setBounds(a.to);
  }

  /**
   * Advance every animation. Returns whether anything moved.
   *
   * The return value is what stops a repaint thirty times a second when nothing
   * is happening -- which for a plugin editor is almost always.
   */
  bool tick(double now) {
    if (animations_.empty()) return false;
    bool moved = false;

    // setBounds() runs resized(), which is user layout code and may start or
    // cancel animations re-entrantly -- an ordinary responsive panel does
    // exactly this. No reference into animations_ may survive that call, so
    // the loop walks a SNAPSHOT of component keys (each unique, since
    // animateBounds cancels before it adds) and re-finds the live entry each
    // iteration; one whose callback deleted a later one simply does not find
    // it. A terminal entry is erased BEFORE its final setBounds.
    std::vector<Component*> keys;
    keys.reserve(animations_.size());
    for (const Animation& a : animations_) keys.push_back(a.component);

    for (Component* key : keys) {
      auto it = std::find_if(animations_.begin(), animations_.end(),
                             [key](const Animation& a) { return a.component == key; });
      if (it == animations_.end()) continue; // cancelled by an earlier callback
      // Destroyed since the last tick, or by an earlier callback in this one:
      // dropped, never touched.
      if (!it->component || it->gone()) {
        animations_.erase(it);
        continue;
      }

      double t = (now - it->startTime) / it->duration;
      // A clock that went BACKWARDS -- a caller resetting its own counter, or a
      // system clock stepping -- would otherwise send the animation into
      // reverse and then leave it stuck.
      if (t < 0.0) t = 0.0;
      const bool finished = t >= 1.0;
      if (finished) t = 1.0;

      const float f = (float) (it->hasSpring ? it->spring(t) : ease(t, it->easing));
      Component* comp = it->component;
      const Rect to = it->to;
      const Rect now_ = Rect(lerp(it->from.x, it->to.x, f), lerp(it->from.y, it->to.y, f),
                             lerp(it->from.w, it->to.w, f), lerp(it->from.h, it->to.h, f));

      if (finished) {
        // Snapped to the exact target, not to whatever the interpolation
        // produced at t=1. Off the list first, so the setBounds below can
        // re-enter safely.
        animations_.erase(it);
        comp->setBounds(to);
        moved = true;
        continue;
      }

      const Rect current = comp->bounds();
      if (now_.x != current.x || now_.y != current.y || now_.w != current.w ||
          now_.h != current.h) {
        comp->setBounds(now_); // may re-enter; `it` is not touched afterwards
        moved = true;
      }
    }
    return moved;
  }

  /** easeCurve, by the name this class has always had for it. */
  static double ease(double t, Easing easing) { return easeCurve(t, easing); }

private:
  static float lerp(float from, float to, float f) { return from + (to - from) * f; }

  /** One component in flight: its bounds when it started and where it is
   *  going, in seconds on the caller's clock, shaped by `easing` -- or by
   *  `spring` when hasSpring is set. At most one per component. */
  struct Animation {
    Component* component = nullptr;
    /** Marked if `component` is destroyed; shared so an Animator stays
     *  copyable. */
    std::shared_ptr<detail::DeletionWatch> alive;
    bool gone() const { return alive && alive->deleted; }
    Rect from, to;
    double startTime = 0.0;
    double duration = 0.0;
    Easing easing = Easing::Out;
    SpringEasingOptions spring;
    bool hasSpring = false;
  };

  std::vector<Animation> animations_;
};

/**
 * Moves a NUMBER over time, not a component's bounds.
 *
 * Which is most of what an editor animates: an opacity fading in, a meter's
 * peak-hold falling back, a colour sweeping between two states, a knob's glow.
 * Animator next door moves rectangles and cannot express any of it.
 *
 * ── The clock is an argument ──
 *
 * Same rule as everything else here. tick(now) is called from the editor's
 * 33 ms clock, and every behaviour below -- the easing shape, landing exactly
 * on the target, a delay elapsing -- is an exact test rather than a sleep.
 *
 * ── Delay is how things compose ──
 *
 * There is no sequence builder, deliberately. An animation can start after a
 * delay, and that one concept expresses both arrangements: two with the same
 * delay run TOGETHER, and one with a delay equal to another's duration runs
 * AFTER it. A builder would be a second way to say the same thing, with its
 * own state machine to get wrong -- and CSS reached the same conclusion.
 */
class ValueAnimator {
public:
  /** Where the value goes. Called with every intermediate value and, exactly
   *  once, with the target. */
  using Setter = std::function<void(double)>;

  /**
   * Start one. Returns a handle, so a caller can cancel this animation without
   * cancelling everything.
   *
   * `now` is whatever clock tick() will be given -- seconds, milliseconds,
   * samples; this never compares it to anything but itself.
   */
  int animate(Setter setter, double from, double to, double duration, double now,
              Easing easing = Easing::Out, double delay = 0.0) {
    if (!setter || duration <= 0.0) {
      // A zero duration is not an error and not an animation: it is a value
      // being set. Doing it immediately is what the caller meant, and
      // refusing would make every "animate, unless the user turned animation
      // off" call site write the branch itself.
      if (setter) setter(to);
      return 0;
    }
    Running running;
    running.id = ++nextId_;
    running.setter = std::move(setter);
    running.from = from;
    running.to = to;
    running.duration = duration;
    running.start = now + (delay > 0.0 ? delay : 0.0);
    running.easing = easing;
    animations_.push_back(std::move(running));
    return animations_.back().id;
  }

  /** With a Bézier instead of a named curve. */
  int animateWithCurve(Setter setter, double from, double to, double duration, double now,
                       CubicBezierEasing curve, double delay = 0.0) {
    const int id = animate(std::move(setter), from, to, duration, now, Easing::Linear, delay);
    for (Running& running : animations_)
      if (running.id == id) {
        running.curve = curve;
        running.hasCurve = true;
      }
    return id;
  }

  /** With a spring: overshoots as the spring says and still lands exactly on
   *  `to` when `duration` is up. */
  int animateWithSpring(Setter setter, double from, double to, double duration, double now,
                        const SpringEasingOptions& spring, double delay = 0.0) {
    const int id = animate(std::move(setter), from, to, duration, now, Easing::Linear, delay);
    for (Running& running : animations_)
      if (running.id == id) {
        running.spring = spring;
        running.hasSpring = true;
      }
    return id;
  }

  /** Told when one finishes, by handle. What chains a callback onto the end
   *  without a sequence type. */
  std::function<void(int id)> onFinished;

  /** Whether anything is running or waiting out its delay. */
  bool isAnimating() const { return !animations_.empty(); }

  /** Whether the animation with handle `id` is still running or delayed. */
  bool isAnimating(int id) const {
    for (const Running& running : animations_)
      if (running.id == id) return true;
    return false;
  }

  /** How many are running or delayed. */
  int numAnimating() const { return (int) animations_.size(); }

  /**
   * Stop one. `moveToEnd` decides whether it lands on its target or stops
   * where it is -- the difference between cancelling because the user did
   * something else and cancelling because the window is closing.
   */
  void cancel(int id, bool moveToEnd) {
    for (size_t i = 0; i < animations_.size(); ++i) {
      if (animations_[i].id != id) continue;
      // Copy, erase, THEN call: the setter is user code that may cancel or
      // start animations, and erasing by the same index i afterwards -- as
      // this once did -- would remove the wrong element if the setter shrank
      // the vector ahead of it.
      auto setter = animations_[i].setter;
      const double to = animations_[i].to;
      animations_.erase(animations_.begin() + (long) i);
      if (moveToEnd && setter) setter(to);
      return;
    }
  }

  /** Stop every animation: each setter is given its target if `moveToEnd`, else
   *  nothing more. onFinished is not called. */
  void cancelAll(bool moveToEnd) {
    // Over a COPY, because a setter can start another animation -- which is
    // how a caller chains one onto the end -- and that would grow the vector
    // being iterated.
    const std::vector<Running> copy = animations_;
    animations_.clear();
    if (!moveToEnd) return;
    for (const Running& running : copy)
      if (running.setter) running.setter(running.to);
  }

  /**
   * Advance everything. Returns whether anything MOVED, which is what stops a
   * repaint thirty times a second when nothing is happening.
   *
   * An animation still inside its delay has not moved and does not report
   * that it has -- a delay that forced repaints would make staggering a row
   * of controls cost more than not staggering them.
   */
  bool tick(double now) {
    bool moved = false;
    std::vector<int> finished;

    // A setter is user code and may start or cancel animations -- the class
    // endorses exactly this ("a setter can start another animation ... how a
    // caller chains one onto the end"). So no reference into animations_ may
    // survive a setter call. Walk a SNAPSHOT of ids and re-find each; copy the
    // setter and target before invoking, and re-find to erase, because the
    // setter may have moved or removed the entry. An animation started during
    // this tick is not in the snapshot and runs next tick, which is correct --
    // it begins at `now`.
    std::vector<int> ids;
    ids.reserve(animations_.size());
    for (const Running& r : animations_) ids.push_back(r.id);

    for (int id : ids) {
      auto it = std::find_if(animations_.begin(), animations_.end(),
                             [id](const Running& r) { return r.id == id; });
      if (it == animations_.end()) continue; // cancelled by an earlier setter
      if (now < it->start) continue;

      const double elapsed = now - it->start;
      double progress = elapsed / it->duration;
      if (progress > 1.0) progress = 1.0;
      const double shaped = it->hasSpring  ? it->spring(progress)
                            : it->hasCurve ? it->curve(progress)
                                           : ease(progress, it->easing);
      const double value = it->from + (it->to - it->from) * shaped;

      auto setter = it->setter;
      const double to = it->to;
      const bool done = progress >= 1.0;

      if (done) {
        // Off the list first, then the exact target -- once. The interpolated
        // value at the end is the target up to rounding (0.1 + 0.2 is not
        // 0.3), and calling the setter with both sent a parameter-writing
        // setter's host two edits for one landing. The erase-before-callback
        // keeps a re-entrant setter from dangling this entry.
        animations_.erase(it);
        if (setter) setter(to);
        finished.push_back(id);
      } else if (setter) {
        setter(value); // may mutate animations_; `it` is not used afterwards
      }
      moved = true;
    }

    // AFTER the loop, so a callback that starts another animation cannot edit
    // the vector being walked.
    if (onFinished)
      for (int fid : finished) onFinished(fid);
    return moved;
  }

  /** The curve alone, for anything drawing its own interpolation. */
  static double ease(double t, Easing easing) { return easeCurve(t, easing); }

private:
  /** One value in flight, known by the id animate() returned. `start` already
   *  includes the delay. The shape is the spring if hasSpring, else the curve
   *  if hasCurve, else `easing`. */
  struct Running {
    int id = 0;
    Setter setter;
    double from = 0.0, to = 0.0;
    double duration = 0.0, start = 0.0;
    Easing easing = Easing::Out;
    CubicBezierEasing curve{0.25, 0.1, 0.25, 1.0};
    bool hasCurve = false;
    SpringEasingOptions spring;
    bool hasSpring = false;
  };

  std::vector<Running> animations_;
  int nextId_ = 0;
};

} // namespace gfx
} // namespace sonore
