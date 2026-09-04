# Third-party notices

The Sonore SDK is Apache-2.0 (see `LICENSE`). Its own code, everything under
`sdk/include/sonore`, `sdk/examples` and `sdk/tests`, is original work.

This file lists everything that is **not** ours: code vendored into the tree,
code fetched at build time, and the format obligations that no software
licence can grant you.

## Vendored in this repository

These are committed under `sdk/third_party/`, unmodified except where noted.
Each retains its own licence, which travels with any copy you distribute.

| Component | Path | Licence | Copyright |
| --- | --- | --- | --- |
| CLAP plugin headers (v1.2) | `sdk/third_party/clap/` | MIT | © 2021 Alexandre BIQUE |
| LV2 specification headers | `sdk/third_party/lv2/` | ISC | © 2006–2012 Steve Harris, David Robillard; © 2000–2002 Richard W.E. Furse, Paul Barton-Davis, Stefan Westerfeld |
| VST3 C API (`vst3_c_api.h`) | `sdk/third_party/vst3/` | BSD-3-Clause | © 2025 Steinberg Media Technologies GmbH |
| minimp3 | `sdk/third_party/minimp3.h` | CC0 (public domain dedication) | lieff |
| stb_vorbis (v1.22) | `sdk/third_party/stb_vorbis.c` | MIT **or** public domain (Unlicense), at your option | Sean Barrett |

Full licence texts live beside the code: `clap/LICENSE`, `lv2/COPYING`,
`vst3/LICENSE.txt`, and inline at the foot of `minimp3.h` / `stb_vorbis.c`.

The vendored `stb_vorbis.c` carries TWO local changes, each marked `SONORE
LOCAL FIX` in the source, both in the error path a corrupt header takes:
`vorbis_deinit` guards its comment-list loop against the list being null
(the allocation of the list failed), and `start_decoder` zeroes the list
after allocating it (the allocation of an ENTRY failed, and the cleanup was
freeing the uninitialised entries after it). Everything else is upstream
v1.22 byte for byte. Both were found by `tests/fuzz_parsers.cpp`.

`minimp3` and `stb_vorbis` are compiled inside an **anonymous namespace**, so
each translation unit gets an internal-linkage copy. A header-only SDK that
emitted external symbols from vendored C would collide the moment a plugin was
built from two files.

## Fetched at build time, never vendored

| Component | How | Licence |
| --- | --- | --- |
| Microsoft WebView2 SDK | CMake `FetchContent` (`sdk/cmake/FetchWebView2.cmake`), Windows GUI builds only | BSD-3-Clause, redistributable |

It is fetched rather than committed because the static loader is ~10 MB per
architecture. Linking is static, so a shipped `.clap` needs no DLL beside it.

## Deliberately absent: do not commit these

Three SDKs this framework can *talk to* are not redistributable. The build
system skips each of them loudly and names where to obtain it; nothing here
will silently fail.

- **Steinberg ASIO SDK**: required by `sonore_enable_asio()`. Not
  redistributable; obtain it from Steinberg under their licence.
  `sdk/third_party/asio/` is git-ignored for this reason.
- **Avid AAX SDK**: requires Avid's developer agreement, and shipping AAX
  binaries requires PACE signing. `sonore_add_aax()` is a hook that explains
  this and stops.
- **Apple SDKs**: used only by the optional cross-compilation images. Never
  push an image containing them to a public registry.

## Format obligations a licence cannot grant you

Apache-2.0 governs *this code*. It cannot grant rights the format owners hold:

- **VST3**: the C API header is BSD-3-Clause, but Steinberg requires
  registration for commercial distribution of VST3 products, under their VST3
  licensing terms. Read them before selling a VST3 built with this SDK.
- **VST2**: impossible on purpose. Steinberg ended VST2 licensing in October
  2018 and no longer distributes the SDK; `sonore_add_vst2()` exists solely to
  fail with that explanation so nobody mistakes its absence for an oversight.
- **AAX**: Avid agreement plus PACE signing, as above.
- **Audio Units**: building and distributing AUs is governed by Apple's
  developer terms for the platform you ship on.

CLAP and LV2 carry no such obligation: both are free formats, which is why the
SDK treats CLAP as its reference target.

## Trademarks

"Sonore", "Sonorie" and the Sonorie logo are not licensed by Apache-2.0
(section 6). You may state that your product is built with the Sonore SDK; you
may not name a fork or a derived product in a way that suggests it is the
official one.

VST is a trademark of Steinberg Media Technologies GmbH. Audio Units and macOS
are trademarks of Apple Inc. All other trademarks are the property of their
respective owners, and are used here only to identify the formats this SDK
can build.

Licensing or naming questions, including whether a particular use of the name
is fine, go to **legal@sonorie.com**. Bugs and features belong in the issue
tracker, and vulnerabilities in `SECURITY.md`.
