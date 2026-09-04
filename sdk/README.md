# Sonore SDK

Sonorie's own audio-plugin framework. Header-only C++17, no framework
dependency, and **ours**, which is the point.

Questions, or a measurement that disagrees with one of ours:
**[Discord](https://discord.gg/4bvsn8r8CG)**.

## Why this exists

Sonorie used to generate projects against a third-party framework, and the
commercial terms of that kind of framework do not survive contact with this
business. They forbid building a product that creates other products, which is
precisely what a SaaS that generates plugins is, so no standard tier covers the
model, and the copyleft alternative would make every wasm preview we serve and
every binary our build farm ships a copyleft work, incompatible with selling
closed plugins on the marketplace.

Owning the framework removes the question entirely: the only licence that
governs a generated plugin is the one *we* choose.

It also happens to be much smaller. A built example:

| Artifact | Size |
| --- | --- |
| `SonoreSaturator.clap` (native, stereo saturator) | 18.5 KB |
| `saturator.wasm` (same DSP, browser preview) | 10.6 KB |

## What's in it

```
sdk/
  include/sonore/
    audio.h           ProcessSpec, AudioBlock, MidiMessage, MidiBuffer
    dsp.h             the toolkit's entry point (pulls in the three below)
      effects.h       reverb, chorus, phaser, limiter, gate, crossover, panner
      transport.h     tempo, bar position, note lengths, tempo-synced LFO
      fft.h           FFT, windows, spectrum analyser, partitioned convolution
      resample.h      oversampling (FIR + minimum-phase IIR), rate conversion
      shaping.h       waveshaper, lookup table, ballistics, processor chain
      fir.h           FIR design to a spec (windowed sinc)
      metering.h      BS.1770 K-weighting, LUFS (M/S/I) + LRA, true peak, correlation
      dynamics.h      expander, upward compressor, transient shaper, de-esser
      lofi.h          bit crusher, decimator, wavefolder, Oversampled<> wrapper
      fdn.h           feedback-delay-network reverb (Jot / Hadamard)
      pitch.h         YIN pitch detector, two-head pitch shifter
      stft.h          a processable STFT frame (spectral gate/freeze/vocoder substrate)
      synth.h         wavetable (mip-mapped), unison, hard sync, FM operator,
                      DAHDSR, Karplus-Strong string, modal resonator
      tone.h          tilt EQ, the Baxandall NETWORK (symbolic, 4th order)
      delay.h         stereo delay: crossfade/glide time changes, ping-pong, tempo sync
      tape.h          Jiles-Atherton hysteresis, head loss by speed, wow and flutter
      tube.h          Koren triode stage, '59 Bassman tone stack
      crossover.h     multiband splitter (2..8 bands) that sums flat
      va_filters.h    diode ladder (303), Sallen-Key (Korg35 / MS-20)
      granular.h      grain engine: live or loaded buffer, pitch, freeze, reverse
      phase_vocoder.h pitch shifting by peak shifting (Laroche-Dolson)
      spring.h        spring reverb: dispersive allpass chain in a transit loop
      vocoder.h       channel vocoder, formant (vowel) filter
      compressors.h   opto (LA-2A T4), FET (1176, loop solved per sample), VCA (dbx/SSL)
      eq.h            8-band parametric on Vicanek matched biquads, optional linear phase
      clipper.h       the diode-clipper ODE (Yeh/Abel/Smith), trapezoidal + Newton, 8x
      hilbert.h       elliptic allpass Hilbert pair, single-sideband frequency shifter
      modulation.h    LFO (six shapes), tremolo, auto-pan, vibrato in cents, ring mod, flanger
      multiband.h     N-band downward + upward (OTT) dynamics on the phase-matched splitter
      reflections.h   image-source early reflections (Allen-Berkley), two ears
      pitch_correct.h scale-locked pitch correction: YIN -> cents -> phase vocoder
      distortion.h    the drive object: curve at 8x, DC blocker, equal-RMS auto gain, tone
      circuit.h       nodal DK engine: diodes, BJTs, JFETs, ideal op-amps; Newton on ports
      fuzz.h          the Fuzz Face solved whole on the circuit engine (germanium PNP pair)
      passive_eq.h    Pultec-style program EQ: the boost+cut trick, coupled bandwidth
      dynamic_eq.h    per-band compression on matched bells (the surgical tool)
      limiting.h      true-peak limiter: BS.1770-4 4x detection, dBTP ceiling
      plate.h         the Dattorro 1997 plate, paper-exact delays and taps
      screamer.h      the TS-808 drive stage as its netlist (op-amp + diode pair)
    filter_design.h   Butterworth / Chebyshev cascades to an order
    random.h          white, pink, velvet noise, dither
    simd.h            SSE2 / NEON / wasm-simd128 / scalar, 4 floats at a time
    plugin.h          ParamInfo, PluginDescriptor, value formatting/parsing
    presets.h         factory presets, compiled in rather than files on disk
    gui.h             the window.sonore bridge, meters + VU, UI event queue
    webview_win32.h   Windows backend (Edge WebView2)
    webview_gtk.h     Linux backend (GTK3 + WebKitGTK, dlopened at runtime)
    webview_cocoa.h   macOS backend (WKWebView via the Objective-C runtime)
    clap_wrapper.h    the CLAP format wrapper (one #include = a plugin)
    vst3_wrapper.h    VST3, over Steinberg's C ABI (our code, no C++ SDK)
    au_wrapper.h      Audio Units (macOS), auval-validated on CI, see below
    au_view.h         the AU editor: a CocoaUI class registered at runtime
    lv2_wrapper.h     LV2 (+ the TTL generator, from the same source)
    wasm_abi.h        the browser ABI glue (same DSP, AudioWorklet host)
    standalone.h      a runnable app: window + faceplate + OS audio, no DAW
    audio_wasapi.h    Windows output (WASAPI shared mode, event driven)
    audio_alsa.h      Linux output (ALSA, dlopened: no dev packages)
    audio_coreaudio.h macOS output (default-output AudioUnit)
    wav.h             WAV read/write (16/24-bit PCM in, float out)
  third_party/clap/   the CLAP headers (MIT, vendored)
  third_party/vst3/   Steinberg's C ABI header (BSD-3, vendored)
  third_party/lv2/    the LV2 headers (ISC, vendored)
  cmake/              WebView2 SDK fetch (BSD-3, downloaded, never vendored)
  examples/           a saturator (effect), an 8-voice synth (instrument),
                      and a GUI probe (test fixture)
  tests/              DSP units, a real CLAP host, the wasm round-trip,
                      and gui_shot (photographs a built plugin's interface)
```

Everything under `include/sonore` is our own implementation of published,
standard DSP: RBJ's EQ cookbook coefficients, TPT/zero-delay-feedback filter
topologies, polyBLEP band limiting, Schroeder reverb elements. All of it written
from the maths, not adapted from any framework's source.

## Writing a plugin

A plugin is a `SonoreDsp` struct, a parameter table, a descriptor, and one
include. That's the whole surface:

```cpp
#define SONORE_NUM_PARAMS 2
#include <sonore/dsp.h>

struct SonoreDsp {
  sonore::Biquad lowpass;
  sonore::Smooth gainSm;
  float lastCutoff = -1.0f;

  void prepare(const sonore::ProcessSpec& spec) {
    lowpass.setSampleRate((float) spec.sampleRate);
    lowpass.reset();
    gainSm.setup((float) spec.sampleRate, 10.0f);
    lastCutoff = -1.0f;
  }

  void process(sonore::AudioBlock<float>& io, const float* p) {
    if (p[0] != lastCutoff) { lowpass.lowpass(p[0], 0.707f); lastCutoff = p[0]; }
    float* L = io.getChannelPointer(0);
    for (size_t i = 0; i < io.getNumSamples(); ++i)
      L[i] = lowpass.process(L[i]) * gainSm.next(p[1]);
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"cutoff", "Cutoff", "Hz", 20.0f, 20000.0f, 1000.0f, 0},
    {"gain",   "Gain",   "x",  0.0f,  2.0f,     1.0f,    0},
};

static const sonore::PluginDescriptor kDesc = {
    "com.example.myfilter", "My Filter", "Me", "1.0.0",
    "A filter.", "https://example.com",
    false,              // effect (true = instrument)
    kParamTable, SONORE_NUM_PARAMS,
};

#include <sonore/clap_wrapper.h>   // native plugin
```

**Instruments** take a third argument and get note ports, no audio input bus,
and instrument categorisation automatically; the wrapper detects the signature
at compile time, so effects need no changes:

```cpp
void process(sonore::AudioBlock<float>& io, const float* p, sonore::MidiBuffer& midi);
```

**Latency** is opt-in too, and matters more than it looks: a DSP that delays its
signal (a look-ahead limiter, a convolver) must declare
`int latencySamples() const`, or the host cannot time-align it and every
parallel mix it sits in is silently smeared. The wrapper reports it, and tells
the host again when it changes at a new sample rate.

**Musical time** is opt-in the same way: declare `setTransport` and the wrapper
calls it before every block, so a delay can lock to eighth notes and an LFO can
restart on the bar. A DSP that doesn't declare it pays nothing.

```cpp
void setTransport(const sonore::TransportInfo& t);
// t.tempo, t.positionBeats, t.barPhase(), t.isPlaying
// sonore::noteLengthInSamples(t, sampleRate, 8, NoteFlavour::Dotted)
// sonore::SyncedLfo: phase derived from the timeline, so a loop repeats exactly
```

The **same source** builds for the browser by including `<sonore/wasm_abi.h>`
instead of the CLAP wrapper. That is what makes the live preview bit-identical
to the shipped plugin: not a similar model of it, the same code.

## The interface

A plugin's face is an HTML document shown in an OS webview (Edge WebView2 on
Windows), wired through a `window.sonore` bridge that is **API-identical to the
studio preview's**. Define `SONORE_UI_HTML` and the same markup that was
approved in the browser opens in the DAW:

```cpp
#define SONORE_UI_WIDTH 620
#define SONORE_UI_HEIGHT 300
#define SONORE_UI_HTML R"HTML(<!doctype html>...)HTML"
#include <sonore/clap_wrapper.h>
```

Without one, the wrapper generates a plain panel from the parameter table, so a
plugin is never faceless.

What the page gets:

| API | |
| --- | --- |
| `sonore.name` / `sonore.params` | identity and the parameter table |
| `sonore.get(i)` / `set(i, v)` | plain-value access |
| `sonore.begin(i)` / `end(i)` | gestures, one undo step per drag |
| `sonore.on(cb)` | any change, from the UI *or* host automation |
| `sonore.onLevel(cb)` / `onVU(cb)` | meters at ~30 Hz |
| `sonore.format(i, v)` | the same text the host shows |
| `sonore.noteOn/noteOff` | on-screen keyboards for instruments |
| `sonore.presets` / `loadPreset(i)` | factory preset names, applied C++-side |
| `'sonore:ready'` event | fired once the bridge exists |

Turning a knob on the page emits real CLAP gesture + value events, so it records
as automation exactly like a host-side control. Edits cross to the audio thread
through a lock-free queue: never a lock, never an allocation, in the callback.

Per platform:

- **Windows**: Edge WebView2. The SDK (BSD-3-Clause) is fetched at configure
  time rather than vendored, and linked statically so a built `.clap` has no DLL
  beside it. A missing Edge runtime shows a readable message, not a black box.
- **Linux**: GTK3 + WebKitGTK, embedded through `gtk_plug_new` (XEmbed).
  Everything is **dlopened at runtime**, newest WebKit first: `webkit2gtk-4.1`
  and `4.0` cannot coexist in one process, so linking either would make the
  plugin refuse to load on half the distributions. It also means the SDK builds
  with no GTK dev packages installed.
- **macOS**: WKWebView, driven through the Objective-C **runtime** rather than
  written in Objective-C, so a generated project stays plain C++ with no `.mm`
  file and no mixed-language build.

Only *embedded* windows are supported (floating is refused); it is what every
host implements, and a second window lifetime is a second one to get wrong.

## Building

```cmake
add_subdirectory(sdk)
sonore_add_clap(MyPlugin     SOURCES plugin.cpp)  # -> MyPlugin.clap
sonore_add_vst3(MyPluginVST3 SOURCES plugin.cpp)  # -> MyPlugin.vst3
sonore_add_au(MyPluginAU     SOURCES plugin.cpp)  # -> MyPlugin.component (macOS)
sonore_add_standalone(MyApp  SOURCES plugin.cpp)  # -> a runnable MyApp
sonore_add_lv2(MyPluginLV2   SOURCES plugin.cpp)  # -> MyPluginLV2.lv2/
```

LV2's bundle metadata (manifest.ttl + plugin.ttl) is **generated by a companion
executable built from the same source**: hosts read the TTL to learn the port
layout before loading code, so metadata that disagrees with `connect_port()` is
a crash, and generating both from one descriptor makes disagreement impossible.

The standalone opens the SAME faceplate in its own window with live OS audio.
Effects process a built-in test source (a saw arpeggio, harmonically rich
enough that filters and drive are audible); instruments are played from the
on-screen keyboard the fallback page grows for them. And because an app judged
only by ear cannot be tested, it has offline modes that need no device and no
display, which is what makes it CI-testable on all three platforms:

```
MyApp --verify              # render offline, assert health, exit code = verdict
MyApp --render out.wav 2    # two seconds to a float WAV
MyApp --play 2              # live audio, no window
MyApp --shot out.bmp 5      # (Windows) open, wait, screenshot, exit
```

**The same source builds every format.** The CLAP wrapper owns the shared
machinery (the DSP instance, parameters, state, the webview bridge) and the
others adapt it, so a plugin cannot behave differently depending on which format
a host loaded. One build produces a single binary exporting both `clap_entry`
and `GetPluginFactory`.

| Format | Licence of what we depend on | Status |
| --- | --- | --- |
| CLAP | MIT headers | proven on Windows + Linux |
| VST3 | Steinberg **C ABI** header, BSD-3 | proven on Windows + Linux |
| AU | Apple frameworks | proven on macOS CI: `auval` passes all six examples, zero warnings |
| LV2 | ISC headers | proven on Windows + Linux |
| AAX | Avid SDK, **not public** | hook only, see below |

VST3 is built against Steinberg's one-file C ABI rather than their C++ SDK: the
header is BSD-3 (not the SDK's dual GPLv3/proprietary), so the wrapper is our
code and the plugin links no framework. Distributing VST3 commercially still
means registering with Steinberg; that is their licence, not the header's.

**AAX is deliberately a hook, not an implementation.** Avid's SDK is distributed
only under their developer agreement (developer.avid.com). There is no public
header, unlike VST3's C ABI, and shipping AAX binaries requires PACE signing.
`sonore_add_aax()` exists and says exactly why it is skipping; writing a wrapper
without the SDK would be fabrication, which is worse than absence. Once the SDK
is obtained, set `SONORE_AAX_SDK_ROOT` and implement against it.

## Verifying

```
npm run verify:sdk
```

Which does four things, in order of how much they prove:

1. **DSP units**: measured, not eyeballed. A lowpass must measurably cut, the
   compressor's measured ratio must match its declared one, polyBLEP must beat a
   naive saw on aliasing, an ADSR's release must hit −60 dB in its declared time.
2. **CLAP host test**: loads the *built* `.clap` as a DAW does (entry → factory
   → create → activate → process), pushes real parameter and note events, and
   round-trips state. What is tested is the shipped artifact.
3. **Instrument physics**: a synth must be silent at rest, sound on note-on,
   release back to silence, survive a chord, and accept both CLAP notes and raw
   MIDI. Graded separately from effects, because feeding a synth audio and
   demanding output would fail every correct synth.
4. **wasm round-trip**: the same DSP compiled through emscripten and driven
   through the browser's C ABI.

Plus the GUI, which is graded by the same "prove it, don't assert it" rule: the
`clap_gui_bridge` test loads the GUI probe plugin, embeds its window, pumps a
real message loop, and asserts that a parameter the *page* drove on load arrives
on the audio thread. A silently blank webview passes every extension-contract
check ever written, but not that one.

And a **Linux cross-check**: on Windows, `verify:sdk` also builds and runs the
whole suite under WSL. Building on one OS hides real bugs. MSVC leaks
`<cstddef>` and `<cstdlib>` through other headers where gcc does not, and a
Windows-only build once let `nativeWindowApi()` return `"win32"` on Linux, which
would have meant no exported Linux plugin ever showed a UI.

The WSL run covers everything except *window embedding* (Xlib headers and
WebKitGTK are dev packages). For the full Linux GUI proof (a real GTK main
loop, a real X window, a real page driving a parameter) there is a container:

```
docker build -f sdk/tests/linux-gui.Dockerfile -t sonore-linux-gui sdk
docker run --rm sonore-linux-gui
```

A build without those packages still runs and prints *"window embedding not
exercised on this build"*, so a green run is never mistaken for proof that a
window appeared.

### The sanitizers

```
npm run verify:sanitize
```

Slow and opt-in: every example built as `.clap`, `.vst3` and `.lv2` with
AddressSanitizer and UndefinedBehaviorSanitizer and driven by the same host
tests as above, the DSP suite the same way, the parser fuzzer
(`tests/fuzz_parsers.cpp`: PNG, TrueType, SVG, zip, MIDI files, FLAC, WAV,
AIFF, MP3, Ogg, JSON, OSC, the state bag, base64, Turtle, VST3 presets, the
plugin cache, with every truncation, byte flips and runs of the bytes that break
size arithmetic) at depth, the sampler example's sample handover
(`tests/sampler_stress_test.cpp`, a loading thread against a rendering
thread) and the two threads a plugin lives on (`tests/concurrency_test.cpp`:
state loads, preset loads, parameter reads, render-mode flips and interface
edits on the main thread while `process()` runs, against the saturator
compiled into the test) under ASan and then ThreadSanitizer. gcc only:
natively on Linux, through WSL on Windows. The quick passes of all three run
in `verify:sdk` through ctest, and the CLAP host test also feeds every
example's state blob back through its loader under mutation.

Parameters cross threads as relaxed atomics (`SharedParam` in
clap_wrapper.h) snapshotted once per block, which is what lets the
ThreadSanitizer leg be a gate rather than a list of known noise: a report
from it is a bug.

What else is measured rather than trusted (all of it in `verify:sdk` unless
said otherwise):

- **Events under mutation.** Every host test feeds its plugin hundreds of
  blocks of malformed events: headers whose size lies both ways, unknown
  types and spaces, offsets past the block, undeclared parameter ids, NaN and
  infinite values, notes outside MIDI, sysex whose size and buffer disagree,
  transports with every field random, LV2 atom sequences with lying sizes and
  control ports reading NaN. It asserts finite audio and in-range
  parameters after every one. Its first run found the CLAP wrapper casting an
  event to its type without checking the header's size.
- **Every rate and block size.** The RT-safety test activates each example at
  nine rates from 8 kHz to 384 kHz with block sizes from 1 to 8192 and asserts
  finite, bounded output, a latency that fits in a second, and zero
  allocations while processing.
- **The export table** of every built binary (`tests/exports_test.cpp`, a PE
  and ELF reader): the format's entry points and nothing else, because hidden
  visibility is a flag and a flag can stop being applied. Its first Linux run
  found that the flag was never enough: libstdc++ marks its own templates
  default-visibility, so every plugin still exported `std::vector`
  instantiations and shared_ptr typeinfo. Plugins now link through a version
  script (a symbol list on macOS) that makes the entry points the only exports.
- **clang-cl.** The Windows build farm compiles with LLVM's MSVC mode, so the
  suite is built and run with clang-cl + lld-link too (portable LLVM in
  `C:\dev\llvm`, Ninja in `C:\dev\ninja`).
- **The editor, opened and closed** twelve times with the process's handle
  count and private bytes read after each (`tests/editor_soak_test.cpp`); a
  cycle counts only once the page has driven a parameter through the bridge.
- **Every example through the browser ABI**, including the sidechain form,
  which the glue could not dispatch before this existed.
- **A real DAW's render against ours** (`npm run verify:daw-render`, needs
  REAPER): each effect renders the same file in REAPER and through
  `sonore/host.h`, aligned by the plugin's reported latency, and the two must
  agree to the output format's precision.

What it has found that nothing else did: a `Component` that could be copied
over a live one, an SVG path that looped forever on one corrupt byte, the
rasteriser segfault that loop had been hiding, a FLAC channel sum that
overflowed, a host test lying about the length of its own array.

### The Windows-only tests, run by hand

Three things are proved natively on Windows, because `verify:sdk`'s Windows
leg builds under WSL. Each one proves something no other test reaches:

```
# the native window peer: HiDPI, keyboard routing, and a real UI Automation
# client walking the tree the way a screen reader does
g++ -std=c++17 -O2 -static -I include -I third_party/clap/include \
    -o native_window_test.exe tests/native_window_test.cpp \
    -lgdi32 -luiautomationcore -lole32 -loleaut32
./native_window_test.exe

# the standalone's window minimum, driven on the BUILT application
g++ -std=c++17 -O2 -static -DSONORE_BUILD_STANDALONE -I include \
    -I third_party/clap/include -o SatApp.exe examples/saturator/plugin.cpp \
    -lole32 -loleaut32 -lwinmm -luuid -lgdi32 -luiautomationcore -lshlwapi -ldwmapi
g++ -std=c++17 -O2 -static -o standalone_size_test.exe \
    tests/standalone_size_test.cpp -lgdi32
./standalone_size_test.exe ./SatApp.exe 320 200
```

`-static` is not optional for a mingw build you intend to *run*: without it the
executable needs `libstdc++-6.dll` and friends on `PATH` and dies at load with
`0xC0000139`, which looks exactly like a program that ran and printed nothing.

The size test exists because `applyEditorConstraints` being right does not make
the window right. `WM_GETMINMAXINFO` has to arrive before the first `WM_SIZE`,
`ptMinTrackSize` is in *window* pixels rather than client pixels, and
`AdjustWindowRect` has to convert against the style the window was actually
created with. Get any of those wrong and the arithmetic is perfect and the
window still collapses. So the test launches the application, asks Windows to
make its window 50x50, and measures what happened: a resize is refused by the
window procedure, not by the caller, which is what makes reading the result
back the whole test. It also asks for a size *inside* the range and asserts it
is untouched, because a minimum that pinned the window to one size would pass
the first check on its own. What the WSL run does verify is the **dlopen resolution**: on a
machine missing WebKitGTK the backend must name the exact library rather than
fail vaguely, the difference between an actionable bug report and "the editor
is blank".

To look at a plugin's interface with your own eyes:

```
sdk/build/Release/gui_shot.exe <plugin.clap> shot.bmp [seconds]
```

### How feature parity is measured

Three checks, each closing a direction the other two cannot see. All run as part
of `verify:sdk`, and separately:

```
npm run verify:features    # every capability the map CLAIMS is still in the headers
npm run verify:unclaimed   # every public type in the headers appears in the map
npm run verify:parity      # every audited reference class has a DECISION
```

The first two are closed loops between `scripts/feature-map.mjs` and this
codebase. They catch a claim that rotted and a capability nobody wrote down.
`unclaimed` was added after a `ProcessorChain` was nearly written a second time,
ten minutes from shipping a duplicate of one already sitting in `shaping.h`.

Neither can see the thing that actually matters. A class the reference has that
nobody here has ever thought about appears in no header and in no row, so both
checks pass while knowing nothing about it. **"0 gaps" only ever meant "0 among
the rows somebody wrote down."** The first walk across six GUI groups found
**36 classes with no row at all**.

`verify:parity` closes that. **550 reference classes across 19 modules carry a
decision, with none undecided.** The bar is deliberately low: a class name must
be *decided about*, and *"out of scope, because…"* closes one as well as
building it does. Most honestly are: `MenuBarComponent`, `SidePanel`,
`MultiDocumentPanel`, the whole plugin-**hosting** half of
the audio-processors module, and roughly forty core classes that the C++
standard library has done since C++11. What the check refuses is silence.

It has already caught things the other two structurally could not:

- A row asserting **"(the reference has no MIDI-CI)"** when it has an entire
  MIDI-CI module. A row that is wrong *about the reference* is invisible to any
  check that only compares this map against this codebase.
- Three capabilities that existed and were **wired into nothing**: parameter
  groups, the resize grip, and the host context menu.
- Two real gaps now named rather than unknown: the macOS `NSAccessibility`
  bridge, and IME/composed text input.

`scripts/reference-index.mjs` holds the class names, taken from the public
documentation index, **names only**, no source, prose, or signatures. It also
lists, by name, every area **not** walked, and when that list is empty the tool
prints a warning rather than a clean bill: an empty pending list means "nothing
is left in the list", which is not the same as "nothing is left".

## Licensing

**Apache-2.0**: the licence text is at the repository root, with the
third-party notices beside it ([`LICENSE`](../LICENSE),
[`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md)). Use it in closed
commercial products; the patent grant travels with it.

Everything under `include/sonore`, `examples` and `tests` is our own code,
and its DSP is implemented from published mathematics rather than adapted from
another framework's source, which is what makes the provenance clean enough to
publish. `third_party/` holds five vendored pieces, each permissive: the CLAP
headers (MIT), the LV2 specification headers (ISC), Steinberg's VST3 C API
header (BSD-3-Clause), minimp3 (CC0) and stb_vorbis (MIT or public domain).

CLAP is the native format, and it is free of obligations. Other formats reach
hosts through our own wrappers rather than through another framework; keeping
our code as the plugin core is precisely what stops a framework licence from
governing what anyone may generate with this. What those FORMATS require of
you is separate from any software licence and is listed in the third-party
notices: VST3 needs registration with Steinberg for commercial distribution,
AAX needs Avid's agreement plus PACE signing, and VST2 is deliberately
impossible.
