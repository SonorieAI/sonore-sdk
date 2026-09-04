// SPDX-License-Identifier: Apache-2.0
//
// The screens, and which one something is on.
//
// ── Why this is not "the screen size" ───────────────────────────────────────
//
// Everything that places a popup asked the peer for screenSize(), which
// answered for the MAIN display. On a single monitor that is right. On two it
// is wrong in a specific and confusing way: a menu opened near the bottom of a
// second screen is checked against the first screen's height, decides it fits,
// and is drawn off the bottom of the monitor it is actually on. Worse on a
// monitor placed ABOVE or LEFT of the primary, where coordinates are NEGATIVE
// and a "does it fit on screen" test against 0..height fails for every point.
//
// Producers use two monitors more than almost anyone.
//
// ── Why the peers supply it ─────────────────────────────────────────────────
//
// Enumerating displays is a platform call, and this header must compile with no
// platform at all -- sdk_tests has no window. So it is a list a peer fills in,
// with a sensible single-screen default, exactly like Clipboard.
#pragma once

#include <cmath>
#include <vector>

#include "geometry.h"

namespace sonore {
namespace gfx {

/**
 * One screen.
 *
 * NOTE: Xlib also typedefs a global `Display`, which is a CONNECTION rather
 * than a monitor. Any file that says `using namespace sonore::gfx` and includes
 * window_x11.h has to spell one of them out -- the same trap `Font` and
 * `Window` already spring, and documented in window_x11.h's header for the same
 * reason.
 */
struct Display {
  /** In DESKTOP coordinates, which on a multi-monitor setup can be negative:
   *  a screen to the left of the primary starts at a negative x. Code that
   *  assumes 0,0 is the top-left of the desktop is wrong on that setup and
   *  right on every single-monitor one, which is why it survives. */
  Rect bounds;
  /** The area minus taskbars and docks. What a window should actually be
   *  placed inside. */
  Rect workArea;
  /**
   * Device pixels per logical pixel on THIS screen: 1.0, 1.25, 1.5, 2.0, 3.0.
   *
   * Per-screen rather than per-desktop, because the mixed setup is the normal
   * one -- a 4K laptop panel at 200% with a 1080p monitor at 100% beside it is
   * what most people who own both are looking at. A window dragged from one to
   * the other has to be told, and a single global scale has no way to say it.
   *
   * In a PLUGIN this is usually not what decides the editor's scale: the host
   * owns the window and tells us through clap.gui set_scale,
   * IPlugViewContentScaleSupport or ui:scaleFactor, and its answer wins because
   * it may be compositing the view somewhere we cannot see. This is what the
   * STANDALONE uses, and what a plugin falls back to when the host says
   * nothing.
   */
  float scale = 1.0f;
  bool isMain = false;
};

/**
 * Every screen, as the peers last reported them.
 *
 * A list rather than a query, because it is asked for on every popup and a
 * platform call per menu is a round trip nobody needs -- and because a build
 * with no window still has to answer something sensible.
 */
class Displays {
public:
  /** Replaced wholesale by a peer when a window opens, and whenever the
   *  arrangement changes -- somebody plugging in a monitor mid-session is a
   *  thing that happens, and a cached list that never updated would place menus
   *  on a screen that is no longer there. */
  static std::vector<Display>& all() {
    static std::vector<Display> displays = {defaultDisplay()};
    return displays;
  }

  static void set(std::vector<Display> displays) {
    if (displays.empty()) displays.push_back(defaultDisplay());
    all() = std::move(displays);
  }

  static const Display& main() {
    const std::vector<Display>& list = all();
    for (const Display& d : list)
      if (d.isMain) return d;
    return list[0];
  }

  /**
   * The display containing `p`, or the NEAREST one if it is between screens.
   *
   * Nearest rather than main: a point that falls in the gap between two
   * monitors of different heights belongs to whichever is closer, and answering
   * "the primary" would throw a menu across the desk.
   */
  static const Display& containing(Point p) {
    const std::vector<Display>& list = all();
    for (const Display& d : list)
      if (d.bounds.contains(p)) return d;

    size_t best = 0;
    float bestDistance = 1e30f;
    for (size_t i = 0; i < list.size(); ++i) {
      const float distance = distanceTo(list[i].bounds, p);
      if (distance < bestDistance) {
        bestDistance = distance;
        best = i;
      }
    }
    return list[best];
  }

  /** The whole desktop, which is every screen unioned. */
  static Rect totalBounds() {
    const std::vector<Display>& list = all();
    Rect out = list[0].bounds;
    for (size_t i = 1; i < list.size(); ++i) out = out.united(list[i].bounds);
    return out;
  }

  static int count() { return (int) all().size(); }


  /** The scale of the screen a point is on. The reason placement takes a whole
   *  Display rather than a size: on a mixed setup the answer differs per
   *  monitor, and asking the main one is right until somebody has two. */
  static float scaleAt(Point p) { return containing(p).scale; }

private:
  /** What a build with no window peer gets. 1920x1080 rather than zero, because
   *  every placement rule divides or compares against it and zero would put
   *  every menu at the origin. */
  static Display defaultDisplay() {
    Display d;
    d.bounds = Rect(0.0f, 0.0f, 1920.0f, 1080.0f);
    d.workArea = d.bounds;
    d.isMain = true;
    return d;
  }

  /** Zero inside, otherwise the distance to the nearest edge. */
  static float distanceTo(const Rect& r, Point p) {
    const float dx = p.x < r.x ? r.x - p.x : (p.x > r.right() ? p.x - r.right() : 0.0f);
    const float dy = p.y < r.y ? r.y - p.y : (p.y > r.bottom() ? p.y - r.bottom() : 0.0f);
    return std::sqrt(dx * dx + dy * dy);
  }
};

} // namespace gfx
} // namespace sonore
