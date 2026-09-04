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
// No spring physics, no keyframes, no property animation beyond bounds and
// alpha. A plugin animates a panel sliding, a meter's ballistics (which belong
// in the meter, not here) and nothing else. Component animators elsewhere are
// the same shape for the same reason.
#pragma once

#include <cmath>
#include <algorithm>
#include <vector>

#include <functional>

#include "component.h"

namespace sonore {
namespace gfx {

/** How the value moves between its ends. */
enum class Easing {
  Linear,
  /** Slow to start. For something leaving: it looks like it is being let go. */
  In,
  /** Slow to stop. For something ARRIVING, which is most things -- an object
   *  that decelerates into place reads as having weight. */
  Out,
  InOut,
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
  CubicBezierEasing(double x1, double y1, double x2, double y2)
      : x1_(clamp01(x1)), y1_(y1), x2_(clamp01(x2)), y2_(y2) {}

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
 * Moves components' bounds over time.
 *
 * Holds raw pointers and does not own anything, like every other container
 * here. A component destroyed mid-flight must be removed with cancel() -- the
 * same contract MouseRouter::forget has, and for the same reason.
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
    a.from = component->bounds();
    a.to = target;
    a.startTime = now;
    a.duration = seconds;
    a.easing = easing;
    animations_.push_back(a);
  }

  bool isAnimating(const Component* component) const {
    for (const Animation& a : animations_)
      if (a.component == component) return true;
    return false;
  }

  bool isAnimating() const { return !animations_.empty(); }
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
      animations_.erase(animations_.begin() + (long) i);
      if (moveToEnd && c) c->setBounds(to);
    }
  }

  void cancelAll(bool moveToEnd) {
    if (moveToEnd)
      for (Animation& a : animations_)
        if (a.component) a.component->setBounds(a.to);
    animations_.clear();
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
      if (!it->component) {
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

      const float f = (float) ease(t, it->easing);
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

  static double ease(double t, Easing easing) {
    if (t <= 0.0) return 0.0;
    if (t >= 1.0) return 1.0;
    switch (easing) {
      case Easing::In: return t * t;
      case Easing::Out: return 1.0 - (1.0 - t) * (1.0 - t);
      case Easing::InOut:
        return t < 0.5 ? 2.0 * t * t : 1.0 - 2.0 * (1.0 - t) * (1.0 - t);
      default: return t;
    }
  }

private:
  static float lerp(float from, float to, float f) { return from + (to - from) * f; }

  struct Animation {
    Component* component = nullptr;
    Rect from, to;
    double startTime = 0.0;
    double duration = 0.0;
    Easing easing = Easing::Out;
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

  /** Told when one finishes, by handle. What chains a callback onto the end
   *  without a sequence type. */
  std::function<void(int id)> onFinished;

  bool isAnimating() const { return !animations_.empty(); }

  bool isAnimating(int id) const {
    for (const Running& running : animations_)
      if (running.id == id) return true;
    return false;
  }

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
      const double shaped =
          it->hasCurve ? it->curve(progress) : ease(progress, it->easing);
      const double value = it->from + (it->to - it->from) * shaped;

      auto setter = it->setter;
      const double to = it->to;
      const bool done = progress >= 1.0;

      if (done) {
        // Off the list first, exact target second -- Back and Elastic do not
        // end at exactly 1, and the erase-before-callback keeps a re-entrant
        // setter from dangling this entry.
        animations_.erase(it);
        if (setter) setter(value);
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
  static double ease(double t, Easing easing) {
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
        const double period = 0.3;
        const double u = t - 1.0;
        return std::pow(2.0, -10.0 * t) * std::sin((u - period / 4.0) * 6.28318530718 / period) +
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

private:
  struct Running {
    int id = 0;
    Setter setter;
    double from = 0.0, to = 0.0;
    double duration = 0.0, start = 0.0;
    Easing easing = Easing::Out;
    CubicBezierEasing curve{0.25, 0.1, 0.25, 1.0};
    bool hasCurve = false;
  };

  std::vector<Running> animations_;
  int nextId_ = 0;
};

} // namespace gfx
} // namespace sonore
