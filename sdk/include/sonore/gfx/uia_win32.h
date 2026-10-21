// SPDX-License-Identifier: Apache-2.0
//
// The Windows end of accessibility: handing our component tree to UI Automation.
//
// ── What this is for ────────────────────────────────────────────────────────
//
// accessible_info.h gives every component a role, a name, the value it reads
// and a range. That is what a screen reader would want; this is what actually
// hands it over. Without this file the description layer is a well-tested
// object nobody outside the process can see, and NVDA, JAWS and Narrator all
// say nothing about a plugin built with this SDK.
//
// ── Why it is written by hand ───────────────────────────────────────────────
//
// UIA is COM, and the usual way to write a provider is ATL. ATL is a Visual
// C++ library: depending on it would make this header MSVC-only inside a file
// that is already Windows-only, and would put a second toolchain requirement
// on anyone building a generated plugin. IUnknown is three methods. They are
// written out below and that is the whole of the cost.
//
// ── The shape ───────────────────────────────────────────────────────────────
//
// Windows asks a window for its provider with WM_GETOBJECT, and everything
// else follows from the object returned:
//
//   RootProvider      -- the window itself, and the fragment root
//   ElementProvider   -- one per accessible component
//
// Both read a SNAPSHOT of collectAccessible, and only the window's own thread
// takes it: when UIA calls the root there, and on the editor's 33 ms clock
// while any client is listening. UIA calls in on its OWN threads as well --
// measured: an element's properties, values and navigation arrive on a UIA
// worker while the root's arrive on the window's thread -- and the first
// version walked the live component tree from whichever thread asked, while
// the clock was rewriting the very strings it read, and rebuilt the snapshot
// under elements still reading it. Now a walk off the window's thread keeps
// the last snapshot, and every read copies what it needs under a lock, so no
// pointer into the snapshot ever leaves it.
//
// ── Events ──────────────────────────────────────────────────────────────────
//
// A reader is told, without asking, when the focus moves to another control
// and when the focused control's value changes -- which is what makes
// automation audible while a knob moves under the host's hand. Raised from
// the editor's clock (announce(), on the window's thread, never from inside
// a call UIA made), against the element's index, which is its runtime id.
// Only the FOCUSED control's value, and at most four times a second with the
// last value always said: every value of every meter at 30 Hz is a reader
// that will not stop talking.
#pragma once

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <objbase.h>
#include <oleacc.h>
#include <uiautomation.h>

// The three UIA entry points this file calls live in uiautomationcore.dll, and
// the BSTR and SAFEARRAY helpers in oleaut32. Both ship with Windows and
// neither is optional -- unlike WebView2 or libjack, there is no machine where
// they might be absent, so this is a link rather than a dlopen.
//
// Declared here so a consumer's build does not have to know: a plugin author
// including one SDK header should not be told to add two libraries to their
// linker line.
#if defined(_MSC_VER)
#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "oleaut32.lib")
#endif

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "accessibility.h"

namespace sonore {
namespace gfx {
namespace uia {

/** UTF-8 to the BSTR every UIA property returns. Null for empty, which is what
 *  UIA means by "this element has no such property" -- an empty BSTR is a
 *  property that exists and is blank, and a reader announces the difference. */
inline BSTR toBstr(const std::string& utf8) {
  if (utf8.empty()) return nullptr;
  const int chars =
      MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int) utf8.size(), nullptr, 0);
  if (chars <= 0) return nullptr;
  BSTR out = SysAllocStringLen(nullptr, (UINT) chars);
  if (!out) return nullptr;
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int) utf8.size(), out, chars);
  return out;
}

/**
 * Our roles in UIA's vocabulary.
 *
 * The mapping is the whole reason AccessibleRole is a closed set: it happens
 * once, in accessibility.h's accessibleRoleMapping table -- which the macOS
 * bridge reads too, and which carries the reasoning for each row (why a
 * ToggleButton is a Button, why a Table is not a DataGrid, why Unknown is a
 * Pane). The table holds UIA's ids as numbers so it needs no Windows header;
 * these asserts are what keep those numbers UIA's.
 */
static_assert(UIA_ButtonControlTypeId == 50000 && UIA_ComboBoxControlTypeId == 50003 &&
                  UIA_EditControlTypeId == 50004 && UIA_ListItemControlTypeId == 50007 &&
                  UIA_ListControlTypeId == 50008 && UIA_ProgressBarControlTypeId == 50012 &&
                  UIA_ScrollBarControlTypeId == 50014 && UIA_SliderControlTypeId == 50015 &&
                  UIA_TextControlTypeId == 50020 && UIA_GroupControlTypeId == 50026 &&
                  UIA_WindowControlTypeId == 50032 && UIA_PaneControlTypeId == 50033 &&
                  UIA_TableControlTypeId == 50036,
              "accessibleRoleMapping's UIA control type ids are not UIAutomationClient.h's");

inline long controlTypeFor(AccessibleRole role) {
  return accessibleRoleMapping(role).uiaControlType;
}

class RootProvider;

/**
 * One accessible component, as UIA sees it.
 *
 * Holds an ID rather than a Component*, for the reason in the header: UIA
 * calls on its own thread and the editor is repainting on another. The id
 * names the CONTROL, carried across snapshots for as long as its component
 * and role last (detail::matchAccessibleNodes, as the macOS bridge carries its
 * elements). It was the control's position in the snapshot, so a reader
 * holding the third control heard the fourth once a panel above it closed --
 * its name, its value and its runtime id -- and an element whose control had
 * gone answered for whatever came next. Now it answers
 * UIA_E_ELEMENTNOTAVAILABLE, which is how a provider says "that one is gone".
 */
class ElementProvider : public IRawElementProviderSimple,
                        public IRawElementProviderFragment,
                        public IValueProvider,
                        public IRangeValueProvider {
public:
  /** Holds a REFERENCE to its root, taken here and given back in the
   *  destructor (both defined below, where RootProvider is complete). A reader
   *  keeps elements as long as it likes; with a raw pointer, one still holding
   *  a slider after the window closed and UIA dropped the root called into
   *  freed memory. */
  ElementProvider(RootProvider* root, int id);
  virtual ~ElementProvider();

  // ── IUnknown ───────────────────────────────────────────────────────────
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG) InterlockedIncrement(&refs_); }

  ULONG STDMETHODCALLTYPE Release() override {
    const LONG left = InterlockedDecrement(&refs_);
    if (left == 0) delete this;
    return (ULONG) left;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override;

  // ── IRawElementProviderSimple ──────────────────────────────────────────
  HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* options) override {
    if (!options) return E_INVALIDARG;
    // UseComThreading, because everything here is reached from UIA's thread and
    // none of it touches the component tree. Without it UIA marshals every call
    // to the UI thread, which is a deadlock the moment the UI thread is inside
    // a host's own blocking call.
    *options = (ProviderOptions) (ProviderOptions_ServerSideProvider |
                                  ProviderOptions_UseComThreading);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** out) override;
  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* out) override;

  HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(
      IRawElementProviderSimple** out) override {
    // Null: only the ROOT is hosted by the HWND. An element that claimed to be
    // would inherit the window's own name and bounds and appear twice.
    if (out) *out = nullptr;
    return S_OK;
  }

  // ── IRawElementProviderFragment ────────────────────────────────────────
  HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
                                     IRawElementProviderFragment** out) override;
  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override;
  HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* out) override;

  HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
    if (out) *out = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetFocus() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override;

  // ── IValueProvider ─────────────────────────────────────────────────────
  HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR) override { return UIA_E_NOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE get_Value(BSTR* out) override;
  HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* out) override {
    if (out) *out = TRUE;
    return S_OK;
  }

  // ── IRangeValueProvider ────────────────────────────────────────────────
  HRESULT STDMETHODCALLTYPE SetValue(double) override { return UIA_E_NOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE get_Value(double* out) override;
  HRESULT STDMETHODCALLTYPE get_Maximum(double* out) override;
  HRESULT STDMETHODCALLTYPE get_Minimum(double* out) override;
  HRESULT STDMETHODCALLTYPE get_LargeChange(double* out) override {
    if (out) *out = 0.1;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE get_SmallChange(double* out) override {
    if (out) *out = 0.01;
    return S_OK;
  }

  int id() const { return id_; }

private:
  /** A COPY of this element's info, taken under the root's lock: false when
   *  the element is gone (the window closed, or its control left the tree). */
  bool info(AccessibleInfo& out) const;

  RootProvider* root_;
  int id_;
  LONG refs_ = 1;
};

/**
 * The window, and the root of the fragment tree.
 *
 * Owns the snapshot every ElementProvider reads through. Its lifetime is the
 * window's: the peer holds one reference and releases it on close, and UIA may
 * hold others for as long as it likes -- which is why the snapshot is copied
 * data rather than pointers into a tree that is about to be destroyed.
 */
class RootProvider : public IRawElementProviderSimple,
                     public IRawElementProviderFragment,
                     public IRawElementProviderFragmentRoot {
public:
  /** Constructed on the window's own thread, which is the only one that may
   *  walk `content`. */
  RootProvider(HWND hwnd, Component* content, std::string windowName)
      : hwnd_(hwnd), content_(content), name_(std::move(windowName)),
        uiThread_(GetCurrentThreadId()) {}

  virtual ~RootProvider() = default;

  /** Called from the UI thread when the window is going away. Everything after
   *  this answers as an empty tree rather than reaching a destroyed editor --
   *  UIA can and does call in after a window has closed. */
  void detach() {
    content_ = nullptr;
    std::lock_guard<std::mutex> guard(lock_);
    snapshot_.clear();
    ids_.clear();
  }

  // ── IUnknown ───────────────────────────────────────────────────────────
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG) InterlockedIncrement(&refs_); }

  ULONG STDMETHODCALLTYPE Release() override {
    const LONG left = InterlockedDecrement(&refs_);
    if (left == 0) delete this;
    return (ULONG) left;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_INVALIDARG;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple))
      *out = static_cast<IRawElementProviderSimple*>(this);
    else if (iid == __uuidof(IRawElementProviderFragment))
      *out = static_cast<IRawElementProviderFragment*>(this);
    else if (iid == __uuidof(IRawElementProviderFragmentRoot))
      *out = static_cast<IRawElementProviderFragmentRoot*>(this);
    else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  // ── IRawElementProviderSimple ──────────────────────────────────────────
  HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* options) override {
    if (!options) return E_INVALIDARG;
    *options = (ProviderOptions) (ProviderOptions_ServerSideProvider |
                                  ProviderOptions_UseComThreading);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** out) override {
    if (out) *out = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* out) override {
    if (!out) return E_INVALIDARG;
    VariantInit(out);
    switch (property) {
      case UIA_NamePropertyId:
        out->vt = VT_BSTR;
        out->bstrVal = toBstr(name_);
        if (!out->bstrVal) VariantInit(out);
        return S_OK;
      case UIA_ControlTypePropertyId:
        out->vt = VT_I4;
        out->lVal = UIA_PaneControlTypeId;
        return S_OK;
      case UIA_IsControlElementPropertyId:
      case UIA_IsContentElementPropertyId:
      case UIA_IsEnabledPropertyId:
        out->vt = VT_BOOL;
        out->boolVal = VARIANT_TRUE;
        return S_OK;
      default:
        break;
    }
    return S_OK; // VT_EMPTY: "ask the default provider"
  }

  HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(
      IRawElementProviderSimple** out) override {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    // The HWND's own provider, which is what gives the element its window
    // identity, its process and its native handle. A root that returned null
    // here is an element no client can find from a window handle.
    return UiaHostProviderFromHwnd(hwnd_, out);
  }

  // ── IRawElementProviderFragment ────────────────────────────────────────
  HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
                                     IRawElementProviderFragment** out) override {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    refresh();
    if (direction == NavigateDirection_FirstChild) *out = makeChild(firstTopLevel());
    else if (direction == NavigateDirection_LastChild) *out = makeChild(lastTopLevel());
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override {
    // Null: the root's identity comes from its HWND, and a runtime id here
    // would compete with it.
    if (out) *out = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* out) override {
    if (!out) return E_INVALIDARG;
    RECT r{};
    GetWindowRect(hwnd_, &r);
    out->left = r.left;
    out->top = r.top;
    out->width = r.right - r.left;
    out->height = r.bottom - r.top;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
    if (out) *out = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetFocus() override { return S_OK; }

  HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
    if (!out) return E_INVALIDARG;
    *out = this;
    AddRef();
    return S_OK;
  }

  // ── IRawElementProviderFragmentRoot ────────────────────────────────────
  HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y,
                                                     IRawElementProviderFragment** out) override {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    refresh();
    POINT p{(LONG) x, (LONG) y};
    ScreenToClient(hwnd_, &p);
    const float lx = (float) p.x / scale(), ly = (float) p.y / scale();
    int hit = -1;
    {
      std::lock_guard<std::mutex> guard(lock_);
      // Backwards, so the TOPMOST element at a point wins -- the snapshot is
      // in paint order and the last thing drawn is the thing the user sees.
      for (size_t i = snapshot_.size(); i-- > 0;) {
        const Rect& b = snapshot_[i].info.bounds;
        if (lx >= b.x && ly >= b.y && lx < b.right() && ly < b.bottom()) {
          hit = (int) i;
          break;
        }
      }
    }
    *out = makeChild(hit);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** out) override {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    refresh();
    int focused = -1;
    {
      std::lock_guard<std::mutex> guard(lock_);
      for (size_t i = 0; i < snapshot_.size(); ++i)
        if (snapshot_[i].info.focused) {
          focused = (int) i;
          break;
        }
    }
    *out = makeChild(focused);
    return S_OK;
  }

  // ── Used by ElementProvider, on any thread ─────────────────────────────
  //
  // By the element's id, looked up in the snapshot under the same lock as
  // the read, so a refresh between the two cannot hand back another
  // control's answer. The results are positions in the snapshot, which
  // makeChild turns back into ids.

  bool copyInfo(int id, AccessibleInfo& out) const {
    std::lock_guard<std::mutex> guard(lock_);
    const int index = indexOfId(id);
    if (index < 0) return false;
    out = snapshot_[(size_t) index].info;
    return true;
  }

  bool exists(int id) const {
    std::lock_guard<std::mutex> guard(lock_);
    return indexOfId(id) >= 0;
  }

  /** Where `id`'s parent is, -1 for the fragment root; false when `id` is
   *  gone. */
  bool parentOf(int id, int* parent) const {
    std::lock_guard<std::mutex> guard(lock_);
    const int index = indexOfId(id);
    if (index < 0) return false;
    *parent = snapshot_[(size_t) index].parent;
    return true;
  }

  /** `id`'s first or last child, or -1. */
  int childOf(int id, bool last) const {
    std::lock_guard<std::mutex> guard(lock_);
    const int index = indexOfId(id);
    if (index < 0) return -1;
    int found = -1;
    for (int i = 0; i < (int) snapshot_.size(); ++i)
      if (snapshot_[(size_t) i].parent == index) {
        found = i;
        if (!last) break;
      }
    return found;
  }

  /** The next or previous sibling of `id`, or -1. Linear over a list that
   *  is a handful of controls long; a plugin editor is not a document. */
  int siblingOf(int id, bool next) const {
    std::lock_guard<std::mutex> guard(lock_);
    const int index = indexOfId(id);
    if (index < 0) return -1;
    const int parent = snapshot_[(size_t) index].parent;
    int previous = -1;
    bool passed = false;
    for (int i = 0; i < (int) snapshot_.size(); ++i) {
      if (snapshot_[(size_t) i].parent != parent) continue;
      if (i == index) {
        passed = true;
        continue;
      }
      if (passed && next) return i;
      if (!passed) previous = i;
    }
    return next ? -1 : previous;
  }

  int firstChildOf(int index) const {
    std::lock_guard<std::mutex> guard(lock_);
    for (int i = 0; i < (int) snapshot_.size(); ++i)
      if (snapshot_[(size_t) i].parent == index) return i;
    return -1;
  }

  int lastChildOf(int index) const {
    std::lock_guard<std::mutex> guard(lock_);
    int last = -1;
    for (int i = 0; i < (int) snapshot_.size(); ++i)
      if (snapshot_[(size_t) i].parent == index) last = i;
    return last;
  }

  /** The element for the control at `index` in the snapshot, or null. */
  IRawElementProviderFragment* makeChild(int index) {
    int id = 0;
    {
      std::lock_guard<std::mutex> guard(lock_);
      if (index < 0 || index >= (int) ids_.size()) return nullptr;
      id = ids_[(size_t) index];
    }
    return static_cast<IRawElementProviderFragment*>(new ElementProvider(this, id));
  }

  HWND hwnd() const { return hwnd_; }
  float scale() const { return scale_.load(); }
  void setScale(float scale) { scale_ = scale > 0.0f ? scale : 1.0f; }

  /** Retake the snapshot -- on the window's thread, and only there; anywhere
   *  else this keeps the last one. Called at the top of every navigation that
   *  reaches the window's thread, because a client walks the tree at whatever
   *  moment it likes, and by the window on its clock while a client is
   *  listening, so what UIA's own threads read is never more than a tick old.
   *  Walked outside the lock -- the walk is the window thread's own business
   *  -- and swapped in under it. */
  void refresh() {
    if (GetCurrentThreadId() != uiThread_ || !content_) return;
    std::vector<AccessibleNode> fresh;
    collectAccessible(content_, fresh);
    // A control has the keyboard focus only while the user is IN this
    // editor: the OS focus on this window, inside it, or on a window that
    // holds it -- the host's plugin frame, which keeps the OS focus while a
    // knob is focused so the host's space bar still works (see
    // syncKeyboardFocus). The component tree keeps its focused control when
    // the user clicks elsewhere in the host, and a reader told
    // HasKeyboardFocus -- and sent focus and value events -- for an editor in
    // the background talked over what the user was doing. Asked here, on the
    // window's thread, which is the one GetFocus answers for.
    const HWND focus = ::GetFocus();
    const bool inHere = focus && (focus == hwnd_ || IsChild(hwnd_, focus) || IsChild(focus, hwnd_));
    if (!inHere)
      for (AccessibleNode& node : fresh) node.info.focused = false;
    // Each control keeps its id while its component and role last, and a new
    // one gets a new id -- never a number a gone control had, which a reader
    // may still hold. Matched against the last snapshot without the lock:
    // this thread is the only one that writes it.
    const std::vector<int> match = detail::matchAccessibleNodes(snapshot_, fresh);
    std::vector<int> freshIds(fresh.size());
    for (size_t i = 0; i < fresh.size(); ++i)
      freshIds[i] = match[i] >= 0 ? ids_[(size_t) match[i]] : nextId_++;
    std::lock_guard<std::mutex> guard(lock_);
    snapshot_.swap(fresh);
    ids_.swap(freshIds);
  }

  /** On the window's clock, after refresh(): tell a listening reader what
   *  changed since it was last told -- the focus moving to another control,
   *  or the focused control's value. Compared against what was ANNOUNCED,
   *  not against the previous snapshot, which a UIA call on this thread may
   *  have retaken in between. `nowMs` is the clock's time, for the rate
   *  limit on values. */
  void announce(unsigned long long nowMs) {
    if (GetCurrentThreadId() != uiThread_) return;
    int focused = -1, focusedId = 0;
    AccessibleInfo info;
    {
      std::lock_guard<std::mutex> guard(lock_);
      for (size_t i = 0; i < snapshot_.size(); ++i)
        if (snapshot_[i].info.focused) {
          focused = (int) i;
          focusedId = ids_[i];
          info = snapshot_[i].info;
          break;
        }
    }
    if (focused < 0) {
      // Out of the editor: nothing to say, and the next control focused is
      // news even if it is the one that had it before.
      announcedId_ = 0;
      return;
    }
    // The same control by its id -- its component and role -- not by name: a
    // focused button whose label changes is still the button, and comparing
    // names raised a focus event -- the whole control said again -- on every
    // change. Nor by position, which a panel closing above it changes.
    const bool sameControl = focusedId == announcedId_;
    if (!sameControl) {
      IRawElementProviderFragment* element = makeChild(focused);
      if (element) {
        IRawElementProviderSimple* simple = nullptr;
        element->QueryInterface(__uuidof(IRawElementProviderSimple), (void**) &simple);
        if (simple) {
          UiaRaiseAutomationEvent(simple, UIA_AutomationFocusChangedEventId);
          simple->Release();
        }
        element->Release();
      }
      announcedId_ = focusedId;
      announced_ = info;
      valuePending_ = false;
      lastValueMs_ = nowMs;
      return;
    }
    const bool changed = info.value != announced_.value ||
                         (info.hasRange && info.currentValue != announced_.currentValue);
    if (changed) valuePending_ = true;
    if (!valuePending_ || nowMs - lastValueMs_ < 250) return;
    IRawElementProviderFragment* element = makeChild(focused);
    if (!element) return;
    IRawElementProviderSimple* simple = nullptr;
    element->QueryInterface(__uuidof(IRawElementProviderSimple), (void**) &simple);
    if (simple) {
      if (info.value != announced_.value) {
        VARIANT before, after;
        VariantInit(&before);
        VariantInit(&after);
        before.vt = VT_BSTR;
        before.bstrVal = toBstr(announced_.value);
        after.vt = VT_BSTR;
        after.bstrVal = toBstr(info.value);
        UiaRaiseAutomationPropertyChangedEvent(simple, UIA_ValueValuePropertyId, before, after);
        VariantClear(&before);
        VariantClear(&after);
      }
      if (info.hasRange && info.currentValue != announced_.currentValue) {
        VARIANT before, after;
        VariantInit(&before);
        VariantInit(&after);
        before.vt = VT_R8;
        before.dblVal = announced_.currentValue;
        after.vt = VT_R8;
        after.dblVal = info.currentValue;
        UiaRaiseAutomationPropertyChangedEvent(simple, UIA_RangeValueValuePropertyId, before, after);
      }
      simple->Release();
    }
    element->Release();
    announced_ = info;
    valuePending_ = false;
    lastValueMs_ = nowMs;
  }

  int firstTopLevel() const { return firstChildOf(-1); }
  int lastTopLevel() const { return lastChildOf(-1); }

private:
  /** Where control `id` is in the snapshot, or -1. The caller holds lock_. */
  int indexOfId(int id) const {
    for (size_t i = 0; i < ids_.size(); ++i)
      if (ids_[i] == id) return (int) i;
    return -1;
  }

  HWND hwnd_;
  Component* content_; // the window thread's alone
  std::string name_;
  DWORD uiThread_;
  mutable std::mutex lock_; // over snapshot_ and ids_
  std::vector<AccessibleNode> snapshot_;
  /** Each snapshot node's id, which its element holds. */
  std::vector<int> ids_;
  int nextId_ = 1; // the window thread's alone
  // What a reader was last told, on the window's thread only.
  int announcedId_ = 0;
  AccessibleInfo announced_;
  bool valuePending_ = false;
  unsigned long long lastValueMs_ = 0;
  std::atomic<float> scale_{1.0f};
  LONG refs_ = 1;
};

// ── ElementProvider, now that RootProvider is complete ────────────────────

inline ElementProvider::ElementProvider(RootProvider* root, int id) : root_(root), id_(id) {
  root_->AddRef();
}

inline ElementProvider::~ElementProvider() { root_->Release(); }

inline bool ElementProvider::info(AccessibleInfo& out) const { return root_->copyInfo(id_, out); }

inline HRESULT STDMETHODCALLTYPE ElementProvider::QueryInterface(REFIID iid, void** out) {
  if (!out) return E_INVALIDARG;
  if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple))
    *out = static_cast<IRawElementProviderSimple*>(this);
  else if (iid == __uuidof(IRawElementProviderFragment))
    *out = static_cast<IRawElementProviderFragment*>(this);
  else if (iid == __uuidof(IValueProvider))
    *out = static_cast<IValueProvider*>(this);
  else if (iid == __uuidof(IRangeValueProvider))
    *out = static_cast<IRangeValueProvider*>(this);
  else {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::GetPatternProvider(PATTERNID pattern,
                                                                    IUnknown** out) {
  if (!out) return E_INVALIDARG;
  *out = nullptr;
  AccessibleInfo i;
  if (!info(i)) return UIA_E_ELEMENTNOTAVAILABLE;

  // Value for anything that reads something, Range only for what genuinely has
  // one. Offering RangeValue on a button would give a reader a percentage to
  // announce for a thing that has no position.
  if (pattern == UIA_ValuePatternId && !i.value.empty()) {
    *out = static_cast<IValueProvider*>(this);
    AddRef();
  } else if (pattern == UIA_RangeValuePatternId && i.hasRange) {
    *out = static_cast<IRangeValueProvider*>(this);
    AddRef();
  }
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::GetPropertyValue(PROPERTYID property,
                                                                  VARIANT* out) {
  if (!out) return E_INVALIDARG;
  VariantInit(out);
  AccessibleInfo i;
  if (!info(i)) return UIA_E_ELEMENTNOTAVAILABLE;

  switch (property) {
    case UIA_NamePropertyId:
      out->vt = VT_BSTR;
      out->bstrVal = toBstr(i.name);
      if (!out->bstrVal) VariantInit(out);
      return S_OK;
    case UIA_HelpTextPropertyId:
      if (i.description.empty()) return S_OK;
      out->vt = VT_BSTR;
      out->bstrVal = toBstr(i.description);
      if (!out->bstrVal) VariantInit(out);
      return S_OK;
    case UIA_ControlTypePropertyId:
      out->vt = VT_I4;
      out->lVal = controlTypeFor(i.role);
      return S_OK;
    case UIA_IsEnabledPropertyId:
      out->vt = VT_BOOL;
      out->boolVal = i.enabled ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
    case UIA_IsKeyboardFocusablePropertyId:
      out->vt = VT_BOOL;
      out->boolVal = i.focusable ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
    case UIA_HasKeyboardFocusPropertyId:
      out->vt = VT_BOOL;
      out->boolVal = i.focused ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
    case UIA_IsControlElementPropertyId:
    case UIA_IsContentElementPropertyId:
      out->vt = VT_BOOL;
      out->boolVal = VARIANT_TRUE;
      return S_OK;
    default:
      break;
  }
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::Navigate(NavigateDirection direction,
                                                           IRawElementProviderFragment** out) {
  if (!out) return E_INVALIDARG;
  *out = nullptr;
  switch (direction) {
    case NavigateDirection_Parent: {
      int parent = -1;
      if (!root_->parentOf(id_, &parent)) return UIA_E_ELEMENTNOTAVAILABLE;
      if (parent < 0) {
        // The fragment ROOT is the parent of a top-level element. Returning
        // null instead detaches the whole subtree from the window, and a
        // client walking up from a knob falls off the world.
        return root_->QueryInterface(__uuidof(IRawElementProviderFragment), (void**) out);
      }
      *out = root_->makeChild(parent);
      return S_OK;
    }
    case NavigateDirection_NextSibling:
      *out = root_->makeChild(root_->siblingOf(id_, /*next=*/true));
      break;
    case NavigateDirection_PreviousSibling:
      *out = root_->makeChild(root_->siblingOf(id_, /*next=*/false));
      break;
    case NavigateDirection_FirstChild:
      *out = root_->makeChild(root_->childOf(id_, /*last=*/false));
      break;
    case NavigateDirection_LastChild:
      *out = root_->makeChild(root_->childOf(id_, /*last=*/true));
      break;
    default:
      break;
  }
  // Nothing found is "none" only for an element that still exists.
  return *out || root_->exists(id_) ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::GetRuntimeId(SAFEARRAY** out) {
  if (!out) return E_INVALIDARG;
  // UiaAppendRuntimeId plus our id: the first tells UIA to prefix the
  // window's own id, so two plugin windows in one host cannot produce colliding
  // element ids. Without it a client caching elements from one editor would
  // hand them back for another.
  SAFEARRAY* array = SafeArrayCreateVector(VT_I4, 0, 2);
  if (!array) return E_OUTOFMEMORY;
  int values[2] = {UiaAppendRuntimeId, id_};
  for (LONG i = 0; i < 2; ++i) SafeArrayPutElement(array, &i, &values[i]);
  *out = array;
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::get_BoundingRectangle(UiaRect* out) {
  if (!out) return E_INVALIDARG;
  *out = UiaRect{0, 0, 0, 0};
  AccessibleInfo i;
  if (!info(i)) return UIA_E_ELEMENTNOTAVAILABLE;

  // Logical to device, then client to screen. UIA speaks SCREEN pixels, and a
  // provider that answered in its own coordinates puts a reader's highlight
  // rectangle in the top-left corner of the desktop.
  const float scale = root_->scale();
  POINT origin{(LONG) (i.bounds.x * scale), (LONG) (i.bounds.y * scale)};
  ClientToScreen(root_->hwnd(), &origin);
  out->left = origin.x;
  out->top = origin.y;
  out->width = i.bounds.w * scale;
  out->height = i.bounds.h * scale;
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::get_FragmentRoot(
    IRawElementProviderFragmentRoot** out) {
  if (!out) return E_INVALIDARG;
  return root_->QueryInterface(__uuidof(IRawElementProviderFragmentRoot), (void**) out);
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::get_Value(BSTR* out) {
  if (!out) return E_INVALIDARG;
  *out = nullptr;
  AccessibleInfo i;
  if (!info(i)) return UIA_E_ELEMENTNOTAVAILABLE;
  *out = toBstr(i.value);
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::get_Value(double* out) {
  if (!out) return E_INVALIDARG;
  *out = 0.0;
  AccessibleInfo i;
  if (!info(i)) return UIA_E_ELEMENTNOTAVAILABLE;
  *out = i.currentValue;
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::get_Maximum(double* out) {
  if (!out) return E_INVALIDARG;
  *out = 0.0;
  AccessibleInfo i;
  if (!info(i)) return UIA_E_ELEMENTNOTAVAILABLE;
  *out = i.maxValue;
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::get_Minimum(double* out) {
  if (!out) return E_INVALIDARG;
  *out = 0.0;
  AccessibleInfo i;
  if (!info(i)) return UIA_E_ELEMENTNOTAVAILABLE;
  *out = i.minValue;
  return S_OK;
}

} // namespace uia
} // namespace gfx
} // namespace sonore

#endif // _WIN32
