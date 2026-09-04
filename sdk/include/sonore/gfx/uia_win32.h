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
// Both are built from a SNAPSHOT of collectAccessible, refreshed whenever UIA
// asks the root for its children. That is deliberate: UIA calls in on its own
// thread, and walking a live component tree from there would race with the
// editor's own 33 ms clock repainting it. A snapshot is a copy of small structs
// taken under the same lock nothing else needs, and it cannot dangle.
//
// ── What is deliberately not here ───────────────────────────────────────────
//
// Events. UiaRaiseAutomationEvent lets a provider tell a reader that a value
// changed without being asked, which is what makes automation audible while a
// knob moves under the host's control. It needs the editor's clock to notice
// changes and a stable element identity to raise them against, and doing it
// wrong means either silence or a reader that will not stop talking. It is the
// next piece, and it is named here rather than left to be discovered missing.
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
 * once, here, and a second platform maps the same enum to its own names
 * without either being able to change what the other says.
 */
inline long controlTypeFor(AccessibleRole role) {
  switch (role) {
    case AccessibleRole::Button: return UIA_ButtonControlTypeId;
    // A ToggleButton is a Button that also implements the Toggle pattern.
    // UIA has no separate control type, and inventing one would produce an
    // element no reader has a phrase for.
    case AccessibleRole::ToggleButton: return UIA_ButtonControlTypeId;
    case AccessibleRole::Slider: return UIA_SliderControlTypeId;
    case AccessibleRole::Label: return UIA_TextControlTypeId;
    case AccessibleRole::TextField: return UIA_EditControlTypeId;
    case AccessibleRole::ComboBox: return UIA_ComboBoxControlTypeId;
    case AccessibleRole::List: return UIA_ListControlTypeId;
    case AccessibleRole::ListItem: return UIA_ListItemControlTypeId;
    // Table, not DataGrid. DataGrid promises the GridPattern -- a client may
    // ask for the cell at row 4 column 2 and expect an element back -- and
    // claiming a pattern that is not implemented is worse than claiming a
    // simpler control type: the reader asks, gets nothing, and says nothing.
    case AccessibleRole::Table: return UIA_TableControlTypeId;
    case AccessibleRole::ScrollBar: return UIA_ScrollBarControlTypeId;
    case AccessibleRole::ProgressBar: return UIA_ProgressBarControlTypeId;
    case AccessibleRole::Dialog: return UIA_WindowControlTypeId;
    case AccessibleRole::Window: return UIA_WindowControlTypeId;
    // Group for both, and MusicalKeyboard deliberately: no platform has that
    // role, and a Group named "Keyboard" is a phrase every reader can say.
    case AccessibleRole::Group:
    case AccessibleRole::MusicalKeyboard: return UIA_GroupControlTypeId;
    case AccessibleRole::Unknown: break;
  }
  // Pane, not Custom. A Custom element makes a reader ask the provider for a
  // localized type it would then have to invent; Pane is the honest "a region
  // that holds things".
  return UIA_PaneControlTypeId;
}

class RootProvider;

/**
 * One accessible component, as UIA sees it.
 *
 * Holds a COPY of the info rather than a Component*, for the reason in the
 * header: UIA calls on its own thread and the editor is repainting on another.
 */
class ElementProvider : public IRawElementProviderSimple,
                        public IRawElementProviderFragment,
                        public IValueProvider,
                        public IRangeValueProvider {
public:
  ElementProvider(RootProvider* root, int index) : root_(root), index_(index) {}
  virtual ~ElementProvider() = default;

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

  int index() const { return index_; }

private:
  const AccessibleInfo* info() const;

  RootProvider* root_;
  int index_;
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
  RootProvider(HWND hwnd, Component* content, std::string windowName)
      : hwnd_(hwnd), content_(content), name_(std::move(windowName)) {}

  virtual ~RootProvider() = default;

  /** Called from the UI thread when the window is going away. Everything after
   *  this answers as an empty tree rather than reaching a destroyed editor --
   *  UIA can and does call in after a window has closed. */
  void detach() {
    content_ = nullptr;
    snapshot_.clear();
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
    if (snapshot_.empty()) return S_OK;
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
    // Backwards, so the TOPMOST element at a point wins -- the snapshot is in
    // paint order and the last thing drawn is the thing the user sees.
    for (size_t i = snapshot_.size(); i-- > 0;) {
      const Rect& b = snapshot_[i].info.bounds;
      const float lx = (float) p.x / scale_, ly = (float) p.y / scale_;
      if (lx >= b.x && ly >= b.y && lx < b.right() && ly < b.bottom()) {
        *out = makeChild((int) i);
        return S_OK;
      }
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** out) override {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    refresh();
    for (size_t i = 0; i < snapshot_.size(); ++i)
      if (snapshot_[i].info.focused) {
        *out = makeChild((int) i);
        return S_OK;
      }
    return S_OK;
  }

  // ── Used by ElementProvider ────────────────────────────────────────────
  const AccessibleInfo* infoAt(int index) const {
    if (index < 0 || index >= (int) snapshot_.size()) return nullptr;
    return &snapshot_[(size_t) index].info;
  }

  int parentOf(int index) const {
    if (index < 0 || index >= (int) snapshot_.size()) return -1;
    return snapshot_[(size_t) index].parent;
  }

  /** The next or previous sibling of `index`, or -1. Linear over a list that
   *  is a handful of controls long; a plugin editor is not a document. */
  int siblingOf(int index, bool next) const {
    if (index < 0 || index >= (int) snapshot_.size()) return -1;
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
    for (int i = 0; i < (int) snapshot_.size(); ++i)
      if (snapshot_[(size_t) i].parent == index) return i;
    return -1;
  }

  int lastChildOf(int index) const {
    int last = -1;
    for (int i = 0; i < (int) snapshot_.size(); ++i)
      if (snapshot_[(size_t) i].parent == index) last = i;
    return last;
  }

  IRawElementProviderFragment* makeChild(int index) {
    if (index < 0 || index >= (int) snapshot_.size()) return nullptr;
    return static_cast<IRawElementProviderFragment*>(new ElementProvider(this, index));
  }

  HWND hwnd() const { return hwnd_; }
  float scale() const { return scale_; }
  void setScale(float scale) { scale_ = scale > 0.0f ? scale : 1.0f; }

  /** Retake the snapshot. Called at the top of every navigation, because a
   *  client walks the tree at whatever moment it likes and the editor may have
   *  changed a value since the last one. */
  void refresh() {
    snapshot_.clear();
    if (content_) collectAccessible(content_, snapshot_);
  }

  int firstTopLevel() const { return firstChildOf(-1); }
  int lastTopLevel() const { return lastChildOf(-1); }

private:
  HWND hwnd_;
  Component* content_;
  std::string name_;
  std::vector<AccessibleNode> snapshot_;
  float scale_ = 1.0f;
  LONG refs_ = 1;
};

// ── ElementProvider, now that RootProvider is complete ────────────────────

inline const AccessibleInfo* ElementProvider::info() const { return root_->infoAt(index_); }

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
  const AccessibleInfo* i = info();
  if (!i) return S_OK;

  // Value for anything that reads something, Range only for what genuinely has
  // one. Offering RangeValue on a button would give a reader a percentage to
  // announce for a thing that has no position.
  if (pattern == UIA_ValuePatternId && !i->value.empty()) {
    *out = static_cast<IValueProvider*>(this);
    AddRef();
  } else if (pattern == UIA_RangeValuePatternId && i->hasRange) {
    *out = static_cast<IRangeValueProvider*>(this);
    AddRef();
  }
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::GetPropertyValue(PROPERTYID property,
                                                                  VARIANT* out) {
  if (!out) return E_INVALIDARG;
  VariantInit(out);
  const AccessibleInfo* i = info();
  if (!i) return S_OK;

  switch (property) {
    case UIA_NamePropertyId:
      out->vt = VT_BSTR;
      out->bstrVal = toBstr(i->name);
      if (!out->bstrVal) VariantInit(out);
      return S_OK;
    case UIA_HelpTextPropertyId:
      if (i->description.empty()) return S_OK;
      out->vt = VT_BSTR;
      out->bstrVal = toBstr(i->description);
      if (!out->bstrVal) VariantInit(out);
      return S_OK;
    case UIA_ControlTypePropertyId:
      out->vt = VT_I4;
      out->lVal = controlTypeFor(i->role);
      return S_OK;
    case UIA_IsEnabledPropertyId:
      out->vt = VT_BOOL;
      out->boolVal = i->enabled ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
    case UIA_IsKeyboardFocusablePropertyId:
      out->vt = VT_BOOL;
      out->boolVal = i->focusable ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
    case UIA_HasKeyboardFocusPropertyId:
      out->vt = VT_BOOL;
      out->boolVal = i->focused ? VARIANT_TRUE : VARIANT_FALSE;
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
      const int parent = root_->parentOf(index_);
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
      *out = root_->makeChild(root_->siblingOf(index_, /*next=*/true));
      return S_OK;
    case NavigateDirection_PreviousSibling:
      *out = root_->makeChild(root_->siblingOf(index_, /*next=*/false));
      return S_OK;
    case NavigateDirection_FirstChild:
      *out = root_->makeChild(root_->firstChildOf(index_));
      return S_OK;
    case NavigateDirection_LastChild:
      *out = root_->makeChild(root_->lastChildOf(index_));
      return S_OK;
    default:
      break;
  }
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::GetRuntimeId(SAFEARRAY** out) {
  if (!out) return E_INVALIDARG;
  // UiaAppendRuntimeId plus our index: the first tells UIA to prefix the
  // window's own id, so two plugin windows in one host cannot produce colliding
  // element ids. Without it a client caching elements from one editor would
  // hand them back for another.
  SAFEARRAY* array = SafeArrayCreateVector(VT_I4, 0, 2);
  if (!array) return E_OUTOFMEMORY;
  int values[2] = {UiaAppendRuntimeId, index_ + 1};
  for (LONG i = 0; i < 2; ++i) SafeArrayPutElement(array, &i, &values[i]);
  *out = array;
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::get_BoundingRectangle(UiaRect* out) {
  if (!out) return E_INVALIDARG;
  *out = UiaRect{0, 0, 0, 0};
  const AccessibleInfo* i = info();
  if (!i) return S_OK;

  // Logical to device, then client to screen. UIA speaks SCREEN pixels, and a
  // provider that answered in its own coordinates puts a reader's highlight
  // rectangle in the top-left corner of the desktop.
  const float scale = root_->scale();
  POINT origin{(LONG) (i->bounds.x * scale), (LONG) (i->bounds.y * scale)};
  ClientToScreen(root_->hwnd(), &origin);
  out->left = origin.x;
  out->top = origin.y;
  out->width = i->bounds.w * scale;
  out->height = i->bounds.h * scale;
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::get_FragmentRoot(
    IRawElementProviderFragmentRoot** out) {
  if (!out) return E_INVALIDARG;
  return root_->QueryInterface(__uuidof(IRawElementProviderFragmentRoot), (void**) out);
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::get_Value(BSTR* out) {
  if (!out) return E_INVALIDARG;
  const AccessibleInfo* i = info();
  *out = i ? toBstr(i->value) : nullptr;
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::get_Value(double* out) {
  if (!out) return E_INVALIDARG;
  const AccessibleInfo* i = info();
  *out = i ? i->currentValue : 0.0;
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::get_Maximum(double* out) {
  if (!out) return E_INVALIDARG;
  const AccessibleInfo* i = info();
  *out = i ? i->maxValue : 0.0;
  return S_OK;
}

inline HRESULT STDMETHODCALLTYPE ElementProvider::get_Minimum(double* out) {
  if (!out) return E_INVALIDARG;
  const AccessibleInfo* i = info();
  *out = i ? i->minValue : 0.0;
  return S_OK;
}

} // namespace uia
} // namespace gfx
} // namespace sonore

#endif // _WIN32
