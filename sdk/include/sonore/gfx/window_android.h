// SPDX-License-Identifier: Apache-2.0
//
// The Android window, so the native UI runs on a phone and a tablet.
//
// ── One surface, composited here ────────────────────────────────────────────
//
// A desktop has windows: the editor is one, a popup menu is another, a tooltip
// a third, and the OS stacks them. An Android activity has ONE surface, an
// ANativeWindow the size of the screen. So the peers here are LAYERS of that
// surface: AndroidSurface keeps them bottom to top, paints each into its own
// Backing as the desktop peers do, and copies them into the window's buffer
// in order -- the editor, then whatever menu or tooltip is open over it. A
// touch goes to the topmost layer it lands on; a touch outside a menu that
// grabs the pointer dismisses it, as a click outside one does on a desktop.
//
// ── Pixels go straight in ───────────────────────────────────────────────────
//
// The rasteriser's bitmap is R G B A, premultiplied, and so is the window's
// buffer (WINDOW_FORMAT_RGBA_8888): rows are copied, nothing converted. Only
// what changed is copied, through the window's dirty-rectangle lock, which
// hands back the previous frame around it.
//
// ── Touch, pen and mouse ────────────────────────────────────────────────────
//
// Fingers arrive as AMotionEvents with a pointer id each. The first finger
// down is the PRIMARY pointer: it moves the focus and goes through the router
// as the mouse does; the others are delivered as touches of their own
// (MouseInputSources). A stylus is a pen with pressure, a mouse (a Chromebook,
// a tablet with a trackpad) is a mouse with hover, buttons and a wheel.
//
// ── Scale ───────────────────────────────────────────────────────────────────
//
// The surface's density (dpi / 160, AConfiguration) is the scale: a phone at
// 420 dpi draws every logical pixel as 2.625 device pixels, which is what makes
// a 24-point control the size of a fingertip. The app sizes the editor to the
// surface's logical size (android_app.h).
//
// The UI thread is the activity's main thread; every call here is made on it.
#pragma once

#if defined(__ANDROID__)

#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_window.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "backing.h"
#include "clipboard.h"
#include "component.h"
#include "mouse_input.h"

namespace sonore {
namespace gfx {

class NativeWindowAndroid;

/**
 * The activity's surface, and the layers drawn on it. One per process: an
 * Android app has one activity window at a time (android_app.h attaches it).
 */
class AndroidSurface {
public:
  static AndroidSurface& instance() {
    static AndroidSurface surface;
    return surface;
  }

  /** The window the activity was given. Laid out RGBA at its own size. */
  void attach(ANativeWindow* window) {
    if (window_ == window) return;
    detach();
    window_ = window;
    if (!window_) return;
    ANativeWindow_acquire(window_);
    ANativeWindow_setBuffersGeometry(window_, 0, 0, WINDOW_FORMAT_RGBA_8888);
    width_ = ANativeWindow_getWidth(window_);
    height_ = ANativeWindow_getHeight(window_);
    fullFrame_ = true;
  }

  /** The window is going away: nothing is drawn until the next attach. */
  void detach() {
    if (!window_) return;
    ANativeWindow_release(window_);
    window_ = nullptr;
  }

  bool attached() const { return window_ != nullptr; }
  /** In device pixels; 0 while no window is attached. */
  int width() const { return window_ ? width_ : 0; }
  int height() const { return window_ ? height_ : 0; }

  /** The density scale: logical pixel to device pixels. */
  void setDensityScale(float scale) { density_ = scale > 0.25f ? scale : 1.0f; }
  float densityScale() const { return density_; }

  /**
   * What the system draws over the surface's edges, in device pixels: the
   * status and navigation bars and a camera cutout. From Android 15 an app
   * draws under them whether it asked to or not, so the editor is placed
   * inside them; the strip under a bar shows the ground.
   */
  void setInsets(int left, int top, int right, int bottom);
  int insetLeft() const { return insetLeft_; }
  int insetTop() const { return insetTop_; }
  /** The part of the surface nothing covers, in device pixels. */
  int contentWidth() const { return std::max(0, width() - insetLeft_ - insetRight_); }
  int contentHeight() const { return std::max(0, height() - insetTop_ - insetBottom_); }

  /** The surface's size changed (a rotation, a split screen). */
  void resized() {
    if (!window_) return;
    width_ = ANativeWindow_getWidth(window_);
    height_ = ANativeWindow_getHeight(window_);
    fullFrame_ = true;
  }

  /** Paint every layer from scratch at the next present. */
  void invalidate() { fullFrame_ = true; }

  /** The UI clock: each layer's own tick, then whatever changed, onto the
   *  surface. ~60 times a second while the activity is visible. */
  void tick();

  /** Composite what changed into the window and post it. */
  void present();

  /** A motion or key event from the activity's input queue. True when it was
   *  used; false hands it back to the system (the Back key). */
  bool handleInput(const AInputEvent* event);

  /** Where a layer was asked to paint something new. */
  void damage(const PixelRect& r) { dirty_.push_back(r); }

private:
  friend class NativeWindowAndroid;
  AndroidSurface() = default;

  void add(NativeWindowAndroid* layer) {
    layers_.push_back(layer);
    fullFrame_ = true;
  }
  void remove(NativeWindowAndroid* layer) {
    layers_.erase(std::remove(layers_.begin(), layers_.end(), layer), layers_.end());
    if (touchLayer_ == layer) touchLayer_ = nullptr;
    fullFrame_ = true;
  }
  NativeWindowAndroid* layerAt(int x, int y) const;
  NativeWindowAndroid* grabbingPopup() const;
  bool handleMotion(const AInputEvent* event);
  bool handleKey(const AInputEvent* event);
  void copyLayers(uint8_t* dst, int stride, const PixelRect& area) const;

  ANativeWindow* window_ = nullptr;
  int width_ = 0, height_ = 0;
  int insetLeft_ = 0, insetTop_ = 0, insetRight_ = 0, insetBottom_ = 0;
  float density_ = 1.0f;
  bool fullFrame_ = true;
  std::vector<PixelRect> dirty_;
  std::vector<NativeWindowAndroid*> layers_;
  /** The layer the current gesture started on: every pointer of one gesture
   *  goes to it, as a capture does on a desktop. */
  NativeWindowAndroid* touchLayer_ = nullptr;
  int32_t primaryPointer_ = -1;
  /** Touch slots by pointer id: a finger keeps its slot for its gesture. */
  int32_t slotIds_[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
};

/**
 * The native window peer on Android: one layer of the activity's surface.
 * The same surface as the Win32, Cocoa and X11 peers (native_editor.h).
 */
class NativeWindowAndroid {
public:
  NativeWindowAndroid() = default;
  ~NativeWindowAndroid() { close(); }
  NativeWindowAndroid(const NativeWindowAndroid&) = delete;
  NativeWindowAndroid& operator=(const NativeWindowAndroid&) = delete;

  /** Called once a frame by the surface's clock. */
  std::function<void()> onTick;
  /** A touch landed outside this popup while it grabs the pointer. */
  std::function<void()> onDismissedOutside;

  /** Whether a window can be drawn at all: true once an activity has
   *  attached its surface. */
  static bool isAvailable() { return AndroidSurface::instance().attached(); }

  /** The editor layer: at the surface's top left, `width` x `height` logical
   *  pixels at the surface's density. `parent` is unused -- there is one
   *  surface -- and accepted for the shape every peer has. */
  bool open(void* parent, Component& content, int width, int height) {
    return open(parent, content, width, height, 0, 0);
  }

  bool open(void* /*parent*/, Component& content, int width, int height, int screenX, int screenY) {
    const bool popup = isPopup_;
    const bool grab = grabsMouse_;
    const float popupScale = popupScale_;
    close();
    isPopup_ = popup;
    grabsMouse_ = grab;
    AndroidSurface& surface = AndroidSurface::instance();
    const float scale = isPopup_ && popupScale > 0.0f ? popupScale : surface.densityScale();
    content_ = &content;
    router_ = new MouseRouter(content);
    backing_.setContent(content_);
    backing_.setScale(scale);
    logicalWidth_ = (float) width;
    logicalHeight_ = (float) height;
    backing_.setDeviceSize(Backing::toDevice(logicalWidth_, scale), Backing::toDevice(logicalHeight_, scale));
    // A popup is where it was asked to be; the editor is inside the bars.
    x_ = isPopup_ ? screenX : surface.insetLeft();
    y_ = isPopup_ ? screenY : surface.insetTop();
    visible_ = true;
    open_ = true;
    surface.add(this);
    render();
    return true;
  }

  /** A popup layer at surface coordinates: a menu (grabbing the pointer, so a
   *  touch elsewhere dismisses it) or a tooltip (not grabbing). */
  bool openPopup(Component& content, int screenX, int screenY, int width, int height,
                 bool grabMouse = true, float scale = 0.0f) {
    isPopup_ = true;
    grabsMouse_ = grabMouse;
    popupScale_ = scale;
    const bool ok = open(nullptr, content, width, height, screenX, screenY);
    if (!ok) isPopup_ = false;
    return ok;
  }

  void close() {
    if (open_) AndroidSurface::instance().remove(this);
    open_ = false;
    delete router_;
    router_ = nullptr;
    content_ = nullptr;
    backing_.setContent(nullptr);
    backing_.release();
    isPopup_ = false;
    grabsMouse_ = false;
    popupScale_ = 0.0f;
  }

  /** This layer, as the platform handle a caller compares and passes back. */
  void* handle() const { return open_ ? (void*) this : nullptr; }
  bool isOpen() const { return open_; }
  bool isPopup() const { return isPopup_; }
  bool grabsPointer() const { return isPopup_ && grabsMouse_; }

  Bitmap& bitmap() { return backing_.bitmap(); }
  const Bitmap& bitmap() const { return backing_.bitmap(); }
  float scale() const { return backing_.scale(); }
  float logicalWidth() const { return logicalWidth_; }
  float logicalHeight() const { return logicalHeight_; }
  MouseRouter* router() { return router_; }
  Point lastMousePosition() const { return lastPointer_; }

  void setScale(float scale) {
    backing_.setScale(scale);
    setSize(Backing::toDevice(logicalWidth_, scale), Backing::toDevice(logicalHeight_, scale));
  }

  void setLogicalSize(float width, float height) {
    logicalWidth_ = width;
    logicalHeight_ = height;
    setSize(Backing::toDevice(width, backing_.scale()), Backing::toDevice(height, backing_.scale()));
  }

  /** In device pixels. */
  void setSize(int width, int height) {
    if (!open_) return;
    backing_.setDeviceSize(width, height);
    AndroidSurface::instance().invalidate();
    render();
  }

  void setVisible(bool shouldBeVisible) {
    if (visible_ == shouldBeVisible) return;
    visible_ = shouldBeVisible;
    AndroidSurface::instance().invalidate();
  }
  bool visible() const { return visible_; }

  /** Where this layer sits on the surface, in device pixels. */
  void desktopOrigin(int* x, int* y) const {
    if (x) *x = x_;
    if (y) *y = y_;
  }
  /** The surface, in device pixels: the whole screen there is. */
  void screenSize(int* width, int* height) const {
    if (width) *width = AndroidSurface::instance().width();
    if (height) *height = AndroidSurface::instance().height();
  }

  /** No connection to hand anybody: the activity's looper delivers input. */
  int connectionFd() const { return -1; }
  /** Input arrives through the activity (AndroidSurface::handleInput). */
  void processEvents() {}

  /** A name a screen reader would read, kept for when one is wired. */
  void setAccessibleWindowName(const std::string& name) { accessibleName_ = name; }

  /** In-process: the system clipboard needs Java's ClipboardManager, which a
   *  native window has no handle on. Copy and paste work inside the app. */
  void setClipboardText(const std::string& utf8) { Clipboard::fallbackStore() = utf8; }
  bool getClipboardText(std::string* out) {
    if (!out) return false;
    *out = Clipboard::fallbackStore();
    return !out->empty();
  }

  /** Paint whatever changed and tell the surface where. */
  bool renderIfDirty() {
    if (!open_ || !content_ || !content_->isDirty()) return false;
    render();
    return true;
  }

private:
  friend class AndroidSurface;

  void render() {
    if (!open_) return;
    const std::vector<PixelRect>& changed = backing_.render();
    for (const PixelRect& r : changed)
      AndroidSurface::instance().damage({r.x + x_, r.y + y_, r.w, r.h});
  }

  bool contains(int x, int y) const {
    const Bitmap& b = backing_.bitmap();
    return visible_ && x >= x_ && y >= y_ && x < x_ + b.width() && y < y_ + b.height();
  }

  Point toLogical(float deviceX, float deviceY) const {
    return backing_.toLogical((int) (deviceX - (float) x_), (int) (deviceY - (float) y_));
  }

  Component* content_ = nullptr;
  MouseRouter* router_ = nullptr;
  Backing backing_;
  float logicalWidth_ = 0.0f, logicalHeight_ = 0.0f;
  int x_ = 0, y_ = 0;
  bool open_ = false;
  bool visible_ = true;
  bool isPopup_ = false;
  bool grabsMouse_ = false;
  float popupScale_ = 0.0f;
  Point lastPointer_;
  std::string accessibleName_;
};

// ── AndroidSurface, after the peer it composites ─────────────────────────────

inline NativeWindowAndroid* AndroidSurface::layerAt(int x, int y) const {
  for (auto it = layers_.rbegin(); it != layers_.rend(); ++it)
    if ((*it)->contains(x, y)) return *it;
  return nullptr;
}

inline NativeWindowAndroid* AndroidSurface::grabbingPopup() const {
  for (auto it = layers_.rbegin(); it != layers_.rend(); ++it)
    if ((*it)->grabsPointer() && (*it)->visible()) return *it;
  return nullptr;
}

inline void AndroidSurface::setInsets(int left, int top, int right, int bottom) {
  insetLeft_ = std::max(0, left);
  insetTop_ = std::max(0, top);
  insetRight_ = std::max(0, right);
  insetBottom_ = std::max(0, bottom);
  for (NativeWindowAndroid* layer : layers_) {
    if (layer->isPopup()) continue;
    layer->x_ = insetLeft_;
    layer->y_ = insetTop_;
  }
  fullFrame_ = true;
}

inline void AndroidSurface::tick() {
  // A copy: a tick may open a tooltip or close a menu, which edits the list.
  const std::vector<NativeWindowAndroid*> layers = layers_;
  for (NativeWindowAndroid* layer : layers) {
    if (std::find(layers_.begin(), layers_.end(), layer) == layers_.end()) continue;
    if (layer->onTick) layer->onTick();
  }
  for (NativeWindowAndroid* layer : layers_) layer->renderIfDirty();
  present();
}

inline void AndroidSurface::copyLayers(uint8_t* dst, int stride, const PixelRect& area) const {
  // Under every layer, the ground: black, which is what the edges of a
  // screen show past an editor that does not reach them.
  for (int y = area.y; y < area.y + area.h; ++y)
    std::memset(dst + ((size_t) y * (size_t) stride + (size_t) area.x) * 4u, 0, (size_t) area.w * 4u);
  for (NativeWindowAndroid* layer : layers_) {
    if (!layer->visible()) continue;
    const Bitmap& b = layer->bitmap();
    if (b.isEmpty()) continue;
    const int x0 = std::max(area.x, layer->x_), y0 = std::max(area.y, layer->y_);
    const int x1 = std::min(area.x + area.w, layer->x_ + b.width());
    const int y1 = std::min(area.y + area.h, layer->y_ + b.height());
    if (x1 <= x0 || y1 <= y0) continue;
    for (int y = y0; y < y1; ++y)
      std::memcpy(dst + ((size_t) y * (size_t) stride + (size_t) x0) * 4u,
                  b.data() + b.offsetOf(x0 - layer->x_, y - layer->y_), (size_t) (x1 - x0) * 4u);
  }
}

inline void AndroidSurface::present() {
  if (!window_ || width_ <= 0 || height_ <= 0) {
    dirty_.clear();
    return;
  }
  if (!fullFrame_ && dirty_.empty()) return;
  // One rectangle around everything that changed: what the window's lock
  // takes, and what it may grow to the whole buffer when the previous frame
  // cannot be copied back.
  ARect bounds{0, 0, width_, height_};
  if (!fullFrame_) {
    int l = width_, t = height_, r = 0, b = 0;
    for (const PixelRect& d : dirty_) {
      l = std::min(l, std::max(0, d.x));
      t = std::min(t, std::max(0, d.y));
      r = std::max(r, std::min(width_, d.x + d.w));
      b = std::max(b, std::min(height_, d.y + d.h));
    }
    if (r <= l || b <= t) {
      dirty_.clear();
      return;
    }
    bounds = ARect{l, t, r, b};
  }
  ANativeWindow_Buffer buffer;
  if (ANativeWindow_lock(window_, &buffer, &bounds) != 0) return; // try again next frame
  const PixelRect area{bounds.left, bounds.top, std::min<int>(bounds.right, buffer.width) - bounds.left,
                       std::min<int>(bounds.bottom, buffer.height) - bounds.top};
  if (area.w > 0 && area.h > 0 &&
      (buffer.format == WINDOW_FORMAT_RGBA_8888 || buffer.format == WINDOW_FORMAT_RGBX_8888))
    copyLayers(static_cast<uint8_t*>(buffer.bits), buffer.stride, area);
  ANativeWindow_unlockAndPost(window_);
  dirty_.clear();
  fullFrame_ = false;
}

inline bool AndroidSurface::handleInput(const AInputEvent* event) {
  if (!event) return false;
  switch (AInputEvent_getType(event)) {
    case AINPUT_EVENT_TYPE_MOTION: return handleMotion(event);
    case AINPUT_EVENT_TYPE_KEY: return handleKey(event);
    default: return false;
  }
}

inline bool AndroidSurface::handleMotion(const AInputEvent* event) {
  const int32_t action = AMotionEvent_getAction(event);
  const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
  const size_t actionIndex =
      (size_t) ((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
  const int32_t meta = AMotionEvent_getMetaState(event);
  const double time = (double) AMotionEvent_getEventTime(event) * 1e-9;
  MouseInputSources& sources = MouseInputSources::instance();

  auto slotOf = [this](int32_t id, bool claim) -> int {
    for (int s = 0; s < 10; ++s)
      if (slotIds_[s] == id) return s;
    if (!claim) return -1;
    for (int s = 0; s < 10; ++s)
      if (slotIds_[s] < 0) {
        slotIds_[s] = id;
        return s;
      }
    return -1;
  };

  // One pointer's report into the layer the gesture belongs to.
  auto deliver = [&](size_t index, PointerSample::Phase phase) {
    NativeWindowAndroid* layer = touchLayer_;
    if (!layer || !layer->router()) return;
    const int32_t id = AMotionEvent_getPointerId(event, index);
    const int32_t tool = AMotionEvent_getToolType(event, index);
    PointerSample s;
    s.phase = phase;
    s.rootPosition = layer->toLogical(AMotionEvent_getX(event, index), AMotionEvent_getY(event, index));
    s.time = time;
    s.shiftDown = (meta & AMETA_SHIFT_ON) != 0;
    s.ctrlDown = (meta & AMETA_CTRL_ON) != 0;
    s.altDown = (meta & AMETA_ALT_ON) != 0;
    if (tool == AMOTION_EVENT_TOOL_TYPE_MOUSE) {
      s.input.type = MouseInputSourceType::Mouse;
      s.input.index = 0;
      s.rightButton = (AMotionEvent_getButtonState(event) & AMOTION_EVENT_BUTTON_SECONDARY) != 0;
    } else if (tool == AMOTION_EVENT_TOOL_TYPE_STYLUS || tool == AMOTION_EVENT_TOOL_TYPE_ERASER) {
      s.input.type = MouseInputSourceType::Pen;
      s.input.index = 0;
      s.input.pressure = AMotionEvent_getPressure(event, index);
      s.rightButton = (AMotionEvent_getButtonState(event) & AMOTION_EVENT_BUTTON_STYLUS_PRIMARY) != 0;
    } else {
      const int slot = slotOf(id, phase == PointerSample::Phase::Down);
      if (slot < 0) return;
      s.input.type = MouseInputSourceType::Touch;
      s.input.index = slot;
      s.input.pressure = AMotionEvent_getPressure(event, index);
    }
    s.primary = id == primaryPointer_;
    if (s.primary) layer->lastPointer_ = s.rootPosition;
    sources.handle(*layer->router(), s);
    if ((phase == PointerSample::Phase::Up || phase == PointerSample::Phase::Cancel) &&
        s.input.type == MouseInputSourceType::Touch)
      slotIds_[s.input.index] = -1;
  };

  switch (masked) {
    case AMOTION_EVENT_ACTION_DOWN: {
      // A new gesture: which layer it belongs to is decided here, once. A
      // touch outside a menu that grabs the pointer dismisses the menu and
      // goes nowhere else -- as a click outside one does on a desktop.
      const int x = (int) AMotionEvent_getX(event, 0), y = (int) AMotionEvent_getY(event, 0);
      for (int32_t& id : slotIds_) id = -1;
      NativeWindowAndroid* popup = grabbingPopup();
      if (popup && !popup->contains(x, y)) {
        touchLayer_ = nullptr;
        primaryPointer_ = -1;
        if (popup->onDismissedOutside) popup->onDismissedOutside();
        return true;
      }
      touchLayer_ = popup ? popup : layerAt(x, y);
      primaryPointer_ = AMotionEvent_getPointerId(event, 0);
      deliver(0, PointerSample::Phase::Down);
      return true;
    }
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
      deliver(actionIndex, PointerSample::Phase::Down);
      return true;
    case AMOTION_EVENT_ACTION_MOVE:
      for (size_t i = 0; i < AMotionEvent_getPointerCount(event); ++i) deliver(i, PointerSample::Phase::Move);
      return true;
    case AMOTION_EVENT_ACTION_POINTER_UP:
      deliver(actionIndex, PointerSample::Phase::Up);
      return true;
    case AMOTION_EVENT_ACTION_UP:
      deliver(0, PointerSample::Phase::Up);
      touchLayer_ = nullptr;
      primaryPointer_ = -1;
      return true;
    case AMOTION_EVENT_ACTION_CANCEL:
      for (size_t i = 0; i < AMotionEvent_getPointerCount(event); ++i) deliver(i, PointerSample::Phase::Cancel);
      touchLayer_ = nullptr;
      primaryPointer_ = -1;
      return true;
    case AMOTION_EVENT_ACTION_HOVER_MOVE: {
      // A mouse or a pen in range, with nothing pressed.
      const int x = (int) AMotionEvent_getX(event, 0), y = (int) AMotionEvent_getY(event, 0);
      NativeWindowAndroid* layer = layerAt(x, y);
      if (!layer || !layer->router()) return true;
      const Point at = layer->toLogical((float) x, (float) y);
      layer->lastPointer_ = at;
      layer->router()->mouseMove(at, (meta & AMETA_SHIFT_ON) != 0, (meta & AMETA_CTRL_ON) != 0,
                                 (meta & AMETA_ALT_ON) != 0);
      return true;
    }
    case AMOTION_EVENT_ACTION_HOVER_EXIT:
      for (NativeWindowAndroid* layer : layers_)
        if (layer->router()) layer->router()->mouseExitWindow();
      return true;
    case AMOTION_EVENT_ACTION_SCROLL: {
      const int x = (int) AMotionEvent_getX(event, 0), y = (int) AMotionEvent_getY(event, 0);
      NativeWindowAndroid* layer = layerAt(x, y);
      if (!layer || !layer->router()) return true;
      const float dx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HSCROLL, 0);
      const float dy = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_VSCROLL, 0);
      // One delta, as every wheel here: the vertical one, the horizontal
      // where a trackpad gave only that.
      layer->router()->mouseWheel(layer->toLogical((float) x, (float) y), dy != 0.0f ? dy : dx,
                                  (meta & AMETA_SHIFT_ON) != 0, (meta & AMETA_CTRL_ON) != 0,
                                  (meta & AMETA_ALT_ON) != 0);
      return true;
    }
    default:
      return false;
  }
}

inline bool AndroidSurface::handleKey(const AInputEvent* event) {
  // A hardware keyboard, into whichever layer has the focus: the top one.
  if (AKeyEvent_getAction(event) != AKEY_EVENT_ACTION_DOWN) return false;
  const int32_t code = AKeyEvent_getKeyCode(event);
  if (code == AKEYCODE_BACK) return false; // the system's: it ends the activity
  if (layers_.empty() || !layers_.back()->router()) return false;
  const int32_t meta = AKeyEvent_getMetaState(event);
  KeyPress k;
  k.shiftDown = (meta & AMETA_SHIFT_ON) != 0;
  k.ctrlDown = (meta & AMETA_CTRL_ON) != 0;
  k.altDown = (meta & AMETA_ALT_ON) != 0;
  switch (code) {
    case AKEYCODE_DPAD_LEFT: k.keyCode = KeyPress::Left; break;
    case AKEYCODE_DPAD_RIGHT: k.keyCode = KeyPress::Right; break;
    case AKEYCODE_DPAD_UP: k.keyCode = KeyPress::Up; break;
    case AKEYCODE_DPAD_DOWN: k.keyCode = KeyPress::Down; break;
    case AKEYCODE_MOVE_HOME: k.keyCode = KeyPress::Home; break;
    case AKEYCODE_MOVE_END: k.keyCode = KeyPress::End; break;
    case AKEYCODE_PAGE_UP: k.keyCode = KeyPress::PageUp; break;
    case AKEYCODE_PAGE_DOWN: k.keyCode = KeyPress::PageDown; break;
    case AKEYCODE_DEL: k.keyCode = KeyPress::Backspace; break;
    case AKEYCODE_FORWARD_DEL: k.keyCode = KeyPress::Delete; break;
    case AKEYCODE_ENTER:
    case AKEYCODE_NUMPAD_ENTER: k.keyCode = KeyPress::Return; break;
    case AKEYCODE_ESCAPE: k.keyCode = KeyPress::Escape; break;
    case AKEYCODE_TAB: k.keyCode = KeyPress::Tab; break;
    case AKEYCODE_SPACE: k.character = ' '; break;
    default:
      if (code >= AKEYCODE_A && code <= AKEYCODE_Z)
        k.character = (uint32_t) ((k.shiftDown ? 'A' : 'a') + (code - AKEYCODE_A));
      else if (code >= AKEYCODE_0 && code <= AKEYCODE_9)
        k.character = (uint32_t) ('0' + (code - AKEYCODE_0));
      else
        return false;
  }
  return layers_.back()->router()->keyPressed(k);
}

} // namespace gfx
} // namespace sonore

#endif // __ANDROID__
