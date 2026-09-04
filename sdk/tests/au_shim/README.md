# A shim of Apple's declarations, so a compiler can read the macOS files

It started as a shim for `au_wrapper.h` alone. It now also carries an
Objective-C runtime (`objc/`), which is what lets a compiler read
`webview_cocoa.h`, every Mac plugin's editor today, and
`gfx/window_cocoa.h`, the native peer meant to replace it. Everything below
applies to all of them equally.

`au_view.h` is deliberately NOT covered. It needs `clap_wrapper.h`, which
selects its webview backend, its window API and its parent-handle cast on
`_WIN32` first, so the chain would compile the Windows branches and report
success for macOS code it never read. Forcing the Apple branch at five points
in production headers, each a place a real build could break for the benefit of
a test, is not worth it for one file. It is named here rather than quietly
counted as covered.

## What this is

The macOS-facing headers are the ones a Windows or Linux machine cannot
compile. macOS CI builds and validates them on every push, but a CI round
trip costs minutes and a typo costs seconds. This shim lets any machine
answer "does this file still parse" with `-fsyntax-only`, before the code
ever reaches a Mac.

## What this proves, and what it does not

It proves the file is **internally consistent**: that every name it uses
exists, every call has the right number of arguments, every struct member it
touches is one it declared, and the whole thing compiles.

It does **not** prove the ABI is right. These declarations are written from
Apple's published signatures, and if one of them is wrong here then
`au_wrapper.h` compiles happily against a fiction. A shim can only ever be as
correct as the person writing it believed Apple's headers to be.

The value is in the class of error it *does* catch: typos, wrong argument
counts, members that were renamed, functions whose signature drifted. Those
are what a syntax check is for. The ABI is proved on macOS CI, by `auval`.

## Rules for this directory

- **Never** put it on the include path of a real build. It exists for
  `SONORE_APPLE_SYNTAX_CHECK` and nothing else. A build that picked these up
  instead of Apple's would produce a `.component` that cannot load.
- Declare only what `au_wrapper.h` actually uses. A shim that grows beyond
  its caller is a second implementation nobody asked for.
- When a declaration here differs from Apple's, the shim is wrong. Fix the
  shim, not the wrapper.
