# Third-party notices

The Sonore SDK is Apache-2.0 (see `LICENSE`). Its own code (everything under
`sdk/include/sonore`, `sdk/examples` and `sdk/tests`) is original work, but
for the text shaping below that follows HarfBuzz.

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

Unicode Character Database data, © 1991–2022 Unicode, Inc., under the
[Unicode License v3](https://www.unicode.org/license.txt): the tables generated
into `sdk/include/sonore/gfx/bidi.h`, `gfx/text_shaper.h` and
`gfx/line_break.h` (bidi classes, paired brackets, mirroring, Arabic joining
types, scripts, combining classes, canonical decompositions and compositions,
line breaking classes -- see `scripts/unicode/`), and
`sdk/tests/data/bidi/BidiCharacterTest-15.0.0-sample.txt` and
`sdk/tests/data/linebreak/LineBreakTest-15.0.0-sample.txt`, samples of
Unicode's bidi and line breaking conformance files. The licence asks that its notice travel with copies of
the data, which this paragraph and the files' own headers do.

HarfBuzz, © 2010–2023 Google, Inc., Behdad Esfahbod, Red Hat, Inc. and the
others its `COPYING` names, under the "Old MIT" licence: the shapers in
`sdk/include/sonore/gfx/text_shaper.h` for Thai and Lao, Hangul, the Indic
scripts, Khmer, Myanmar and the Universal Shaping Engine follow HarfBuzz's
closely, and the syllable grammars and the Indic and Universal categories
`scripts/unicode/gen_syllables.py`, `gen_indic.py` and `gen_use.py`
regenerate are HarfBuzz's. The licence asks that its copyright
notice and two paragraphs appear in all copies; they are at the head of
`text_shaper.h`, which carries them wherever the header goes.

Microsoft's `IndicShapingInvalidCluster.txt`,
`IndicSyllabicCategory-Additional.txt` and
`IndicPositionalCategory-Additional.txt`, © Microsoft Corporation, under the
MIT licence (`scripts/unicode/ms-use/COPYING`): the sequences of a vowel
letter and a sign that look like another letter, and the Universal Shaping
Engine's corrections to the UCD's categories, generated into
`text_shaper.h` as `kInvalidClusters` and `kUseRanges`.

Full licence texts live beside the code: `clap/LICENSE`, `lv2/COPYING`,
`vst3/LICENSE.txt`, and inline at the foot of `minimp3.h` / `stb_vorbis.c`.
An installed SDK (`cmake --install`) carries `minimp3.h` and `stb_vorbis.c`
in `include/sonore/third_party/`, licences inline as above.

The vendored `stb_vorbis.c` carries TWO local changes, each marked `SONORE
LOCAL FIX` in the source, both in the error path a corrupt header takes:
`vorbis_deinit` guards its comment-list loop against the list being null
(the allocation of the list failed), and `start_decoder` zeroes the list
after allocating it (the allocation of an ENTRY failed, and the cleanup was
freeing the uninitialised entries after it). Everything else is upstream
v1.22 byte for byte. Both were found by `tests/fuzz_parsers.cpp`.

`minimp3` and `stb_vorbis` are compiled inside an **anonymous namespace**, so
each translation unit gets an internal-linkage copy: a header-only SDK that
emitted external symbols from vendored C would collide the moment a plugin was
built from two files. For that, both files carry one more local change, marked
`SONORE LOCAL CHANGE`: their `extern "C"` blocks are skipped when
`SONORE_CODEC_CPP_LINKAGE` is defined, which `audiofile_codecs.h` does, because
C language linkage made MSVC give the functions external linkage even inside
the anonymous namespace.

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
- **Avid AAX SDK**: Avid's, under the AAX SDK licence or the GPLv3, given to
  registered developers at developer.avid.com/aax. Never vendored here:
  `sonore_add_aax()` builds against the copy `SONORE_AAX_SDK_ROOT` names and
  skips, saying why, without one. `aax_wrapper.h` is our own code. A plugin
  built with it links Avid's AAX library, so its builder is bound by the
  licence they accepted when they downloaded the SDK.
- **Apple SDKs**: used only by the optional cross-compilation images. Never
  push an image containing them to a public registry.

## Format obligations a licence cannot grant you

Apache-2.0 governs *this code*. It cannot grant rights the format owners hold:

- **VST3**: the C API header is BSD-3-Clause. Since VST 3.8 (October 2025)
  Steinberg asks for no licence agreement to distribute a VST3, commercially
  or not; the VST name and logo are optional, and using them means following
  Steinberg's usage guidelines.
- **VST2**: impossible on purpose. Steinberg ended VST2 licensing in October
  2018 and no longer distributes the SDK; `sonore_add_vst2()` exists solely to
  fail with that explanation so nobody mistakes its absence for an oversight.
- **AAX**: a retail Pro Tools loads only a plugin Avid has approved and PACE
  has signed. Ask audiosdk@avid.com for the signing tools, and sign every
  build with `wraptool` before shipping it; a developer build of Pro Tools
  loads unsigned plugins for testing.
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

Licensing or naming questions (including whether a particular use of the name
is fine) go to **legal@sonorie.com**. Bugs and features belong in the issue
tracker, and vulnerabilities in `SECURITY.md`.
