// SPDX-License-Identifier: Apache-2.0
// Compile the macOS UI files. Nothing here runs -- the compiler reading them IS
// the test.
//
// webview_cocoa.h is what every macOS plugin's editor is made of today, and
// window_cocoa.h is the native peer that will replace it as the default. No
// compiler had ever looked at either: macOS CI is the only machine that could,
// and it has not started a job since the account's spending limit was reached.
// So "does the editor a Mac user opens even parse" was genuinely unknown, and
// every blind edit was stacked on the last.
//
// See au_shim/README.md for exactly what a green run here proves. Short
// version: internal consistency -- every name exists, every call has the right
// number of arguments, every struct member is one that was declared -- and NOT
// ABI correctness. The Objective-C runtime shim next door is written from
// Apple's published signatures, and if one of them is wrong then the file above
// it compiles happily against a fiction.
//
// ── What is NOT covered here, and why ───────────────────────────────────────
//
// au_view.h. It needs au_wrapper.h and clap_wrapper.h, and clap_wrapper.h
// selects its webview backend, its window API and its parent-handle cast on
// _WIN32 FIRST -- so on this machine the chain compiles the Windows branches
// and would report success for macOS code it never read. That is the exact
// failure midi_input.h already carries a comment about.
//
// Fixing it means forcing the Apple branch at five separate points in
// production headers, each one a place a real Windows or Linux build could
// break for the benefit of a test. Not worth it for the two files that are
// self-contained anyway. au_view.h is compiled instead by the MSVC build's
// sonore_mac_syntax_plugin target, which forces the platform from the command
// line rather than in the headers: see tests/mac_syntax_plugin.cpp.
#include <sonore/gfx/window_cocoa.h>
#include <sonore/webview_cocoa.h>
// The display link (CVDisplayLink through dlsym, the main-queue hop) and the
// kinetic scroller that ticks from it. window_cocoa.h already brings in the
// tablet (mouse_input.h) and pasteboard-text (text_drag.h) halves.
#include <sonore/gfx/animated_position.h>
#include <sonore/gfx/vblank.h>

int main() {
  // Named so nothing above can be discarded as unused before the compiler has
  // read it. Nothing is CALLED: there is no Objective-C runtime here to call
  // into, and no Mac to call it on.
  const void* peer = (const void*) &sonore::gfx::NativeWindowCocoa::isAvailable;
  const void* view = (const void*) &sonore::gfx::sonoreViewDrawRect;
  const void* web = (const void*) &sonore::cocoa::nsString;
  const void* pen = (const void*) &sonore::gfx::detail::pointerInputFromNSEvent;
  const void* text = (const void*) &sonore::gfx::detail::cocoaDraggedText;
  const void* refresh = (const void*) &sonore::gfx::VBlankAttachment::displayRefreshRate;
  return (peer && view && web && pen && text && refresh) ? 0 : 1;
}
