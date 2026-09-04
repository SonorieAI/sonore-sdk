// SPDX-License-Identifier: Apache-2.0
//
// The system clipboard.
//
// ── Why this is not optional ────────────────────────────────────────────────
//
// TextEditor had select-all and no copy, cut or paste. A field somebody cannot
// paste a preset name or a file path into is not a text field -- it is a shape
// that looks like one. Every other control in this SDK degrades gracefully when
// something is missing; a text field without the clipboard just does not work.
//
// ── The three platforms are genuinely different ─────────────────────────────
//
// Windows: a global object you open, read and close. Ownership is not a thing.
//
// macOS: NSPasteboard, likewise a global with a change count.
//
// X11: there is no clipboard. There is a SELECTION, which is a protocol -- the
// application that copied still OWNS the text and must answer requests for it,
// and when it quits the clipboard goes with it. That is why pasting on X11 is
// asynchronous and why copying means agreeing to serve data later. A plugin
// that ignored this would appear to copy and then paste nothing.
//
// The interface here hides none of that in the sense that matters: setText is
// best-effort everywhere, and getText can genuinely fail on X11 if nothing is
// serving, which is a real state and not an error.
#pragma once

#include <string>

#if defined(_WIN32)
// windows.h defines min and max as MACROS, which turn any later std::min or
// std::max into a syntax error -- and only when this header happens to be
// included FIRST. That made it an include-order landmine rather than a bug:
// gfx/viewport.h compiled for months and then stopped the day plugin_editor.h
// gained one more include, because the new one reached windows.h before
// viewport.h was seen.
//
// WIN32_LEAN_AND_MEAN for the ordinary reason: winsock, RPC, OLE and the shell
// are a large amount of preprocessing nobody here asked for.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace sonore {
namespace gfx {

#if defined(_WIN32)

/**
 * The Windows clipboard.
 *
 * Every call opens and closes it. Holding it open would block every other
 * application on the desktop from copying, and a plugin holding a global lock
 * because somebody left an editor open is not acceptable.
 */
struct Clipboard {
  /**
   * OpenClipboard, with retries.
   *
   * Only one process may have the clipboard open at a time, and something is
   * always touching it -- a clipboard manager, the shell, another plugin
   * window. A single attempt fails intermittently, which is exactly the
   * "sometimes copy doesn't work" complaint nobody can ever reproduce.
   *
   * Found by a test: two copies in a row, and the second returned false. It
   * would have shipped, because a copy that works nineteen times in twenty
   * looks like a copy that works.
   *
   * Ten tries at a millisecond apart. Longer would be a visible stall on a
   * keystroke; shorter loses to a manager that holds it for a frame.
   */
  static bool openWithRetry() {
    for (int attempt = 0; attempt < 10; ++attempt) {
      if (OpenClipboard(nullptr)) return true;
      Sleep(1);
    }
    return false;
  }

  static bool setText(const std::string& utf8) {
    if (!openWithRetry()) return false;
    EmptyClipboard();

    const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int) utf8.size(), nullptr, 0);
    // GMEM_MOVEABLE, and the clipboard OWNS it afterwards -- freeing it here
    // would be a double free the moment anything pasted.
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, ((size_t) wide + 1) * sizeof(wchar_t));
    if (!handle) {
      CloseClipboard();
      return false;
    }
    wchar_t* buffer = (wchar_t*) GlobalLock(handle);
    if (!buffer) {
      GlobalFree(handle);
      CloseClipboard();
      return false;
    }
    if (wide > 0)
      MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int) utf8.size(), buffer, wide);
    buffer[wide] = L'\0';
    GlobalUnlock(handle);

    const bool ok = SetClipboardData(CF_UNICODETEXT, handle) != nullptr;
    if (!ok) GlobalFree(handle); // only ours to free if the clipboard refused it
    CloseClipboard();
    return ok;
  }

  static bool getText(std::string* out) {
    if (!out) return false;
    out->clear();
    // UNICODE, not CF_TEXT: the ANSI form is in the system code page and turns
    // an accented preset name into question marks.
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return false;
    if (!openWithRetry()) return false;

    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
      CloseClipboard();
      return false;
    }
    const wchar_t* text = (const wchar_t*) GlobalLock(handle);
    if (!text) {
      CloseClipboard();
      return false;
    }
    const int length = (int) wcslen(text);
    if (length > 0) {
      const int bytes =
          WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
      out->resize((size_t) bytes);
      WideCharToMultiByte(CP_UTF8, 0, text, length, &(*out)[0], bytes, nullptr, nullptr);
    }
    GlobalUnlock(handle);
    CloseClipboard();
    return true;
  }
};

#else

/**
 * Everywhere else, the clipboard is reached through the window peer.
 *
 * On X11 it has to be: a selection belongs to a WINDOW, and serving it means
 * answering events on that window's connection. There is no free function that
 * can do it. macOS could have one, but two shapes for one idea is worse than
 * one shape that fits both.
 *
 * So this is a holder the peers fill in, and TextEditor asks it rather than the
 * operating system. A build with no peer gets an in-process clipboard, which is
 * exactly right for a test: copy and paste work, and nothing outside sees them.
 */
struct Clipboard {
  /** Set by a window peer when one opens. Null means the fallback below. */
  static void (**setter())(const std::string&) {
    static void (*fn)(const std::string&) = nullptr;
    return &fn;
  }
  static bool (**getter())(std::string*) {
    static bool (*fn)(std::string*) = nullptr;
    return &fn;
  }

  static std::string& fallbackStore() {
    static std::string text;
    return text;
  }

  static bool setText(const std::string& utf8) {
    fallbackStore() = utf8;
    if (*setter()) (*setter())(utf8);
    return true;
  }

  static bool getText(std::string* out) {
    if (!out) return false;
    if (*getter() && (*getter())(out)) return true;
    // Nothing is serving a selection -- which on X11 is a real state, not an
    // error: the application that copied may have quit, and its clipboard went
    // with it.
    *out = fallbackStore();
    return !out->empty();
  }
};

#endif

} // namespace gfx
} // namespace sonore
