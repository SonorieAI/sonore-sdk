// SPDX-License-Identifier: Apache-2.0
//
// Dragging TEXT: between components, in from other applications, and out to
// them.
//
// ── The three paths, and why they meet at one interface ─────────────────────
//
// A TextDragAndDropTarget (component.h) is told about text being dragged over
// it, wherever the text came from:
//
//   Inside the editor. A field the user selects text in starts the drag with
//   MouseRouter::startTextDrag, and the router's ordinary drag machinery
//   carries it -- the same capture, the same walk up from the pointer to find
//   a component that wants it. Component's default item-drag handlers hand a
//   text drag to the TextDragAndDropTarget, so a target needs no other code.
//
//   From another application. The platform's drop target -- an OLE
//   IDropTarget on Windows, the view's dragging destination on macOS -- reads
//   the text off the drag and hands it to a TextDragDispatcher, which finds the
//   target the same way and tells it the same things.
//
//   Out to another application. performExternalDragDropOfText puts the text
//   on the platform's drag pasteboard and runs the system's drag.
//
// ── Windows: why OLE now, when files came by WM_DROPFILES ───────────────────
//
// window_win32.h explains why files arrive by WM_DROPFILES: one call, no COM,
// nothing to unwind. Text has no such message -- dragged text exists only as
// an IDataObject handed to a registered IDropTarget -- so accepting it means
// registering one. Once one is registered, Windows sends it EVERYTHING dropped
// on the window and stops sending WM_DROPFILES, so the drop target here reads
// files (CF_HDROP) as well as text (CF_UNICODETEXT, then CF_TEXT), and files
// gain the live hover feedback WM_DROPFILES could not give.
//
// The cost the old comment named is real and is paid carefully. The target is
// registered only on a thread the host has already made a single-threaded
// apartment -- every host that does drag and drop has -- and OleInitialize,
// which on such a thread adds OLE without changing the apartment, is balanced
// at close. A thread with no apartment or in the multithreaded one is left
// untouched and the window keeps WM_DROPFILES exactly as before. ole32 is
// loaded at run time rather than linked, so a plugin that never drags text
// imports nothing new.
//
// ── macOS ───────────────────────────────────────────────────────────────────
//
// The view registers for the string pasteboard type beside the file types and
// reads public.utf8-plain-text off a drag that has no files. Compiled against
// the shim, never run. Dragging text OUT on macOS is not implemented: it needs
// an NSDraggingSource registered at run time and the mouse-down NSEvent that
// started the gesture, which the component tree does not keep.
//
// X11's XDND already negotiates text/plain for the file path; the X11 peer
// does not yet hand text drops to a dispatcher, and the row says so.
#pragma once

#include <cstring>
#include <functional>
#include <string>
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
#include <ole2.h>
#include <shellapi.h>
#endif
#if defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#endif

namespace sonore {
namespace gfx {

/**
 * Takes text dragged in from OUTSIDE the tree and delivers it to whichever
 * TextDragAndDropTarget is under the pointer.
 *
 * One per window, driven by the platform's drop target. Finds its target the
 * way the router finds an item-drag target -- up from the component under the
 * pointer to the first that is interested -- and holds it only while it is
 * alive: a target destroyed mid-drag is forgotten, not called.
 */
class TextDragDispatcher {
public:
  /** For the tree under `root`, the window's root component, whose
   *  coordinates dragMove and drop take. `root` must outlive the dispatcher. */
  explicit TextDragDispatcher(Component& root) : root_(&root) { guard_.owner = this; }
  TextDragDispatcher(const TextDragDispatcher&) = delete;
  TextDragDispatcher& operator=(const TextDragDispatcher&) = delete;

  /** The pointer is at `rootPos` with `text`. Returns whether anything under
   *  it would take it, which is what the platform asks before the drop. */
  bool dragMove(const std::string& text, Point rootPos) {
    Component* over = findTarget(text, rootPos);
    if (over != current_) {
      leave();
      setCurrent(over);
      if (current_)
        current_->asTextDragAndDropTarget()->textDragEnter(text, current_->rootToLocal(rootPos));
    }
    if (current_) {
      current_->asTextDragAndDropTarget()->textDragMove(text, current_->rootToLocal(rootPos));
      return current_ != nullptr;
    }
    return false;
  }

  /** The drag left the window or was cancelled: the target it was over is
   *  told textDragExit and forgotten. Does nothing when there is none. */
  void dragExit() { leave(); }

  /** Returns whether a target took it. The enter/exit pair is closed before
   *  the drop, as the router does for files. */
  bool drop(const std::string& text, Point rootPos) {
    Component* over = findTarget(text, rootPos);
    leave();
    if (!over) return false;
    over->asTextDragAndDropTarget()->textDropped(text, over->rootToLocal(rootPos));
    return true;
  }

  /** The component the drag is over that wants the text, or null; cleared
   *  if that component is destroyed during the drag. */
  Component* currentTarget() const { return current_; }

private:
  /** Listens to the target the drag is over, so a target destroyed in the
   *  middle of a drag clears current_ rather than leaving a dangling pointer
   *  for the next dragMove or dragExit to call. */
  struct Guard : ComponentListener {
    TextDragDispatcher* owner = nullptr;
    void componentBeingDeleted(Component& c) override {
      if (owner->current_ == &c) owner->current_ = nullptr;
    }
  };

  void setCurrent(Component* c) {
    if (current_) current_->removeComponentListener(&guard_);
    // Held only if it takes the listener: one being destroyed refuses it.
    current_ = (c && c->addComponentListener(&guard_)) ? c : nullptr;
  }

  void leave() {
    Component* was = current_;
    setCurrent(nullptr);
    if (was)
      if (TextDragAndDropTarget* t = was->asTextDragAndDropTarget()) t->textDragExit(lastText_);
  }

  Component* findTarget(const std::string& text, Point rootPos) {
    lastText_ = text;
    Component* hit = root_->hitTest(rootPos);
    for (Component* c = hit; c != nullptr; c = c->parent())
      if (TextDragAndDropTarget* t = c->asTextDragAndDropTarget())
        if (t->isInterestedInTextDrag(text)) return c;
    return nullptr;
  }

  Component* root_;
  Component* current_ = nullptr;
  std::string lastText_;
  Guard guard_;
};

// ═══════════════════════════════════════════════════════════════════════════
// Windows: OLE
// ═══════════════════════════════════════════════════════════════════════════

#if defined(_WIN32)

namespace detail {

/** The handful of ole32 entry points, looked up when first wanted. */
struct Ole32Api {
  HRESULT(WINAPI* oleInitialize)(LPVOID) = nullptr;
  void(WINAPI* oleUninitialize)() = nullptr;
  HRESULT(WINAPI* registerDragDrop)(HWND, LPDROPTARGET) = nullptr;
  HRESULT(WINAPI* revokeDragDrop)(HWND) = nullptr;
  HRESULT(WINAPI* doDragDrop)(LPDATAOBJECT, LPDROPSOURCE, DWORD, LPDWORD) = nullptr;
  void(WINAPI* releaseStgMedium)(LPSTGMEDIUM) = nullptr;
  HRESULT(WINAPI* coGetApartmentType)(APTTYPE*, APTTYPEQUALIFIER*) = nullptr;
  bool ok() const {
    return oleInitialize && oleUninitialize && registerDragDrop && revokeDragDrop && doDragDrop &&
           releaseStgMedium;
  }
};

inline const Ole32Api& ole32() {
  static Ole32Api api = [] {
    Ole32Api a;
    HMODULE ole = LoadLibraryExW(L"ole32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ole) return a;
    auto get = [ole](const char* name) { return reinterpret_cast<void*>(GetProcAddress(ole, name)); };
    a.oleInitialize = reinterpret_cast<HRESULT(WINAPI*)(LPVOID)>(get("OleInitialize"));
    a.oleUninitialize = reinterpret_cast<void(WINAPI*)()>(get("OleUninitialize"));
    a.registerDragDrop = reinterpret_cast<HRESULT(WINAPI*)(HWND, LPDROPTARGET)>(get("RegisterDragDrop"));
    a.revokeDragDrop = reinterpret_cast<HRESULT(WINAPI*)(HWND)>(get("RevokeDragDrop"));
    a.doDragDrop = reinterpret_cast<HRESULT(WINAPI*)(LPDATAOBJECT, LPDROPSOURCE, DWORD, LPDWORD)>(
        get("DoDragDrop"));
    a.releaseStgMedium = reinterpret_cast<void(WINAPI*)(LPSTGMEDIUM)>(get("ReleaseStgMedium"));
    a.coGetApartmentType = reinterpret_cast<HRESULT(WINAPI*)(APTTYPE*, APTTYPEQUALIFIER*)>(
        get("CoGetApartmentType"));
    return a;
  }();
  return api;
}

inline std::string oleUtf8(const wchar_t* wide, size_t length) {
  if (!wide || length == 0) return {};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, (int) length, nullptr, 0, nullptr, nullptr);
  std::string out(bytes > 0 ? (size_t) bytes : 0u, '\0');
  if (bytes > 0) WideCharToMultiByte(CP_UTF8, 0, wide, (int) length, &out[0], bytes, nullptr, nullptr);
  return out;
}

inline std::wstring oleWide(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int chars = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int) utf8.size(), nullptr, 0);
  std::wstring out(chars > 0 ? (size_t) chars : 0u, L'\0');
  if (chars > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int) utf8.size(), &out[0], chars);
  return out;
}

/**
 * The text on a data object: CF_UNICODETEXT if offered, else CF_TEXT in the
 * ANSI code page. Untrusted bytes from another process, so the length is the
 * smaller of the first terminator and the global's actual size -- a source
 * that forgot its terminator gets its text cut at the buffer's end, not a read
 * past it.
 */
inline bool oleReadText(IDataObject* data, std::string* out) {
  if (!data || !out || !ole32().ok()) return false;
  FORMATETC unicode{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (SUCCEEDED(data->GetData(&unicode, &medium))) {
    bool ok = false;
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal) {
      const SIZE_T bytes = GlobalSize(medium.hGlobal);
      if (const wchar_t* w = (const wchar_t*) GlobalLock(medium.hGlobal)) {
        const size_t most = (size_t) bytes / sizeof(wchar_t);
        size_t n = 0;
        while (n < most && w[n] != 0) ++n;
        *out = oleUtf8(w, n);
        GlobalUnlock(medium.hGlobal);
        ok = true;
      }
    }
    ole32().releaseStgMedium(&medium);
    if (ok) return true;
  }
  FORMATETC ansi{CF_TEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM narrow{};
  if (FAILED(data->GetData(&ansi, &narrow))) return false;
  bool ok = false;
  if (narrow.tymed == TYMED_HGLOBAL && narrow.hGlobal) {
    const SIZE_T bytes = GlobalSize(narrow.hGlobal);
    if (const char* a = (const char*) GlobalLock(narrow.hGlobal)) {
      size_t n = 0;
      while (n < (size_t) bytes && a[n] != 0) ++n;
      const int chars = MultiByteToWideChar(CP_ACP, 0, a, (int) n, nullptr, 0);
      std::wstring w(chars > 0 ? (size_t) chars : 0u, L'\0');
      if (chars > 0) MultiByteToWideChar(CP_ACP, 0, a, (int) n, &w[0], chars);
      *out = oleUtf8(w.c_str(), w.size());
      GlobalUnlock(narrow.hGlobal);
      ok = true;
    }
  }
  ole32().releaseStgMedium(&narrow);
  return ok;
}

/** The files on a data object (CF_HDROP), as UTF-8 paths. */
inline bool oleReadFiles(IDataObject* data, std::vector<std::string>* out) {
  if (!data || !out || !ole32().ok()) return false;
  FORMATETC hdrop{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  if (FAILED(data->GetData(&hdrop, &medium))) return false;
  bool ok = false;
  if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal) {
    HDROP drop = (HDROP) medium.hGlobal;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
      const UINT chars = DragQueryFileW(drop, i, nullptr, 0);
      if (chars == 0) continue;
      std::wstring wide((size_t) chars + 1, L'\0');
      DragQueryFileW(drop, i, &wide[0], chars + 1);
      out->push_back(oleUtf8(wide.c_str(), chars));
    }
    ok = !out->empty();
  }
  ole32().releaseStgMedium(&medium);
  return ok;
}

/** A FORMATETC enumerator over a fixed list -- what EnumFormatEtc hands a
 *  drop target asking which formats a drag carries. */
class OleFormatEnumerator : public IEnumFORMATETC {
public:
  OleFormatEnumerator(const std::vector<FORMATETC>& formats, size_t at) : formats_(formats), at_(at) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IEnumFORMATETC)) {
      *out = static_cast<IEnumFORMATETC*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG) InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return (ULONG) n;
  }
  HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC* out, ULONG* fetched) override {
    if (!out) return E_POINTER;
    ULONG n = 0;
    while (n < count && at_ < formats_.size()) out[n++] = formats_[at_++];
    if (fetched) *fetched = n;
    return n == count ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Skip(ULONG count) override {
    at_ += count;
    return at_ <= formats_.size() ? S_OK : S_FALSE;
  }
  HRESULT STDMETHODCALLTYPE Reset() override {
    at_ = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** out) override {
    if (!out) return E_POINTER;
    *out = new OleFormatEnumerator(formats_, at_);
    return S_OK;
  }

private:
  virtual ~OleFormatEnumerator() = default;
  std::vector<FORMATETC> formats_;
  size_t at_;
  LONG refs_ = 1;
};

/**
 * Text, as a data object: CF_UNICODETEXT, and CF_TEXT for the applications
 * that still ask for nothing else. What performExternalDragDropOfText hands
 * DoDragDrop -- and what a test hands the drop target, to read back.
 */
class OleTextDataObject : public IDataObject {
public:
  explicit OleTextDataObject(const std::string& utf8) : wide_(oleWide(utf8)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDataObject)) {
      *out = static_cast<IDataObject*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG) InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return (ULONG) n;
  }

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
    if (!format || !medium) return E_INVALIDARG;
    if (QueryGetData(format) != S_OK) return DV_E_FORMATETC;
    HGLOBAL global = nullptr;
    if (format->cfFormat == CF_UNICODETEXT) {
      const SIZE_T bytes = (wide_.size() + 1) * sizeof(wchar_t);
      global = GlobalAlloc(GMEM_MOVEABLE, bytes);
      if (!global) return E_OUTOFMEMORY;
      if (void* p = GlobalLock(global)) {
        std::memcpy(p, wide_.c_str(), bytes);
        GlobalUnlock(global);
      }
    } else {
      const int bytes = WideCharToMultiByte(CP_ACP, 0, wide_.c_str(), (int) wide_.size(), nullptr, 0,
                                            nullptr, nullptr);
      global = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T) (bytes > 0 ? bytes : 0) + 1);
      if (!global) return E_OUTOFMEMORY;
      if (char* p = (char*) GlobalLock(global)) {
        if (bytes > 0)
          WideCharToMultiByte(CP_ACP, 0, wide_.c_str(), (int) wide_.size(), p, bytes, nullptr, nullptr);
        p[bytes > 0 ? bytes : 0] = 0;
        GlobalUnlock(global);
      }
    }
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = global;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
    if (!format) return E_INVALIDARG;
    if (format->cfFormat != CF_UNICODETEXT && format->cfFormat != CF_TEXT) return DV_E_FORMATETC;
    if (!(format->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
    if (format->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
    if (out) out->ptd = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
    if (!out) return E_POINTER;
    if (direction != DATADIR_GET) {
      *out = nullptr;
      return E_NOTIMPL;
    }
    std::vector<FORMATETC> formats;
    formats.push_back({CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
    formats.push_back({CF_TEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL});
    *out = new OleFormatEnumerator(formats, 0);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }

private:
  virtual ~OleTextDataObject() = default;
  std::wstring wide_;
  LONG refs_ = 1;
};

/** The source half of DoDragDrop: Escape cancels, releasing the button drops. */
class OleDropSource : public IDropSource {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDropSource)) {
      *out = static_cast<IDropSource*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG) InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return (ULONG) n;
  }
  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
    if (escape) return DRAGDROP_S_CANCEL;
    if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
  virtual ~OleDropSource() = default;
  LONG refs_ = 1;
};

/**
 * The window's IDropTarget: text to the TextDragDispatcher, files to the
 * router's file path -- with hover feedback for both, which WM_DROPFILES never
 * had.
 *
 * `toLogical(screenX, screenY)` converts a SCREEN point, which is what OLE
 * reports, into the tree's units.
 */
class OleDropTarget : public IDropTarget {
public:
  using ToLogical = std::function<Point(LONG, LONG)>;

  OleDropTarget(MouseRouter& router, Component& root, ToLogical toLogical)
      : router_(&router), text_(root), toLogical_(std::move(toLogical)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDropTarget)) {
      *out = static_cast<IDropTarget*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG) InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&refs_);
    if (n == 0) delete this;
    return (ULONG) n;
  }

  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
    if (detached_) return refuse(effect);
    files_.clear();
    text_.dragExit();
    hasText_ = false;
    if (!oleReadFiles(data, &files_)) {
      files_.clear();
      hasText_ = oleReadText(data, &dragged_);
    }
    return over(pt, effect);
  }

  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
    if (detached_) return refuse(effect);
    return over(pt, effect);
  }

  HRESULT STDMETHODCALLTYPE DragLeave() override {
    if (detached_) return S_OK;
    if (router_) router_->fileDragExit();
    text_.dragExit();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) override {
    if (detached_) return refuse(effect);
    // Read again: the data offered at the drop is the authority, and a source
    // may only render it now.
    std::vector<std::string> files;
    std::string text;
    const bool haveFiles = oleReadFiles(data, &files);
    const bool haveText = !haveFiles && oleReadText(data, &text);
    const Point p = toLogical_ ? toLogical_(pt.x, pt.y) : Point((float) pt.x, (float) pt.y);
    bool taken = false;
    if (haveFiles && router_) {
      router_->fileDragMove(files, p);
      // fileDragEnter is application code too, and may have closed it.
      if (!detached_) taken = router_->filesDropped(files, p);
    } else if (haveText) {
      taken = text_.drop(text, p);
    }
    if (effect) *effect = taken ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    // The drop's handler may have closed the window (and detached this).
    if (!detached_ && onChanged) onChanged();
    return S_OK;
  }

  /** Called after anything was delivered -- a hover, a drop -- so the window
   *  can repaint at once rather than at its next tick. */
  std::function<void()> onChanged;

  /**
   * The window is going. OLE holds a reference of its own for as long as a
   * drag is over the window -- and through the very Drop whose handler
   * closed it -- and goes on calling the object after it is revoked and
   * released. Every call from now on answers "no drop" and reaches nothing:
   * the router, the tree and the window these hold are about to be freed.
   */
  void detach() {
    detached_ = true;
    router_ = nullptr;
    files_.clear();
    hasText_ = false;
  }

  /** What the last DragEnter read -- for a test of the parsing. */
  const std::string& draggedText() const { return dragged_; }
  const std::vector<std::string>& draggedFiles() const { return files_; }
  TextDragDispatcher& textDispatcher() { return text_; }

private:
  virtual ~OleDropTarget() = default;

  HRESULT over(POINTL pt, DWORD* effect) {
    const Point p = toLogical_ ? toLogical_(pt.x, pt.y) : Point((float) pt.x, (float) pt.y);
    bool wanted = false;
    if (!files_.empty() && router_) wanted = router_->fileDragMove(files_, p);
    else if (hasText_) wanted = text_.dragMove(dragged_, p);
    if (effect) *effect = wanted ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (!detached_ && onChanged) onChanged(); // a hover highlight may have changed
    return S_OK;
  }

  static HRESULT refuse(DWORD* effect) {
    if (effect) *effect = DROPEFFECT_NONE;
    return S_OK;
  }

  MouseRouter* router_;
  bool detached_ = false;
  TextDragDispatcher text_;
  ToLogical toLogical_;
  std::vector<std::string> files_;
  std::string dragged_;
  bool hasText_ = false;
  LONG refs_ = 1;
};

/**
 * Register a drop target on `hwnd`, or return null and leave the window on
 * WM_DROPFILES. `*oleInitialised` says whether OleUninitialize is owed.
 */
inline OleDropTarget* registerOleDropTarget(HWND hwnd, MouseRouter& router, Component& root,
                                            OleDropTarget::ToLogical toLogical,
                                            bool* oleInitialised) {
  if (oleInitialised) *oleInitialised = false;
  const Ole32Api& api = ole32();
  if (!hwnd || !api.ok() || !api.coGetApartmentType) return nullptr;
  // Only on a thread the HOST already made a single-threaded apartment --
  // which every host that does drag and drop itself has. A thread with no
  // apartment, or in the multithreaded one, is left exactly as it was: making
  // it an STA from inside a plugin would change how every COM call on it is
  // dispatched -- UI Automation's calls into the accessibility provider start
  // being marshalled through this thread's message loop, which a host (or a
  // test) that is not pumping at that moment never answers. Such a window
  // stays on WM_DROPFILES, as it always was.
  APTTYPE apartment = APTTYPE_MTA;
  APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
  if (FAILED(api.coGetApartmentType(&apartment, &qualifier))) return nullptr;
  if (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) return nullptr;
  // On an STA, OleInitialize adds OLE's part without changing the apartment.
  // S_OK or S_FALSE both owe a balancing OleUninitialize at close.
  const HRESULT init = api.oleInitialize(nullptr);
  if (FAILED(init)) return nullptr;
  auto* target = new OleDropTarget(router, root, std::move(toLogical));
  if (FAILED(api.registerDragDrop(hwnd, target))) {
    target->Release();
    api.oleUninitialize();
    return nullptr;
  }
  if (oleInitialised) *oleInitialised = true;
  return target;
}

inline void revokeOleDropTarget(HWND hwnd, OleDropTarget*& target, bool& oleInitialised) {
  const Ole32Api& api = ole32();
  if (target) {
    if (hwnd && api.revokeDragDrop) api.revokeDragDrop(hwnd);
    target->detach(); // OLE may hold it still; see detach()
    target->Release();
    target = nullptr;
  }
  if (oleInitialised && api.oleUninitialize) api.oleUninitialize();
  oleInitialised = false;
}

} // namespace detail

/**
 * Drag `text` out of the editor, to wherever the user lets go -- another
 * application's text field, the desktop, another plugin.
 *
 * Call from a component's mouseDrag once it has decided the gesture is a text
 * drag. Runs the system's drag loop and returns when it ends: true if
 * something took the text. Windows only; elsewhere it returns false.
 */
inline bool performExternalDragDropOfText(const std::string& text) {
  const detail::Ole32Api& api = detail::ole32();
  if (!api.ok() || text.empty()) return false;
  const HRESULT init = api.oleInitialize(nullptr);
  if (FAILED(init)) return false;
  auto* data = new detail::OleTextDataObject(text);
  auto* source = new detail::OleDropSource();
  DWORD effect = DROPEFFECT_NONE;
  const HRESULT result = api.doDragDrop(data, source, DROPEFFECT_COPY, &effect);
  data->Release();
  source->Release();
  api.oleUninitialize();
  return result == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE;
}

#else // !_WIN32

inline bool performExternalDragDropOfText(const std::string& text) {
  (void) text;
  return false;
}

#endif

// ═══════════════════════════════════════════════════════════════════════════
// macOS: the dragging pasteboard
// ═══════════════════════════════════════════════════════════════════════════

#if defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)

namespace detail {

/** The plain text on a drag's pasteboard, if it carries any:
 *  NSPasteboardTypeString, whose value is public.utf8-plain-text. */
inline bool cocoaDraggedText(id sender, std::string* out) {
  if (!sender || !out) return false;
  using IdFn = id (*)(id, SEL);
  using IdStrFn = id (*)(id, SEL, const char*);
  using IdIdFn = id (*)(id, SEL, id);
  using CStrFn = const char* (*)(id, SEL);
  id board = reinterpret_cast<IdFn>(objc_msgSend)(sender, sel_registerName("draggingPasteboard"));
  if (!board) return false;
  id type = reinterpret_cast<IdStrFn>(objc_msgSend)((id) objc_getClass("NSString"),
                                                    sel_registerName("stringWithUTF8String:"),
                                                    "public.utf8-plain-text");
  if (!type) return false;
  id string = reinterpret_cast<IdIdFn>(objc_msgSend)(board, sel_registerName("stringForType:"), type);
  if (!string) return false;
  const char* utf8 = reinterpret_cast<CStrFn>(objc_msgSend)(string, sel_registerName("UTF8String"));
  if (!utf8) return false;
  *out = utf8;
  return true;
}

/** The pasteboard types a view registers for: the file types it always had,
 *  and plain text. An NSArray, autoreleased. */
inline id cocoaDragTypes() {
  using IdStrFn = id (*)(id, SEL, const char*);
  // arrayWithObjects:count:, never the nil-terminated arrayWithObjects: --
  // that one is VARIADIC, and on Apple silicon variadic arguments travel on
  // the stack where a fixed-signature objc_msgSend call puts them in
  // registers. It would compile everywhere and read garbage on an M1.
  using ArrayFn = id (*)(id, SEL, const id*, unsigned long);
  id cls = (id) objc_getClass("NSString");
  id files = reinterpret_cast<IdStrFn>(objc_msgSend)(cls, sel_registerName("stringWithUTF8String:"),
                                                     "NSFilenamesPboardType");
  id text = reinterpret_cast<IdStrFn>(objc_msgSend)(cls, sel_registerName("stringWithUTF8String:"),
                                                    "public.utf8-plain-text");
  if (!files || !text) return nullptr;
  const id both[2] = {files, text};
  return reinterpret_cast<ArrayFn>(objc_msgSend)((id) objc_getClass("NSArray"),
                                                 sel_registerName("arrayWithObjects:count:"), both,
                                                 2ul);
}

} // namespace detail

#endif

} // namespace gfx
} // namespace sonore
