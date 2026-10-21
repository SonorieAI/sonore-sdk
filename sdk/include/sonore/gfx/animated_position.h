// SPDX-License-Identifier: Apache-2.0
//
// AnimatedPosition: a position that a finger throws, and that carries on
// moving after it lets go -- and ViewportDragScroller, which uses two of them
// to scroll a Viewport by dragging its content.
//
// ── Why the application has to integrate a flick ────────────────────────────
//
// A wheel or a precision trackpad arrives already smoothed: the operating
// system turns a flick into a decelerating stream of scroll events, and the
// Viewport only has to follow them. A finger on a touch screen, or a pen,
// arrives as a DRAG -- positions, and then a release. Nothing after the
// release says the list should keep moving; the momentum has to be worked out
// from how fast the finger was going when it left, and then integrated, frame
// by frame, against friction. That is this.
//
// ── Behaviours ──────────────────────────────────────────────────────────────
//
// The reference's two, with its defaults:
//
//   ContinuousWithMomentum  coasts at the release velocity, losing a fraction
//                           of it to friction every 60th of a second, and
//                           stops once slower than a minimum. The friction is
//                           integrated exactly over the elapsed time rather
//                           than applied per call, so a flick goes the same
//                           distance at 30, 60 or 144 Hz.
//   SnapToPageBoundaries    settles on the nearest whole number -- a page --
//                           or the next one in the direction of a fast flick.
//
// A behaviour is any type with releasedWithVelocity(position, velocity),
// getNextPosition(old, elapsed) and isStopped(position): write a third for a
// detent, a rubber band, a carousel.
//
// ── The clock ───────────────────────────────────────────────────────────────
//
// As everywhere in this SDK, time is an argument or an injectable function,
// so the decay and the snap are exact tests. ViewportDragScroller ticks itself
// from a VBlankAttachment while it coasts -- once per display refresh, which
// is what makes the motion smooth -- unless told to leave the ticking to its
// owner.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include "component.h"
#include "vblank.h"
#include "viewport.h"

namespace sonore {
namespace gfx {

namespace AnimatedPositionBehaviours {

/** Keeps moving after release, slowing under friction. */
struct ContinuousWithMomentum {
  /** The fraction of its velocity lost every 1/60 s. 0.08 by default. */
  void setFriction(double newFriction) {
    damping = 1.0 - (newFriction < 0.0 ? 0.0 : (newFriction > 1.0 ? 1.0 : newFriction));
  }
  /** Below this, in units per second, it stops. */
  void setMinimumVelocity(double newMinimum) { minimumVelocity = std::fabs(newMinimum); }

  /** Start coasting at `releaseVelocity`, in units per second. */
  void releasedWithVelocity(double, double releaseVelocity) { velocity = releaseVelocity; }

  /** Where it is `elapsedSeconds` later, with the friction integrated exactly,
   *  so how far a flick travels does not depend on the tick rate. */
  double getNextPosition(double oldPos, double elapsedSeconds) {
    // The reference multiplies the velocity by `damping` once per 60 Hz tick
    // and then moves by it, which makes the length of a coast depend on the
    // tick rate -- 8 % shorter at 30 Hz than at 144. Here the same friction is
    // integrated EXACTLY over the elapsed time: velocity decays as
    // damping^(60 t), i.e. exp(-lambda t), and the distance covered meanwhile
    // is v (1 - decay) / lambda. A flick travels v0 / lambda whatever the rate.
    const double decay = std::pow(damping, elapsedSeconds * 60.0);
    const double lambda = damping > 0.0 && damping < 1.0 ? -60.0 * std::log(damping) : 0.0;
    const double travelled = lambda > 0.0 ? velocity * (1.0 - decay) / lambda
                                          : velocity * elapsedSeconds * (damping > 0.0 ? 1.0 : 0.0);
    velocity *= decay;
    if (std::fabs(velocity) < minimumVelocity) velocity = 0.0;
    return oldPos + travelled;
  }

  /** Whether the velocity has fallen to 0. */
  bool isStopped(double) const { return velocity == 0.0; }

  double velocity = 0.0, /**< units per second, now */ damping = 0.92, /**< velocity kept per 1/60 s: 1 - friction */ minimumVelocity = 0.05; ///< units per second
};

/** Settles on a whole number: page boundaries, when one unit is one page. */
struct SnapToPageBoundaries {
  /** Aim for the nearest whole number, or the next one in the direction of a
   *  flick faster than one unit per second. */
  void releasedWithVelocity(double position, double releaseVelocity) {
    targetSnapPosition = std::floor(position + 0.5);
    // A flick faster than a page a second goes on to the next page in its
    // direction, even if it was released short of halfway.
    if (releaseVelocity > 1.0 && targetSnapPosition < position) ++targetSnapPosition;
    if (releaseVelocity < -1.0 && targetSnapPosition > position) --targetSnapPosition;
  }

  /** Covers ten times `elapsedSeconds` of the remaining distance (all of it
   *  after 0.1 s), and lands exactly on the target. */
  double getNextPosition(double oldPos, double elapsedSeconds) const {
    if (isStopped(oldPos)) return targetSnapPosition;
    const double snapSpeed = 10.0;
    const double step = (targetSnapPosition - oldPos) * (std::min)(1.0, snapSpeed * elapsedSeconds);
    const double newPos = oldPos + step;
    return isStopped(newPos) ? targetSnapPosition : newPos;
  }

  /** Within 0.001 of the target. */
  bool isStopped(double position) const { return std::fabs(targetSnapPosition - position) < 0.001; }

  /** The whole number it is settling on. */
  double targetSnapPosition = 0.0;
};

} // namespace AnimatedPositionBehaviours

/**
 * A one-dimensional position that can be dragged and then let go of.
 *
 * beginDrag / drag(deltaFromStart) / endDrag follow a gesture, measuring its
 * velocity as it goes; on endDrag the behaviour takes over and update(now)
 * moves the position until the behaviour says it has stopped.
 */
template <typename Behaviour>
class AnimatedPosition {
public:
  /** At 0, with no limits, still, timed by the steady clock. */
  AnimatedPosition() {
    clock_ = [] {
      return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
          .count();
    };
  }

  /** Told each time the position actually changes -- after clamping to the
   *  limits, so a drag pinned at an end is silent -- from whichever call moved
   *  it (drag, update, setPosition, setLimits), on the thread that made that
   *  call. Not owned: remove one before it dies. One removed during a
   *  notification is not called for it. */
  class Listener {
  public:
    virtual ~Listener() = default;
    /** The position is now `newPosition`, already within the limits. */
    virtual void positionChanged(AnimatedPosition&, double newPosition) = 0;
  };

  /** Same notification, as a function. */
  std::function<void(double)> onPositionChanged;

  /** Not owned; adding one twice is a no-op. */
  void addListener(Listener* l) {
    if (l && std::find(listeners_.begin(), listeners_.end(), l) == listeners_.end())
      listeners_.push_back(l);
  }
  /** Safe to call from inside a notification. */
  void removeListener(Listener* l) {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
  }

  /** Seconds, any origin. */
  void setClock(std::function<double()> clock) {
    if (clock) clock_ = std::move(clock);
  }

  /** Keep the position within low..high, swapped if given the wrong way round.
   *  The position is clamped at once, notifying if that moved it. */
  void setLimits(double low, double high) {
    low_ = (std::min)(low, high);
    high_ = (std::max)(low, high);
    setPositionAndSendChange(position_);
  }
  /** The lower limit; effectively none (-1e300) until setLimits. */
  double lowLimit() const { return low_; }
  /** The upper limit; effectively none (1e300) until setLimits. */
  double highLimit() const { return high_; }

  /** Start a gesture from the current position: stops any coast and starts
   *  measuring velocity afresh. */
  void beginDrag() {
    grabbed_ = position_;
    releaseVelocity_ = 0.0;
    lastDrag_ = clock_();
    animating_ = false;
    dragging_ = true;
  }

  /** Where the drag is relative to where it began. */
  void drag(double deltaFromStartOfDrag) {
    moveTo(grabbed_ + deltaFromStartOfDrag);
  }

  /** Let go: the behaviour takes the release velocity, and update() runs it. */
  void endDrag() {
    dragging_ = false;
    // A finger that stopped before lifting has no momentum, however fast it
    // was going a moment earlier -- the velocity is aged by how long it has
    // been since the last movement.
    const double sinceLast = clock_() - lastDrag_;
    if (sinceLast > 0.05) releaseVelocity_ = 0.0;
    behaviour.releasedWithVelocity(position_, releaseVelocity_);
    lastUpdate_ = clock_();
    animating_ = true;
  }

  /** Move by `delta` as though dragged there and released with no speed. */
  void nudge(double delta) {
    beginDrag();
    drag(delta);
    endDrag();
  }

  /** The current position, within the limits. */
  double getPosition() const { return position_; }

  /** Jump, stopping any motion. */
  void setPosition(double newPosition) {
    animating_ = false;
    dragging_ = false;
    setPositionAndSendChange(newPosition);
  }

  /** Between beginDrag and endDrag (or a setPosition). */
  bool isDragging() const { return dragging_; }
  /** Coasting after endDrag, until the behaviour stops or a limit is reached. */
  bool isAnimating() const { return animating_; }
  /** The smoothed drag velocity, in units per second -- what endDrag hands the
   *  behaviour, or 0 if the drag had been still for over 50 ms. */
  double releaseVelocity() const { return releaseVelocity_; }

  /** Advance the coast to `now`. Returns whether it is still moving. */
  bool update(double now) {
    if (!animating_) return false;
    double elapsed = now - lastUpdate_;
    lastUpdate_ = now;
    if (elapsed <= 0.0) return true;
    // A stall -- the window hidden, a breakpoint -- must not throw the list
    // a screenful in one step.
    if (elapsed > 0.1) elapsed = 0.1;
    const double next = behaviour.getNextPosition(position_, elapsed);
    const double clamped = clampToLimits(next);
    setPositionAndSendChange(clamped);
    // Hitting a limit ends the coast: there is nowhere further to go.
    if (behaviour.isStopped(position_) || clamped != next) animating_ = false;
    return animating_;
  }

  /** update(now) by this position's own clock. */
  bool update() { return update(clock_()); }

  /** What runs after the release; its settings may be changed directly. */
  Behaviour behaviour;

private:
  double clampToLimits(double v) const { return v < low_ ? low_ : (v > high_ ? high_ : v); }

  void moveTo(double target) {
    const double now = clock_();
    const double previous = position_;
    setPositionAndSendChange(target);
    const double elapsed = now - lastDrag_;
    if (elapsed > 0.005) {
      // Smoothed over the last few moves: a single fast twitch at the end of
      // a slow drag should not fling the list.
      const double v = (position_ - previous) / elapsed;
      releaseVelocity_ = releaseVelocity_ * 0.4 + v * 0.6;
      lastDrag_ = now;
    }
  }

  void setPositionAndSendChange(double v) {
    const double clamped = clampToLimits(v);
    if (clamped == position_) return;
    position_ = clamped;
    const std::vector<Listener*> told = listeners_;
    for (Listener* l : told)
      if (std::find(listeners_.begin(), listeners_.end(), l) != listeners_.end())
        l->positionChanged(*this, position_);
    if (onPositionChanged) onPositionChanged(position_);
  }

  std::function<double()> clock_;
  std::vector<Listener*> listeners_;
  double position_ = 0.0, grabbed_ = 0.0;
  double low_ = -1.0e300, high_ = 1.0e300;
  double releaseVelocity_ = 0.0;
  double lastDrag_ = 0.0, lastUpdate_ = 0.0;
  bool dragging_ = false, animating_ = false;
};

/**
 * Scroll a Viewport by dragging its content, with momentum -- the reference's
 * Viewport::setScrollOnDragMode, as an object attached to the viewport.
 *
 * Listens to everything inside the viewed component, so a drag that starts
 * on a row, a label or a button still scrolls. It does not start until the
 * pointer has moved a few pixels, so a tap is still a tap. Drags on the
 * viewport's own scroll bars, and on any component given to
 * setIgnoresDragsFrom, are left alone -- a slider in a scrolling list must
 * move its value, not the list.
 */
class ViewportDragScroller : private MouseListener {
public:
  /** never: off. nonHover: fingers and pens only -- a mouse drag means what
   *  it always meant. all: the mouse too. */
  enum class ScrollOnDragMode { never, /**< off */ nonHover, /**< fingers and pens only */ all /**< the mouse too */ };

  /** Scroll `viewport` by dragging what it shows, in `mode`. The viewport is not
   *  owned and must outlive this. */
  explicit ViewportDragScroller(Viewport& viewport, ScrollOnDragMode mode = ScrollOnDragMode::nonHover)
      : viewport_(&viewport), mode_(mode) {
    viewport.addMouseListener(this, /*nestedChildren=*/true);
    x_.onPositionChanged = [this](double) { pushToViewport(); };
    y_.onPositionChanged = [this](double) { pushToViewport(); };
  }

  ~ViewportDragScroller() override = default; ///< MouseListener detaches itself

  /** Which pointers scroll by dragging; takes effect at the next press. */
  void setScrollOnDragMode(ScrollOnDragMode mode) { mode_ = mode; }
  /** Which pointers scroll by dragging. */
  ScrollOnDragMode scrollOnDragMode() const { return mode_; }

  /** Drags beginning on `c`, or inside it, are its own. */
  void setIgnoresDragsFrom(Component* c) {
    if (c && std::find(ignored_.begin(), ignored_.end(), c) == ignored_.end()) ignored_.push_back(c);
  }

  /** Pixels the pointer must travel before a press becomes a scroll. */
  float dragThreshold = 8.0f;

  /** By default the coast ticks itself from the display refresh. Off, the
   *  owner calls tick() -- what a test does, with its own clock. */
  void setTicksFromVBlank(bool shouldTick) { vblankTicks_ = shouldTick; }

  /** Time the drag and the coast by `clock`, in seconds, instead of the steady
   *  clock -- for a test. */
  void setClock(std::function<double()> clock) {
    x_.setClock(clock);
    y_.setClock(clock);
    clock_ = std::move(clock);
  }

  /** Advance the coast. Returns whether it is still moving. */
  bool tick(double now) {
    const bool movingX = x_.update(now);
    const bool movingY = y_.update(now);
    // Destroying the attachment from inside its own callback is allowed: it
    // runs from a copy of the callback (see VBlankAttachment::fire).
    if (!movingX && !movingY) vblank_.reset();
    return movingX || movingY;
  }

  /** Whether a press has moved past dragThreshold and is scrolling now. */
  bool isScrolling() const { return scrolling_; }
  /** Whether the view is still moving after the release. */
  bool isCoasting() const { return x_.isAnimating() || y_.isAnimating(); }

  /** The horizontal view position, in logical pixels; its friction and minimum
   *  velocity can be set through behaviour. */
  AnimatedPosition<AnimatedPositionBehaviours::ContinuousWithMomentum>& horizontal() { return x_; }
  /** The vertical view position, likewise. */
  AnimatedPosition<AnimatedPositionBehaviours::ContinuousWithMomentum>& vertical() { return y_; }

private:
  bool wants(const MouseEvent& e) const {
    if (mode_ == ScrollOnDragMode::never || e.rightButton) return false;
    if (mode_ == ScrollOnDragMode::nonHover && e.sourceType == MouseInputSourceType::Mouse)
      return false;
    Component* viewed = viewport_->viewedComponent();
    if (!viewed) return false;
    // Inside the viewed component -- not the bars, which are the viewport's
    // other children -- and not inside anything told to keep its drags.
    bool inside = false;
    for (Component* c = e.eventComponent; c != nullptr; c = c->parent()) {
      if (std::find(ignored_.begin(), ignored_.end(), c) != ignored_.end()) return false;
      if (c == viewed) inside = true;
      if (c == viewport_) break;
    }
    return inside;
  }

  void syncLimits() {
    Component* viewed = viewport_->viewedComponent();
    const Rect visible = viewport_->visibleArea();
    const double maxX = viewed ? (std::max)(0.0f, viewed->bounds().w - visible.w) : 0.0;
    const double maxY = viewed ? (std::max)(0.0f, viewed->bounds().h - visible.h) : 0.0;
    pushing_ = true;
    x_.setLimits(0.0, maxX);
    y_.setLimits(0.0, maxY);
    x_.setPosition(viewport_->viewPositionX());
    y_.setPosition(viewport_->viewPositionY());
    pushing_ = false;
  }

  void pushToViewport() {
    if (pushing_) return;
    viewport_->setViewPosition((float) x_.getPosition(), (float) y_.getPosition());
  }

  void mouseDown(const MouseEvent& e) override {
    pressed_ = wants(e);
    scrolling_ = false;
    if (!pressed_) return;
    vblank_.reset();
    syncLimits(); // also stops any coast: a finger on a moving list stops it
    downAt_ = e.rootPosition;
  }

  void mouseDrag(const MouseEvent& e) override {
    if (!pressed_) return;
    const float dx = e.rootPosition.x - downAt_.x, dy = e.rootPosition.y - downAt_.y;
    if (!scrolling_) {
      if (std::sqrt(dx * dx + dy * dy) < dragThreshold) return;
      scrolling_ = true;
      x_.beginDrag();
      y_.beginDrag();
    }
    // Dragging the content DOWN scrolls the view UP: the view position moves
    // against the finger.
    x_.drag(-(double) dx);
    y_.drag(-(double) dy);
  }

  void mouseUp(const MouseEvent&) override {
    if (!pressed_) return;
    pressed_ = false;
    if (!scrolling_) return;
    scrolling_ = false;
    x_.endDrag();
    y_.endDrag();
    if (vblankTicks_ && isCoasting())
      vblank_ = std::make_unique<VBlankAttachment>(viewport_, [this](double) {
        // The positions were timed by the steady clock (or the one given to
        // setClock), so the coast is advanced by the same one -- never by the
        // refresh's timestamp, whose origin is the platform's.
        tick(clock_ ? clock_() : refreshClock());
      });
  }

  static double refreshClock() { return detail::VBlankDispatcher::seconds(); }

  Viewport* viewport_;
  ScrollOnDragMode mode_;
  AnimatedPosition<AnimatedPositionBehaviours::ContinuousWithMomentum> x_, y_;
  std::vector<Component*> ignored_;
  std::unique_ptr<VBlankAttachment> vblank_;
  std::function<double()> clock_;
  Point downAt_;
  bool pressed_ = false, scrolling_ = false, pushing_ = false, vblankTicks_ = true;
};

} // namespace gfx
} // namespace sonore
