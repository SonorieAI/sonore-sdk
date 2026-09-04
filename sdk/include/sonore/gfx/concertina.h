// SPDX-License-Identifier: Apache-2.0
//
// Stacked sections that open and close.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// A plugin with forty parameters. Shown as forty rows it is a wall, and the
// four the user came for are somewhere in the middle of it. Shown as five
// sections -- Oscillator, Filter, Envelope, Effects, Output -- four of them
// closed, it is the one they wanted and a list of where the rest live.
//
// The parameters already carry their group: ParamInfo::group exists and every
// wrapper reports it, so a host's OWN generic panel groups them. Our editor
// was the only thing showing the flat wall.
//
// ── Heights, and why they animate ──────────────────────────────────────────
//
// A section snapping open moves everything below it instantly, and the user has
// to re-find where they were looking. The same movement over 150 milliseconds
// is followed by the eye without effort. This is one of the few places an
// animation is not decoration -- it is what keeps the reader's place.
//
// Through the ValueAnimator, so the clock stays an argument and the layout is
// exactly testable at any point in the transition.
//
// ── One open at a time, or several ─────────────────────────────────────────
//
// Both, because both are right for different editors. A synth wants several
// open; a settings panel wants one. setMaximumOpen decides, and the default is
// several -- a plugin whose sections fight each other is the surprising one.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "animator.h"
#include "lookandfeel.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

class ConcertinaPanel : public Widget {
public:
  ConcertinaPanel() { setWantsKeyboardFocus(true); }

  /**
   * Add a section. The content is NOT owned -- same rule as Viewport, and for
   * the same reason: a panel that owned its contents would decide their
   * lifetime, and the lifetime belongs to whoever built them.
   */
  int addSection(std::string title, Component* content, float contentHeight, bool open = false) {
    Section section;
    section.title = std::move(title);
    section.content = content;
    section.contentHeight = contentHeight > 0.0f ? contentHeight : 0.0f;
    section.open = open;
    section.currentHeight = open ? section.contentHeight : 0.0f;
    sections_.push_back(std::move(section));
    if (content) addChild(content);
    resized();
    return (int) sections_.size() - 1;
  }

  int numSections() const { return (int) sections_.size(); }

  bool isSectionOpen(int index) const {
    return index >= 0 && index < (int) sections_.size() && sections_[(size_t) index].open;
  }

  /** The height a section's content is CURRENTLY given -- mid-animation this is
   *  between zero and its full height, which is what makes the transition
   *  testable rather than a thing you have to watch. */
  float sectionHeight(int index) const {
    if (index < 0 || index >= (int) sections_.size()) return 0.0f;
    return sections_[(size_t) index].currentHeight;
  }

  /**
   * How many may be open at once. 0 means any number.
   *
   * Opening one past the limit CLOSES the least recently opened, rather than
   * refusing: a click that appears to do nothing is worse than one that does
   * something the user can undo by clicking again.
   */
  void setMaximumOpen(int count) {
    maximumOpen_ = count < 0 ? 0 : count;
    enforceLimit(-1, 0.0);
  }

  void setSectionOpen(int index, bool open, bool animated = true, double now = 0.0) {
    if (index < 0 || index >= (int) sections_.size()) return;
    Section& section = sections_[(size_t) index];
    if (section.open == open) return;
    section.open = open;
    if (open) {
      section.openedAt = ++openCounter_;
      enforceLimit(index, now);
    }
    applyHeight(index, animated, now);
    resized();
    if (onSectionToggled) onSectionToggled(index, open);
  }

  std::function<void(int index, bool open)> onSectionToggled;

  /** Drive the transitions. Called from the editor's clock; returns whether
   *  anything moved, so a still panel costs no repaint. */
  bool tick(double now) {
    if (!animator_.tick(now)) return false;
    resized();
    repaint();
    return true;
  }

  void setHeaderHeight(float height) {
    headerHeight_ = height > 4.0f ? height : 4.0f;
    resized();
  }
  float headerHeight() const { return headerHeight_; }

  /** How tall the whole panel wants to be right now. A Viewport above this asks
   *  every frame during a transition, which is what makes the scroll bar grow
   *  with the section rather than after it. */
  float preferredHeight() const {
    float total = 0.0f;
    for (const Section& section : sections_) total += headerHeight_ + section.currentHeight;
    return total;
  }

  void resized() override {
    float y = 0.0f;
    const float width = localBounds().w;
    for (Section& section : sections_) {
      y += headerHeight_;
      if (section.content) {
        section.content->setBounds({0.0f, y, width, section.currentHeight});
        // Hidden when it has no height at all. A zero-height component still
        // takes hit tests and still appears in the accessible tree, and a
        // reader announcing the contents of a closed section is a reader
        // reading the whole plugin.
        section.content->setVisible(section.currentHeight > 0.5f);
      }
      y += section.currentHeight;
    }
  }

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    const float width = localBounds().w;
    float y = 0.0f;
    for (size_t i = 0; i < sections_.size(); ++i) {
      const Section& section = sections_[i];
      const Rect header(0.0f, y, width, headerHeight_);

      g.setColour(lf.background());
      g.fillRect(header);
      g.setColour(lf.outline());
      g.fillRect(Rect(0.0f, y + headerHeight_ - 1.0f, width, 1.0f));

      drawTwisty(g, Rect(kTwistyInset, y, kTwistyWidth, headerHeight_), section.open, lf);
      lf.drawLabel(g, Rect(kTwistyInset + kTwistyWidth, y, width - kTwistyInset - kTwistyWidth,
                           headerHeight_),
                   section.title, font(), Justify::Left, stateFor(false));

      y += headerHeight_ + section.currentHeight;
    }
    paintFocusRing(g);
  }

  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    const int index = headerAt(e.position.y);
    if (index < 0) return;
    focusedSection_ = index;
    setSectionOpen(index, !sections_[(size_t) index].open, true, clockForInput_);
  }

  /**
   * The clock a mouse-driven toggle animates from.
   *
   * A component does not have a clock and must not read one -- that is the rule
   * every timed thing here follows. So the owner tells it what time it is, on
   * the same tick it would call tick() with, and a click animates from there.
   * Without this a click would start its animation at time zero and jump.
   */
  void setInputClock(double now) { clockForInput_ = now; }

  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled() || sections_.empty()) return false;
    if (key.is(KeyPress::Down)) {
      focusedSection_ = focusedSection_ + 1 >= (int) sections_.size() ? 0 : focusedSection_ + 1;
      repaint();
      return true;
    }
    if (key.is(KeyPress::Up)) {
      focusedSection_ = focusedSection_ <= 0 ? (int) sections_.size() - 1 : focusedSection_ - 1;
      repaint();
      return true;
    }
    if (key.is(KeyPress::Return) || key.character == (uint32_t) ' ') {
      if (focusedSection_ < 0) focusedSection_ = 0;
      setSectionOpen(focusedSection_, !sections_[(size_t) focusedSection_].open, true,
                     clockForInput_);
      return true;
    }
    return false;
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Group);
    if (info.name.empty()) info.name = "Sections";
    return info;
  }

  static constexpr float kTwistyWidth = 18.0f;
  static constexpr float kTwistyInset = 6.0f;
  /** How long a section takes to open. Long enough for the eye to follow the
   *  movement, short enough that nobody waits for it. */
  static constexpr double kOpenSeconds = 0.15;

private:
  struct Section {
    std::string title;
    Component* content = nullptr;
    float contentHeight = 0.0f;
    float currentHeight = 0.0f;
    bool open = false;
    /** When it was opened, for the least-recently-opened rule. */
    long openedAt = 0;
    int animation = 0;
  };

  /**
   * Takes an INDEX, not a reference.
   *
   * sections_ is a vector, so adding a section reallocates it -- and an
   * animation holding a Section* would be writing into freed memory the moment
   * a panel gained a section mid-transition. An index survives the
   * reallocation, and it is the only thing here that does.
   */
  void applyHeight(int index, bool animated, double now) {
    if (index < 0 || index >= (int) sections_.size()) return;
    Section& section = sections_[(size_t) index];
    const float target = section.open ? section.contentHeight : 0.0f;
    if (section.animation != 0) animator_.cancel(section.animation, false);
    if (!animated) {
      section.currentHeight = target;
      section.animation = 0;
      return;
    }
    section.animation = animator_.animate(
        [this, index](double value) {
          if (index >= (int) sections_.size()) return;
          sections_[(size_t) index].currentHeight = (float) value;
          resized();
          repaint();
        },
        section.currentHeight, target, kOpenSeconds, now, Easing::Out);
  }

  /** Close the least recently opened until the limit is met. `keep` is the one
   *  just opened, which must survive. */
  void enforceLimit(int keep, double now) {
    if (maximumOpen_ <= 0) return;
    for (;;) {
      int openCount = 0, oldest = -1;
      long oldestAt = 0;
      for (size_t i = 0; i < sections_.size(); ++i) {
        if (!sections_[i].open) continue;
        ++openCount;
        if ((int) i == keep) continue;
        if (oldest < 0 || sections_[i].openedAt < oldestAt) {
          oldest = (int) i;
          oldestAt = sections_[i].openedAt;
        }
      }
      if (openCount <= maximumOpen_ || oldest < 0) return;
      sections_[(size_t) oldest].open = false;
      applyHeight(oldest, true, now);
      if (onSectionToggled) onSectionToggled(oldest, false);
    }
  }

  int headerAt(float y) const {
    float top = 0.0f;
    for (size_t i = 0; i < sections_.size(); ++i) {
      if (y >= top && y < top + headerHeight_) return (int) i;
      top += headerHeight_ + sections_[i].currentHeight;
    }
    return -1;
  }

  /** The same triangle TreeView draws, and deliberately: two twisties in one
   *  editor that pointed different ways would read as two different things. */
  static void drawTwisty(Graphics& g, const Rect& area, bool open, LookAndFeel& lf) {
    const float size = 4.0f;
    const float cx = area.x + area.w * 0.5f;
    const float cy = area.y + area.h * 0.5f;
    Path p;
    if (open) {
      p.moveTo(cx - size, cy - size * 0.6f);
      p.lineTo(cx + size, cy - size * 0.6f);
      p.lineTo(cx, cy + size * 0.8f);
    } else {
      p.moveTo(cx - size * 0.6f, cy - size);
      p.lineTo(cx + size * 0.8f, cy);
      p.lineTo(cx - size * 0.6f, cy + size);
    }
    p.close();
    g.setColour(lf.dimText());
    g.fillPath(p);
  }

  std::vector<Section> sections_;
  ValueAnimator animator_;
  float headerHeight_ = 24.0f;
  int maximumOpen_ = 0;
  int focusedSection_ = -1;
  long openCounter_ = 0;
  double clockForInput_ = 0.0;
};

} // namespace gfx
} // namespace sonore
