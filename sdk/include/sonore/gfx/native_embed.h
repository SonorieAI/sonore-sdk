// SPDX-License-Identifier: Apache-2.0
//
// A platform control inside a component tree: HWNDComponent, NSViewComponent,
// XEmbedComponent and ActiveXControlComponent.
//
// ── The other direction from the peers ─────────────────────────────────────
//
// The window_*.h peers put a component tree inside somebody else's window.
// These put somebody else's window inside a component tree: a component that
// owns a rectangle and keeps a native child window -- a foreign HWND, an
// NSView, an X client, an ActiveX control -- in that rectangle, shown while
// the component is showing.
//
// ── Following the component ────────────────────────────────────────────────
//
// The tree has no "moved" notification (setBounds is not virtual and an
// ancestor's move tells nobody), so the native window is brought into line
// at three moments: when the component is resized, when it is painted (which
// a move always causes, at the old place and the new), and on every tick of
// the TopLevelWindow it is in -- which is what catches an ancestor hidden,
// since a hidden subtree is not painted. Each check costs a comparison; the
// native call is made only for a change.
//
// The host window is the peer's own window inside the TopLevelWindow the
// component is in (TopLevelWindow::getContentHandle). Anywhere else -- a
// plugin editor -- setParentWindow() names it (NativeEditor::handle()), and
// the caller ticks updateBounds() from its own clock.
//
// ── What is here, and what is not ──────────────────────────────────────────
//
//   HWNDComponent            Windows. RUN: parented, moved, sized, hidden and
//                            released, read back from the window manager.
//   ActiveXControlComponent  Windows: a control by CLSID, in-place activated
//                            in a site window through IOleObject, with this
//                            file's IOleClientSite/IOleInPlaceSite/
//                            IOleInPlaceFrame. RUN with the WebBrowser
//                            control, and the failure path with a CLSID that
//                            names nothing.
//   NSViewComponent          macOS: an NSView added to the peer's (flipped)
//                            view. Compiled only.
//   XEmbedComponent          X11: the XEmbed protocol, as embedder -- a
//                            client window reparented into a socket window,
//                            told XEMBED_EMBEDDED_NOTIFY, kept to the socket's
//                            size, given focus and activation messages; or a
//                            socket whose id another process embeds into.
//                            Compiled only.
//   UIViewComponent and AndroidViewComponent are not here: this SDK has no
//   iOS or Android target to put them in.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "component.h"
#include "document_window.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>
#include <oleidl.h>
#include <cwchar>
#if defined(_MSC_VER)
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "uuid.lib") // IID_IOleObject and the other OLE interface ids
#endif
#endif

namespace sonore {
namespace gfx {

namespace detail {
namespace embed {

/** A component holding a native window, kept in line by the tick hook. */
class Embedded {
public:
  virtual ~Embedded() { unlist(); }
  virtual void updateBounds() = 0;
  virtual Component& asComponent() = 0;
  /** `host` -- a TopLevelWindow's peer window -- is about to be destroyed:
   *  take the native window out of it if it is in it. Hidden and parked, not
   *  released: the next updateBounds puts it into whatever window the
   *  component is in by then, a remade frame's included. */
  virtual void detachFromHost(void* host) = 0;

  static std::vector<Embedded*>& all() {
    static std::vector<Embedded*> live;
    return live;
  }

  /** The tick hook: every embedded component in this window's tree. */
  static void syncTree(Component& root) {
    const std::vector<Embedded*> copy = all();
    for (Embedded* e : copy) {
      if (std::find(all().begin(), all().end(), e) == all().end()) continue;
      if (e->asComponent().root() == &root) e->updateBounds();
    }
  }

  /** The detach hook: every embedded component, whatever tree it is in now
   *  -- one taken out of the window's tree still has its native window in
   *  the window, and would go with it. */
  static void detachFrom(void* host) {
    const std::vector<Embedded*> copy = all();
    for (Embedded* e : copy)
      if (std::find(all().begin(), all().end(), e) != all().end()) e->detachFromHost(host);
  }

protected:
  void list() {
    detail::tlw::addTickHook(&Embedded::syncTree);
    detail::tlw::addDetachHook(&Embedded::detachFrom);
    all().push_back(this);
  }
  void unlist() { all().erase(std::remove(all().begin(), all().end(), this), all().end()); }
};

/** Where `c` is now, in device pixels of its window, and whether it shows.
 *  The scale is the host peer's: the size the native window must have. */
struct Placement {
  int x = 0, y = 0, w = 0, h = 0;
  bool visible = false;
  bool operator==(const Placement& o) const {
    return x == o.x && y == o.y && w == o.w && h == o.h && visible == o.visible;
  }
  bool operator!=(const Placement& o) const { return !(*this == o); }
};

inline Placement placementOf(Component& c, float scale) {
  Placement p;
  const Point origin = c.localToRoot(Point(0.0f, 0.0f));
  p.x = (int) std::lround(origin.x * scale);
  p.y = (int) std::lround(origin.y * scale);
  p.w = (std::max)(0, (int) std::lround(c.bounds().w * scale));
  p.h = (std::max)(0, (int) std::lround(c.bounds().h * scale));
  p.visible = c.isShowing() && p.w > 0 && p.h > 0;
  return p;
}

/** The window a component's native child goes into, and its scale: the
 *  explicit one if given, else the TopLevelWindow's peer. */
inline void* hostWindowFor(Component& c, void* explicitParent, float* scale) {
  *scale = 1.0f;
  if (explicitParent) {
#if defined(_WIN32)
    *scale = detail::tlw::frameDpiScale((HWND) explicitParent);
#endif
    return explicitParent;
  }
  TopLevelWindow* w = TopLevelWindow::findTopLevelWindowFor(&c);
  if (!w || !w->isOnDesktop()) return nullptr;
  void* handle = w->getContentHandle();
  if (handle) *scale = w->getPeer().scale();
#if defined(__APPLE__)
  *scale = 1.0f; // NSView frames are in points
#endif
  return handle;
}

} // namespace embed
} // namespace detail

#if defined(_WIN32)

/**
 * A foreign HWND inside the tree. It is not owned: the caller made it and
 * destroys it. While set it is made a child (WS_CHILD) of the host window
 * and kept at this component's place; on release it is hidden and handed
 * back to the desktop with its old style.
 */
class HWNDComponent : public Component, private detail::embed::Embedded {
public:
  /** Holds no HWND until setHWND(). */
  HWNDComponent() { list(); }
  /** Hands the HWND back as setHWND(nullptr) does; it does not destroy it. */
  ~HWNDComponent() override {
    unlist();
    release();
  }

  /**
   * Put `hwnd` (an HWND) in this component's place: its style and parent
   * are noted, and it is made a child of the host window and moved there
   * now -- or hidden, while this is in no window. The HWND held before, if
   * any, is released first: hidden, and given back its old parent and
   * style. Null releases only. Not owned.
   */
  void setHWND(void* hwnd) {
    if ((HWND) hwnd == hwnd_) return;
    release();
    hwnd_ = (HWND) hwnd;
    if (hwnd_) {
      originalStyle_ = GetWindowLongPtrW(hwnd_, GWL_STYLE);
      originalParent_ = GetParent(hwnd_);
    }
    last_ = detail::embed::Placement();
    lastParent_ = nullptr;
    updateBounds();
  }
  /** The HWND setHWND() gave, or null. */
  void* getHWND() const { return hwnd_; }

  /** Where the HWND goes when this is not in a TopLevelWindow: a plugin
   *  editor's window. Null returns to finding it. */
  void setParentWindow(void* parent) {
    explicitParent_ = parent;
    updateBounds();
  }

  /** Make this component the HWND's current size. */
  void resizeToFit() {
    if (!hwnd_) return;
    RECT r{};
    GetWindowRect(hwnd_, &r);
    float scale = 1.0f;
    detail::embed::hostWindowFor(*this, explicitParent_, &scale);
    setSize((float) (r.right - r.left) / scale, (float) (r.bottom - r.top) / scale);
  }

  /** Bring the HWND into line now. */
  void updateBounds() override {
    if (!hwnd_ || !IsWindow(hwnd_)) return;
    float scale = 1.0f;
    HWND parent = (HWND) detail::embed::hostWindowFor(*this, explicitParent_, &scale);
    detail::embed::Placement p = detail::embed::placementOf(*this, scale);
    if (!parent) p.visible = false;
    if (parent && parent != lastParent_) {
      // A child, not a popup: it moves with the window and is clipped to it.
      const LONG_PTR style = (originalStyle_ & ~(LONG_PTR) WS_POPUP) | WS_CHILD | WS_CLIPSIBLINGS;
      SetWindowLongPtrW(hwnd_, GWL_STYLE, style);
      SetParent(hwnd_, parent);
      lastParent_ = parent;
      last_ = detail::embed::Placement();
      last_.visible = !p.visible; // force the first placement
    }
    if (p == last_) return;
    last_ = p;
    SetWindowPos(hwnd_, HWND_TOP, p.x, p.y, (std::max)(1, p.w), (std::max)(1, p.h),
                 SWP_NOACTIVATE | SWP_FRAMECHANGED | (p.visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  }

  /** Brings the HWND into line with the new size. */
  void resized() override { updateBounds(); }
  /** Draws nothing (the HWND covers the rectangle); brings the HWND into
   *  line, since a move repaints the component. */
  void paint(Graphics&) override { updateBounds(); }

private:
  Component& asComponent() override { return *this; }

  /** Handed back as release() hands it back, but still held: the frame is
   *  being remade, or the window closed with this component still alive. */
  void detachFromHost(void* host) override {
    if (!hwnd_ || !lastParent_ || (void*) lastParent_ != host) return;
    unparent();
  }

  void unparent() {
    if (IsWindow(hwnd_)) {
      ShowWindow(hwnd_, SW_HIDE);
      SetParent(hwnd_, originalParent_);
      SetWindowLongPtrW(hwnd_, GWL_STYLE, originalStyle_ & ~(LONG_PTR) WS_VISIBLE);
    }
    lastParent_ = nullptr;
    last_ = detail::embed::Placement();
  }

  void release() {
    if (!hwnd_) return;
    unparent();
    hwnd_ = nullptr;
  }

  HWND hwnd_ = nullptr;
  HWND originalParent_ = nullptr;
  LONG_PTR originalStyle_ = 0;
  HWND lastParent_ = nullptr;
  void* explicitParent_ = nullptr;
  detail::embed::Placement last_;
};

namespace detail {
namespace activex {

/**
 * The container side of OLE in-place activation, as little of it as a
 * control needs to show itself in a window: a client site, an in-place site
 * and an in-place frame, all one object, all answering for one site window.
 */
class Site : public IOleClientSite, public IOleInPlaceSite, public IOleInPlaceFrame {
public:
  explicit Site(HWND window) : window_(window) {}

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IOleClientSite))
      *out = static_cast<IOleClientSite*>(this);
    else if (IsEqualIID(riid, IID_IOleWindow) || IsEqualIID(riid, IID_IOleInPlaceSite))
      *out = static_cast<IOleInPlaceSite*>(this);
    else if (IsEqualIID(riid, IID_IOleInPlaceFrame) || IsEqualIID(riid, IID_IOleInPlaceUIWindow))
      *out = static_cast<IOleInPlaceFrame*>(this);
    if (!*out) return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG) ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const long left = --refs_;
    if (left == 0) delete this;
    return (ULONG) left;
  }

  // IOleClientSite
  HRESULT STDMETHODCALLTYPE SaveObject() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetMoniker(DWORD, DWORD, IMoniker** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetContainer(IOleContainer** out) override {
    if (out) *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE ShowObject() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnShowWindow(BOOL) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE RequestNewObjectLayout() override { return E_NOTIMPL; }

  // IOleWindow (both in-place interfaces)
  HRESULT STDMETHODCALLTYPE GetWindow(HWND* out) override {
    if (!out) return E_POINTER;
    *out = window_;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }

  // IOleInPlaceSite
  HRESULT STDMETHODCALLTYPE CanInPlaceActivate() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnInPlaceActivate() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnUIActivate() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetWindowContext(IOleInPlaceFrame** frame, IOleInPlaceUIWindow** doc,
                                             LPRECT position, LPRECT clip,
                                             LPOLEINPLACEFRAMEINFO info) override {
    if (frame) {
      *frame = static_cast<IOleInPlaceFrame*>(this);
      AddRef();
    }
    if (doc) *doc = nullptr;
    RECT client{};
    GetClientRect(window_, &client);
    if (position) *position = client;
    if (clip) *clip = client;
    if (info) {
      info->fMDIApp = FALSE;
      info->hwndFrame = window_;
      info->haccel = nullptr;
      info->cAccelEntries = 0;
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Scroll(SIZE) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE OnUIDeactivate(BOOL) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnInPlaceDeactivate() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE DiscardUndoState() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DeactivateAndUndo() override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE OnPosRectChange(LPCRECT) override { return S_OK; }

  // IOleInPlaceUIWindow / IOleInPlaceFrame
  HRESULT STDMETHODCALLTYPE GetBorder(LPRECT) override { return INPLACE_E_NOTOOLSPACE; }
  HRESULT STDMETHODCALLTYPE RequestBorderSpace(LPCBORDERWIDTHS) override {
    return INPLACE_E_NOTOOLSPACE;
  }
  HRESULT STDMETHODCALLTYPE SetBorderSpace(LPCBORDERWIDTHS) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetActiveObject(IOleInPlaceActiveObject*, LPCOLESTR) override {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE InsertMenus(HMENU, LPOLEMENUGROUPWIDTHS) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetMenu(HMENU, HOLEMENU, HWND) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE RemoveMenus(HMENU) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetStatusText(LPCOLESTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE EnableModeless(BOOL) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE TranslateAccelerator(LPMSG, WORD) override { return S_FALSE; }

private:
  virtual ~Site() = default;
  HWND window_;
  long refs_ = 1;
};

inline HMODULE module() {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR) &module, &m);
  return m;
}

/** The site window's class: a plain child that keeps its children clipped
 *  and, with mouse input refused, swallows the clicks before they reach the
 *  control (a disabled window passes none to its children). */
inline const wchar_t* siteClass() {
  static std::wstring name;
  static ATOM atom = 0;
  if (atom) return name.c_str();
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &DefWindowProcW;
  wc.hInstance = module();
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  for (int attempt = 0; attempt < 64 && !atom; ++attempt) {
    wchar_t buffer[96];
    std::swprintf(buffer, 96, L"SonoreActiveXSite_%p_%d", (void*) module(), attempt);
    name = buffer;
    wc.lpszClassName = name.c_str();
    atom = RegisterClassExW(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) break;
  }
  return atom ? name.c_str() : nullptr;
}

} // namespace activex
} // namespace detail

/**
 * An ActiveX control by CLSID, in-place active in this component's
 * rectangle. COM must be (or become) single-threaded on this thread:
 * createControl calls OleInitialize, and fails on a thread already in the
 * multi-threaded apartment.
 */
class ActiveXControlComponent : public Component, private detail::embed::Embedded {
public:
  /** No control until createControl(). */
  ActiveXControlComponent() { list(); }
  /** Closes and releases the control, as deleteControl() does. */
  ~ActiveXControlComponent() override {
    unlist();
    deleteControl();
  }

  /** `controlIID` points at the control's CLSID. True when it was made and
   *  activated; a CLSID that names nothing, or a control that refuses to be
   *  in-place active, leaves nothing behind. Needs to be in a window. */
  bool createControl(const void* controlIID) {
    deleteControl();
    if (!controlIID) return false;
    float scale = 1.0f;
    HWND host = (HWND) detail::embed::hostWindowFor(*this, explicitParent_, &scale);
    if (!host) return false;
    const HRESULT ole = OleInitialize(nullptr);
    if (FAILED(ole)) return false; // RPC_E_CHANGED_MODE: an MTA thread
    oleInitialised_ = true;
    const detail::embed::Placement p = detail::embed::placementOf(*this, scale);
    const wchar_t* cls = detail::activex::siteClass();
    site_ = cls ? CreateWindowExW(0, cls, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, p.x,
                                  p.y, (std::max)(1, p.w), (std::max)(1, p.h), host, nullptr,
                                  detail::activex::module(), nullptr)
                : nullptr;
    if (!site_) {
      deleteControl();
      return false;
    }
    siteObject_ = new detail::activex::Site(site_);
    IOleObject* object = nullptr;
    if (FAILED(CoCreateInstance(*static_cast<const CLSID*>(controlIID), nullptr,
                                CLSCTX_INPROC_SERVER, IID_IOleObject, (void**) &object)) ||
        !object) {
      deleteControl();
      return false;
    }
    object_ = object;
    object_->SetClientSite(static_cast<IOleClientSite*>(siteObject_));
    OleSetContainedObject(object_, TRUE);
    object_->SetHostNames(L"Sonore", nullptr);
    RECT rect{0, 0, (std::max)(1, p.w), (std::max)(1, p.h)};
    if (FAILED(object_->DoVerb(OLEIVERB_INPLACEACTIVATE, nullptr,
                               static_cast<IOleClientSite*>(siteObject_), 0, site_, &rect))) {
      deleteControl();
      return false;
    }
    host_ = host;
    last_ = detail::embed::Placement();
    last_.visible = !p.visible;
    EnableWindow(site_, mouseAllowed_ ? TRUE : FALSE);
    updateBounds();
    return true;
  }

  /** Close and release the control, and its site. */
  void deleteControl() {
    if (object_) {
      IOleInPlaceObject* inPlace = nullptr;
      if (SUCCEEDED(object_->QueryInterface(IID_IOleInPlaceObject, (void**) &inPlace)) && inPlace) {
        inPlace->InPlaceDeactivate();
        inPlace->Release();
      }
      object_->Close(OLECLOSE_NOSAVE);
      object_->SetClientSite(nullptr);
      object_->Release();
      object_ = nullptr;
    }
    if (siteObject_) {
      siteObject_->Release();
      siteObject_ = nullptr;
    }
    if (site_) {
      DestroyWindow(site_);
      site_ = nullptr;
    }
    host_ = nullptr;
    if (oleInitialised_) {
      OleUninitialize();
      oleInitialised_ = false;
    }
  }

  /** createControl() succeeded and deleteControl() has not been called. */
  bool isControlOpen() const { return object_ != nullptr; }

  /** One of the control's interfaces, by IID, AddRef'd: the caller releases
   *  it. Null when the control has no such interface or there is none. */
  void* queryInterface(const void* iid) const {
    if (!object_ || !iid) return nullptr;
    void* out = nullptr;
    if (FAILED(object_->QueryInterface(*static_cast<const IID*>(iid), &out))) return nullptr;
    return out;
  }

  /** False makes the control deaf to the mouse (the site is disabled). */
  void setMouseEventsAllowed(bool allowed) {
    mouseAllowed_ = allowed;
    if (site_) EnableWindow(site_, allowed ? TRUE : FALSE);
  }
  /** True (the default) unless setMouseEventsAllowed(false). */
  bool areMouseEventsAllowed() const { return mouseAllowed_; }

  /** The site window the control lives in, for a test. */
  void* getSiteWindow() const { return site_; }

  /** Where the control goes when this is not in a TopLevelWindow: a plugin
   *  editor's window (an HWND). Null returns to finding it. Set it before
   *  createControl(), which fails with no window. Unlike HWNDComponent's,
   *  it moves nothing itself: updateBounds() does. */
  void setParentWindow(void* parent) { explicitParent_ = parent; }

  /** Bring the control's site window into line now: this component's place
   *  and size in the host window's device pixels, shown while it shows, and
   *  the control told its new rectangle. Nothing without a control. */
  void updateBounds() override {
    if (!site_) return;
    float scale = 1.0f;
    HWND host = (HWND) detail::embed::hostWindowFor(*this, explicitParent_, &scale);
    detail::embed::Placement p = detail::embed::placementOf(*this, scale);
    if (host && !host_) {
      // Parked by detachFromHost while its window was remade: into the
      // window it is in now.
      SetParent(site_, host);
      host_ = host;
      last_ = detail::embed::Placement();
      last_.visible = !p.visible;
    }
    if (host != host_) p.visible = false; // taken out of its window
    if (p == last_) return;
    last_ = p;
    SetWindowPos(site_, HWND_TOP, p.x, p.y, (std::max)(1, p.w), (std::max)(1, p.h),
                 SWP_NOACTIVATE | (p.visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    IOleInPlaceObject* inPlace = nullptr;
    if (object_ && SUCCEEDED(object_->QueryInterface(IID_IOleInPlaceObject, (void**) &inPlace)) &&
        inPlace) {
      RECT rect{0, 0, (std::max)(1, p.w), (std::max)(1, p.h)};
      inPlace->SetObjectRects(&rect, &rect);
      inPlace->Release();
    }
  }

  /** Brings the control into line with the new size. */
  void resized() override { updateBounds(); }
  /** Draws nothing (the control covers the rectangle); brings the control
   *  into line, since a move repaints the component. */
  void paint(Graphics&) override { updateBounds(); }

private:
  Component& asComponent() override { return *this; }

  /** The site, control and all, parked as a message-only window -- alive and
   *  in-place active, shown nowhere -- until updateBounds finds it a window. */
  void detachFromHost(void* host) override {
    if (!site_ || !host_ || (void*) host_ != host) return;
    ShowWindow(site_, SW_HIDE);
    SetParent(site_, HWND_MESSAGE);
    host_ = nullptr;
    last_ = detail::embed::Placement();
  }

  IOleObject* object_ = nullptr;
  detail::activex::Site* siteObject_ = nullptr;
  HWND site_ = nullptr;
  HWND host_ = nullptr;
  void* explicitParent_ = nullptr;
  bool oleInitialised_ = false;
  bool mouseAllowed_ = true;
  detail::embed::Placement last_;
};

#elif defined(__APPLE__)

/**
 * An NSView inside the tree, added to the peer's view (which is flipped, so
 * a frame's origin is its top-left as in the tree). Retained while set.
 * NSViewComponentWithParent -- the reverse, a tree inside a host's view -- is
 * a different class elsewhere.
 */
class NSViewComponent : public Component, private detail::embed::Embedded {
public:
  NSViewComponent() { list(); }
  ~NSViewComponent() override {
    unlist();
    setView(nullptr);
  }

  void setView(void* nsView) {
    if ((id) nsView == view_) return;
    if (view_) {
      cocoa::msg<void>(view_, sel_registerName("removeFromSuperview"));
      cocoa::msg<void>(view_, sel_registerName("release"));
    }
    view_ = (id) nsView;
    parent_ = nullptr;
    last_ = detail::embed::Placement();
    if (view_) {
      cocoa::msg<id>(view_, sel_registerName("retain"));
      updateBounds();
    }
  }
  void* getView() const { return (void*) view_; }

  void setParentView(void* parent) {
    explicitParent_ = parent;
    updateBounds();
  }

  /** This component to the view's frame size. */
  void resizeToFitView() {
    if (!view_) return;
    const cocoa::CGRect2 frame = cocoa::msgRect<cocoa::CGRect2>(view_, sel_registerName("frame"));
    setSize((float) frame.size.width, (float) frame.size.height);
  }

  void updateBounds() override {
    if (!view_) return;
    float scale = 1.0f;
    id host = (id) detail::embed::hostWindowFor(*this, explicitParent_, &scale);
    detail::embed::Placement p = detail::embed::placementOf(*this, 1.0f);
    if (!host) p.visible = false;
    if (host && host != parent_) {
      cocoa::msg<void>(view_, sel_registerName("removeFromSuperview"));
      cocoa::msg<void>(host, sel_registerName("addSubview:"), view_);
      parent_ = host;
      last_ = detail::embed::Placement();
      last_.visible = !p.visible;
    }
    if (p == last_) return;
    last_ = p;
    const cocoa::CGRect2 frame{{(double) p.x, (double) p.y}, {(double) p.w, (double) p.h}};
    using SetFrame = void (*)(id, SEL, cocoa::CGRect2);
    reinterpret_cast<SetFrame>(objc_msgSend)(view_, sel_registerName("setFrame:"), frame);
    cocoa::msg<void>(view_, sel_registerName("setHidden:"), (BOOL) (p.visible ? NO : YES));
  }

  void resized() override { updateBounds(); }
  void paint(Graphics&) override { updateBounds(); }

private:
  Component& asComponent() override { return *this; }

  /** Out of a peer view that is going. The view survives it (it is
   *  retained), but `parent_` would be left naming a freed object -- and a
   *  new peer view allocated at the same address would read as "already
   *  there" and never be given the view. */
  void detachFromHost(void* host) override {
    if (!view_ || !parent_ || (void*) parent_ != host) return;
    cocoa::msg<void>(view_, sel_registerName("removeFromSuperview"));
    parent_ = nullptr;
    last_ = detail::embed::Placement();
  }

  id view_ = nullptr;
  id parent_ = nullptr;
  void* explicitParent_ = nullptr;
  detail::embed::Placement last_;
};

#elif defined(SONORE_X11_HEADERS)

/**
 * The XEmbed protocol, embedder side.
 *
 * A SOCKET window is made as a child of the peer's window, on a connection
 * of this component's own, and kept at the component's place. A client is
 * either handed over by id (the constructor taking one) and reparented into
 * the socket, or arrives by reparenting itself into getHostWindowID() -- the
 * id a socket gives another process. Either way it is told
 * XEMBED_EMBEDDED_NOTIFY, mapped when its _XEMBED_INFO says so, kept to the
 * socket's size, and told of activation and focus.
 */
class XEmbedComponent : public Component, private detail::embed::Embedded {
public:
  explicit XEmbedComponent(bool wantsKeyboardFocus = true,
                           bool allowForeignWidgetToResizeComponent = false)
      : allowResize_(allowForeignWidgetToResizeComponent) {
    setWantsKeyboardFocus(wantsKeyboardFocus);
    list();
  }

  XEmbedComponent(unsigned long clientWindow, bool wantsKeyboardFocus = true,
                  bool allowForeignWidgetToResizeComponent = false)
      : XEmbedComponent(wantsKeyboardFocus, allowForeignWidgetToResizeComponent) {
    pendingClient_ = clientWindow;
  }

  ~XEmbedComponent() override {
    unlist();
    removeClient();
    closeSocket();
  }

  /** The socket's window id, for another process to embed into. Makes the
   *  socket if this component is in a window. */
  unsigned long getHostWindowID() {
    updateBounds();
    return (unsigned long) socket_;
  }

  /** Let the client go: unmapped and given back to the root window. */
  void removeClient() {
    if (!display_ || !client_) return;
    const x11::XlibApi& x = x11::xlib();
    x.XUnmapWindow(display_, client_);
    detail::tlw::x11Frame().XReparentWindow(display_, client_, root_, 0, 0);
    x.XFlush(display_);
    client_ = 0;
  }

  void updateBounds() override {
    float scale = 1.0f;
    void* host = detail::embed::hostWindowFor(*this, nullptr, &scale);
    detail::embed::Placement p = detail::embed::placementOf(*this, scale);
    if (!host) p.visible = false;
    if (host && (::Window) (uintptr_t) host != hostWindow_) {
      closeSocket();
      openSocket((::Window) (uintptr_t) host);
      last_ = detail::embed::Placement();
      last_.visible = !p.visible;
    }
    if (!display_ || p == last_) return;
    last_ = p;
    const x11::XlibApi& x = x11::xlib();
    detail::tlw::x11Frame().XMoveResizeWindow(display_, socket_, p.x, p.y,
                                              (unsigned) (std::max)(1, p.w),
                                              (unsigned) (std::max)(1, p.h));
    if (p.visible) x.XMapWindow(display_, socket_);
    else x.XUnmapWindow(display_, socket_);
    if (client_)
      x.XResizeWindow(display_, client_, (unsigned) (std::max)(1, p.w), (unsigned) (std::max)(1, p.h));
    x.XFlush(display_);
  }

  void resized() override { updateBounds(); }
  void paint(Graphics&) override { updateBounds(); }

  void focusGained() override { sendXEmbed(kFocusIn, kFocusCurrent, 0, 0); }
  void focusLost() override { sendXEmbed(kFocusOut, 0, 0, 0); }

private:
  // The protocol's message numbers (XEmbed specification 0.5).
  static constexpr long kEmbeddedNotify = 0, kWindowActivate = 1, kRequestFocus = 3,
                        kFocusIn = 4, kFocusOut = 5, kFocusCurrent = 0;
  static constexpr long kMapped = 1; // _XEMBED_INFO flags

  Component& asComponent() override { return *this; }

  /** The socket is a child of the peer's window, and X destroys a window's
   *  inferiors with it -- the client's window included. So the client is
   *  handed back to the root first, and embedded again when the next socket
   *  opens. */
  void detachFromHost(void* host) override {
    if (!display_ || (::Window) (uintptr_t) host != hostWindow_) return;
    if (client_) {
      const ::Window client = client_;
      removeClient();
      pendingClient_ = (unsigned long) client;
    }
    closeSocket();
    last_ = detail::embed::Placement();
  }

  void openSocket(::Window host) {
    const x11::XlibApi& x = x11::xlib();
    if (!x.ok || !detail::tlw::x11Frame().ok) return;
    display_ = x.XOpenDisplay(nullptr);
    if (!display_) return;
    const int screen = x.XDefaultScreen(display_);
    root_ = RootWindow(display_, screen);
    ::XSetWindowAttributes attrs{};
    // SubstructureNotify: a client reparenting itself in, mapping, resizing.
    attrs.event_mask = SubstructureNotifyMask | StructureNotifyMask;
    socket_ = x.XCreateWindow(display_, host, 0, 0, 1, 1, 0, 0 /*CopyFromParent*/,
                              1 /*InputOutput*/, nullptr, CWEventMask, &attrs);
    if (!socket_) {
      x.XCloseDisplay(display_);
      display_ = nullptr;
      return;
    }
    hostWindow_ = host;
    xembed_ = x.XInternAtom(display_, "_XEMBED", 0);
    xembedInfo_ = x.XInternAtom(display_, "_XEMBED_INFO", 0);
    x.XFlush(display_);
#if defined(__linux__)
    if (MessageManager* mm = MessageManager::getInstanceWithoutCreating()) {
      fd_ = x.XConnectionNumber(display_);
      XEmbedComponent* self = this;
      mm->registerFdCallback(fd_, [self](int) { self->pump(); });
    }
#endif
    if (pendingClient_) {
      const ::Window client = (::Window) pendingClient_;
      pendingClient_ = 0;
      embed(client);
    }
  }

  void closeSocket() {
#if defined(__linux__)
    if (fd_ >= 0)
      if (MessageManager* mm = MessageManager::getInstanceWithoutCreating()) mm->unregisterFdCallback(fd_);
#endif
    fd_ = -1;
    if (!display_) return;
    const x11::XlibApi& x = x11::xlib();
    if (socket_) x.XDestroyWindow(display_, socket_);
    socket_ = 0;
    x.XCloseDisplay(display_);
    display_ = nullptr;
    hostWindow_ = 0;
  }

  void embed(::Window client) {
    if (!display_ || !client) return;
    client_ = client;
    const x11::XlibApi& x = x11::xlib();
    x.XSelectInput(display_, client_, PropertyChangeMask | StructureNotifyMask);
    detail::tlw::x11Frame().XReparentWindow(display_, client_, socket_, 0, 0);
    // _XEMBED_INFO: version, flags. Mapped only if the client says so.
    long flags = kMapped;
    ::Atom type = 0;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    if (x.XGetWindowProperty(display_, client_, xembedInfo_, 0, 2, 0, xembedInfo_, &type, &format,
                             &count, &after, &data) == 0 &&
        data) {
      if (count >= 2 && format == 32) flags = reinterpret_cast<const long*>(data)[1];
      x.XFree(data);
    }
    // Version 0 of the protocol is what this embedder speaks.
    sendXEmbed(kEmbeddedNotify, 0, (long) socket_, 0);
    sendXEmbed(kWindowActivate, 0, 0, 0);
    const Rect b = bounds();
    if (!allowResize_ || b.w <= 0.0f)
      x.XResizeWindow(display_, client_, (unsigned) (std::max)(1, last_.w), (unsigned) (std::max)(1, last_.h));
    if (flags & kMapped) x.XMapWindow(display_, client_);
    x.XFlush(display_);
  }

  void sendXEmbed(long message, long messageDetail, long data1, long data2) {
    if (!display_ || !client_) return;
    ::XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = client_;
    event.xclient.message_type = xembed_;
    event.xclient.format = 32;
    event.xclient.data.l[0] = (long) CurrentTime;
    event.xclient.data.l[1] = message;
    event.xclient.data.l[2] = messageDetail;
    event.xclient.data.l[3] = data1;
    event.xclient.data.l[4] = data2;
    x11::xlib().XSendEvent(display_, client_, 0, 0 /*NoEventMask*/, &event);
    x11::xlib().XFlush(display_);
  }

  void pump() {
    if (!display_) return;
    const x11::XlibApi& x = x11::xlib();
    while (display_ && x.XPending(display_) > 0) {
      ::XEvent event;
      x.XNextEvent(display_, &event);
      switch (event.type) {
        case ReparentNotify:
          // A client that put itself into our socket (the plug side).
          if (event.xreparent.parent == socket_ && event.xreparent.window != client_)
            embed(event.xreparent.window);
          else if (event.xreparent.window == client_ && event.xreparent.parent != socket_)
            client_ = 0;
          break;
        case ConfigureNotify:
          if (allowResize_ && client_ && event.xconfigure.window == client_ && last_.w > 0) {
            const float s = last_.w > 0 && bounds().w > 0.0f ? (float) last_.w / bounds().w : 1.0f;
            setSize((float) event.xconfigure.width / s, (float) event.xconfigure.height / s);
          }
          break;
        case DestroyNotify:
          if (event.xdestroywindow.window == client_) client_ = 0;
          break;
        case ClientMessage:
          if (event.xclient.message_type == xembed_ && event.xclient.data.l[1] == kRequestFocus)
            if (MouseRouter* router = routerFor(this)) router->setFocus(this);
          break;
        default:
          break;
      }
    }
  }

  ::Display* display_ = nullptr;
  ::Window root_ = 0, socket_ = 0, client_ = 0, hostWindow_ = 0;
  ::Atom xembed_ = 0, xembedInfo_ = 0;
  unsigned long pendingClient_ = 0;
  int fd_ = -1;
  bool allowResize_ = false;
  detail::embed::Placement last_;
};

#endif

} // namespace gfx
} // namespace sonore
