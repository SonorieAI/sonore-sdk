// SPDX-License-Identifier: Apache-2.0
//
// ComponentPaintDiagnostics: how long each component takes to paint, and
// where the repaints land.
//
// ── Why a plugin wants this, and when ───────────────────────────────────────
//
// An editor that stutters while a meter animates is almost always one of two
// things: a component whose paint() is far more expensive than it looks (a
// gradient per pixel, a path rebuilt every frame), or a repaint that covers
// far more than it should (a meter whose repaint() marks the whole editor).
// The first is a number per component; the second is a picture. This gives
// both, from the running editor, without a profiler.
//
// ── How it hooks in ─────────────────────────────────────────────────────────
//
// Through ComponentPaintObserver in component.h: one pointer, null unless this
// is installed, tested once per component per paint. Installing it is a
// debugging decision -- it reads a clock twice per component painted -- and
// nothing in the SDK installs one.
//
// ── The flash ───────────────────────────────────────────────────────────────
//
// With flashing on, every region the window repaints is covered, after the
// tree has painted, with a translucent colour that changes every frame. A
// region that stays one colour across frames was not repainted; a region that
// keeps changing colour is being repainted, whether or not anything in it
// moved -- which is the bug this exists to find.
//
// The clock is injectable, as everywhere else in this SDK, so the accounting
// is an exact test rather than a measurement of the machine.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "component.h"

namespace sonore {
namespace gfx {

/**
 * Paint timings per component, and a flash over every repainted region --
 * a debugging aid, off unless install() is called. The file comment says what
 * each finds and how it hooks in.
 *
 * install() makes it THE observer for every paint in the process (a second
 * install replaces the first) until uninstall() or destruction. It is called
 * from inside painting, so it is used, read and reset on the message thread.
 * Timing is on by default once installed; the flash is off until
 * setFlashRepaints(true).
 */
class ComponentPaintDiagnostics : public ComponentPaintObserver {
public:
  /** One component's paint history. `component` is an IDENTITY, never
   *  dereferenced after the paint it was recorded at -- the component may be
   *  gone by the time somebody reads the report. */
  struct Stat {
    /** The component painted: compare with it, never call through it. */
    const Component* component = nullptr;
    /** Its accessible name at its first timed paint, or `component@` and
     *  its address when it had none. */
    std::string name;
    /** How many of its paint() calls were timed. */
    int paints = 0;
    /** Their sum, in seconds of the clock. */
    double totalSeconds = 0.0;
    /** The most recent one, in seconds. */
    double lastSeconds = 0.0;
    /** The longest one, in seconds. */
    double maxSeconds = 0.0;
  };

  /** Not installed; timing on, flash off, timed by std::chrono::steady_clock. */
  ComponentPaintDiagnostics() {
    clock_ = [] {
      return std::chrono::duration<double>(
                 std::chrono::steady_clock::now().time_since_epoch())
          .count();
    };
  }

  /** Uninstalls it if it is still the observer. */
  ~ComponentPaintDiagnostics() override { uninstall(); }

  /** Not copyable: the process's observer pointer may point at this one. */
  ComponentPaintDiagnostics(const ComponentPaintDiagnostics&) = delete;
  /** Not assignable, for the same reason. */
  ComponentPaintDiagnostics& operator=(const ComponentPaintDiagnostics&) = delete;

  /** Start observing every paint in the process. One at a time: installing a
   *  second replaces the first. */
  void install() { detail::paintObserver() = this; }

  /** Stop observing. Does nothing when another one has been installed since;
   *  the stats are kept either way. */
  void uninstall() {
    if (detail::paintObserver() == this) detail::paintObserver() = nullptr;
  }

  /** This one is the observer now: installed, and not replaced since. */
  bool isInstalled() const { return detail::paintObserver() == this; }

  /** Time each component's own paint() (children are timed separately). */
  void setTimePaints(bool on) { timing_ = on; }
  /** Whether paints are timed: true unless setTimePaints(false). */
  bool timesPaints() const { return timing_; }

  /** Cover each repainted region with a translucent colour that changes
   *  every frame. */
  void setFlashRepaints(bool on, float opacity = 0.35f) {
    flashing_ = on;
    flashOpacity_ = opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
  }
  /** Whether repaints are flashed: false until setFlashRepaints(true). */
  bool flashesRepaints() const { return flashing_; }

  /** Seconds, from any origin. */
  void setClock(std::function<double()> clock) {
    if (clock) clock_ = std::move(clock);
  }

  /** Forget every Stat and the flash count, to measure from zero again. */
  void reset() {
    stats_.clear();
    open_.clear();
    flashes_ = 0;
  }

  /** Most expensive first, by total time. */
  std::vector<Stat> stats() const {
    std::vector<Stat> out = stats_;
    std::stable_sort(out.begin(), out.end(),
                     [](const Stat& a, const Stat& b) { return a.totalSeconds > b.totalSeconds; });
    return out;
  }

  /** The Stat of `c`, or null if it has not been timed since the last
   *  reset(). Valid until the next timed paint or reset(): the list grows. */
  const Stat* statFor(const Component* c) const {
    for (const Stat& s : stats_)
      if (s.component == c) return &s;
    return nullptr;
  }

  /** Root paints flashed since the last reset(): one per repaint of a
   *  window's tree while the flash is on. */
  int framesFlashed() const { return flashes_; }

  /** A plain-text table of the `top` most expensive components. */
  std::string report(int top = 10) const {
    std::string out = "component                         paints   total ms    max ms\n";
    const std::vector<Stat> sorted = stats();
    char line[160];
    for (int i = 0; i < (int) sorted.size() && i < top; ++i) {
      const Stat& s = sorted[(size_t) i];
      std::snprintf(line, sizeof(line), "%-32.32s %7d %10.3f %9.3f\n", s.name.c_str(), s.paints,
                    s.totalSeconds * 1000.0, s.maxSeconds * 1000.0);
      out += line;
    }
    return out;
  }

  // ── ComponentPaintObserver ───────────────────────────────────────────────

  /** Called by the tree just before a component's own paint(): notes the
   *  clock. Not for calling directly. */
  void willPaint(Component& c, Graphics&) override {
    if (!timing_) return;
    // A stack, because a paint() may itself paint another tree -- a cache
    // filling itself is exactly that -- and the inner one must not be billed
    // to the outer.
    open_.push_back({&c, clock_()});
  }

  /** Called just after it: adds the time to that component's Stat. A paint
   *  whose start was not seen (timing turned on in between) is not counted. */
  void didPaint(Component& c, Graphics&) override {
    if (!timing_ || open_.empty() || open_.back().component != &c) return;
    const double elapsed = clock_() - open_.back().start;
    open_.pop_back();
    Stat& s = statFor(c);
    ++s.paints;
    s.lastSeconds = elapsed;
    s.totalSeconds += elapsed;
    if (elapsed > s.maxSeconds) s.maxSeconds = elapsed;
  }

  void didPaintTree(Component&, Graphics& g) override {
    if (!flashing_ || g.isClippedOut()) return;
    // Six hues a frame apart: two successive frames never share one, so a
    // region repainted twice in a row visibly changes.
    static const uint32_t kHues[] = {0xFF3B30, 0xFF9500, 0xFFCC00, 0x34C759, 0x007AFF, 0xAF52DE};
    const Colour hue = Colour::fromRGB(kHues[flashes_ % 6]).withMultipliedAlpha(flashOpacity_);
    ++flashes_;
    // fillAll fills the CLIP, which while the root paints is exactly the
    // region the window is repainting this frame.
    g.fillAll(hue);
  }

private:
  /** A paint under way: the component (an identity, as in Stat) and the
   *  clock when willPaint saw it. Kept as a stack for nested paints. */
  struct Open {
    const Component* component;
    double start;
  };

  Stat& statFor(Component& c) {
    for (Stat& s : stats_)
      if (s.component == &c) return s;
    Stat s;
    s.component = &c;
    s.name = c.accessibleName();
    if (s.name.empty()) {
      char id[40];
      std::snprintf(id, sizeof(id), "component@%p", (const void*) &c);
      s.name = id;
    }
    stats_.push_back(s);
    return stats_.back();
  }

  std::function<double()> clock_;
  std::vector<Stat> stats_;
  std::vector<Open> open_;
  bool timing_ = true;
  bool flashing_ = false;
  float flashOpacity_ = 0.35f;
  int flashes_ = 0;
};

} // namespace gfx
} // namespace sonore
