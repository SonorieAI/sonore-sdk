// SPDX-License-Identifier: Apache-2.0
// A shim of <objc/objc.h>, so a compiler can read the Cocoa files.
//
// webview_cocoa.h, au_view.h and window_cocoa.h are roughly fifteen hundred
// Apple-facing lines that no Windows or Linux compiler can read. macOS CI
// builds them on every push; this shim answers the same question in seconds,
// on any machine. Everything the AU shim's README says applies here word for
// word: this proves internal consistency, never ABI correctness.
//
// The Objective-C runtime is a plain C API, which is the only reason a shim of
// it is even possible. Every declaration below is copied from Apple's published
// signature; if one of them is wrong, the file above it compiles against a
// fiction.
#pragma once

#if !defined(SONORE_APPLE_SYNTAX_CHECK)
#error "au_shim is for SONORE_APPLE_SYNTAX_CHECK only -- never on a real include path"
#endif

#include <stddef.h>

typedef struct objc_class* Class;
typedef struct objc_object {
  Class isa;
}* id;
typedef struct objc_selector* SEL;
typedef id (*IMP)(id, SEL, ...);

// On Apple this is `signed char`. Here it is whatever the host platform's own
// BOOL is, because this check runs on Windows, where clap_wrapper.h reaches
// windows.h and windows.h has already said `typedef int BOOL`. C++ allows a
// typedef to be repeated identically and rejects it otherwise, so matching is
// the only way both headers can be in one translation unit.
//
// It costs nothing that matters: the size of a BOOL is not something a syntax
// check can verify anyway, and the ABI caveat in the README already covers it.
#if defined(_WIN32)
typedef int BOOL;
#else
typedef signed char BOOL;
#endif
#define YES ((BOOL) 1)
#define NO ((BOOL) 0)

// A real nil is `(id)0`; this spelling keeps `id x = nil;` and `x == nil` both
// valid in C++ without dragging in <objc/objc-api.h>.
#if !defined(nil)
#define nil ((id) 0)
#endif
#if !defined(Nil)
#define Nil ((Class) 0)
#endif
