// SPDX-License-Identifier: Apache-2.0
//
// The X11 window, so the native UI is not a Windows-only thing.
//
// ── Why this is small ───────────────────────────────────────────────────────
//
// The rasteriser is shared. Everything with a rule in it -- coverage, winding,
// gradients, text, hit-testing, gestures -- already ran and was checked before
// this file existed, and is byte-for-byte identical on every platform. What is
// left for a platform is three things: make a window, put a rectangle of pixels
// in it, and turn its input into MouseEvents. That is the whole file.
//
// ── Why libX11 is opened rather than linked ─────────────────────────────────
//
// Same reasoning as the GTK webview next door. A plugin that links libX11 hard
// fails to LOAD on a host that does not have it -- a Wayland-only session, a
// headless render farm -- and it fails with a loader error, which is the least
// diagnosable failure a plugin can have. Opened at runtime, the plugin loads,
// says why it has no native window, and the wrapper falls back.
//
// ── The headers, though ─────────────────────────────────────────────────────
//
// XEvent is a union of thirty structures and its layout is the ABI. Hand-rolling
// it the way the AU shim hand-rolls Apple's declarations would be inviting a
// silent mismatch, so this compiles against <X11/Xlib.h> when it is there and
// compiles to "unavailable, and here is why" when it is not. Nothing links.
//
// Including them is not free, and the cost is names. Xlib puts Window, Font,
// Cursor, Screen and Time into the GLOBAL namespace as typedefs, and None,
// Bool, Success, Always and Complex in as MACROS. The macros are put away at
// the bottom of this file. The typedefs cannot be -- they are types, not
// definitions -- and two of them collide with names this SDK already has:
//
//   sonore::gfx::Font  vs  ::Font    (an XID for a server-side font)
//   sonore::Window     vs  ::Window  (fft.h has a window-function selector)
//
// Inside this file every X11 type is spelled ::Window, ::Font and so on. A file
// that says `using namespace sonore::gfx` AND includes this one has to spell
// ours out the same way. There is no version of this that costs nothing.
#pragma once

#if defined(__linux__) && defined(__has_include)
#if __has_include(<X11/Xlib.h>)
#define SONORE_X11_HEADERS 1
#endif
#endif

#if defined(SONORE_X11_HEADERS)

#include <dlfcn.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstring>
#include <functional>
#include <string>
#include <vector>

// X.h defines KeyPress and KeyRelease as event-type CONSTANTS, and this SDK
// has a KeyPress struct. Left alone, every `sonore::gfx::KeyPress` below
// expands to `sonore::gfx::2` and the compiler reports it from inside X.h,
// hundreds of lines from anything that mentions either name.
//
// Captured as real constants and undefined here, BEFORE component.h declares
// the struct. The rest of X11's macro pollution is put away at the bottom of
// this file; these two cannot wait that long.
namespace sonore {
namespace gfx {
namespace x11 {
constexpr int kKeyPressEvent = KeyPress;
constexpr int kKeyReleaseEvent = KeyRelease;
/** X11's None: the null resource id. Our KeyPress enum has a None too, and
 *  `KeyPress::None` expanding to `KeyPress::0L` is the same collision one line
 *  down. */
constexpr unsigned long kNoneResource = None;
} // namespace x11
} // namespace gfx
} // namespace sonore
#undef KeyPress
#undef KeyRelease
#undef None

#include "clipboard.h"
#include "backing.h"
#include "component.h"
#include "displays.h"
#include "graphics.h"

namespace sonore {
namespace gfx {
namespace x11 {

/**
 * The pieces of Xlib this needs, resolved once.
 *
 * Deliberately a short list. Every entry here is a thing a plugin window
 * genuinely cannot do without, and the list not growing is the measure of
 * whether the shared-rasteriser design held.
 */
struct XlibApi {
  bool ok = false;

  ::Display* (*XOpenDisplay)(const char*) = nullptr;
  int (*XCloseDisplay)(::Display*) = nullptr;
  ::Window (*XCreateWindow)(::Display*, ::Window, int, int, unsigned, unsigned, unsigned, int, unsigned,
                          ::Visual*, unsigned long, ::XSetWindowAttributes*) = nullptr;
  int (*XDestroyWindow)(::Display*, ::Window) = nullptr;
  int (*XSelectInput)(::Display*, ::Window, long) = nullptr;
  int (*XMapWindow)(::Display*, ::Window) = nullptr;
  int (*XUnmapWindow)(::Display*, ::Window) = nullptr;
  int (*XResizeWindow)(::Display*, ::Window, unsigned, unsigned) = nullptr;
  int (*XFlush)(::Display*) = nullptr;
  int (*XSync)(::Display*, Bool) = nullptr;
  int (*XPending)(::Display*) = nullptr;
  int (*XNextEvent)(::Display*, ::XEvent*) = nullptr;
  ::GC (*XCreateGC)(::Display*, ::Drawable, unsigned long, ::XGCValues*) = nullptr;
  int (*XFreeGC)(::Display*, ::GC) = nullptr;
  ::XImage* (*XCreateImage)(::Display*, ::Visual*, unsigned, int, int, char*, unsigned, unsigned, int,
                          int) = nullptr;
  int (*XPutImage)(::Display*, ::Drawable, ::GC, ::XImage*, int, int, int, int, unsigned,
                   unsigned) = nullptr;
  int (*XFree)(void*) = nullptr;
  int (*XConnectionNumber)(::Display*) = nullptr;
  Status (*XGetWindowAttributes)(::Display*, ::Window, ::XWindowAttributes*) = nullptr;
  int (*XDefaultScreen)(::Display*) = nullptr;
  ::Visual* (*XDefaultVisual)(::Display*, int) = nullptr;
  int (*XDefaultDepth)(::Display*, int) = nullptr;
  int (*XGrabPointer)(::Display*, ::Window, Bool, unsigned, int, int, ::Window, Cursor,
                      ::Time) = nullptr;
  int (*XUngrabPointer)(::Display*, ::Time) = nullptr;
  int (*XDisplayWidth)(::Display*, int) = nullptr;
  /** The X resource database as one string. How a desktop environment tells
   *  everybody its DPI: GNOME, KDE and every login script write Xft.dpi there,
   *  and it is the only answer that reflects what the USER chose rather than
   *  what the monitor's EDID claims. Optional -- a bare server has none. */
  char* (*XResourceManagerString)(::Display*) = nullptr;
  /** The screen's physical width in millimetres, for the fallback when no
   *  desktop has said anything. Optional for the same reason. */
  int (*XDisplayWidthMM)(::Display*, int) = nullptr;
  int (*XDisplayHeight)(::Display*, int) = nullptr;
  Bool (*XTranslateCoordinates)(::Display*, ::Window, ::Window, int, int, int*, int*,
                                ::Window*) = nullptr;
  /** Turns a key event into BOTH a keysym and the text it produced, applying
   *  the layout and any modifiers. Doing this by hand from the keycode would
   *  mean reimplementing every keyboard layout in the world. */
  int (*XLookupString)(::XKeyEvent*, char*, int, ::KeySym*, ::XComposeStatus*) = nullptr;
  Cursor (*XCreateFontCursor)(::Display*, unsigned int) = nullptr;
  int (*XDefineCursor)(::Display*, ::Window, Cursor) = nullptr;
  int (*XFreeCursor)(::Display*, Cursor) = nullptr;
  int (*XSetSelectionOwner)(::Display*, ::Atom, ::Window, ::Time) = nullptr;
  ::Window (*XGetSelectionOwner)(::Display*, ::Atom) = nullptr;
  int (*XDeleteProperty)(::Display*, ::Window, ::Atom) = nullptr;
  ::Atom (*XInternAtom)(::Display*, const char*, Bool) = nullptr;
  int (*XChangeProperty)(::Display*, ::Window, ::Atom, ::Atom, int, int, const unsigned char*,
                         int) = nullptr;
  int (*XGetWindowProperty)(::Display*, ::Window, ::Atom, long, long, Bool, ::Atom, ::Atom*, int*,
                            unsigned long*, unsigned long*, unsigned char**) = nullptr;
  int (*XSendEvent)(::Display*, ::Window, Bool, long, ::XEvent*) = nullptr;
  int (*XConvertSelection)(::Display*, ::Atom, ::Atom, ::Atom, ::Window, ::Time) = nullptr;
};

inline const XlibApi& xlib() {
  static XlibApi api = [] {
    XlibApi a;
    // .so.6 first because that is what every current distribution ships; the
    // bare name only exists where the -dev package is installed, which on a
    // user's machine it is not.
    void* lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) lib = dlopen("libX11.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!lib) return a;

    bool all = true;
    auto bind = [&](const char* name, void* slot) {
      void* sym = dlsym(lib, name);
      if (!sym) all = false;
      std::memcpy(slot, &sym, sizeof(sym));
    };
    bind("XOpenDisplay", &a.XOpenDisplay);
    bind("XCloseDisplay", &a.XCloseDisplay);
    bind("XCreateWindow", &a.XCreateWindow);
    bind("XDestroyWindow", &a.XDestroyWindow);
    bind("XSelectInput", &a.XSelectInput);
    bind("XMapWindow", &a.XMapWindow);
    bind("XUnmapWindow", &a.XUnmapWindow);
    bind("XResizeWindow", &a.XResizeWindow);
    bind("XFlush", &a.XFlush);
    bind("XSync", &a.XSync);
    bind("XPending", &a.XPending);
    bind("XNextEvent", &a.XNextEvent);
    bind("XCreateGC", &a.XCreateGC);
    bind("XFreeGC", &a.XFreeGC);
    bind("XCreateImage", &a.XCreateImage);
    bind("XPutImage", &a.XPutImage);
    bind("XFree", &a.XFree);
    bind("XConnectionNumber", &a.XConnectionNumber);
    bind("XGetWindowAttributes", &a.XGetWindowAttributes);
    bind("XDefaultScreen", &a.XDefaultScreen);
    bind("XDefaultVisual", &a.XDefaultVisual);
    bind("XDefaultDepth", &a.XDefaultDepth);
    bind("XGrabPointer", &a.XGrabPointer);
    bind("XUngrabPointer", &a.XUngrabPointer);
    bind("XDisplayWidth", &a.XDisplayWidth);
    // Not through bind(): a server with no resource manager is normal, and
    // marking the whole API unusable over it would turn "no Xft.dpi" into "no
    // X11 peer at all".
    a.XResourceManagerString =
        (char* (*) (::Display*) ) dlsym(lib, "XResourceManagerString");
    a.XDisplayWidthMM = (int (*)(::Display*, int)) dlsym(lib, "XDisplayWidthMM");
    bind("XDisplayHeight", &a.XDisplayHeight);
    bind("XTranslateCoordinates", &a.XTranslateCoordinates);
    bind("XLookupString", &a.XLookupString);
    bind("XCreateFontCursor", &a.XCreateFontCursor);
    bind("XDefineCursor", &a.XDefineCursor);
    bind("XFreeCursor", &a.XFreeCursor);
    bind("XSetSelectionOwner", &a.XSetSelectionOwner);
    bind("XGetSelectionOwner", &a.XGetSelectionOwner);
    bind("XDeleteProperty", &a.XDeleteProperty);
    bind("XInternAtom", &a.XInternAtom);
    bind("XChangeProperty", &a.XChangeProperty);
    bind("XGetWindowProperty", &a.XGetWindowProperty);
    bind("XSendEvent", &a.XSendEvent);
    bind("XConvertSelection", &a.XConvertSelection);
    a.ok = all;
    return a;
  }();
  return api;
}

} // namespace x11

/**
 * The XDND atoms.
 *
 * X has no drag-and-drop of its own. XDND is a CONVENTION built on client
 * messages and selections, and every desktop implements it because there is
 * nothing else. It is a real handshake, not a notification: the source asks
 * whether we will take the drop and WAITS for an answer before the user is even
 * allowed to release, which is why the X11 peer can light a target on hover
 * where the Win32 one cannot.
 *
 * Interned once per display, because a round trip per atom per drag event would
 * be a dozen round trips a frame while the pointer moves.
 */
struct XdndAtoms {
  ::Atom aware = 0, enter = 0, position = 0, status = 0, drop = 0, leave = 0, finished = 0;
  ::Atom selection = 0, typeList = 0, actionCopy = 0;
  ::Atom uriList = 0, plainText = 0, property = 0;
  /** The CLIPBOARD selection, and the pieces of answering a request for it. */
  ::Atom clipboard = 0, utf8String = 0, targets = 0, clipboardProperty = 0;

  void intern(::Display* display) {
    const x11::XlibApi& x = x11::xlib();
    auto get = [&](const char* name) { return x.XInternAtom(display, name, 0); };
    aware = get("XdndAware");
    enter = get("XdndEnter");
    position = get("XdndPosition");
    status = get("XdndStatus");
    drop = get("XdndDrop");
    leave = get("XdndLeave");
    finished = get("XdndFinished");
    selection = get("XdndSelection");
    typeList = get("XdndTypeList");
    actionCopy = get("XdndActionCopy");
    uriList = get("text/uri-list");
    plainText = get("text/plain");
    property = get("SonoreXdndDrop");
    clipboard = get("CLIPBOARD");
    utf8String = get("UTF8_STRING");
    targets = get("TARGETS");
    clipboardProperty = get("SonoreClipboard");
  }
};

/**
 * A component tree in an X window, with the same surface as the Win32 one.
 *
 * Identical API on purpose: NativeEditor holds one or the other and knows
 * nothing about which, so a difference between the platforms has to be written
 * deliberately rather than arrived at.
 */
class NativeWindowX11 {
public:
  ~NativeWindowX11() { close(); }

  NativeWindowX11() = default;
  NativeWindowX11(const NativeWindowX11&) = delete;
  NativeWindowX11& operator=(const NativeWindowX11&) = delete;

  /** Called about thirty times a second, once someone is pumping. See the note
   *  on processEvents(): X gives a plugin no clock of its own. */
  std::function<void()> onTick;

  /**
   * Open inside a host's window.
   *
   * `parent` carries an X window id THROUGH a pointer, which is how both CLAP
   * and VST3 hand one over -- it is an integer, not an address, and casting it
   * as if it were one is the classic way to get a window that never appears.
   * Null makes a top-level window, which is what the tests use.
   */
  /**
   * Open as a POPUP: an override-redirect window at desktop coordinates,
   * above everything, holding the pointer until it is dismissed.
   *
   * override_redirect is the X term for "the window manager must not touch
   * this": no frame, no title bar, no placement policy, no tiling. It is what
   * every menu on every X desktop is, and without it a tiling window manager
   * puts the plugin's menu in its own tile.
   */
  bool openPopup(Component& content, int screenX, int screenY, int width, int height,
                 bool grabMouse = true) {
    isPopup_ = true;
    grabsMouse_ = grabMouse;
    const bool ok = open(nullptr, content, width, height, screenX, screenY);
    if (!ok) {
      isPopup_ = false;
      return false;
    }
    const x11::XlibApi& x = x11::xlib();
    // An ACTIVE grab, so a press outside the window still arrives here. That is
    // the whole mechanism: without it the click goes to whatever is behind, the
    // menu stays open, and two things respond to one gesture.
    //
    // owner_events false: report every event to this window in ITS coordinates,
    // which for a click outside are negative or past the edge -- exactly what
    // tells the menu it was dismissed.
    // A tooltip passes false: grabbing would stop the user reaching the very
    // control it is describing.
    if (grabsMouse_)
      x.XGrabPointer(display_, window_, 0,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                     GrabModeAsync, 0, 0, CurrentTime);
    x.XFlush(display_);
    return true;
  }

  /** Fires when the mouse goes down anywhere that is NOT this window. */
  std::function<void()> onDismissedOutside;

  // ── The clipboard, which on X11 is a protocol rather than a place ────────
  //
  // There is no clipboard on X11. There is a SELECTION: the application that
  // copied still OWNS the text and must answer requests for it, and when it
  // quits the clipboard goes with it. So copying means taking ownership and
  // agreeing to serve data later, and pasting means asking whoever owns it and
  // WAITING.
  //
  // A plugin that ignored this would appear to copy and then paste nothing.

  /** Take ownership of CLIPBOARD and keep the text to serve. */
  void setClipboardText(const std::string& utf8) {
    if (!isOpen()) return;
    clipboardText_ = utf8;
    const x11::XlibApi& x = x11::xlib();
    if (!x.XSetSelectionOwner) return;
    x.XSetSelectionOwner(display_, xdnd_.clipboard, window_, CurrentTime);
    x.XFlush(display_);
  }

  /**
   * Ask the owner for the clipboard, and wait briefly for it.
   *
   * Synchronous, deliberately. Paste is a keystroke a person just made and the
   * answer has to be in the field before the next frame; making it
   * asynchronous would mean the character appearing some indeterminate time
   * later. The wait is bounded -- an owner that never answers must not hang the
   * host's UI thread, and some do not answer at all.
   */
  bool getClipboardText(std::string* out) {
    if (!out || !isOpen()) return false;
    out->clear();
    const x11::XlibApi& x = x11::xlib();
    if (!x.XGetSelectionOwner || !x.XConvertSelection) return false;

    const ::Window owner = x.XGetSelectionOwner(display_, xdnd_.clipboard);
    if (owner == 0) return false;
    if (owner == window_) {
      // We own it. Asking ourselves would deadlock: the reply is generated by
      // this same event loop, which is currently inside this call.
      *out = clipboardText_;
      return !out->empty();
    }

    x.XConvertSelection(display_, xdnd_.clipboard, xdnd_.utf8String, xdnd_.clipboardProperty,
                        window_, CurrentTime);
    x.XFlush(display_);

    // Up to about 200 ms, which is long enough for any responsive application
    // and short enough that a dead one is not felt as a freeze.
    clipboardPending_ = true;
    clipboardResult_.clear();
    for (int i = 0; i < 200 && clipboardPending_; ++i) {
      ::XEvent event;
      while (x.XPending(display_) > 0) {
        x.XNextEvent(display_, &event);
        handleEvent(event);
        if (!clipboardPending_) break;
      }
      if (!clipboardPending_) break;
      usleep(1000);
    }
    clipboardPending_ = false;
    *out = clipboardResult_;
    return !out->empty();
  }

  bool isPopup() const { return isPopup_; }

  /** Where the pointer last was, in this window's coordinates. */
  Point lastMousePosition() const { return lastMouse_; }

  /**
   * Where this window's top-left corner is on the desktop.
   *
   * XGetWindowAttributes reports x/y relative to the PARENT, and a plugin
   * window is nested inside the host's -- so translating to the root is the
   * only way to get an answer a menu can be placed with. XTranslateCoordinates
   * does exactly that, in one round trip.
   */
  void desktopOrigin(int* x, int* y) const {
    *x = 0;
    *y = 0;
    if (!isOpen()) return;
    const x11::XlibApi& x11api = x11::xlib();
    const int screen = x11api.XDefaultScreen(display_);
    ::Window child = 0;
    int rx = 0, ry = 0;
    if (x11api.XTranslateCoordinates(display_, window_, RootWindow(display_, screen), 0, 0, &rx,
                                     &ry, &child)) {
      *x = rx;
      *y = ry;
    }
  }

  /** The desktop, so a menu can be placed inside it. Zeroes when closed. */
  void screenSize(int* width, int* height) const {
    *width = 0;
    *height = 0;
    if (!display_) return;
    const x11::XlibApi& x = x11::xlib();
    const int screen = x.XDefaultScreen(display_);
    *width = x.XDisplayWidth(display_, screen);
    *height = x.XDisplayHeight(display_, screen);
  }

  bool open(void* parent, Component& content, int width, int height) {
    return open(parent, content, width, height, 0, 0);
  }

  bool open(void* parent, Component& content, int width, int height, int screenX, int screenY) {
    const bool popup = isPopup_;
    close();
    isPopup_ = popup; // close() clears it; this call is the one that set it
    const x11::XlibApi& x = x11::xlib();
    if (!x.ok) return false;

    // Our OWN connection, not the host's. Xlib is not thread-safe per ::Display
    // and a plugin sharing the host's would be two event loops reading one
    // socket -- which fails as lost events rather than as a crash, months
    // later, in somebody else's DAW.
    display_ = x.XOpenDisplay(nullptr);
    // The size arrives in LOGICAL units, as everything above this file is, and
    // the server only speaks device pixels. Converted HERE, before the window is
    // created: a window created at the wrong size and corrected afterwards is a
    // visible jump on every editor that opens on a scaled desktop.
    if (display_) {
      openScale_ = desktopScale(display_);
      logicalWidth_ = (float) width;
      logicalHeight_ = (float) height;
      width = Backing::toDevice(logicalWidth_, openScale_);
      height = Backing::toDevice(logicalHeight_, openScale_);
    }
    if (!display_) return false;

    const int screen = x.XDefaultScreen(display_);
    visual_ = x.XDefaultVisual(display_, screen);
    depth_ = x.XDefaultDepth(display_, screen);
    // 15- and 16-bit visuals still exist on old remote-X setups and would need
    // a second pixel path. Refusing by name beats drawing confetti.
    if (depth_ != 24 && depth_ != 32) {
      closeDisplay();
      return false;
    }

    ::Window parentWindow = parent ? (::Window) (uintptr_t) parent : rootOf(screen);

    ::XSetWindowAttributes attrs{};
    // SelectionRequest and SelectionNotify arrive regardless of the event mask
    // -- they are addressed to the window rather than selected for -- which is
    // why the clipboard needs no bit here.
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                       LeaveWindowMask | EnterWindowMask | StructureNotifyMask | KeyPressMask;
    // No background pixel: every pixel comes from the bitmap, and letting the
    // server paint one first is a flash of flat colour on every resize -- the
    // same reason the Win32 class has no background brush.
    attrs.background_pixmap = x11::kNoneResource;
    attrs.bit_gravity = NorthWestGravity;
    // "The window manager must not touch this": no frame, no title bar, no
    // placement policy. Every menu on every X desktop is one of these, and
    // without it a tiling window manager puts a plugin's menu in its own tile.
    attrs.override_redirect = isPopup_ ? 1 : 0;

    window_ = x.XCreateWindow(display_, parentWindow, isPopup_ ? screenX : 0,
                              isPopup_ ? screenY : 0, (unsigned) width, (unsigned) height, 0,
                              depth_, InputOutput, visual_,
                              CWEventMask | CWBackPixmap | CWBitGravity | CWOverrideRedirect,
                              &attrs);
    if (!window_) {
      closeDisplay();
      return false;
    }

    gc_ = x.XCreateGC(display_, window_, 0, nullptr);
    if (!gc_) {
      x.XDestroyWindow(display_, window_);
      window_ = 0;
      closeDisplay();
      return false;
    }

    content_ = &content;
    router_ = new MouseRouter(content);
    backing_.setContent(content_);
    // The desktop's scale before the first size, so the tree is laid out once
    // at the right one rather than at 100% and again a frame later.
    // The same scale the size was converted with a moment ago, so the two
    // cannot disagree.
    backing_.setScale(openScale_);
    backing_.setDeviceSize(width, height);

    // Tell the desktop this window takes drops. Version 5 is what every
    // current implementation speaks; without this property no source will
    // even send us an XdndEnter.
    xdnd_.intern(display_);
    {
      const unsigned long version = 5;
      x.XChangeProperty(display_, window_, xdnd_.aware, 6 /*XA_ATOM*/, 32, 0 /*PropModeReplace*/,
                        (const unsigned char*) &version, 1);
    }

    x.XMapWindow(display_, window_);
    x.XFlush(display_);

    // The Clipboard front-end has no way to reach a selection on its own: a
    // selection belongs to a WINDOW. So the open window installs itself, and
    // TextEditor's copy and paste reach the desktop rather than an in-process
    // string.
    refreshDisplays();
    activeForClipboard() = this;
    *Clipboard::setter() = [](const std::string& text) {
      if (NativeWindowX11* peer = activeForClipboard()) peer->setClipboardText(text);
    };
    *Clipboard::getter() = [](std::string* out) {
      NativeWindowX11* peer = activeForClipboard();
      return peer != nullptr && peer->getClipboardText(out);
    };

    content_->repaint();
    render();
    return true;
  }

  void close() {
    const x11::XlibApi& x = x11::xlib();
    if (display_) {
      if (image_) {
        // XDestroyImage would free the pixel buffer with it, and that buffer is
        // scratch_, which this object owns. Detach first.
        image_->data = nullptr;
        x.XFree(image_);
        image_ = nullptr;
      }
      if (gc_) {
        x.XFreeGC(display_, gc_);
        gc_ = nullptr;
      }
      if (window_) {
        // Released BEFORE the window goes. A grab outliving its window leaves
        // the whole desktop unable to click on anything until the connection
        // is dropped -- which is the single worst thing a plugin can do to a
        // person's session.
        if (isPopup_ && grabsMouse_) x.XUngrabPointer(display_, CurrentTime);
        x.XDestroyWindow(display_, window_);
        window_ = 0;
      }
      closeDisplay();
    }
    // Uninstalled BEFORE the window goes, or the next copy reaches a peer whose
    // display has been closed.
    if (activeForClipboard() == this) {
      activeForClipboard() = nullptr;
      *Clipboard::setter() = nullptr;
      *Clipboard::getter() = nullptr;
    }
    delete router_;
    router_ = nullptr;
    content_ = nullptr;
    // The backing store too. It holds the same pointer and OUTLIVES the window
    // -- a peer is a member of NativeEditor and is reused across every open --
    // so leaving it set means the next paint reaches a component the editor has
    // already destroyed. On Win32 that was not theoretical: CreateWindowExW
    // dispatches WM_SIZE synchronously, so the second open() of a reused editor
    // painted through the first one's freed viewport before it had returned.
    backing_.setContent(nullptr);
    isPopup_ = false;
  }

  /** The window that currently serves the clipboard. One per process is right:
   *  a host with two editors open has two windows, and either can serve. */
  static NativeWindowX11*& activeForClipboard() {
    static NativeWindowX11* active = nullptr;
    return active;
  }

  void* handle() const { return (void*) (uintptr_t) window_; }
  bool isOpen() const { return window_ != 0 && display_ != nullptr; }
  Bitmap& bitmap() { return backing_.bitmap(); }

  /** A host that implements the scale extension overrides whatever the desktop
   *  said: it may be compositing our view into a surface it scales itself, and
   *  its answer is about the view where ours is only about the screen. */
  void setScale(float scale) {
    backing_.setScale(scale);
    render();
  }

  float scale() const { return backing_.scale(); }
  float logicalWidth() const { return backing_.logicalWidth(); }
  float logicalHeight() const { return backing_.logicalHeight(); }

  /** LOGICAL units, converted at the current scale. */
  void setLogicalSize(float width, float height) {
    logicalWidth_ = width;
    logicalHeight_ = height;
    setSize(Backing::toDevice(width, backing_.scale()),
            Backing::toDevice(height, backing_.scale()));
  }
  MouseRouter* router() { return router_; }

  /**
   * The X socket.
   *
   * A plugin on X11 has no clock and no message loop of its own: the host owns
   * both. CLAP says so explicitly and offers clap_host_posix_fd_support for
   * exactly this -- hand it this descriptor and the host calls back when
   * something arrives. Without it a plugin can only be pumped by a timer, which
   * works but adds up to 33 ms of lag to every click.
   */
  int connectionFd() const {
    if (!display_) return -1;
    return x11::xlib().XConnectionNumber(display_);
  }

  void setVisible(bool shouldBeVisible) {
    if (!isOpen()) return;
    const x11::XlibApi& x = x11::xlib();
    if (shouldBeVisible) x.XMapWindow(display_, window_);
    else x.XUnmapWindow(display_, window_);
    x.XFlush(display_);
  }

  void setSize(int width, int height) {
    if (!isOpen() || width <= 0 || height <= 0) return;
    x11::xlib().XResizeWindow(display_, window_, (unsigned) width, (unsigned) height);
    resized(width, height);
  }

  /**
   * Drain whatever the server has sent, then tick.
   *
   * Called from the host's fd callback, or from a timer where there is no fd
   * support. Never blocks: XPending is checked first, and XNextEvent is only
   * called when it says there is something -- a plugin that blocked here would
   * freeze the host's UI thread, which is the thread it is being called on.
   */
  void processEvents() {
    if (!isOpen()) return;
    const x11::XlibApi& x = x11::xlib();
    ::XEvent event;
    while (x.XPending(display_) > 0) {
      x.XNextEvent(display_, &event);
      handleEvent(event);
    }
    if (onTick) onTick();
    renderIfDirty();
  }

  /** Repaint if anything asked to be. */
  bool renderIfDirty() {
    if (!isOpen() || !content_ || !content_->isDirty()) return false;
    render();
    return true;
  }

private:
  /**
   * What one logical pixel is worth on this desktop.
   *
   * Xft.dpi first, because it is what the USER chose: a desktop environment
   * writes it into the resource database when somebody moves the scaling
   * slider, and every toolkit on the machine reads it. Whatever the monitor's
   * EDID says about its physical size is not that -- a 27-inch 4K screen at
   * arm's length and a 15-inch 4K laptop have nearly the same dot pitch and
   * want completely different scales.
   *
   * The physical fallback is deliberately COARSE, rounded to a quarter and
   * capped: computing 163/96 = 1.7 from a laptop panel and applying it exactly
   * gives an interface at a scale nothing else on the desktop is using. And
   * many servers report a made-up 1000x1000 mm, which comes out at 96 -- the
   * right answer, arrived at by accident, and the reason not to trust it
   * further than the nearest quarter.
   */
  static float desktopScale(::Display* display) {
    const x11::XlibApi& x = x11::xlib();
    if (!display) return 1.0f;

    if (x.XResourceManagerString) {
      if (const char* db = x.XResourceManagerString(display)) {
        // Hand-scanned rather than through Xrm: the database is a handful of
        // lines of "key:\tvalue", and pulling in XrmGetResource means four more
        // symbols and an XrmValue for one number.
        const char* key = "Xft.dpi:";
        for (const char* at = db; (at = std::strstr(at, key)) != nullptr; at += 8) {
          if (at != db && at[-1] != '\n') continue; // "...Xft.dpi:" inside another key
          const char* v = at + 8;
          while (*v == ' ' || *v == '\t') ++v;
          const double dpi = std::atof(v);
          if (dpi >= 48.0 && dpi <= 768.0) return (float) (dpi / 96.0);
          break;
        }
      }
    }

    if (!x.XDisplayWidth) return 1.0f;
    const int screen = x.XDefaultScreen ? x.XDefaultScreen(display) : 0;
    const int pixels = x.XDisplayWidth(display, screen);
    const int mm = x.XDisplayWidthMM ? x.XDisplayWidthMM(display, screen) : 0;
    if (pixels <= 0 || mm <= 0) return 1.0f;
    const double dpi = (double) pixels * 25.4 / (double) mm;
    const double raw = dpi / 96.0;
    // To the nearest quarter, and never below 1: an interface smaller than its
    // logical size is a bug in every case, where a slightly large one is a
    // preference.
    const double quarters = (double) (int) (raw * 4.0 + 0.5) / 4.0;
    return (float) (quarters < 1.0 ? 1.0 : (quarters > 4.0 ? 4.0 : quarters));
  }

  /**
   * Ask the server for every monitor, through Xinerama.
   *
   * Xinerama rather than XRandR: the query is one call returning an array of
   * rectangles, where XRandR needs a resource walk over CRTCs and outputs to
   * arrive at the same answer. Every server that has XRandR also answers
   * Xinerama, because desktop environments have relied on it for twenty years.
   *
   * dlopened separately, like everything else here -- libXinerama is a distinct
   * shared object and linking it would stop a plugin loading on a machine
   * without it, for a feature that degrades perfectly well to one screen.
   *
   * X has no concept of a work area, so bounds and workArea are the same. A
   * panel on X is a window like any other and the SDK cannot ask where it is
   * without a second protocol.
   */
  void refreshDisplays() {
    if (!display_) return;

    struct ScreenInfo {
      short screen_number;
      short x_org, y_org;
      short width, height;
    };
    using QueryFn = ScreenInfo* (*) (::Display*, int*);
    using ActiveFn = Bool (*)(::Display*);

    static void* library = nullptr;
    static QueryFn query = nullptr;
    static ActiveFn active = nullptr;
    static bool tried = false;
    if (!tried) {
      tried = true;
      library = dlopen("libXinerama.so.1", RTLD_LAZY);
      if (!library) library = dlopen("libXinerama.so", RTLD_LAZY);
      if (library) {
        void* q = dlsym(library, "XineramaQueryScreens");
        void* a = dlsym(library, "XineramaIsActive");
        std::memcpy(&query, &q, sizeof(q));
        std::memcpy(&active, &a, sizeof(a));
      }
    }

    std::vector<Display> found;
    if (query && (!active || active(display_))) {
      int count = 0;
      ScreenInfo* screens = query(display_, &count);
      if (screens) {
        for (int i = 0; i < count; ++i) {
          Display d;
          d.bounds = Rect((float) screens[i].x_org, (float) screens[i].y_org,
                          (float) screens[i].width, (float) screens[i].height);
          d.workArea = d.bounds;
          // Xinerama does not say which is primary. Screen 0 is the
          // conventional answer and is what every toolkit assumes.
          d.isMain = i == 0;
          found.push_back(d);
        }
        x11::xlib().XFree(screens);
      }
    }

    if (found.empty()) {
      // No Xinerama, or a server with one screen. The X screen's own size is
      // the honest answer.
      int w = 0, h = 0;
      screenSize(&w, &h);
      if (w > 0 && h > 0) {
        Display d;
        d.bounds = Rect(0.0f, 0.0f, (float) w, (float) h);
        d.workArea = d.bounds;
        d.isMain = true;
        found.push_back(d);
      }
    }
    if (!found.empty()) Displays::set(std::move(found));
  }

  ::Window rootOf(int screen) const {
    // DefaultRootWindow is a macro over the Display struct, which is fine: it
    // reads a field, it does not call into the library.
    return RootWindow(display_, screen);
  }

  void closeDisplay() {
    if (!display_) return;
    x11::xlib().XCloseDisplay(display_);
    display_ = nullptr;
  }

  /** DEVICE pixels: ConfigureNotify reports the server's own, which are
   *  always device. The logical size is derived from the scale. */
  void resized(int width, int height) {
    if (!content_ || width <= 0 || height <= 0) return;
    if (backing_.deviceWidth() == width && backing_.deviceHeight() == height) return;
    backing_.setDeviceSize(width, height);
    // The XImage wraps the old buffer, which the resize has just replaced.
    dropImage();
    render();
  }

  void dropImage() {
    if (!image_) return;
    image_->data = nullptr; // scratch_ is ours; see close()
    x11::xlib().XFree(image_);
    image_ = nullptr;
  }

  /** See the note on the Win32 peer's render(): the ground under the damaged
   *  area has to be cleared before the tree paints over it, or the previous
   *  frame shows through anything transparent. */
  void render() {
    if (!isOpen() || !content_) return;
    // One XPutImage per changed rectangle. Each is a request on the wire, and
    // sending three small ones beats sending one covering the whole window --
    // which is what a union used to produce.
    for (const PixelRect& r : backing_.render()) blit(r);
  }

  /** `area` is the part of the bitmap to send. */
  void blit(const PixelRect& area) {
    const x11::XlibApi& x = x11::xlib();
    const Bitmap& bm = backing_.bitmap();
    const int w = bm.width(), h = bm.height();
    if (w <= 0 || h <= 0) return;

    // Already device pixels and already clipped by the backing store, but
    // intersected again: blit is also called from Expose, where the rectangle
    // is the server's and nothing here has vetted it.
    PixelRect region = area.intersection(bm.bounds());
    if (region.isEmpty()) return;

    // RGBA to BGRA, the same swap the Win32 blit does and for the same reason:
    // the rasteriser's byte order is chosen once, for the rasteriser, and each
    // platform pays for its own convention at the edge.
    scratch_.resize((size_t) w * (size_t) h * 4u);
    const uint8_t* src = bm.data();
    uint8_t* dst = scratch_.data();
    for (size_t i = 0, n = (size_t) w * (size_t) h; i < n; ++i) {
      dst[i * 4 + 0] = src[i * 4 + 2];
      dst[i * 4 + 1] = src[i * 4 + 1];
      dst[i * 4 + 2] = src[i * 4 + 0];
      dst[i * 4 + 3] = src[i * 4 + 3];
    }

    if (!image_ || image_->width != w || image_->height != h) {
      dropImage();
      image_ = x.XCreateImage(display_, visual_, (unsigned) depth_, ZPixmap, 0, (char*) dst,
                              (unsigned) w, (unsigned) h, 32, 0);
      if (!image_) return;
      // Stated rather than inherited. XCreateImage takes the byte order from
      // the DISPLAY, which for a remote X server on a big-endian machine is not
      // ours -- and the failure is a picture with red and blue swapped that
      // nobody can reproduce locally.
      image_->byte_order = LSBFirst;
    }
    image_->data = (char*) dst;
    // Source and destination are the same coordinates -- the scratch buffer is
    // the whole bitmap, and only the named rectangle is sent.
    x.XPutImage(display_, window_, gc_, image_, region.x, region.y, region.x, region.y,
                (unsigned) region.w, (unsigned) region.h);
    x.XFlush(display_);
  }

  /** A keysym, or None for anything that will arrive as text.
   *
   *  The values are from keysymdef.h and are stable ABI -- they have not
   *  changed since X11R6. Written out rather than included, because pulling in
   *  that header for thirteen constants would add another file to the list a
   *  build has to have. */
  static int namedKey(::KeySym sym) {
    switch (sym) {
      case 0xff08: return KeyPress::Backspace; // XK_BackSpace
      case 0xff09: return KeyPress::Tab;       // XK_Tab
      case 0xff0d: return KeyPress::Return;    // XK_Return
      case 0xff8d: return KeyPress::Return;    // XK_KP_Enter
      case 0xff1b: return KeyPress::Escape;    // XK_Escape
      case 0xffff: return KeyPress::Delete;    // XK_Delete
      case 0xff51: return KeyPress::Left;      // XK_Left
      case 0xff53: return KeyPress::Right;     // XK_Right
      case 0xff52: return KeyPress::Up;        // XK_Up
      case 0xff54: return KeyPress::Down;      // XK_Down
      case 0xff50: return KeyPress::Home;      // XK_Home
      case 0xff57: return KeyPress::End;       // XK_End
      case 0xff55: return KeyPress::PageUp;    // XK_Page_Up
      case 0xff56: return KeyPress::PageDown;  // XK_Page_Down
      default: return KeyPress::None;
    }
  }

  /** XLookupString returns UTF-8 in a modern locale and Latin-1 in the C
   *  locale, and a plugin has no say in which one the host set. Decoding as
   *  UTF-8 and falling back to the single byte covers both: a valid multi-byte
   *  sequence cannot occur by accident in Latin-1 text. */
  static uint32_t firstCodePoint(const char* buffer, int length) {
    const unsigned char c = (unsigned char) buffer[0];
    if (c < 0x80) return c;
    int extra = 0;
    uint32_t cp = 0;
    if (c >= 0xf0) { cp = c & 0x07u; extra = 3; }
    else if (c >= 0xe0) { cp = c & 0x0fu; extra = 2; }
    else if (c >= 0xc0) { cp = c & 0x1fu; extra = 1; }
    else return c; // a continuation byte on its own: Latin-1
    if (length <= extra) return c;
    for (int i = 0; i < extra; ++i) {
      const unsigned char cont = (unsigned char) buffer[i + 1];
      if ((cont & 0xc0u) != 0x80u) return c; // not UTF-8 after all
      cp = (cp << 6) | (cont & 0x3fu);
    }
    return cp;
  }

  /**
   * One step of the XDND handshake.
   *
   * Enter says a drag has arrived and lists what it offers. Position asks, over
   * and over as the pointer moves, "will you take it HERE" -- and every one
   * must be answered with a Status or the source stalls. Drop says the user let
   * go. Leave says they went elsewhere.
   */
  void handleXdnd(const ::XClientMessageEvent& msg) {
    const x11::XlibApi& x = x11::xlib();

    if (msg.message_type == xdnd_.enter) {
      dragSource_ = (::Window) msg.data.l[0];
      // Bit 0 of data.l[1] says the source has more than three types and has
      // put the full list in a property. Ours is simple: we take a uri-list if
      // one is offered, and the three inline slots hold it in practice.
      dragHasUriList_ = false;
      for (int i = 2; i <= 4; ++i)
        if ((::Atom) msg.data.l[i] == xdnd_.uriList) dragHasUriList_ = true;
      if ((msg.data.l[1] & 1) != 0) dragHasUriList_ = true; // long list: ask at drop time
      return;
    }

    if (msg.message_type == xdnd_.position) {
      // Coordinates are ROOT-relative and the window is somewhere inside the
      // host's, so they have to be translated. Using them unconverted lights
      // the wrong component, or none.
      int originX = 0, originY = 0;
      desktopOrigin(&originX, &originY);
      const int rootX = (int) ((unsigned long) msg.data.l[2] >> 16);
      const int rootY = (int) ((unsigned long) msg.data.l[2] & 0xffff);
      dragPoint_ = pointOf(rootX - originX, rootY - originY);

      bool accept = false;
      if (dragHasUriList_ && router_) {
        // Nothing is known about the FILES yet -- XDND does not send them until
        // the drop -- so the question asked is "would anything here take files
        // at all". An empty list is the honest way to ask that.
        static const std::vector<std::string> unknown;
        accept = router_->fileDragMove(unknown, dragPoint_);
        renderIfDirty();
      }

      ::XEvent reply{};
      reply.type = ClientMessage;
      reply.xclient.display = display_;
      reply.xclient.window = dragSource_;
      reply.xclient.message_type = xdnd_.status;
      reply.xclient.format = 32;
      reply.xclient.data.l[0] = (long) window_;
      reply.xclient.data.l[1] = accept ? 1 : 0;
      // A zero rectangle means "ask me again on every move", which is what a
      // component tree needs: the target changes as the pointer crosses from
      // one component to the next, and a source told otherwise stops asking.
      reply.xclient.data.l[2] = 0;
      reply.xclient.data.l[3] = 0;
      reply.xclient.data.l[4] = accept ? (long) xdnd_.actionCopy : 0;
      x.XSendEvent(display_, dragSource_, 0, 0, &reply);
      x.XFlush(display_);
      return;
    }

    if (msg.message_type == xdnd_.leave) {
      if (router_) router_->fileDragExit();
      renderIfDirty();
      dragSource_ = 0;
      dragHasUriList_ = false;
      return;
    }

    if (msg.message_type == xdnd_.drop) {
      if (!dragHasUriList_) {
        sendFinished(false);
        return;
      }
      dropTime_ = (::Time) msg.data.l[2];
      // Asks for the data. It arrives later as a SelectionNotify, which is why
      // this cannot be answered here: XDND puts the payload in a selection
      // rather than in the message, because a list of paths does not fit in
      // twenty bytes.
      x.XConvertSelection(display_, xdnd_.selection, xdnd_.uriList, xdnd_.property, window_,
                          dropTime_);
      x.XFlush(display_);
      return;
    }
  }

  void readClipboardReply() {
    clipboardPending_ = false;
    clipboardResult_.clear();
    const x11::XlibApi& x = x11::xlib();
    ::Atom type = 0;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    if (x.XGetWindowProperty(display_, window_, xdnd_.clipboardProperty, 0, 65536, 1 /*delete*/,
                             0 /*any*/, &type, &format, &count, &remaining, &data) != 0 ||
        !data)
      return;
    clipboardResult_.assign((const char*) data, (size_t) count);
    x.XFree(data);
  }

  void readDroppedFiles() {
    const x11::XlibApi& x = x11::xlib();
    ::Atom type = 0;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    // 65536 longs is a quarter of a megabyte of paths, which is more than any
    // real drop and small enough to ask for in one go.
    if (x.XGetWindowProperty(display_, window_, xdnd_.property, 0, 65536, 1 /*delete*/, 0 /*any*/,
                             &type, &format, &count, &remaining, &data) != 0 ||
        !data) {
      sendFinished(false);
      return;
    }

    std::vector<std::string> files = parseUriList((const char*) data, (size_t) count);
    x.XFree(data);

    bool accepted = false;
    if (!files.empty() && router_) {
      router_->fileDragMove(files, dragPoint_);
      accepted = router_->filesDropped(files, dragPoint_);
      renderIfDirty();
    } else if (router_) {
      router_->fileDragExit();
    }
    sendFinished(accepted);
  }

  /** The source is BLOCKED until this arrives. A peer that dropped the reply
   *  leaves the file manager showing a drag that never finishes. */
  void sendFinished(bool accepted) {
    if (!dragSource_) return;
    const x11::XlibApi& x = x11::xlib();
    ::XEvent reply{};
    reply.type = ClientMessage;
    reply.xclient.display = display_;
    reply.xclient.window = dragSource_;
    reply.xclient.message_type = xdnd_.finished;
    reply.xclient.format = 32;
    reply.xclient.data.l[0] = (long) window_;
    reply.xclient.data.l[1] = accepted ? 1 : 0;
    reply.xclient.data.l[2] = accepted ? (long) xdnd_.actionCopy : 0;
    x.XSendEvent(display_, dragSource_, 0, 0, &reply);
    x.XFlush(display_);
    dragSource_ = 0;
    dragHasUriList_ = false;
  }

  /**
   * text/uri-list into paths.
   *
   * The format is RFC 2483: CRLF-separated URIs, and lines beginning with # are
   * comments. Every desktop sends file:// URIs with percent-encoding, so a
   * folder called "My Samples" arrives as My%20Samples and a peer that did not
   * decode it would hand the plugin a path that does not exist.
   */
  static std::vector<std::string> parseUriList(const char* data, size_t length) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < length) {
      size_t end = i;
      while (end < length && data[end] != '\r' && data[end] != '\n') ++end;
      std::string line(data + i, end - i);
      i = end;
      while (i < length && (data[i] == '\r' || data[i] == '\n')) ++i;
      if (line.empty() || line[0] == '#') continue;

      if (line.compare(0, 7, "file://") == 0) {
        line.erase(0, 7);
        // Strip the hostname, which is empty for a local file but present as
        // "localhost" from some sources: file://localhost/home/... is one path,
        // not a host and a path.
        if (line.compare(0, 9, "localhost") == 0) line.erase(0, 9);
      } else if (line.compare(0, 5, "file:") == 0) {
        line.erase(0, 5);
      }

      std::string decoded;
      decoded.reserve(line.size());
      for (size_t k = 0; k < line.size(); ++k) {
        if (line[k] == '%' && k + 2 < line.size()) {
          const int hi = hexDigit(line[k + 1]), lo = hexDigit(line[k + 2]);
          if (hi >= 0 && lo >= 0) {
            decoded.push_back((char) (hi * 16 + lo));
            k += 2;
            continue;
          }
        }
        decoded.push_back(line[k]);
      }
      if (!decoded.empty()) out.push_back(std::move(decoded));
    }
    return out;
  }

public:
  /** The URI parser, for the test that drives it.
   *
   *  Public rather than reached through a friend declaration, because it is a
   *  pure function of a string: nothing about it needs a window, a display or
   *  an instance, and hiding a pure function behind a friend is a way of making
   *  it harder to check than it is to write. */
  static std::vector<std::string> parseUriListForTest(const char* data, size_t length) {
    return parseUriList(data, length);
  }

private:
  static int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  /**
   * Tell the server what the pointer should look like over this window.
   *
   * Only when it CHANGES. XDefineCursor is a round trip to the server, and
   * making one on every motion event -- which arrive as fast as the pointer
   * moves -- would put a request on the wire for every pixel of a drag.
   *
   * The cursors are created once each and cached, because XCreateFontCursor is
   * another round trip and there are seven of them.
   */
  void applyCursor() {
    if (!router_ || !isOpen()) return;
    const MouseCursor wanted = router_->cursorForPointer();
    if (wanted == currentCursor_) return;
    currentCursor_ = wanted;

    const x11::XlibApi& x = x11::xlib();
    if (!x.XCreateFontCursor || !x.XDefineCursor) return;
    // From the standard X cursor font, which every server has and which the
    // user's theme restyles. A plugin drawing its own would be the one thing on
    // the desktop that looked wrong.
    unsigned int shape = 68; // XC_left_ptr
    switch (wanted) {
      case MouseCursor::Pointing: shape = 60; break;        // XC_hand2
      case MouseCursor::DragVertical: shape = 116; break;   // XC_sb_v_double_arrow
      case MouseCursor::DragHorizontal: shape = 108; break; // XC_sb_h_double_arrow
      case MouseCursor::ResizeCorner: shape = 14; break;    // XC_bottom_right_corner
      case MouseCursor::ResizeLeftRight: shape = 108; break; // XC_sb_h_double_arrow
      case MouseCursor::ResizeUpDown: shape = 116; break;    // XC_sb_v_double_arrow
      case MouseCursor::Text: shape = 152; break;           // XC_xterm
      case MouseCursor::Wait: shape = 150; break;           // XC_watch
      default: break;
    }
    const Cursor cursor = x.XCreateFontCursor(display_, shape);
    if (!cursor) return;
    x.XDefineCursor(display_, window_, cursor);
    // Freed straight away: XDefineCursor takes its own reference, so the
    // window keeps it and this handle is ours to drop. Holding all seven would
    // leak one per window per shape across a session.
    if (x.XFreeCursor) x.XFreeCursor(display_, cursor);
    x.XFlush(display_);
  }

  /**
   * A pointer position from the server, in the tree's own units.
   *
   * X reports DEVICE pixels, always. At 200% an unconverted position is twice
   * the coordinate the control was drawn at, so every click lands on whatever
   * is a screenful down and to the right -- which does not read as a coordinate
   * bug, it reads as "the plugin ignores my mouse".
   */
  Point pointOf(int x, int y) const { return backing_.toLogical(x, y); }

  /**
   * The character a keysym stands for, for a MODIFIER combination.
   *
   * X keysyms for ASCII are the ASCII values, so this is mostly a range check.
   * Lowercased for letters, so a shortcut table compares one character and
   * reads shiftDown separately -- the same rule the Win32 peer follows, so a
   * table written once behaves the same on both.
   */
  static uint32_t characterForKeysym(unsigned long keysym) {
    if (keysym < 0x20 || keysym > 0x7e) return 0;
    if (keysym >= 'A' && keysym <= 'Z') return (uint32_t) (keysym - 'A' + 'a');
    return (uint32_t) keysym;
  }
  static bool shiftHeld(unsigned state) { return (state & ShiftMask) != 0; }
  static bool ctrlHeld(unsigned state) { return (state & ControlMask) != 0; }
  static bool altHeld(unsigned state) { return (state & Mod1Mask) != 0; }

  void handleEvent(const ::XEvent& event) {
    switch (event.type) {
      case Expose:
        // Only on the LAST of a burst: the server sends one per exposed
        // rectangle, and blitting the whole window for each is the same picture
        // drawn five times.
        if (event.xexpose.count == 0)
          blit(backing_.bitmap().bounds());
        return;

      case ConfigureNotify:
        resized(event.xconfigure.width, event.xconfigure.height);
        return;

      case ButtonPress: {
        const ::XButtonEvent& b = event.xbutton;
        if (b.button == Button4 || b.button == Button5) {
          // X delivers a wheel as a button press. Button4 is up.
          router_->mouseWheel(pointOf(b.x, b.y), b.button == Button4 ? 1.0f : -1.0f,
                              shiftHeld(b.state), ctrlHeld(b.state), altHeld(b.state));
          renderIfDirty();
          return;
        }
        if (b.button == Button3) {
          // The right button. No grab and no drag state -- see
          // Component::contextMenu for why it is a separate path.
          router_->mouseDown(pointOf(b.x, b.y), 1, shiftHeld(b.state), ctrlHeld(b.state),
                             altHeld(b.state), /*rightButton=*/true);
          renderIfDirty();
          return;
        }
        if (b.button != Button1) return;
        // With the grab held, a press ANYWHERE arrives here in this window's
        // coordinates -- outside means negative or past the edge.
        if (isPopup_ && grabsMouse_) {
          const Rect bounds = content_ ? content_->bounds() : Rect();
          if (!bounds.contains(pointOf(b.x, b.y))) {
            if (onDismissedOutside) onDismissedOutside();
            return;
          }
        }
        // X has no double-click: the server reports two presses and every
        // toolkit decides for itself. 400 ms and 4 pixels are what GTK and Qt
        // both use, and a knob that would not reset to its default on a double
        // click is a control users quietly stop trusting.
        const bool isDouble = lastClickTime_ != 0 && b.time > lastClickTime_ &&
                              (b.time - lastClickTime_) <= 400 &&
                              std::abs(b.x - lastClickX_) <= 4 && std::abs(b.y - lastClickY_) <= 4;
        lastClickTime_ = b.time;
        lastClickX_ = b.x;
        lastClickY_ = b.y;
        // No explicit grab. X gives the pressed window an implicit passive
        // grab until the button is released, so a drag that leaves the window
        // still arrives -- which is exactly what SetCapture buys on Windows.
        router_->mouseDown(pointOf(b.x, b.y), isDouble ? 2 : 1, shiftHeld(b.state),
                           ctrlHeld(b.state), altHeld(b.state));
        renderIfDirty();
        return;
      }

      case ButtonRelease: {
        const ::XButtonEvent& b = event.xbutton;
        if (b.button != Button1) return;
        router_->mouseUp(pointOf(b.x, b.y), shiftHeld(b.state), ctrlHeld(b.state),
                         altHeld(b.state));
        renderIfDirty();
        return;
      }

      case MotionNotify: {
        const ::XMotionEvent& m = event.xmotion;
        lastMouse_ = pointOf(m.x, m.y);
        router_->mouseMove(pointOf(m.x, m.y), shiftHeld(m.state), ctrlHeld(m.state),
                           altHeld(m.state));
        applyCursor();
        renderIfDirty();
        return;
      }

      // ── Keyboard ───────────────────────────────────────────────────────
      //
      // XLookupString gives BOTH halves at once: the keysym, which names the
      // key regardless of layout, and the bytes it produced, which is what the
      // layout actually made of it. Windows needs two messages for the same
      // information and macOS needs two properties of one event; X is the tidy
      // one here.
      case x11::kKeyPressEvent: {
        ::XKeyEvent copy = event.xkey; // XLookupString takes a non-const pointer
        char buffer[32] = {0};
        ::KeySym keysym = 0;
        const int n = x11::xlib().XLookupString(&copy, buffer, (int) sizeof(buffer) - 1, &keysym,
                                                nullptr);
        KeyPress key;
        key.keyCode = namedKey(keysym);
        key.shiftDown = shiftHeld(copy.state);
        key.ctrlDown = ctrlHeld(copy.state);
        key.altDown = altHeld(copy.state);
        // Only when it is NOT a named key. Return produces "\r" and Backspace
        // produces "\b" here, and letting those through as characters would
        // type a control code into a field that already handled the key.
        if (key.keyCode == KeyPress::None) {
          if (key.ctrlDown || key.altDown) {
            // ── A modifier combination ──
            //
            // XLookupString applies Ctrl, so Ctrl+A comes back as 0x01 rather
            // than 'a' -- exactly as WM_CHAR does on Windows, and with exactly
            // the same consequence: every shortcut dead, because TextEditor
            // compares against 'a' and nothing ever delivered it.
            //
            // The KEYSYM is the right source here. Ctrl does not change it, so
            // it still names the key the user pressed with their layout
            // applied, which taking the raw keycode would throw away.
            key.character = characterForKeysym(keysym);
          } else if (n > 0) {
            // No modifier: the TEXT is the only correct source. It has seen
            // dead keys, compose sequences and the input method, none of which
            // a keysym knows about.
            key.character = firstCodePoint(buffer, n);
          }
        }
        if (key.keyCode != KeyPress::None || key.character != 0) {
          router_->keyPressed(key);
          renderIfDirty();
        }
        return;
      }

      case ClientMessage:
        handleXdnd(event.xclient);
        return;

      // Somebody wants what we copied. Answering is the whole obligation of
      // owning a selection; a window that ignores this is one whose clipboard
      // silently does nothing in every other application.
      case SelectionRequest: {
        const ::XSelectionRequestEvent& request = event.xselectionrequest;
        const x11::XlibApi& x = x11::xlib();
        ::XEvent reply{};
        reply.type = SelectionNotify;
        reply.xselection.display = display_;
        reply.xselection.requestor = request.requestor;
        reply.xselection.selection = request.selection;
        reply.xselection.target = request.target;
        reply.xselection.time = request.time;
        reply.xselection.property = 0; // refused, unless the branches below say otherwise

        if (request.target == xdnd_.targets) {
          // "What formats do you have?" -- answered with a list of atoms.
          const ::Atom offered[2] = {xdnd_.targets, xdnd_.utf8String};
          x.XChangeProperty(display_, request.requestor, request.property, 4 /*XA_ATOM*/, 32,
                            0 /*PropModeReplace*/, (const unsigned char*) offered, 2);
          reply.xselection.property = request.property;
        } else if (request.target == xdnd_.utf8String && !clipboardText_.empty()) {
          x.XChangeProperty(display_, request.requestor, request.property, xdnd_.utf8String, 8,
                            0 /*PropModeReplace*/,
                            (const unsigned char*) clipboardText_.data(),
                            (int) clipboardText_.size());
          reply.xselection.property = request.property;
        }
        x.XSendEvent(display_, request.requestor, 0, 0, &reply);
        x.XFlush(display_);
        return;
      }

      case SelectionNotify:
        // Both the drop and the paste arrive here, and they are told apart by
        // which PROPERTY the reply names. Handling either as the other reads a
        // file list as clipboard text or the reverse.
        if (event.xselection.property == xdnd_.clipboardProperty) {
          readClipboardReply();
          return;
        }
        // The file list we asked for has arrived. XDND transfers the data
        // through a SELECTION, not in the message -- a path list can be longer
        // than a client message's twenty bytes.
        readDroppedFiles();
        return;

      case LeaveNotify:
        // Not while a button is down: X sends a Leave when the implicit grab
        // starts, and taking it at face value ends the hover on every press.
        if ((event.xcrossing.state & Button1Mask) == 0) {
          router_->mouseExitWindow();
          renderIfDirty();
        }
        return;

      default:
        return;
    }
  }

  ::Display* display_ = nullptr;
  ::Window window_ = 0;
  ::Visual* visual_ = nullptr;
  ::GC gc_ = nullptr;
  ::XImage* image_ = nullptr;
  int depth_ = 0;

  Component* content_ = nullptr;
  MouseRouter* router_ = nullptr;
  Backing backing_;
  /** What the caller asked for, kept so a scale change can re-derive the device
   *  size without drifting by a rounding each time. */
  float logicalWidth_ = 0.0f, logicalHeight_ = 0.0f;
  float openScale_ = 1.0f;
  std::vector<uint8_t> scratch_;

  bool isPopup_ = false;
  bool grabsMouse_ = true;
  MouseCursor currentCursor_ = MouseCursor::Default;
  std::string clipboardText_;   // what we serve, while we own the selection
  std::string clipboardResult_; // what a paste got back
  bool clipboardPending_ = false;
  Point lastMouse_;
  XdndAtoms xdnd_;
  ::Window dragSource_ = 0;
  ::Time dropTime_ = 0;
  Point dragPoint_;
  bool dragHasUriList_ = false;
  ::Time lastClickTime_ = 0;
  int lastClickX_ = 0, lastClickY_ = 0;
};

} // namespace gfx
} // namespace sonore

// -- Putting X11's macros away ---------------------------------------------
//
// Xlib is from 1985 and defines single-word macros that a C++ code base cannot
// help colliding with: None, Status, Bool, Success, Always, Complex. This SDK
// already has an EditorBackend::None, and `#define None 0L` turns it into
// `EditorBackend::0L` -- an error the compiler reports inside X.h, hundreds of
// lines away from anything that mentions it.
//
// Undefined here, at the bottom, after this file has finished using them. Every
// inline body above was already parsed, so the macros did their job; everything
// that includes this header is then compiling ordinary C++.
//
// Not moved into a wrapper .cpp instead, because this SDK is header-only and a
// plugin that had to link one would no longer be a single translation unit.
// None is already gone -- it had to be undefined at the TOP, before
// component.h declares a KeyPress with a None of its own. Listed here only so
// this block still reads as the full set of what X11 defined.
#undef Status
#undef Bool
#undef Success
#undef Always
#undef Complex

#endif // SONORE_X11_HEADERS
