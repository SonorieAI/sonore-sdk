// SPDX-License-Identifier: Apache-2.0
//
// MultiDocumentPanel: several documents in one area, as tabs or as windows.
//
// ── What it is for here ─────────────────────────────────────────────────────
//
// A plugin with more than one thing open at once -- several sample editors in
// a sampler, a modulation matrix beside an envelope, the scripts of a
// scriptable effect -- and a standalone app built on this SDK. The panel holds
// documents (any Component) and shows them one of two ways: as tabs, one
// document filling the area under a tab strip, or as windows floating inside
// the panel that can be moved, resized, raised and closed. Switching between
// the two keeps every window's place.
//
// ── Windows inside the editor, not on the desktop ───────────────────────────
//
// The windows are components, drawn with a title bar by the look-and-feel.
// A plugin may not open top-level windows of its own without risking them
// behind the host's -- alert.h has that story -- and documents that float
// inside the editor cannot be lost.
//
// An inactive window takes the FIRST click to come to the front and nothing
// else: its content is covered until then. A click that both raised a window
// and pressed the button under it would act on a document the user had not
// yet seen come forward.
//
// ── Closing is a question ───────────────────────────────────────────────────
//
// A document with unsaved changes has to be able to say no. tryToCloseDocument
// is asked first and answers through a callback rather than a return value, so
// the answer can come after an AlertOverlay has asked the user -- nothing here
// blocks, and nothing in a plugin may.
//
// ── Destroyed later, not now ────────────────────────────────────────────────
//
// A window is closed from its own close button, and an owned document often
// from a button inside it. Destroying either there frees the object whose
// click is still on the stack. They are taken out of the tree at once -- the
// router forgets everything inside -- and freed at the next paint.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "side_panel.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

class MultiDocumentPanel;

/**
 * One floating document: a frame with a title bar, a close button and a
 * resize corner, around the document.
 */
class MultiDocumentPanelWindow : public Widget {
public:
  /** The title bar's height, logical units. */
  static constexpr float kTitleHeight = 24.0f;
  /** The frame's width at the sides and the bottom, logical units. */
  static constexpr float kBorder = 3.0f;
  /** The side of the square resize corner at the bottom right. */
  static constexpr float kGrip = 12.0f;
  /** The smallest width a resize drag leaves. */
  static constexpr float kMinWidth = 120.0f;
  /** The smallest height a resize drag leaves. */
  static constexpr float kMinHeight = 70.0f;

  /** Made by MultiDocumentPanel when its documents float; `document` stays
   *  the panel's (or its caller's) and is not owned by the window. A
   *  transparent `background` leaves the content area unfilled. */
  MultiDocumentPanelWindow(MultiDocumentPanel& owner, Component* document, std::string title,
                           Colour background);

  /** The document the window frames. */
  Component* document() const { return document_; }
  /** The text in the title bar: the document's name in the panel. */
  const std::string& title() const { return title_; }
  /** Change the title bar's text only; MultiDocumentPanel::setDocumentName
   *  changes both the name and this. */
  void setTitle(std::string title) {
    title_ = std::move(title);
    repaint();
  }
  /** Its document is the panel's active one. */
  bool isActiveWindow() const;
  /** Filling the panel after toggleMaximised(); not movable or resizable
   *  while it is. */
  bool isMaximised() const { return maximised_; }

  /** Where the document sits, in this window's coordinates. */
  Rect contentBounds() const {
    const Rect b = localBounds();
    return {kBorder, kTitleHeight, (std::max)(0.0f, b.w - kBorder * 2.0f),
            (std::max)(0.0f, b.h - kTitleHeight - kBorder)};
  }
  /** The title bar, in this window's coordinates: a press there drags the
   *  window, a double click toggles maximised. */
  Rect titleBarBounds() const { return {0.0f, 0.0f, localBounds().w, kTitleHeight}; }
  /** The close button, in this window's coordinates. It closes the
   *  document through the panel, asking tryToCloseDocument first. */
  Rect closeButtonBounds() const { return close_.bounds(); }
  /** The resize corner, in this window's coordinates. */
  Rect resizeGripBounds() const {
    const Rect b = localBounds();
    return {b.w - kGrip, b.h - kGrip, kGrip, kGrip};
  }

  /** Fill the panel, or go back to the bounds it had before. */
  void toggleMaximised();

  /** Places the close button, the document and the resize corner. */
  void resized() override {
    const float side = kTitleHeight - 6.0f;
    close_.setBounds({localBounds().w - side - 4.0f, 3.0f, side, side});
    const Rect content = contentBounds();
    if (document_ && document_->parent() == this) document_->setBounds(content);
    catcher_.setBounds(content);
    grip_.setBounds(resizeGripBounds());
    grip_.setVisible(!maximised_);
  }

  /** The frame and title bar from the look and feel's drawDocumentWindow,
   *  then the background colour under the document. */
  void paint(Graphics& g) override;

  /** Makes its document active; on the title bar starts a move, or on a
   *  double click toggles maximised. */
  void mouseDown(const MouseEvent& e) override;
  /** Moves the window, keeping 40 units of the title bar inside the panel
   *  and never above its top. */
  void mouseDrag(const MouseEvent& e) override;
  /** Ends a move or a resize. */
  void mouseUp(const MouseEvent&) override { drag_ = Drag::None; }

  MouseCursor cursor() const override { return MouseCursor::Default; }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Window);
    if (info.name.empty()) info.name = title_;
    return info;
  }

private:
  friend class MultiDocumentPanel;

  /** Laid over an inactive window's content: the first click raises the
   *  window and goes no further. */
  class ActivationCatcher : public Component {
  public:
    MultiDocumentPanelWindow* window = nullptr;
    ActivationCatcher() { setAccessibilityIgnored(true); }
    void mouseDown(const MouseEvent&) override;
  };

  /** The resize corner, as a child ABOVE the document: drawn by the window
   *  underneath, the corner was the document's -- every press on it went to
   *  whatever filled the window and nothing resized. */
  class ResizeGrip : public Component {
  public:
    MultiDocumentPanelWindow* window = nullptr;
    ResizeGrip() {
      setAccessibilityIgnored(true);
      setCursor(MouseCursor::ResizeCorner);
    }
    void paint(Graphics& g) override {
      if (window) window->lookAndFeel().drawResizableCorner(g, localBounds(), window->drag_ == Drag::Resize);
    }
    void mouseDown(const MouseEvent& e) override {
      if (window) window->beginDrag(Drag::Resize, e.rootPosition);
    }
    void mouseDrag(const MouseEvent& e) override {
      if (window) window->dragTo(e.rootPosition);
    }
    void mouseUp(const MouseEvent&) override {
      if (window) window->drag_ = Drag::None;
      repaint();
    }
  };

  enum class Drag { None, Move, Resize };

  void beginDrag(Drag kind, Point rootPos);
  void dragTo(Point rootPos);

  /** Inactive: the catcher covers the content, and the grip stays on top of
   *  both so an inactive window can still be resized. */
  void updateActiveState(bool active) {
    catcher_.setVisible(!active);
    if (!active) catcher_.toFront();
    grip_.toFront();
    repaint();
  }

  MultiDocumentPanel& owner_;
  Component* document_;
  std::string title_;
  Colour background_;
  detail::CloseButton close_;
  ActivationCatcher catcher_;
  ResizeGrip grip_;
  Drag drag_ = Drag::None;
  Rect dragStart_;
  Point dragRootStart_;
  Rect restored_;
  bool maximised_ = false;
};

/**
 * Several documents in one area, as tabs or as windows floating inside it --
 * the reference's MultiDocumentPanel. The file comment covers the windows,
 * closing as a question, and why destruction is deferred.
 *
 * A document is any Component, either owned by the caller (it must outlive
 * its time here, and is handed back out of any window when it closes or the
 * panel goes) or handed over as a unique_ptr and destroyed when it closes --
 * at the next paint, not inside the close call. Message thread only;
 * tryToCloseDocument may answer later, after asking the user.
 */
class MultiDocumentPanel : public Widget {
public:
  /** How the documents are shown (the reference's names). */
  enum class LayoutMode {
    /** Each document in a MultiDocumentPanelWindow floating in the panel. */
    FloatingWindows,
    /** One document at a time filling the panel under a strip of tabs (the
     *  default). */
    MaximisedWindowsWithTabs
  };

  /** The tab strip's height, logical units. */
  static constexpr float kTabHeight = 26.0f;
  /** The widest a tab gets; narrower when the panel's width shared by the
   *  documents is less. */
  static constexpr float kMaxTabWidth = 180.0f;

  /** An empty panel showing tabs, with no limit on the number of documents. */
  MultiDocumentPanel() : alive_(std::make_shared<bool>(true)) { setAccessibleName("Documents"); }

  /** Destroys the documents it owns and hands the caller's back out of any
   *  window, unparented. An answer to tryToCloseDocument that comes later
   *  finds the panel gone and closes nothing. */
  ~MultiDocumentPanel() override {
    *alive_ = false;
    // Documents the caller owns are handed back clean, out of any window.
    for (auto& doc : docs_)
      if (!doc->owned && doc->component->parent()) doc->component->parent()->removeChild(doc->component);
  }

  // ── Documents ────────────────────────────────────────────────────────────

  /**
   * Add a document the caller owns. False when the panel is full -- see
   * setMaximumNumDocuments -- or the component is already here.
   */
  bool addDocument(Component* document, std::string name, Colour background = Colour(0, 0, 0, 0)) {
    if (!document) return false;
    return add(document, nullptr, std::move(name), background);
  }

  /** Add a document the panel owns from now on, and destroys when it closes. */
  bool addDocument(std::unique_ptr<Component> document, std::string name,
                   Colour background = Colour(0, 0, 0, 0)) {
    if (!document) return false;
    Component* raw = document.get();
    return add(raw, std::move(document), std::move(name), background);
  }

  /** How many documents the panel holds. */
  int numDocuments() const { return (int) docs_.size(); }
  /** The document at `index`, 0-based in the order they were added (the
   *  tabs' order); null outside 0..numDocuments()-1. */
  Component* document(int index) const {
    return index >= 0 && index < (int) docs_.size() ? docs_[(size_t) index]->component : nullptr;
  }
  /** The 0-based index of `document`, or -1 when it is not in the panel. */
  int indexOf(const Component* document) const {
    for (size_t i = 0; i < docs_.size(); ++i)
      if (docs_[i]->component == document) return (int) i;
    return -1;
  }
  /** The name shown on its tab and window title; empty when it is not in
   *  the panel. */
  std::string documentName(const Component* document) const {
    const int i = indexOf(document);
    return i >= 0 ? docs_[(size_t) i]->name : std::string();
  }
  /** Rename a document: its tab and its window's title. Ignored for a
   *  component that is not in the panel. */
  void setDocumentName(Component* document, std::string name) {
    const int i = indexOf(document);
    if (i < 0) return;
    docs_[(size_t) i]->name = name;
    if (docs_[(size_t) i]->window) docs_[(size_t) i]->window->setTitle(std::move(name));
    repaint();
  }

  /** Asked before a document closes, when the close asks to be checked. Call
   *  `answer(true)` to let it go and `answer(false)` to keep it -- now, or
   *  later, after asking the user. Unset, every document may close. */
  std::function<void(Component* document, std::function<void(bool)> answer)> tryToCloseDocument;

  /**
   * Close a document, asking first when `checkItsOkToCloseFirst`.
   * `done` hears whether it closed.
   */
  void closeDocument(Component* document, bool checkItsOkToCloseFirst,
                     std::function<void(bool closed)> done = nullptr) {
    if (indexOf(document) < 0) {
      if (done) done(false);
      return;
    }
    if (!checkItsOkToCloseFirst || !tryToCloseDocument) {
      removeDocument(document);
      if (done) done(true);
      return;
    }
    std::weak_ptr<bool> alive = alive_;
    tryToCloseDocument(document, [this, alive, document, done](bool ok) {
      // The answer may come after the panel has gone, or after the document
      // was closed some other way -- an alert left up while the editor shut.
      const bool panelAlive = !alive.expired() && *alive.lock();
      bool closed = false;
      if (ok && panelAlive && indexOf(document) >= 0) {
        removeDocument(document);
        closed = true;
      }
      if (done) done(closed);
    });
  }

  /** Close every document, in turn, stopping at the first that says no. */
  void closeAllDocuments(bool checkItsOkToCloseFirst, std::function<void(bool allClosed)> done = nullptr) {
    if (docs_.empty()) {
      if (done) done(true);
      return;
    }
    std::weak_ptr<bool> alive = alive_;
    Component* first = docs_.back()->component;
    closeDocument(first, checkItsOkToCloseFirst, [this, alive, checkItsOkToCloseFirst, done](bool closed) {
      if (!closed || alive.expired() || !*alive.lock()) {
        if (done) done(false);
        return;
      }
      closeAllDocuments(checkItsOkToCloseFirst, done);
    });
  }

  // ── Which one is in front ────────────────────────────────────────────────

  /** The document in front: the tab shown, or the top window. Null only
   *  when the panel is empty. */
  Component* activeDocument() const { return active_; }

  /** Bring `document` forward: its tab shown, or its window raised. If the
   *  keyboard focus was elsewhere in the panel it moves to the document's
   *  first focusable component. Calls onActiveDocumentChanged; ignored for
   *  a component not in the panel or already active. */
  void setActiveDocument(Component* document) {
    if (indexOf(document) < 0 || document == active_) return;
    MouseRouter* router = routerFor(this);
    const bool focusWasInside = router && isInside(router->focused());
    active_ = document;
    touch(document);
    applyArrangement();
    // The keyboard follows a document brought forward from the keyboard --
    // Ctrl+Tab that left the keys in the old document would be no switch at
    // all.
    if (focusWasInside && router && !isInside(router->focused(), document))
      router->setFocus(firstFocusable(document));
    if (onActiveDocumentChanged) onActiveDocumentChanged(active_);
  }

  /** Called when the active document changes: setActiveDocument, a click,
   *  Ctrl+Tab, a document added (it becomes active) or the active one
   *  closed (null when it was the last). */
  std::function<void(Component* newActive)> onActiveDocumentChanged;

  /** Most recently active last. What decides who comes forward when the
   *  active document closes: the one the user was in before it. */
  const std::vector<Component*>& activationOrder() const { return order_; }

  /** The windows bottom to top, by their documents. Empty unless floating. */
  std::vector<Component*> windowOrder() const {
    std::vector<Component*> out;
    for (Component* c : children())
      for (const auto& doc : docs_)
        if (doc->window.get() == c) out.push_back(doc->component);
    return out;
  }

  /** The window framing `document`, owned by the panel; null when it is
   *  not in the panel or has not floated yet. Kept, out of the tree, while
   *  the panel shows tabs, so the window comes back where it was. */
  MultiDocumentPanelWindow* windowFor(const Component* document) const {
    const int i = indexOf(document);
    return i >= 0 ? docs_[(size_t) i]->window.get() : nullptr;
  }

  // ── Arrangement ──────────────────────────────────────────────────────────

  /** Switch between tabs and floating windows; every window keeps its
   *  place across the switch. */
  void setLayoutMode(LayoutMode mode) {
    if (mode == mode_) return;
    mode_ = mode;
    applyArrangement();
  }
  /** The mode asked for (MaximisedWindowsWithTabs by default). One document
   *  with useFullscreenWhenOneDocument shows neither tabs nor a window. */
  LayoutMode layoutMode() const { return mode_; }

  /** 0 means no limit. Lowering it closes nothing: the limit is on adding. */
  void setMaximumNumDocuments(int maximum) { maximum_ = maximum < 0 ? 0 : maximum; }
  /** The limit addDocument() obeys; 0 (the default) means none. */
  int maximumNumDocuments() const { return maximum_; }

  /** One document fills the panel with no frame and no tab -- a tab strip
   *  holding a single tab is a strip that says nothing. */
  void useFullscreenWhenOneDocument(bool shouldUse) {
    fullscreenWhenOne_ = shouldUse;
    applyArrangement();
  }
  /** What useFullscreenWhenOneDocument() set (false by default). */
  bool isFullscreenWhenOneDocument() const { return fullscreenWhenOne_; }

  /** The colour behind the tabs and around floating windows; unset, a
   *  shade darker than the look and feel's background. */
  void setBackgroundColour(Colour c) {
    background_ = c;
    hasBackground_ = true;
    repaint();
  }

  /** True when the documents are showing as tabs right now. */
  bool isShowingTabs() const { return arrangement() == Arrangement::Tabs; }

  /** The tab of document `index` (0-based), in the panel's coordinates;
   *  empty when tabs are not showing or `index` is out of range. */
  Rect tabBounds(int index) const {
    if (!isShowingTabs() || index < 0 || index >= (int) docs_.size()) return {};
    const float w = tabWidth();
    return {(float) index * w, 0.0f, w, kTabHeight};
  }
  /** The close box inside that tab; empty when the tab is. A click on it
   *  closes the document, asking tryToCloseDocument first. */
  Rect tabCloseBounds(int index) const {
    const Rect t = tabBounds(index);
    if (t.isEmpty()) return {};
    const float side = kTabHeight - 10.0f;
    return {t.right() - side - 4.0f, t.y + 5.0f, side, side};
  }

  // ── Component ────────────────────────────────────────────────────────────

  /** Re-lays the documents for the new size: the shown tab fills the area
   *  under the strip; floating windows keep their bounds. */
  void resized() override { applyArrangement(); }

  /** Frees the closed documents and windows waiting to be freed, fills the
   *  background and draws the tab strip when tabs are showing. */
  void paint(Graphics& g) override {
    retired_.clear(); // see the header: freed here, never inside a click
    LookAndFeel& lf = lookAndFeel();
    g.fillAll(hasBackground_ ? background_ : lf.background().darkened(0.02f));
    if (!isShowingTabs()) return;
    for (int i = 0; i < (int) docs_.size(); ++i) {
      const Rect t = tabBounds(i);
      lf.drawTab(g, Rect(t.x, t.y, t.w - kTabHeight * 0.7f, t.h), docs_[(size_t) i]->name, font(),
                 docs_[(size_t) i]->component == active_, i == hoveredTab_);
      WidgetState st;
      st.hovered = i == hoveredTab_ && hoveredClose_;
      lf.drawCloseButton(g, tabCloseBounds(i), st);
    }
  }

  /** On the tab strip: a tab's close box closes its document (asking
   *  first), the rest of a tab makes its document active. */
  void mouseDown(const MouseEvent& e) override {
    if (!isShowingTabs() || e.position.y >= kTabHeight) return;
    for (int i = 0; i < (int) docs_.size(); ++i) {
      if (tabCloseBounds(i).contains(e.position)) {
        closeDocument(docs_[(size_t) i]->component, true);
        return;
      }
      if (tabBounds(i).contains(e.position)) {
        setActiveDocument(docs_[(size_t) i]->component);
        return;
      }
    }
  }

  /** Tracks the tab and close box under the pointer, for their hover look. */
  void mouseMove(const MouseEvent& e) override {
    int over = -1;
    bool overClose = false;
    if (isShowingTabs() && e.position.y < kTabHeight)
      for (int i = 0; i < (int) docs_.size(); ++i)
        if (tabBounds(i).contains(e.position)) {
          over = i;
          overClose = tabCloseBounds(i).contains(e.position);
        }
    if (over != hoveredTab_ || overClose != hoveredClose_) {
      hoveredTab_ = over;
      hoveredClose_ = overClose;
      repaint();
    }
  }

  /** Clears the hovered tab. */
  void mouseExit(const MouseEvent& e) override {
    Widget::mouseExit(e);
    hoveredTab_ = -1;
    hoveredClose_ = false;
  }

  /** Ctrl+Tab and Ctrl+Shift+Tab walk the documents; Ctrl+F4 closes the
   *  active one, asking first. Reached when the keyboard is in a document. */
  bool keyPressed(const KeyPress& key) override {
    if (docs_.empty()) return false;
    if (key.is(KeyPress::Tab) && key.ctrlDown && !key.altDown) {
      const int n = (int) docs_.size();
      const int at = indexOf(active_);
      const int next = ((at < 0 ? 0 : at) + (key.shiftDown ? -1 : 1) + n) % n;
      setActiveDocument(docs_[(size_t) next]->component);
      return true;
    }
    if (key.is(KeyPress::F4) && key.ctrlDown && !key.altDown) {
      if (active_) closeDocument(active_, true);
      return true;
    }
    return false;
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Group);
    if (active_) info.value = documentName(active_);
    if (!docs_.empty()) {
      info.hasRange = true;
      info.minValue = 0.0;
      info.maxValue = (double) docs_.size() - 1.0;
      const int at = indexOf(active_);
      info.currentValue = (double) (at < 0 ? 0 : at);
    }
    return info;
  }

private:
  friend class MultiDocumentPanelWindow;

  enum class Arrangement { Single, Tabs, Windows };

  /** One document: the component, `owned` set when the panel owns it, its
   *  tab and window title, the window frame while it floats, and the window
   *  bounds kept across a switch to tabs and back. */
  struct Doc {
    Component* component = nullptr;
    std::unique_ptr<Component> owned;
    std::string name;
    Colour background;
    std::unique_ptr<MultiDocumentPanelWindow> window;
    Rect windowBounds;
  };

  Arrangement arrangement() const {
    if (docs_.size() == 1 && fullscreenWhenOne_) return Arrangement::Single;
    return mode_ == LayoutMode::MaximisedWindowsWithTabs ? Arrangement::Tabs : Arrangement::Windows;
  }

  float tabWidth() const {
    if (docs_.empty()) return 0.0f;
    const float share = localBounds().w / (float) docs_.size();
    return share < kMaxTabWidth ? share : kMaxTabWidth;
  }

  bool add(Component* document, std::unique_ptr<Component> owned, std::string name, Colour background) {
    if (indexOf(document) >= 0) return false;
    if (maximum_ > 0 && (int) docs_.size() >= maximum_) return false;
    std::unique_ptr<Doc> doc(new Doc());
    doc->component = document;
    doc->owned = std::move(owned);
    doc->name = std::move(name);
    doc->background = background;
    // Cascaded, so a new window never lands exactly on top of the last one
    // and hides it.
    const Rect area = localBounds();
    const float step = 24.0f * (float) (docs_.size() % 8);
    const Rect natural = document->bounds();
    const float w = natural.w > 0.0f ? natural.w + MultiDocumentPanelWindow::kBorder * 2.0f
                                     : (std::max)(MultiDocumentPanelWindow::kMinWidth, area.w * 0.6f);
    const float h = natural.h > 0.0f ? natural.h + MultiDocumentPanelWindow::kTitleHeight + MultiDocumentPanelWindow::kBorder
                                     : (std::max)(MultiDocumentPanelWindow::kMinHeight, area.h * 0.6f);
    doc->windowBounds = Rect(step, step, w, h);
    docs_.push_back(std::move(doc));
    active_ = document;
    touch(document);
    applyArrangement();
    if (onActiveDocumentChanged) onActiveDocumentChanged(active_);
    return true;
  }

  void removeDocument(Component* document) {
    const int i = indexOf(document);
    if (i < 0) return;
    std::unique_ptr<Doc> doc = std::move(docs_[(size_t) i]);
    docs_.erase(docs_.begin() + (long) i);
    for (size_t k = 0; k < order_.size(); ++k)
      if (order_[k] == document) {
        order_.erase(order_.begin() + (long) k);
        break;
      }
    // Out of the tree now -- the router forgets anything focused or captured
    // inside -- and freed at the next paint, never here (see the header).
    if (Component* p = doc->component->parent()) p->removeChild(doc->component);
    if (doc->window) {
      if (Component* p = doc->window->parent()) p->removeChild(doc->window.get());
      retired_.push_back(std::unique_ptr<Component>(doc->window.release()));
    }
    if (doc->owned) retired_.push_back(std::move(doc->owned));
    hoveredTab_ = -1;

    const bool wasActive = active_ == document;
    if (wasActive) active_ = order_.empty() ? nullptr : order_.back();
    applyArrangement();
    repaint();
    if (wasActive && onActiveDocumentChanged) onActiveDocumentChanged(active_);
  }

  void touch(Component* document) {
    for (size_t k = 0; k < order_.size(); ++k)
      if (order_[k] == document) {
        order_.erase(order_.begin() + (long) k);
        break;
      }
    order_.push_back(document);
  }

  /** Put every document where the current arrangement says. */
  void applyArrangement() {
    const Arrangement a = arrangement();
    const Rect area = localBounds();
    for (auto& docPtr : docs_) {
      Doc& doc = *docPtr;
      const bool isActive = doc.component == active_;
      if (a == Arrangement::Windows) {
        if (!doc.window) {
          doc.window.reset(new MultiDocumentPanelWindow(*this, doc.component, doc.name, doc.background));
          doc.window->setBounds(doc.windowBounds);
          addChild(doc.window.get());
        } else if (doc.window->parent() != this) {
          addChild(doc.window.get());
        }
        if (doc.component->parent() != doc.window.get()) {
          doc.window->addChild(doc.component);
          doc.window->catcher_.toFront();
        }
        doc.component->setVisible(true);
        doc.window->setVisible(true);
        doc.window->resized();
        doc.window->updateActiveState(isActive);
      } else {
        if (doc.window) {
          doc.windowBounds = doc.window->bounds();
          if (doc.window->parent()) doc.window->parent()->removeChild(doc.window.get());
        }
        if (doc.component->parent() != this) addChild(doc.component);
        doc.component->setVisible(isActive);
        const float top = a == Arrangement::Tabs ? kTabHeight : 0.0f;
        doc.component->setBounds({0.0f, top, area.w, (std::max)(0.0f, area.h - top)});
      }
    }
    // Floating: the active window on top, the rest in the order they were
    // last used, so closing the front one uncovers the one used before it.
    if (a == Arrangement::Windows)
      for (Component* c : order_) {
        const int i = indexOf(c);
        if (i >= 0 && docs_[(size_t) i]->window) docs_[(size_t) i]->window->toFront();
      }
    repaint();
  }

  bool isInside(const Component* c, const Component* within = nullptr) const {
    const Component* top = within ? within : this;
    for (const Component* p = c; p; p = p->parent())
      if (p == top) return true;
    return false;
  }

  static Component* firstFocusable(Component* c) {
    if (!c || !c->isVisible()) return nullptr;
    if (c->wantsKeyboardFocus()) return c;
    for (Component* child : c->children())
      if (Component* found = firstFocusable(child)) return found;
    return nullptr;
  }

  std::vector<std::unique_ptr<Doc>> docs_;
  std::vector<Component*> order_;
  std::vector<std::unique_ptr<Component>> retired_;
  std::shared_ptr<bool> alive_;
  Component* active_ = nullptr;
  LayoutMode mode_ = LayoutMode::MaximisedWindowsWithTabs;
  int maximum_ = 0;
  bool fullscreenWhenOne_ = false;
  Colour background_;
  bool hasBackground_ = false;
  int hoveredTab_ = -1;
  bool hoveredClose_ = false;
};

// ── MultiDocumentPanelWindow, now that the panel is complete ─────────────────

inline MultiDocumentPanelWindow::MultiDocumentPanelWindow(MultiDocumentPanel& owner, Component* document,
                                                          std::string title, Colour background)
    : owner_(owner), document_(document), title_(std::move(title)), background_(background) {
  close_.onClick = [this]() { owner_.closeDocument(document_, true); };
  catcher_.window = this;
  grip_.window = this;
  addChild(&close_);
  addChild(&catcher_);
  addChild(&grip_);
}

inline bool MultiDocumentPanelWindow::isActiveWindow() const { return owner_.activeDocument() == document_; }

inline void MultiDocumentPanelWindow::paint(Graphics& g) {
  LookAndFeel& lf = lookAndFeel();
  lf.drawDocumentWindow(g, localBounds(), titleBarBounds(), title_, font(), isActiveWindow());
  if (!background_.isTransparent()) {
    g.setColour(background_);
    g.fillRect(contentBounds());
  }
}

inline void MultiDocumentPanelWindow::mouseDown(const MouseEvent& e) {
  owner_.setActiveDocument(document_);
  drag_ = Drag::None;
  if (e.clickCount >= 2 && titleBarBounds().contains(e.position)) {
    toggleMaximised();
    return;
  }
  if (titleBarBounds().contains(e.position)) beginDrag(Drag::Move, e.rootPosition);
}

inline void MultiDocumentPanelWindow::mouseDrag(const MouseEvent& e) { dragTo(e.rootPosition); }

inline void MultiDocumentPanelWindow::beginDrag(Drag kind, Point rootPos) {
  // A maximised window is not moved or resized: it is the panel's size by
  // definition, and a grip on it would be a control that does nothing.
  if (maximised_) return;
  owner_.setActiveDocument(document_);
  drag_ = kind;
  dragStart_ = bounds();
  dragRootStart_ = rootPos;
  grip_.repaint();
}

inline void MultiDocumentPanelWindow::dragTo(Point rootPos) {
  if (drag_ == Drag::None) return;
  const float dx = rootPos.x - dragRootStart_.x, dy = rootPos.y - dragRootStart_.y;
  const Rect area = owner_.localBounds();
  if (drag_ == Drag::Move) {
    // The title bar stays reachable: at least a grab's worth of it inside the
    // panel, and never above the top, where nothing could pull it back.
    float x = dragStart_.x + dx, y = dragStart_.y + dy;
    const float keep = 40.0f;
    x = (std::max)(keep - dragStart_.w, (std::min)(x, area.w - keep));
    y = (std::max)(0.0f, (std::min)(y, area.h - kTitleHeight));
    setBounds({x, y, dragStart_.w, dragStart_.h});
  } else {
    setBounds({dragStart_.x, dragStart_.y, (std::max)(kMinWidth, dragStart_.w + dx),
               (std::max)(kMinHeight, dragStart_.h + dy)});
  }
}

inline void MultiDocumentPanelWindow::toggleMaximised() {
  if (maximised_) {
    maximised_ = false;
    setBounds(restored_);
  } else {
    restored_ = bounds();
    maximised_ = true;
    setBounds(owner_.localBounds());
  }
  grip_.setVisible(!maximised_);
  repaint();
}

inline void MultiDocumentPanelWindow::ActivationCatcher::mouseDown(const MouseEvent&) {
  if (window) window->owner_.setActiveDocument(window->document_);
}

} // namespace gfx
} // namespace sonore
