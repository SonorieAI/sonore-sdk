// SPDX-License-Identifier: Apache-2.0
//
// MouseInputSource: the mouse, every finger and the pen, as separate pointers
// -- with a pen's pressure, tilt, rotation and eraser. And
// MouseInactivityDetector, which watches them stop.
//
// ── Why more than one pointer ───────────────────────────────────────────────
//
// A plugin editor on a desktop has one pointer, and every control here was
// written for one. On a touch screen -- a Surface running a DAW, a tablet
// mixer -- it has as many as there are fingers, and two faders moved by two
// fingers at once is the first thing anybody tries. A pen adds what a mouse
// cannot say at all: how hard, and at what angle, which is what a drawing
// control (an envelope, a wavetable, a hand-drawn LFO) wants.
//
// ── How it fits the existing tree without changing it ──────────────────────
//
// The PRIMARY pointer -- the mouse, the pen, the first finger down -- goes
// through the window's MouseRouter exactly as a mouse always has, with the
// router told what it is (MouseRouter::setPointerInput) so the MouseEvents it
// makes carry the source, the pressure and the PenDetails. Capture, focus,
// hover, drag-and-drop: all unchanged, and every control works with a pen or a
// finger with no code of its own.
//
// SECONDARY fingers are delivered here, each with its own capture: the
// component under the finger when it went down gets that finger's drags and
// its release, wherever the finger goes, as a slider expects. They take no
// focus and start no drag-and-drop -- a second finger is a second gesture, not
// a second keyboard. A control that wants to tell them apart reads
// MouseEvent::sourceIndex.
//
// ── Where the platforms' reports come from ──────────────────────────────────
//
//   Windows  WM_POINTERDOWN / UPDATE / UP / LEAVE / CAPTURECHANGED, for pen
//            and touch. The mouse keeps arriving as WM_MOUSEMOVE and friends
//            (a plugin must not call EnableMouseInPointer: it is process-wide
//            and the host's); the window keeps the mouse's source from those
//            as it hands them to the router. Handling a pen or touch pointer
//            message here is what stops Windows synthesising the legacy mouse
//            messages for it, so nothing is delivered twice. The pointer
//            functions are looked up at run time from user32, so a plugin
//            still loads where they do not exist, and a test can substitute
//            them: tests drive real WM_POINTER messages through a real window
//            with synthesised POINTER_PEN_INFO, through exactly the code the
//            window runs.
//   macOS    NSEvent's subtype, pressure, tilt and rotation on the ordinary
//            mouse events a tablet sends, and tabletProximity: for the eraser
//            end. Compiled against the shim, never run.
//   X11      Not available. The X11 peer reads the core protocol's
//            ButtonPress/MotionNotify, which carry no pressure; XInput2 would
//            be needed and the peer does not open it. Every X11 event is a
//            mouse event with no pressure, which is what it always was.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include "component.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#if defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#endif

namespace sonore {
namespace gfx {

/**
 * One report from a peer about a non-mouse pointer: what happened, where (in
 * the window's logical coordinates), and what the pointer said about itself.
 */
struct PointerSample {
  /** What the pointer did. */
  enum class Phase { Down, /**< touched, or pressed */ Move, /**< moved, touching or in range */ Up, /**< lifted */ Leave, /**< left the window, or out of range */ Cancel /**< taken away mid-gesture: ends as a release outside everything */ };
  /** What happened. */
  Phase phase = Phase::Move;
  /** Where, in the window's logical coordinates -- what the router takes. */
  Point rootPosition;
  /** Which pointer (type and index) and what it reported: pressure, tilt and
   *  the rest. */
  PointerInput input;
  /** The first finger down, or the pen. Only the primary pointer moves the
   *  focus, hovers, and goes through the router. */
  bool primary = true;
  /** A pen's barrel button held at contact, which is a right-click. */
  bool rightButton = false;
  bool shiftDown = false, /**< Shift held */ ctrlDown = false, /**< Control held */ altDown = false; ///< Alt held
  /** Seconds, any origin. Used for double-tap detection. */
  double time = 0.0;
};

/**
 * One pointer: the mouse, a finger, or a pen.
 *
 * Kept by MouseInputSources and updated by the peers as reports arrive, so a
 * component can ask about the pointer it is being driven by -- how hard the
 * pen is pressing NOW, whether this finger is still down -- between events.
 *
 * Reports given to MouseInputSources::handle update it, and today that is the
 * Windows pen and fingers; the Windows window also keeps the MOUSE's source
 * up to date (position, press, clicks in a row, time) through noteMouse. On
 * macOS and X11 the mouse, and a pen on macOS, go to the router directly and
 * their source keeps its defaults. getComponentUnderMouse answers for the
 * mouse on every platform; for the pen and the primary finger that answer is
 * null -- ask the router (captured, hovered) instead.
 */
class MouseInputSource {
public:
  /** A pointer of `type`; `index` is 0 for the mouse and the pen, the finger's
   *  slot for touch. Made by MouseInputSources::sourceFor, not by a caller. */
  MouseInputSource(MouseInputSourceType type, int index) : type_(type), index_(index) {
    guard_.owner = this;
  }
  MouseInputSource(const MouseInputSource&) = delete;
  MouseInputSource& operator=(const MouseInputSource&) = delete;

  /** Mouse, finger or pen. */
  MouseInputSourceType getType() const { return type_; }
  /** Whether this is the mouse. */
  bool isMouse() const { return type_ == MouseInputSourceType::Mouse; }
  /** Whether this is a finger. */
  bool isTouch() const { return type_ == MouseInputSourceType::Touch; }
  /** Whether this is a pen. */
  bool isPen() const { return type_ == MouseInputSourceType::Pen; }
  /** A finger cannot hover: it is only anywhere while it touches. */
  bool canHover() const { return type_ != MouseInputSourceType::Touch; }
  /** 0 for the mouse and the pen; for a finger, the lowest slot that was free
   *  when it went down (0, 1, 2...), so two fingers can be told apart. */
  int getIndex() const { return index_; }

  /** Where it last was, in its window's logical coordinates. */
  Point getRootPosition() const { return position_; }
  /** Whether it is down: touching, or pressed with its tip. */
  bool isDragging() const { return down_; }
  /** 0..1 as last reported, negative when it reports none. */
  float getCurrentPressure() const { return input_.pressure; }
  /** Whether getCurrentPressure holds a reading. */
  bool isPressureValid() const { return input_.pressure >= 0.0f; }
  /** A touch contact's ellipse orientation in radians as last reported; 0 when
   *  unreported. */
  float getCurrentOrientation() const { return input_.orientation; }
  /** A pen's barrel rotation, radians clockwise 0..2pi; 0 when unreported. */
  float getCurrentRotation() const { return input_.pen.rotation; }
  /** `xAxis` true for the left-right tilt, false for the away-towards one. */
  float getCurrentTilt(bool xAxis) const { return xAxis ? input_.pen.tiltX : input_.pen.tiltY; }
  /** The pen's tilt, rotation, eraser and barrel button, as last reported. */
  const PenDetails& getPenDetails() const { return input_.pen; }
  /** Taps in a row at the last press: 1, then 2, 3... while each lands within
   *  doubleTapSeconds and doubleTapDistance of the one before. The events
   *  themselves report at most 2. */
  int getNumberOfMultipleClicks() const { return clicks_; }
  /** When it last went down, in the seconds of PointerSample::time (any
   *  origin); far in the past before its first press. */
  double getLastMouseDownTime() const { return lastDownTime_; }

  /**
   * What it is over, or holding. For the mouse, asked of the live routers --
   * the mouse never comes through here. For a secondary finger, what it went
   * down on; null once that component is destroyed.
   */
  Component* getComponentUnderMouse() const {
    if (isMouse()) {
      for (MouseRouter* r : liveRouters())
        if (Component* c = r->captured() ? r->captured() : r->hovered()) return c;
      return nullptr;
    }
    return captured_ ? captured_ : over_;
  }

private:
  friend class MouseInputSources;

  /** Clears the capture when the captured component is destroyed: the same
   *  rule as the router's forget(), for the captures the router does not hold. */
  struct Guard : ComponentListener {
    MouseInputSource* owner = nullptr;
    void componentBeingDeleted(Component& c) override {
      if (owner->captured_ == &c) owner->captured_ = nullptr;
      if (owner->over_ == &c) owner->over_ = nullptr;
      // The component unhooks this listener itself as it goes; the pointer
      // to it must go too, or the next watch() would unhook from freed memory.
      if (owner->watched_ == &c) owner->watched_ = nullptr;
    }
  };

  /** Returns false -- and holds nothing -- for a component already being
   *  destroyed, which takes no listeners. */
  bool watch(Component* c) {
    if (watched_ == c) return true;
    if (watched_) watched_->removeComponentListener(&guard_);
    watched_ = (c && c->addComponentListener(&guard_)) ? c : nullptr;
    return watched_ == c;
  }

  MouseInputSourceType type_;
  int index_;
  Point position_;
  Point downLocal_;
  Point lastDownPosition_;
  PointerInput input_;
  bool down_ = false;
  int clicks_ = 0;
  double lastDownTime_ = -1.0e9;
  Component* captured_ = nullptr;
  Component* over_ = nullptr;
  Component* watched_ = nullptr;
  Guard guard_;
};

/**
 * Every pointer this process has seen, and the dispatch of their reports.
 *
 * Process-wide, like the reference's list of mouse sources: there is one mouse
 * and one pen whatever the number of windows, and a finger belongs to
 * whichever window it touched.
 */
class MouseInputSources {
public:
  /** The process's one list. Not synchronised: use it from the thread the
   *  windows run on. */
  static MouseInputSources& instance() {
    static MouseInputSources sources;
    return sources;
  }

  /** How many pointers have been seen. A source is made the first time its
   *  pointer reports and kept for the life of the process. */
  int getNumMouseSources() const { return (int) sources_.size(); }
  /** The `i`th source seen, or null out of range. Owned by this list. */
  MouseInputSource* getMouseSource(int i) const {
    return (i >= 0 && i < (int) sources_.size()) ? sources_[(size_t) i].get() : nullptr;
  }
  /** The mouse, made on first use. Kept up to date on Windows only; elsewhere
   *  its fields stay at their defaults: see MouseInputSource. */
  MouseInputSource& getMainMouseSource() { return sourceFor(MouseInputSourceType::Mouse, 0); }

  /** How many sources are down right now. */
  int getNumDraggingMouseSources() const {
    int n = 0;
    for (const auto& s : sources_)
      if (s->isDragging()) ++n;
    return n;
  }

  /** The source of this type and index, made on first use. The reference stays
   *  valid for the life of the process. */
  MouseInputSource& sourceFor(MouseInputSourceType type, int index) {
    for (const auto& s : sources_)
      if (s->getType() == type && s->getIndex() == index) return *s;
    sources_.push_back(std::make_unique<MouseInputSource>(type, index));
    return *sources_.back();
  }

  /** How close in time and space a second tap must be to count as a double
   *  -- the platform's double-click time where there is one. */
  double doubleTapSeconds = 0.5;
  float doubleTapDistance = 6.0f;  ///< in logical pixels; on Windows doubleTapSeconds follows the system's double-click time

  /**
   * Deliver one report into the tree `router` dispatches for.
   *
   * The primary pointer goes through the router, which is told what it is
   * for the length of the call; a secondary finger is delivered here, with
   * its own capture.
   */
  void handle(MouseRouter& router, const PointerSample& s) {
    MouseInputSource& src = sourceFor(s.input.type, s.input.index);
    src.position_ = s.rootPosition;
    src.input_ = s.input;

    if (s.phase == PointerSample::Phase::Down) countPress(src, s);
    const int clickCount = src.clicks_ >= 2 ? 2 : 1;

    if (s.primary) {
      router.setPointerInput(s.input);
      switch (s.phase) {
        case PointerSample::Phase::Down:
          router.mouseDown(s.rootPosition, clickCount, s.shiftDown, s.ctrlDown, s.altDown,
                           s.rightButton);
          break;
        case PointerSample::Phase::Move:
          router.mouseMove(s.rootPosition, s.shiftDown, s.ctrlDown, s.altDown);
          break;
        case PointerSample::Phase::Up:
          if (src.down_) router.mouseUp(s.rootPosition, s.shiftDown, s.ctrlDown, s.altDown);
          break;
        case PointerSample::Phase::Leave:
          router.mouseExitWindow();
          break;
        case PointerSample::Phase::Cancel:
          // A gesture taken away -- the pen left range mid-stroke, the
          // system took the pointer -- ends like a lost capture: a release
          // nowhere, so no control acts on where it happened to be.
          if (src.down_) router.mouseUp({-1.0f, -1.0f});
          break;
      }
      router.resetPointerInput();
      if (s.phase == PointerSample::Phase::Up || s.phase == PointerSample::Phase::Cancel)
        src.down_ = false;
      return;
    }
    deliverSecondary(router, src, s, clickCount);
  }

  /**
   * Keep the MOUSE's source in step with a report its peer is giving the
   * router itself, and deliver nothing: the mouse goes to the router directly,
   * as it always has. Down, Move and Up move it; Down counts the press as
   * handle() does; Up and Cancel end the press; Leave changes nothing.
   * `s.input` is ignored -- a mouse reports no pressure.
   */
  void noteMouse(const PointerSample& s) {
    if (s.phase == PointerSample::Phase::Leave) return;
    MouseInputSource& src = sourceFor(MouseInputSourceType::Mouse, 0);
    src.input_ = PointerInput{};
    if (s.phase != PointerSample::Phase::Cancel) src.position_ = s.rootPosition;
    if (s.phase == PointerSample::Phase::Down) countPress(src, s);
    if (s.phase == PointerSample::Phase::Up || s.phase == PointerSample::Phase::Cancel)
      src.down_ = false;
  }

private:
  MouseInputSources() = default;

  /** A press: one more in a row when close enough in time and space to the
   *  last, else the first. A right press counts but does not hold it down. */
  void countPress(MouseInputSource& src, const PointerSample& s) const {
    const bool again = s.time - src.lastDownTime_ <= doubleTapSeconds &&
                       s.rootPosition.distanceTo(src.lastDownPosition_) <= doubleTapDistance;
    src.clicks_ = again ? src.clicks_ + 1 : 1;
    src.lastDownTime_ = s.time;
    src.lastDownPosition_ = s.rootPosition;
    src.down_ = !s.rightButton;
  }

  static MouseEvent eventFor(Component* target, const MouseInputSource& src,
                             const PointerSample& s, int clickCount) {
    MouseEvent e;
    e.position = target->rootToLocal(s.rootPosition);
    e.rootPosition = s.rootPosition;
    e.downPosition = src.downLocal_;
    e.clickCount = clickCount;
    e.shiftDown = s.shiftDown;
    e.ctrlDown = s.ctrlDown;
    e.altDown = s.altDown;
    e.sourceType = s.input.type;
    e.sourceIndex = s.input.index;
    e.pressure = s.input.pressure;
    e.orientation = s.input.orientation;
    e.pen = s.input.pen;
    e.eventComponent = target;
    return e;
  }

  void deliverSecondary(MouseRouter& router, MouseInputSource& src, const PointerSample& s,
                        int clickCount) {
    Component* root = router.rootComponent();
    if (!root) return;
    switch (s.phase) {
      case PointerSample::Phase::Down: {
        Component* target = s.rightButton ? nullptr : root->hitTest(s.rootPosition);
        if (!src.watch(target)) target = nullptr;
        src.captured_ = target;
        src.over_ = target;
        if (!target) return;
        src.downLocal_ = target->rootToLocal(s.rootPosition);
        target->mouseDown(eventFor(target, src, s, clickCount));
        return;
      }
      case PointerSample::Phase::Move:
        if (src.captured_) src.captured_->mouseDrag(eventFor(src.captured_, src, s, 1));
        return;
      case PointerSample::Phase::Up:
      case PointerSample::Phase::Cancel: {
        Component* target = src.captured_;
        src.captured_ = nullptr;
        src.down_ = false;
        if (target) {
          PointerSample at = s;
          if (s.phase == PointerSample::Phase::Cancel) at.rootPosition = {-1.0f, -1.0f};
          target->mouseUp(eventFor(target, src, at, 1));
        }
        return;
      }
      case PointerSample::Phase::Leave:
        src.over_ = nullptr;
        // Still guarding a capture -- a finger that left the window while
        // down holds on to what it pressed. Unwatched, a capture is a raw
        // pointer nothing clears; the fuzzer found exactly that.
        if (!src.captured_) src.watch(nullptr);
        return;
    }
  }

  std::vector<std::unique_ptr<MouseInputSource>> sources_;
};

/**
 * Tells you when the mouse has been still for a while, and when it moves
 * again.
 *
 * For hiding an overlay of controls on a full-screen visualiser, dimming a
 * transport bar, starting an attract animation -- never for hiding the
 * pointer itself inside a host's window, which is not a plugin's to hide.
 *
 * Watches its target AND everything inside it, through a MouseListener, so an
 * editor covered in controls is watched as a whole. The clock is read through
 * a function, steady by default and replaceable by a test; tick() is what
 * notices the stillness, and belongs in the editor's own tick.
 */
class MouseInactivityDetector : private MouseListener {
public:
  /** Told when the mouse goes still (from tick()) and when it moves again
   *  (from the mouse event that woke it) -- once per change, not per event,
   *  on the message thread. Both default to doing nothing. Not owned: remove
   *  one before it dies. */
  class Listener {
  public:
    virtual ~Listener() = default;
    /** The mouse moved past the tolerance, clicked or turned the wheel after
     *  being still. */
    virtual void mouseBecameActive() {}
    /** The mouse has been still for the delay, as noticed by tick(). */
    virtual void mouseBecameInactive() {}
  };

  /** Watch `target` and everything inside it, starting active with the clock at
   *  now. Either may be destroyed first. */
  explicit MouseInactivityDetector(Component& target) {
    clock_ = [] {
      return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
          .count();
    };
    target.addMouseListener(this, /*nestedChildren=*/true);
    lastActivity_ = clock_();
  }

  ~MouseInactivityDetector() override = default; ///< MouseListener detaches itself

  /** How long the mouse must be still, in milliseconds. 1000 by default, as
   *  in the reference. */
  void setDelay(int milliseconds) { delayMs_ = milliseconds > 0 ? milliseconds : 0; }
  /** In milliseconds. */
  int getDelay() const { return delayMs_; }

  /** How far it may drift, in pixels, and still count as still -- a hand
   *  resting on a mouse is never perfectly still. 50 by default. */
  void setMouseMoveTolerance(int pixels) { tolerance_ = pixels > 0 ? pixels : 0; }
  /** In logical pixels. */
  int getMouseMoveTolerance() const { return tolerance_; }

  /** Read time from `clock` (seconds, any origin) instead of the steady clock:
   *  for a test. Null is ignored. Restarts the wait for stillness. */
  void setClock(std::function<double()> clock) {
    if (!clock) return;
    clock_ = std::move(clock);
    lastActivity_ = clock_();
  }

  /** Not owned; adding one twice is a no-op. */
  void addListener(Listener* l) {
    if (l && std::find(listeners_.begin(), listeners_.end(), l) == listeners_.end())
      listeners_.push_back(l);
  }
  /** Safe to call from inside a notification. */
  void removeListener(Listener* l) {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
  }

  /** The same notifications, for a caller with no class to derive. */
  std::function<void()> onBecameActive, onBecameInactive; ///< each called after the Listeners

  /** True at construction; false from the tick() that noticed stillness until
   *  the next activity. */
  bool isActive() const { return active_; }

  /** Notice stillness. Call at the editor's tick rate; the precision of the
   *  notification is the interval between calls. */
  void tick() {
    if (!active_) return;
    if ((clock_() - lastActivity_) * 1000.0 >= (double) delayMs_) {
      active_ = false;
      notify(false);
    }
  }

private:
  void activity(const MouseEvent& e, bool definite) {
    const Point p = e.rootPosition;
    if (!active_) {
      // Waking needs a real movement past the tolerance, or a click: the
      // jitter of a resting hand must not wake a hidden overlay.
      if (!definite && p.distanceTo(stillAt_) < (float) tolerance_) return;
      active_ = true;
      stillAt_ = p;
      lastActivity_ = clock_();
      notify(true);
      return;
    }
    if (definite || p.distanceTo(stillAt_) >= (float) tolerance_) {
      stillAt_ = p;
      lastActivity_ = clock_();
    }
  }

  void notify(bool nowActive) {
    const std::vector<Listener*> told = listeners_;
    for (Listener* l : told) {
      if (std::find(listeners_.begin(), listeners_.end(), l) == listeners_.end()) continue;
      if (nowActive) l->mouseBecameActive();
      else l->mouseBecameInactive();
    }
    if (nowActive && onBecameActive) onBecameActive();
    if (!nowActive && onBecameInactive) onBecameInactive();
  }

  void mouseMove(const MouseEvent& e) override { activity(e, false); }
  void mouseEnter(const MouseEvent& e) override { activity(e, false); }
  void mouseDrag(const MouseEvent& e) override { activity(e, false); }
  void mouseDown(const MouseEvent& e) override { activity(e, true); }
  void mouseUp(const MouseEvent& e) override { activity(e, true); }
  void mouseWheelMove(const MouseEvent& e, float) override { activity(e, true); }

  std::function<double()> clock_;
  std::vector<Listener*> listeners_;
  double lastActivity_ = 0.0;
  Point stillAt_;
  int delayMs_ = 1000;
  int tolerance_ = 50;
  bool active_ = true;
};

namespace detail {

/** Tell a router what the events of one platform call come from, and put it
 *  back afterwards -- whatever path the call leaves by. */
struct ScopedPointerInput {
  ScopedPointerInput(MouseRouter* r, const PointerInput& input) : router(r) {
    if (router) router->setPointerInput(input);
  }
  ~ScopedPointerInput() {
    if (router) router->resetPointerInput();
  }
  ScopedPointerInput(const ScopedPointerInput&) = delete;
  ScopedPointerInput& operator=(const ScopedPointerInput&) = delete;
  MouseRouter* router;
};

} // namespace detail

// ── Windows: WM_POINTER ─────────────────────────────────────────────────────

#if defined(_WIN32)
namespace detail {

/**
 * The mouse's source, kept from the legacy mouse message the window is about
 * to give its router: `rootPosition` is what the router gets, the time is the
 * message's. Only the pen and fingers updated a source before, so the mouse's
 * said (0,0), never down, no press, whatever it did.
 */
inline void noteWin32Mouse(PointerSample::Phase phase, Point rootPosition, bool rightButton = false) {
  PointerSample s;
  s.phase = phase;
  s.rootPosition = rootPosition;
  s.rightButton = rightButton;
  // The same tick count a POINTER_INFO's dwTime is in, read unsigned as that is.
  s.time = (double) (DWORD) GetMessageTime() / 1000.0;
  MouseInputSources& sources = MouseInputSources::instance();
  if (phase == PointerSample::Phase::Down)
    sources.doubleTapSeconds = (double) GetDoubleClickTime() / 1000.0;
  sources.noteMouse(s);
}

} // namespace detail

#if defined(WM_POINTERDOWN) && defined(WINVER) && (WINVER >= 0x0602)
#define SONORE_HAS_WIN32_POINTER 1

namespace detail {

/**
 * The four pointer functions, looked up at run time.
 *
 * Not linked, for two reasons. They are Windows 8 and later, and a plugin that
 * imports them fails to LOAD on anything older -- a loader error, the least
 * diagnosable failure a plugin can have. And looked up through this table, a
 * test can put its own in: real WM_POINTER messages to a real window, with
 * the pen's report synthesised, through every line the window runs.
 */
struct Win32PointerApi {
  BOOL(WINAPI* getPointerType)(UINT32, POINTER_INPUT_TYPE*) = nullptr;
  BOOL(WINAPI* getPointerInfo)(UINT32, POINTER_INFO*) = nullptr;
  BOOL(WINAPI* getPointerPenInfo)(UINT32, POINTER_PEN_INFO*) = nullptr;
  BOOL(WINAPI* getPointerTouchInfo)(UINT32, POINTER_TOUCH_INFO*) = nullptr;
};

inline Win32PointerApi& win32PointerApi() {
  static Win32PointerApi api = [] {
    Win32PointerApi a;
    if (HMODULE user = GetModuleHandleW(L"user32.dll")) {
      a.getPointerType = reinterpret_cast<BOOL(WINAPI*)(UINT32, POINTER_INPUT_TYPE*)>(
          reinterpret_cast<void*>(GetProcAddress(user, "GetPointerType")));
      a.getPointerInfo = reinterpret_cast<BOOL(WINAPI*)(UINT32, POINTER_INFO*)>(
          reinterpret_cast<void*>(GetProcAddress(user, "GetPointerInfo")));
      a.getPointerPenInfo = reinterpret_cast<BOOL(WINAPI*)(UINT32, POINTER_PEN_INFO*)>(
          reinterpret_cast<void*>(GetProcAddress(user, "GetPointerPenInfo")));
      a.getPointerTouchInfo = reinterpret_cast<BOOL(WINAPI*)(UINT32, POINTER_TOUCH_INFO*)>(
          reinterpret_cast<void*>(GetProcAddress(user, "GetPointerTouchInfo")));
    }
    return a;
  }();
  return api;
}

/** Touch pointer ids are large and reused; a finger's INDEX is the lowest
 *  slot free when it went down, which is what a control comparing fingers
 *  wants -- 0, 1, 2 -- and what the reference gives. */
inline int win32TouchSlotFor(UINT32 pointerId, bool allocate, bool release) {
  static std::vector<UINT32> slots; // 0 = free
  for (size_t i = 0; i < slots.size(); ++i)
    if (slots[i] == pointerId + 1u) {
      if (release) slots[i] = 0;
      return (int) i;
    }
  if (!allocate) return -1;
  for (size_t i = 0; i < slots.size(); ++i)
    if (slots[i] == 0) {
      slots[i] = pointerId + 1u;
      return (int) i;
    }
  slots.push_back(pointerId + 1u);
  return (int) slots.size() - 1;
}

/** A pen's report, in this SDK's units: 0..1 pressure from 0..1024, radians
 *  clockwise from whole degrees, -1..1 tilt from -90..90 degrees. Fields the
 *  pen did not report keep their defaults. */
inline PointerInput pointerInputFromPen(const POINTER_PEN_INFO& pen) {
  PointerInput in;
  in.type = MouseInputSourceType::Pen;
  in.index = 0;
  in.pressure = (pen.penMask & PEN_MASK_PRESSURE) ? (float) pen.pressure / 1024.0f : -1.0f;
  if (pen.penMask & PEN_MASK_ROTATION)
    in.pen.rotation = (float) pen.rotation * 3.14159265f / 180.0f;
  if (pen.penMask & PEN_MASK_TILT_X) in.pen.tiltX = (float) pen.tiltX / 90.0f;
  if (pen.penMask & PEN_MASK_TILT_Y) in.pen.tiltY = (float) pen.tiltY / 90.0f;
  in.pen.eraser = (pen.penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0;
  in.pen.barrelButton = (pen.penFlags & PEN_FLAG_BARREL) != 0;
  return in;
}

inline PointerInput pointerInputFromTouch(const POINTER_TOUCH_INFO& touch, int slot) {
  PointerInput in;
  in.type = MouseInputSourceType::Touch;
  in.index = slot;
  in.pressure = (touch.touchMask & TOUCH_MASK_PRESSURE) ? (float) touch.pressure / 1024.0f : -1.0f;
  if (touch.touchMask & TOUCH_MASK_ORIENTATION)
    in.orientation = (float) touch.orientation * 3.14159265f / 180.0f;
  return in;
}

} // namespace detail

/**
 * Handle one WM_POINTER* message for a window whose tree `router` dispatches.
 *
 * Returns false for anything that is not a pen or a finger -- the mouse, a
 * touchpad, a message whose pointer cannot be read -- and the window then
 * passes it to DefWindowProc, which turns it into the legacy mouse messages
 * every existing path already handles. Returns true when it was delivered,
 * and the window must then return 0: that is what stops Windows synthesising
 * a WM_LBUTTONDOWN for the same touch.
 *
 * `toLogical(clientX, clientY)` converts a client-area device pixel to the
 * tree's units, which is Backing::toLogical in the window.
 */
template <typename ToLogical>
bool handleWin32PointerMessage(MouseRouter& router, HWND hwnd, UINT msg, WPARAM wp,
                               ToLogical&& toLogical, bool shift, bool ctrl, bool alt) {
  detail::Win32PointerApi& api = detail::win32PointerApi();
  if (!api.getPointerType) return false;
  const UINT32 id = (UINT32) LOWORD(wp);
  POINTER_INPUT_TYPE type = PT_POINTER;
  if (!api.getPointerType(id, &type)) return false;

  POINTER_INFO info{};
  PointerInput input;
  const bool ending = msg == WM_POINTERUP || msg == WM_POINTERCAPTURECHANGED;
  if (type == PT_PEN) {
    POINTER_PEN_INFO pen{};
    if (!api.getPointerPenInfo || !api.getPointerPenInfo(id, &pen)) return false;
    info = pen.pointerInfo;
    input = detail::pointerInputFromPen(pen);
  } else if (type == PT_TOUCH) {
    POINTER_TOUCH_INFO touch{};
    if (!api.getPointerTouchInfo || !api.getPointerTouchInfo(id, &touch)) return false;
    info = touch.pointerInfo;
    const int slot = detail::win32TouchSlotFor(id, msg == WM_POINTERDOWN, false);
    if (slot < 0) return false;
    input = detail::pointerInputFromTouch(touch, slot);
  } else {
    return false; // the mouse and touchpads stay on the legacy path
  }

  POINT p = info.ptPixelLocation; // screen pixels
  ScreenToClient(hwnd, &p);

  PointerSample s;
  s.rootPosition = toLogical((int) p.x, (int) p.y);
  s.input = input;
  // The pen is always primary: there is one. A finger is primary when
  // Windows says so -- the first one down.
  s.primary = type == PT_PEN || (info.pointerFlags & POINTER_FLAG_PRIMARY) != 0;
  s.shiftDown = shift;
  s.ctrlDown = ctrl;
  s.altDown = alt;
  s.time = (double) info.dwTime / 1000.0;
  // A pen's barrel button held as the tip touches is the pen's right-click,
  // which Windows reports as the second button.
  s.rightButton = (info.pointerFlags & POINTER_FLAG_SECONDBUTTON) != 0;
  if (msg == WM_POINTERDOWN) s.phase = PointerSample::Phase::Down;
  else if (msg == WM_POINTERUP) s.phase = PointerSample::Phase::Up;
  else if (msg == WM_POINTERLEAVE) s.phase = PointerSample::Phase::Leave;
  else if (msg == WM_POINTERCAPTURECHANGED || (info.pointerFlags & POINTER_FLAG_CANCELED))
    s.phase = PointerSample::Phase::Cancel;
  else s.phase = PointerSample::Phase::Move;

  MouseInputSources::instance().doubleTapSeconds = (double) GetDoubleClickTime() / 1000.0;
  MouseInputSources::instance().handle(router, s);
  if (type == PT_TOUCH && (ending || msg == WM_POINTERLEAVE))
    detail::win32TouchSlotFor(id, false, /*release=*/true);
  return true;
}

#endif // WM_POINTERDOWN && WINVER >= 0x0602
#endif // _WIN32

// ── macOS: NSEvent ──────────────────────────────────────────────────────────

#if defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)

namespace detail {

/** Whether the pen in proximity is pointing its ERASER end down: Cocoa says
 *  so once, in the proximity event, and not on the points that follow. */
inline bool& cocoaEraserInProximity() {
  static bool eraser = false;
  return eraser;
}

/** tabletProximity: -- NSPointingDeviceTypeEraser is 3. */
inline void noteCocoaTabletProximity(id event) {
  if (!event) return;
  using EnterFn = BOOL (*)(id, SEL);
  using TypeFn = unsigned long (*)(id, SEL);
  const BOOL entering =
      reinterpret_cast<EnterFn>(objc_msgSend)(event, sel_registerName("isEnteringProximity"));
  const unsigned long device =
      reinterpret_cast<TypeFn>(objc_msgSend)(event, sel_registerName("pointingDeviceType"));
  cocoaEraserInProximity() = entering && device == 3ul;
}

/**
 * What an NSEvent says about the pointer that sent it.
 *
 * A tablet's events are ordinary mouse events with subtype
 * NSEventSubtypeTabletPoint (1); anything else is the mouse, and gets the
 * defaults. pressure is already 0..1; tilt is already -1..1 on both axes, with
 * y up -- flipped here to the SDK's y-down "towards the user is positive";
 * rotation is degrees COUNTER-clockwise, turned into radians clockwise.
 */
inline PointerInput pointerInputFromNSEvent(id event) {
  PointerInput in;
  if (!event) return in;
  using ShortFn = short (*)(id, SEL);
  using FloatFn = float (*)(id, SEL);
  struct NSPointPair {
    double x, y;
  };
  using PointFn = NSPointPair (*)(id, SEL);
  const short subtype = reinterpret_cast<ShortFn>(objc_msgSend)(event, sel_registerName("subtype"));
  if (subtype != 1) return in;
  in.type = MouseInputSourceType::Pen;
  in.pressure = reinterpret_cast<FloatFn>(objc_msgSend)(event, sel_registerName("pressure"));
  const NSPointPair tilt =
      reinterpret_cast<PointFn>(objc_msgSend)(event, sel_registerName("tilt"));
  in.pen.tiltX = (float) tilt.x;
  in.pen.tiltY = (float) -tilt.y;
  const float degrees =
      reinterpret_cast<FloatFn>(objc_msgSend)(event, sel_registerName("rotation"));
  float clockwise = -degrees * 3.14159265f / 180.0f;
  while (clockwise < 0.0f) clockwise += 6.28318531f;
  in.pen.rotation = clockwise;
  in.pen.eraser = cocoaEraserInProximity();
  return in;
}

} // namespace detail

#endif

} // namespace gfx
} // namespace sonore
