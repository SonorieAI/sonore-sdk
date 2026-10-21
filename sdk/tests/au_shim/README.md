# A shim of Apple's declarations, so a compiler can read the macOS files

It started as a shim for `au_wrapper.h` alone. It now also carries an
Objective-C runtime (`objc/`), which is what lets a compiler read
`webview_cocoa.h` -- every Mac plugin's editor today -- and
`gfx/window_cocoa.h`, the native peer meant to replace it. And it carries
ExtAudioFile, the device-property calls behind the system volume, and
(`msvc/spawn.h`, `msvc/sys/wait.h`, `crt_externs.h`) the process-launching
names, for the Mac halves of `system_audio_format.h`, `system_volume.h` and
`lame_encoder.h` -- see `mac_syntax_audio_io.cpp`. Everything below applies
to all of them equally.

`au_view.h` and the Mac halves of the wrappers are not covered by the
one-file checks: `clap_wrapper.h` selects its webview backend, its window API
and its parent-handle cast on `_WIN32` first, so on a Windows compiler the
chain takes the Windows branches. The MSVC build covers them another way, with
no change to production headers: `sonore_mac_syntax_plugin` and
`sonore_mac_syntax_standalone` (`tests/mac_syntax_*.cpp`) compile a whole
plugin -- every format, the Cocoa editor, `au_view.h` -- and the standalone
with `__APPLE__` defined and `_WIN32` undefined, against this directory and
`msvc/`, the few POSIX names MSVC lacks. Same proof, same limits, as below.

The hosting side came later: `sonore_mac_syntax_mac_host`
(`tests/mac_syntax_mac_host.cpp`) compiles `host_au.h` -- hosting an Audio
Unit -- with `audio_workgroup.h`, the realtime `ThreadPool`, `apple_block.h`
and `gfx/nsview_component.h`. That brought the AU client calls, the event
listener, `os/workgroup.h`, three Mach headers (`mach/`) and `pthread.h` and
`dirent.h` for MSVC. It also found the shim WRONG: five parameter-unit numbers
and three parameter-flag bits did not match Apple's, which had cost nothing
while only `au_wrapper.h` read them by name and would have the moment anything
restated them as numbers. `tests/mac_host_constants.h` now lists every number
the hosting code restates beside Apple's name for it; against this shim it
proves the two copies agree, and on a Mac (`tests/mac_host_selftest.cpp`) it is
the real check.

## What this is

`au_wrapper.h` is a thousand lines that had been edited for many weeks
without a compiler ever looking at them. macOS CI is the only machine that
could, and it has not started a job since the account's spending limit was
reached. Every run since then reports:

> The job was not started because recent account payments have failed or
> your spending limit needs to be increased.

So "does this file even parse" was genuinely unknown, and every blind edit
was stacked on top of the last one.

## What this proves, and what it does not

It proves the file is **internally consistent**: that every name it uses
exists, every call has the right number of arguments, every struct member it
touches is one it declared, and the whole thing compiles.

It does **not** prove the ABI is right. These declarations are written from
Apple's published signatures, and if one of them is wrong here then
`au_wrapper.h` compiles happily against a fiction. A shim can only ever be as
correct as the person writing it believed Apple's headers to be.

That is worth stating plainly rather than letting a green check imply more
than it earned. The value is in the class of error it *does* catch (typos,
wrong argument counts, members that were renamed, functions whose signature
drifted), which is exactly what accumulates in code nobody compiles.

## Rules for this directory

- **Never** put it on the include path of a real build. It exists for
  `SONORE_APPLE_SYNTAX_CHECK` and nothing else. A build that picked these up
  instead of Apple's would produce a `.component` that cannot load.
- Declare only what `au_wrapper.h` actually uses. A shim that grows beyond
  its caller is a second implementation nobody asked for.
- When a declaration here differs from Apple's, the shim is wrong. Fix the
  shim, not the wrapper.
