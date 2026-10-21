// SPDX-License-Identifier: Apache-2.0
//
// A preferences window's contents: a row of page buttons along the top, and
// the chosen page below them.
//
// ── How it differs from a TabbedComponent ──────────────────────────────────
//
// containers.h's tabs OWN nothing and keep every page alive: right for an
// instrument's oscillator/filter/envelope pages, which hold controls bound to
// parameters and are switched between constantly. A preferences page is the
// opposite. It is opened rarely, it may be expensive to build (an audio
// device list that asks every driver what it has), and it should show the
// state of things NOW, not as they were when the window first opened. So --
// as the reference does -- a page here is a component made on demand by
// createComponentForPage() when its button is chosen, and destroyed when
// another is: switching back to "Audio" re-reads the devices.
//
// The buttons are square, an icon above a label, the current one drawn
// toggled; they are painted by the LookAndFeel's icon-button background so a
// theme reaches them without a new hook. An icon is an SVG string (svg.h's
// Drawable), drawn in the theme's colour when it uses currentColor.
//
// Where it is shown is the caller's business: in an application window, in a
// DocumentWindow, or inside a plugin editor as an Overlay's content.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "component.h"
#include "graphics.h"
#include "svg.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * A preferences window's contents: page buttons along the top and the
 * chosen page below -- the reference's PreferencesPanel. The file comment
 * says why a page is built on demand rather than kept.
 *
 * Supply pages by overriding createComponentForPage() or by setting
 * createPage -- before the first addSettingsPage(), which builds the first
 * page at once. The panel owns the current page and lets it go whenever
 * another is chosen -- with its own buttons, or with setCurrentPage from
 * anywhere, a handler on the page itself included: the page is then kept
 * until the next paint, so that handler returns into a page still there.
 * Nothing should keep a pointer from getCurrentPageComponent() across a
 * switch. Message thread only.
 */
class PreferencesPanel : public Widget {
public:
  /** The reference's default button size. */
  static constexpr float kDefaultButtonSize = 70.0f;
  /** The space between the button row and the page. */
  static constexpr float kPageGap = 5.0f;

  /** An empty panel with no pages; it takes the keyboard, for Left and
   *  Right. */
  PreferencesPanel() { setWantsKeyboardFocus(true); }

  /** Destroys the current page and any page waiting to be freed. */
  ~PreferencesPanel() override { dropPage(); }

  /**
   * Makes a page, for a caller that would rather not subclass:
   * createComponentForPage() calls it when it is not overridden.
   */
  std::function<std::unique_ptr<Component>(const std::string& pageName)> createPage;

  /** Told after the page changes, with the new page's name. */
  std::function<void(const std::string& pageName)> onPageChanged;

  /**
   * Add a page button. `iconSvg` may be empty (a label alone). The first page
   * added becomes the current one, as in the reference.
   */
  void addSettingsPage(const std::string& title, const std::string& iconSvg = std::string()) {
    Page page;
    page.title = title;
    if (!iconSvg.empty()) page.icon = Drawable::parse(iconSvg);
    pages_.push_back(std::move(page));
    resized();
    repaint();
    if (current_ < 0) setCurrentPage(title);
  }

  /** How many pages were added. */
  int getNumPages() const { return (int) pages_.size(); }

  /** The title (and name) of page `index`, 0-based in the order added;
   *  empty out of range. */
  const std::string& getPageTitle(int index) const {
    static const std::string none;
    return index >= 0 && index < (int) pages_.size() ? pages_[(size_t) index].title : none;
  }

  /**
   * Show the page with this name: the old page's component leaves the panel
   * and a new one is made for this page. Choosing the page already showing
   * does nothing (no rebuild). An unknown name is ignored.
   *
   * The old page is destroyed at the panel's next paint, not here: this is
   * called from inside the page as often as not -- a button on it that goes
   * to another page -- and destroying it here freed the button whose click
   * was still on the stack, and the page name its handler had passed in. A
   * switch made with the panel's own buttons or keys destroys it at once.
   */
  void setCurrentPage(const std::string& pageName) {
    int index = -1;
    for (size_t i = 0; i < pages_.size(); ++i)
      if (pages_[i].title == pageName) index = (int) i;
    if (index < 0 || index == current_) return;
    retirePage();
    current_ = index;
    page_ = createComponentForPage(pageName);
    if (page_) {
      addChild(page_.get());
      page_->setBounds(pageArea());
    }
    repaint();
    if (onPageChanged) onPageChanged(pageName);
  }

  /** The current page's name; empty before any page was added. */
  const std::string& getCurrentPageName() const { return getPageTitle(current_); }

  /** The current page's 0-based index; -1 before any page was added. */
  int getCurrentPageIndex() const { return current_; }

  /** The current page's component -- owned by this panel. */
  Component* getCurrentPageComponent() const { return page_.get(); }

  /** The buttons' width and height. */
  void setButtonSize(float size) {
    buttonSize_ = size < 16.0f ? 16.0f : size;
    resized();
    repaint();
  }
  /** The buttons' side in logical units: kDefaultButtonSize, or what
   *  setButtonSize() set (at least 16). */
  float getButtonSize() const { return buttonSize_; }

  /** Where button `index` is, in this component's coordinates. */
  Rect pageButtonBounds(int index) const {
    if (index < 0 || index >= (int) pages_.size()) return {};
    return {(float) index * buttonSize_, 0.0f, buttonSize_, buttonSize_};
  }

  /** Where the page goes: everything below the buttons. */
  Rect pageArea() const {
    const Rect b = localBounds();
    const float top = buttonSize_ + kPageGap;
    return {0.0f, top, b.w, b.h > top ? b.h - top : 0.0f};
  }

  /**
   * Make the component for a page. Override it, or set createPage. The
   * panel owns what comes back; null shows an empty page.
   */
  virtual std::unique_ptr<Component> createComponentForPage(const std::string& pageName) {
    return createPage ? createPage(pageName) : nullptr;
  }

  // ── Component ─────────────────────────────────────────────────────────────

  /** Fits the current page to pageArea(). */
  void resized() override {
    if (page_) page_->setBounds(pageArea());
  }

  /** Frees the pages switched away from, then draws the buttons (the look
   *  and feel's icon-button background, the icon, the label) and the rule
   *  under them. */
  void paint(Graphics& g) override {
    retired_.clear(); // see setCurrentPage: freed here, never inside a click
    LookAndFeel& lf = lookAndFeel();
    g.fillAll(lf.background());
    const Font& f = font();
    const float labelHeight = f.isValid() ? f.lineHeight() : 0.0f;
    for (size_t i = 0; i < pages_.size(); ++i) {
      const int index = (int) i;
      const Rect button = pageButtonBounds(index).reduced(2.0f);
      const bool toggled = index == current_;
      WidgetState state;
      state.hovered = index == hovered_;
      state.pressed = index == pressed_;
      state.toggled = toggled;
      state.enabled = isEnabled();
      lf.drawIconButtonBackground(g, button, state, toggled);
      const float pad = button.w * 0.18f;
      const Rect iconArea(button.x + pad, button.y + pad * 0.6f, button.w - pad * 2.0f,
                          button.h - pad * 1.2f - labelHeight);
      if (!pages_[i].icon.isEmpty() && iconArea.w > 0.0f && iconArea.h > 0.0f)
        pages_[i].icon.draw(g, iconArea, lf.iconButtonColour(state, toggled));
      if (labelHeight > 0.0f) {
        g.setColour(toggled ? lf.text() : lf.dimText());
        // With no icon the label sits in the middle; with one, under it.
        const Rect label = pages_[i].icon.isEmpty()
                               ? button
                               : Rect(button.x, button.bottom() - labelHeight - 3.0f, button.w,
                                      labelHeight);
        f.drawIn(g, pages_[i].title, label, Justify::Centred);
      }
    }
    // The rule between the buttons and the page, as the reference draws it.
    g.setColour(lf.outline());
    g.fillRect(Rect(0.0f, buttonSize_ + 2.0f, localBounds().w, 1.0f));
  }

  /** Presses the page button under the pointer; the page changes on the
   *  release. */
  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    pressed_ = buttonAt(e.position);
    if (pressed_ >= 0) repaint();
  }

  /** Released over the button pressed: that page becomes current, and the
   *  old one is freed at once. */
  void mouseUp(const MouseEvent& e) override {
    const int pressed = pressed_;
    pressed_ = -1;
    if (pressed < 0) return;
    repaint();
    // Released over the button it went down on, as every button does.
    if (isEnabled() && buttonAt(e.position) == pressed) choosePage(pressed);
  }

  /** Tracks the button under the pointer, for its hover look. */
  void mouseMove(const MouseEvent& e) override {
    const int over = buttonAt(e.position);
    if (over == hovered_) return;
    hovered_ = over;
    repaint();
  }

  /** Clears the hovered button. */
  void mouseExit(const MouseEvent& e) override {
    Widget::mouseExit(e);
    if (hovered_ < 0) return;
    hovered_ = -1;
    repaint();
  }

  /** Left and right walk the pages, for a keyboard user. */
  bool keyPressed(const KeyPress& key) override {
    if (pages_.empty() || !isEnabled()) return false;
    if (key.is(KeyPress::Left) && current_ > 0) {
      choosePage(current_ - 1);
      return true;
    }
    if (key.is(KeyPress::Right) && current_ + 1 < (int) pages_.size()) {
      choosePage(current_ + 1);
      return true;
    }
    return false;
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Group);
    if (info.name.empty()) info.name = "Preferences";
    info.value = getCurrentPageName();
    return info;
  }

private:
  /** One page button: its title, which is also the page's name, and its icon
   *  parsed once from the SVG (empty for a label alone). */
  struct Page {
    std::string title;
    Drawable icon;
  };

  int buttonAt(Point p) const {
    for (size_t i = 0; i < pages_.size(); ++i)
      if (pageButtonBounds((int) i).contains(p)) return (int) i;
    return -1;
  }

  void dropPage() {
    if (!page_) return;
    removeChild(page_.get());
    page_.reset();
  }

  /** Out of the tree now -- the router forgets whatever inside it had the
   *  focus or the mouse -- and freed at the next paint: see setCurrentPage. */
  void retirePage() {
    if (!page_) return;
    removeChild(page_.get());
    retired_.push_back(std::move(page_));
  }

  /** A switch from the panel's own buttons or keys, which no page is on the
   *  stack for: the old page is freed at once. Unless onPageChanged closed
   *  the window this panel was in, and the panel with it. */
  void choosePage(int index) {
    detail::DeletionWatch alive(this);
    setCurrentPage(pages_[(size_t) index].title);
    if (!alive.deleted) retired_.clear();
  }

  std::vector<Page> pages_;
  std::unique_ptr<Component> page_;
  /** Pages switched away from, kept until the next paint. */
  std::vector<std::unique_ptr<Component>> retired_;
  int current_ = -1;
  int hovered_ = -1;
  int pressed_ = -1;
  float buttonSize_ = kDefaultButtonSize;
};

} // namespace gfx
} // namespace sonore
