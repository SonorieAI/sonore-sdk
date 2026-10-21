# Sonore SDK

Sonorie's own audio-plugin framework. Header-only C++17, no framework
dependency, and **ours**, which is the point.

Questions, or a measurement that disagrees with one of ours:
**[Discord](https://discord.gg/4bvsn8r8CG)**.

## Documentation

Guides, short and task by task, whose code the test suite compiles and runs:

- [Getting started](docs/guides/getting-started.md) -- a generated project into a DAW
- [Parameters and state](docs/guides/parameters-and-state.md) and [presets](docs/guides/presets.md)
- [The native editor](docs/guides/native-editor.md) and [the web editor](docs/guides/web-editor.md)
- [MIDI and MPE](docs/guides/midi-and-mpe.md) and [hosting plugins](docs/guides/hosting.md)
- [Testing your plugin](docs/guides/testing.md), [platforms and formats](docs/guides/platforms-and-formats.md), [versions and compatibility](docs/guides/versions.md)

The API reference is generated from the headers' own comments, grouped by area
([docs/index.md](docs/index.md) lists the areas): `npm run docs` builds it into
`sdk/build/docs/html` -- on Windows, `npm run docs:fetch` fetches Doxygen first
-- and CI keeps every build of it as an artifact.

## Why this exists

Sonorie used to generate projects against a third-party framework, and the
commercial terms of that kind of framework do not survive contact with this
business. They forbid building a product that creates other products (which is
precisely what a SaaS that generates plugins is), so no standard tier covers the
model, and the copyleft alternative would make every wasm preview we serve and
every binary our build farm ships a copyleft work, incompatible with selling
closed plugins on the marketplace.

Owning the framework removes the question entirely: the only licence that
governs a generated plugin is the one *we* choose.

It also happens to be small. A built example:

| Artifact | Size |
| --- | --- |
| `SonoreSaturator.clap` (Windows x64, MSVC Release: every format's code in one module, the native editor, the WebView2 loader linked statically) | 494 KB |
| `saturator.wasm` (same DSP, browser preview) | 10.6 KB |

The `.clap` figure was measured on 2026-10-12 (it was 434 KB on 2026-10-10). This table used to say
18.5 KB, which was a CLAP built before the native editor, the page bridge and
the other formats existed -- true once, and repeated long after it stopped
being. The wasm figure has not been re-measured since the browser ABI grew
pooled instances and a key inlet; `npm run verify:sdk` with emscripten on
PATH builds it into `sdk/build/wasm/`.

What changed between versions, and what an upgrade can change for an existing
plugin, is in [CHANGELOG.md](CHANGELOG.md); `<sonore/version.h>` says which version
a build has.

## What's in it

```
sdk/
  include/sonore/
    audio.h           ProcessSpec, AudioBlock, MidiMessage, MidiBuffer
    dsp.h             the toolkit's entry point (pulls in everything indented below)
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
    formats.h         the last include of a plugin: CLAP + every format the build asks for
    clap_wrapper.h    the CLAP format wrapper, which every other format adapts
    vst3_wrapper.h    VST3, over Steinberg's C ABI (our code, no C++ SDK)
    au_wrapper.h      Audio Units (macOS): auval-validated on CI, see below
    au_view.h         the AU editor: a CocoaUI class registered at runtime
    lv2_wrapper.h     LV2 (+ the TTL generator, from the same source)
    wasm_abi.h        the browser ABI glue (same DSP, AudioWorklet host)
    standalone.h      a runnable app: window + faceplate + OS audio, no DAW
    audio_wasapi.h    Windows output (WASAPI shared mode, event driven)
    audio_alsa.h      Linux output (ALSA, dlopened: no dev packages)
    audio_coreaudio.h macOS output (the HAL output AudioUnit): compiled, never heard
    wav.h             WAV read/write (8/16/24/32-bit PCM and float in, float out)
    -- beyond a plugin: what an application, a host or a tool built on it needs --
    host.h, host_au.h hosting CLAP, VST3, LV2, LADSPA and (macOS) Audio Units
    value_tree_state.h AudioProcessorValueTreeState over ParamInfo, with widget attachments
    audio_source.h    the pull-model AudioSource family: buffering, resampling, mixer, transport
    system_audio_format.h, lame_encoder.h, audio_cd.h, system_volume.h   the OS's codecs and devices
    ump128.h, midi_collector.h, bluetooth_midi.h   MIDI 2.0's other packets, timed MIDI in
    xml.h, expression.h, javascript.h, localisation.h, text_diff.h, statistics.h
    bigint.h, crypto.h, hash.h   BigInteger, RSA, Blowfish, MD5, Whirlpool, SHA-256
    net.h, process.h, ipc.h, streams.h, file_types.h   URL/HTTP, sockets, child processes, IPC
    message_manager.h, application.h, application_properties.h, file_document.h
    analytics.h, audio_workgroup.h, unit_test.h, leak_detector.h
    gfx/              the native GUI: widgets, menus, toolbars, colour selector, code editor,
                      text areas with bidi and complex-script shaping, JPEG/GIF/PNG/SVG,
                      desktop windows and message boxes, OpenGL, video, accessibility
  third_party/clap/   the CLAP headers (MIT, vendored)
  third_party/vst3/   Steinberg's C ABI header (BSD-3, vendored)
  third_party/lv2/    the LV2 headers (ISC, vendored)
  third_party/minimp3.h, stb_vorbis.c   MP3 and Ogg Vorbis decoding (vendored;
                      licences in THIRD_PARTY_NOTICES.md)
  cmake/              the sonore_add_* helpers (SonoreSDKHelpers.cmake), the
                      find_package() config, and the WebView2 SDK fetch (BSD-3,
                      downloaded, never vendored)
  examples/           saturator (effect with its own page), trim, ducker
                      (sidechain), splitter (aux outputs), reverb (convolution),
                      synth and sampler (instruments), arp (MIDI out), a
                      GUI probe (test fixture); showcase (a waveshaper with a
                      native editor of its own: attachments, menus, a curve
                      typed as an expression) and player (a desktop
                      application on the AudioSource chain)
  tests/              DSP units, a real CLAP host, the wasm round-trip,
                      and gui_shot (photographs a built plugin's interface)
```

Everything under `include/sonore` is our own implementation of published,
standard DSP (RBJ's EQ cookbook coefficients, TPT/zero-delay-feedback filter
topologies, polyBLEP band limiting, Schroeder reverb elements), written from the
maths, not adapted from any framework's source.

## Trying it

From the repository root, with CMake 3.21+ and a C++17 compiler (on Windows,
Visual Studio 2022 or newer):

```
cmake -S sdk -B sdk/build
cmake --build sdk/build --config Release --parallel
ctest --test-dir sdk/build -C Release            # everything, about 15 minutes
```

Every example lands in `sdk/build/Release` (`sdk/build` on single-config
generators): `SonoreShowcase.clap`, the `vst3/SonoreShowcaseVST3.vst3`
folder (on macOS, `SonoreShowcaseVST3.vst3` itself), `SonoreShowcaseLV2.lv2`,
and the same for each example. Copy each as it is: a host finds the binary
in a VST3 folder by the folder's name
(`SonoreShowcaseVST3.vst3/Contents/x86_64-win/SonoreShowcaseVST3.vst3`), so
a renamed one does not load in a host that looks for it that way -- the
SDK's own `host.h` among them. To hear one, copy it where your DAW looks --
CLAP: `%LOCALAPPDATA%\Programs\Common\CLAP` or `~/Library/Audio/Plug-Ins/CLAP`
or `~/.clap`; VST3: `%LOCALAPPDATA%\Programs\Common\VST3`,
`~/Library/Audio/Plug-Ins/VST3` or `~/.vst3` -- and rescan. Start with the
showcase: its editor is built from the SDK's own widgets, and its curve is
whatever expression you type.

`SonorePlayer` is a desktop application made with the SDK: open an audio file
and play it through the default output. Each example also has a standalone
build that runs without a DAW (`SonoreShowcaseApp`, `SonoreSaturatorApp`,
`SonoreSynthApp`, `SonoreArpApp`).

User presets live in a folder per plugin -- `Documents\<Vendor>\<Plugin>\Presets` on Windows,
`~/Library/Audio/Presets/<Vendor>/<Plugin>` on macOS, `$XDG_DATA_HOME/<vendor>/<plugin>/presets`
on Linux -- and `SONORE_PRESET_ROOT` points every plugin somewhere else, which is what the tests do.
The showcase's Presets menu saves into it.

With REAPER installed, `npm run verify:reaper` drives every example inside it
and reports what a DAW made of them -- in a REAPER profile of its own, never
yours (see "Verifying" below).

What has run where: everything on Windows; Linux in CI (X11 under xvfb);
macOS and the Audio Unit wrapper only in CI that is not running at the
moment, and the Mac-only parts written since (Audio Unit hosting, workgroups,
VoiceOver, input methods) compiled against the repo's Apple shims and never
run -- `tests/mac_host_selftest.cpp` and `tests/mac_input_selftest.cpp` are
the first things to run on a Mac.

## Writing a plugin

The quickest start is a generated project: every format, the standalone and
their self-checks from one `src/plugin.cpp`, with only CMake needed.

```
cmake -DNAME="My Filter" -DVENDOR="My Company" -P sdk/cmake/sonore_new_plugin.cmake
cmake -S MyFilter -B MyFilter/build
cmake --build MyFilter/build --config Release
ctest --test-dir MyFilter/build -C Release
```

`-DTYPE=instrument` starts an eight-voice synth instead of a filter. Run from
a checkout, the project builds against that checkout (and its `ctest` adds
the real-time audit); run from an installed SDK
(`<prefix>/share/SonoreSDK/cmake/`), it uses `find_package`. It never writes
into a folder that has anything in it. The id it derives,
`com.mycompany.myfilter`, is the plugin's identity in every saved session:
pass `-DID=` now if that is not the one to keep, and `-DAU_SUBTYPE=` /
`-DAU_MANUFACTURER=` for the Audio Unit's codes. Both templates pass
clap-validator and pluginval at strictness 10 as generated, and the
`new_plugin` and `install_consume` tests build one of each on every run.

What the file holds: a `SonoreDsp` struct, a parameter table, a descriptor,
and one include at the end. That's the whole surface:

```cpp
#define SONORE_NUM_PARAMS 2
#include <sonore/dsp.h>

struct SonoreDsp {
  sonore::SVF filter[2];          // one per channel: a filter is its state
  sonore::Smooth cutoffSm, gainSm;
  bool fresh = true;

  void prepare(const sonore::ProcessSpec& spec) {
    for (auto& f : filter) {
      f.setSampleRate((float) spec.sampleRate);
      f.reset();
    }
    cutoffSm.setup((float) spec.sampleRate, 20.0f);
    gainSm.setup((float) spec.sampleRate, 20.0f);
    fresh = true;
  }

  void process(sonore::AudioBlock<float>& io, const float* p) {
    const float gain = sonore::dbToGain(p[1]);
    if (fresh) {                  // the first block starts AT the settings,
      cutoffSm.snap(p[0]);        // rather than gliding in from zero
      gainSm.snap(gain);
      fresh = false;
    }
    const size_t channels = io.getNumChannels() < 2 ? io.getNumChannels() : 2;
    for (size_t i = 0; i < io.getNumSamples(); ++i) {
      // Every control that reaches a coefficient or a gain is smoothed, or
      // automation zippers.
      const float hz = cutoffSm.next(p[0]), g = gainSm.next(gain);
      for (size_t c = 0; c < channels; ++c) {
        float& x = io.getChannelPointer(c)[i];
        filter[c].set(hz, 0.707f);
        filter[c].process(x);
        x = filter[c].lp * g;
      }
    }
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    // The last field is the knob's skew: 1 kHz at the middle of its travel,
    // where a linear 20 Hz..20 kHz would put it at five per cent.
    {"cutoff", "Cutoff", "Hz", 20.0f, 20000.0f, 1000.0f, 0, nullptr, nullptr, 0, true, false, false,
     sonore::skewForCentre(20.0f, 20000.0f, 1000.0f)},
    {"gain", "Gain", "dB", -24.0f, 24.0f, 0.0f},
};

static const sonore::PluginDescriptor kDesc = {   // the wrappers read `kDesc`
    "com.example.myfilter", "My Filter", "Me", "1.0.0",
    "A filter.", "https://example.com",
    false,              // effect (true = instrument)
    kParamTable, SONORE_NUM_PARAMS,
};

#include <sonore/formats.h>   // CLAP, and every format this target is built as
```

The names are fixed: the DSP is `SonoreDsp`, the descriptor `kDesc`, and
`SONORE_NUM_PARAMS` is the size of the table it points at. The descriptor's
defaults are a fixed-stereo effect, which is why the loop above handles two
channels; a DSP that handles any width widens `minChannels`/`maxChannels`.
A DEBUG build of the CLAP checks the descriptor as it loads -- an empty id, a
range the wrong way round, a default outside it -- and refuses to load with the
reason on stderr, so a host lists nothing; a release build loads it as it is,
and the other formats do not check. The standalone's `--verify` fails on a
malformed descriptor in every build.

**The parameter table is positional.** A parameter's index is its identity to
every host and in every saved session, so a shipped plugin only ever APPENDS:
reordering or removing a row makes old sessions load values into the wrong
controls. Appending keeps every saved value -- and moves nothing else: CLAP's
and VST3's own bypass, and VST3's program change, have fixed ids of their own.
(Before 1.0 they were numbered after the table and moved with it; a session
saved with such a build loses its bypass automation once.)

**Instruments** set `isInstrument` in the descriptor, which gives them note
input, drops the audio input bus and files them as instruments, and take the
MIDI in a third argument:

```cpp
void process(sonore::AudioBlock<float>& io, const float* p, sonore::MidiBuffer& midi);
```

`sonore::MidiMessage::noteOn(channel, ...)` and the other constructors take a
channel from 0 to 15, while `getChannel()` answers 1 to 16, as MIDI is written:
`noteOn(m.getChannel() - 1, ...)` is the round trip.

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
// t.tempo, t.positionBeats, t.barPhase(), t.isPlaying, t.loopStartBeats/loopEndBeats
// sonore::noteLengthInSamples(t, sampleRate, 8, NoteFlavour::Dotted)
// sonore::SyncedLfo: phase derived from the timeline, so a loop repeats exactly
```

The rest of the optional hooks work the same way: declare one and the
wrappers find it at compile time; leave it out and nothing is paid. The
`const` matters -- the wrappers look for these on a const DSP, and a
non-const `latencySamples()`, `tailSamples()` or `saveState()` is a compile
error that says so.

| Declare | and |
| --- | --- |
| `int latencySamples() const` | the host is told the delay, and told again when it changes |
| `int tailSamples() const` | a reverb or delay is not cut off at stop or at the end of a render |
| `void reset()` | a host's "start over" calls it instead of `prepare()` (see below) |
| `void saveState(sonore::StateBag&) const` / `void loadState(const sonore::StateBag&)` | extra state (a file path, a learned value) is saved with the session; include `<sonore/state_bag.h>` before the struct |
| `bool consumeStateDirty()` | the host is told the extra state changed, so it offers to save |
| `void loadFile(const char* purpose, const char* path)` | the page's file picker (`sonore.chooseFile(purpose)`) delivers a path |
| `void setHostInfo(const sonore::HostInfo&)` / `void setTrackInfo(const sonore::TrackInfo&)` | which host, which track (name, colour) |
| `int voiceCapacity() const` | a CLAP host learns the polyphony (voice-info), for voice stacking and MPE allocation |
| `process(AudioBlock<float>& io, AudioBlock<float>& sidechain, const float* p)` | a second, key input bus |
| `process(sonore::ProcessContext& ctx, const float* p)` | everything at once: MIDI in and out, aux outputs, MPE expression, offline flag |
| `process(AudioBlock<double>& io, const float* p)` | 64-bit processing where the host offers it |

**What runs where.** `process()` and `reset()` run on the audio thread:
nothing there may allocate, lock or touch a file. `prepare()` is where
buffers are sized, but it is not only called from the main thread: without a
`reset()`, a host's reset (CLAP `reset()`, an AU's `Reset`) calls `prepare()`
on the audio thread, and an LV2 host's switch into or out of freewheeling
always does. So either give the DSP a `reset()` that forgets what is sounding
and keeps what `prepare()` sized, or make `prepare()` allocate nothing when the
rate and block size have not changed. `sonore_add_rt_test(MyRt SOURCES
plugin.cpp)` checks it: it builds a program with your plugin's source compiled
in, which counts every allocation made on the audio thread across sample rates
and block sizes (allocations only -- locks are not measured). It makes the
executable, not a test: register it with `add_test` yourself. It needs a source
checkout of the SDK; an installed one does not carry the audit.

The **same DSP** builds for the browser through `<sonore/wasm_abi.h>`. That is
what makes the live preview bit-identical to the shipped plugin: not a
similar model of it, the same code. The browser build takes the DSP half of
the source -- everything before `#include <sonore/plugin.h>` -- and appends
the ABI glue in its place:

```
# dsp.cpp = plugin.cpp up to #include <sonore/plugin.h>, then:
#   #include <sonore/wasm_abi.h>
em++ dsp.cpp -o plugin.wasm -O3 -std=c++17 --no-entry -fno-exceptions -fno-rtti \
  -sSTANDALONE_WASM=1 -sERROR_ON_UNDEFINED_SYMBOLS=1 -I sdk/include \
  -sEXPORTED_FUNCTIONS=_sonore_init,_sonore_process,_sonore_in_ptr,_sonore_out_ptr,\
_sonore_params_ptr,_sonore_num_params,_sonore_max_frames,_sonore_channels,\
_sonore_midi_ptr,_sonore_midi_count,_sonore_wants_midi,_sonore_latency
```

The module is driven from an AudioWorklet: `sonore_init(sampleRate)` once,
then per call write up to `sonore_max_frames()` (128) frames of planar float
input at `sonore_in_ptr()` (channel stride 128, two channels), the parameter
values at `sonore_params_ptr()`, any MIDI as `[frame, status, data1, data2]`
quads at `sonore_midi_ptr()` with `sonore_midi_count(n)`, call
`sonore_process(frames)`, and read `sonore_out_ptr()`. `sonore_latency()` is
the delay to compensate; `sonore_set_transport(bpm, beats, playing)` and
`sonore_sidechain_ptr()` carry tempo and a key input, and need adding to the
export list when the host uses them. `tests/wasm_build_test.mjs` builds the examples this
way and drives them as the worklet does; the worklet host itself is Sonorie's
studio, not part of this repository.

## The interface

A plugin without a face of its own gets the **native editor**: controls for
its parameter table, drawn by the SDK in its own window (about two megabytes,
no browser process, on every machine). On Linux it needs the X11 development
headers at build time (`libx11-dev` or your distribution's equivalent); a
build without them quietly uses the webview instead.

Its text is set in the desktop's own interface face (Segoe UI, SF, DejaVu
Sans), kerned from the font's GPOS table or its older kern table. A character
that face lacks -- a preset named in Japanese, a sample folder in Thai -- is
drawn from the faces the desktop falls back to, each read only when a
character of its script first needs it. A plugin with a typeface of its own
passes the file's bytes to `Typeface::load` and the result to `Font`: `.ttf`
and `.otf` both, CFF outlines and CID-keyed CJK fonts included. A variable
font moves along its axes with `Font::withVariation("wght", 650)` -- weight,
width, optical size, or whatever its designer gave it -- and OpenType
features are turned on and off with `Font::withFeature`: `withFeature("tnum")`
gives a value readout tabular figures, which do not jitter as it changes.
Right-to-left text is laid out by the Unicode bidirectional algorithm, and
text that needs shaping
goes through the font's GSUB and GPOS as HarfBuzz shapes it -- Arabic joined
and lam-alef ligated, marks on their letters, Thai's SARA AM, Hangul jamo
composed, and the syllables of the Indic scripts, Khmer, Myanmar and the
eighty-odd scripts of the Universal Shaping Engine -- Balinese, Javanese,
Tibetan, Sinhala, Mongolian and the rest -- reordered, with their reph, half
forms and conjuncts. Emoji are drawn in colour, from the system's emoji
face -- Segoe UI Emoji, Apple Color Emoji, Noto Color Emoji -- and a font's
own COLR layers or sbix and CBDT pictures; sequences -- a skin tone, a flag, a
family joined by ZWJs -- stay together as HarfBuzz keeps them, and are one
character to a `TextEditor`'s caret. Font files are mapped rather than read:
a CJK fallback face costs the pages a label touches, not its 20 MB. `wrapText`
breaks lines where Unicode's line breaking algorithm allows -- between
ideographs, after hyphens, never inside a number or a cluster.

Art for it can be SVG. `Drawable::parse` reads what Figma, Illustrator,
Inkscape and the icon sets export -- gradients, clip paths, masks, `<use>`,
`<style>` sheets, dashes -- once, and `draw` paints it at any size. An icon
drawn in `currentColor` takes the colour it is drawn with, and `IconButton`
gives it the theme's text colour. Text inside an SVG is not drawn: export it
as outlines.

A plugin with a face of its own supplies it as an HTML document, shown in an
OS webview (Edge WebView2 on Windows) and wired through a `window.sonore`
bridge that is **API-identical to the studio preview's**. Define
`SONORE_UI_HTML` and the same markup that was approved in the browser opens in
the DAW:

```cpp
#define SONORE_UI_WIDTH 620
#define SONORE_UI_HEIGHT 300
#define SONORE_UI_HTML R"HTML(<!doctype html>...)HTML"
#include <sonore/formats.h>
```

`PluginDescriptor::editor` overrides the choice (`EditorKind::Native` for a
page you want to skip, `EditorKind::Web` for a webview without a page, which
shows a generated panel), and `editorLimits` sets the size range. The
standalone application always shows the webview, generated panel included.

What the page gets:

| API | |
| --- | --- |
| `sonore.name` / `sonore.params` | identity and the parameter table; a parameter the plugin reports (a meter) has `readOnly`, and `set`, `begin` and `end` ignore it |
| `sonore.get(i)` / `set(i, v)` | plain-value access |
| `sonore.begin(i)` / `end(i)` | gestures: one undo step per drag |
| `sonore.on(cb)` | `cb(values, index, value)` on any change, from the UI *or* host automation |
| `sonore.onLevel(cb)` / `onVU(cb)` | meters, up to ~30 times a second -- when the level moves; a silent or steady signal sends nothing new, so do not use it as a clock |
| `sonore.format(i, v)` | the same text the host shows |
| `sonore.noteOn/noteOff` | on-screen keyboards for instruments |
| `sonore.presets` / `loadPreset(i)` | factory preset names, applied C++-side |
| `'sonore:ready'` event | fired once the bridge exists |

A page written in TypeScript references `sonore/sonore.d.ts`, installed with the
headers, and is checked against every member above.

Turning a knob on the page emits real CLAP gesture + value events, so it records
as automation exactly like a host-side control. Edits cross to the audio thread
through a lock-free queue: never a lock, never an allocation, in the callback.

Per platform:

- **Windows**: Edge WebView2. The SDK (BSD-3-Clause) is fetched from nuget.org
  at configure time rather than vendored, and linked statically so a built
  `.clap` has no DLL beside it. Without the Edge runtime a CLAP or VST3 opens
  the SDK-drawn native editor instead; an LV2 interface is not offered, so the
  host shows its own controls; the standalone says why. `SONORE_WEBVIEW2_DIR` (a CMake variable or an environment
  variable) keeps the download in a directory that outlives the build, so a
  machine that has fetched it once configures offline; without network and
  without that cache, `-DSONORE_GUI=OFF` leaves the webview out (the native
  editor is not affected).
- **Linux**: GTK3 + WebKitGTK, embedded through `gtk_plug_new` (XEmbed).
  Everything is **dlopened at runtime**, newest WebKit first: `webkit2gtk-4.1`
  and `4.0` cannot coexist in one process, so linking either would make the
  plugin refuse to load on half the distributions. It also means the SDK builds
  with no GTK dev packages installed.
- **macOS**: WKWebView, driven through the Objective-C **runtime** rather than
  written in Objective-C, so a generated project stays plain C++ with no `.mm`
  file and no mixed-language build.

Only *embedded* windows are supported (floating is refused): it is what every
host implements, and a second window lifetime is a second one to get wrong.

## Building

CMake 3.21 or newer. Either build the SDK as part of your project, as below, or
install it once (`cmake --install <build> --prefix <dir>`) and find it from any
project -- the helpers come with it:

```cmake
find_package(SonoreSDK 1.0 REQUIRED)   # with <dir> in CMAKE_PREFIX_PATH
sonore_add_clap(MyPlugin SOURCES plugin.cpp)
target_link_libraries(MyTool PRIVATE SonoreSDK::sonore_sdk)  # a plain program
```

The `install_consume` test does exactly that on every run: a CLAP, a VST3, an
LV2, a standalone and a host, built against an installed SDK.

```cmake
add_subdirectory(sdk)
# Every format from ONE compile -- the wrappers are templates over your DSP,
# so compiling once with all of them defined and linking that object into
# each bundle costs a third of compiling per format:
sonore_add_plugin(MyPlugin SOURCES plugin.cpp
  CLAP MyPlugin  VST3 MyPluginVST3  AU MyPluginAU  LV2 MyPluginLV2
  AU_TYPE aufx  AU_SUBTYPE "Mypl" AU_MANUFACTURER "Manu"
  AU_VENDOR "My Company" AU_NAME "My Plugin" AU_BUNDLE_ID com.mycompany.myplugin
  VERSION 1.0.0)

# Or one target per format:
sonore_add_clap(MyPlugin     SOURCES plugin.cpp)  # -> MyPlugin.clap
sonore_add_vst3(MyPluginVST3 SOURCES plugin.cpp)  # -> MyPluginVST3.vst3 (see below)
sonore_add_au(MyPluginAU     SOURCES plugin.cpp   # -> MyPluginAU.component (macOS)
  TYPE aufx SUBTYPE "Mypl" MANUFACTURER "Manu" VENDOR "My Company" NAME "My Plugin")
sonore_add_standalone(MyApp  SOURCES plugin.cpp)  # -> a runnable MyApp
sonore_add_lv2(MyPluginLV2   SOURCES plugin.cpp)  # -> MyPluginLV2.lv2/
```

Each helper defines the macro `formats.h` reads (`SONORE_BUILD_VST3`, ...), so
the same `plugin.cpp` becomes whichever format the target is. `VERSION` in
`sonore_add_plugin` reaches the Audio Unit's plist only; CLAP, VST3 and LV2
report the descriptor's `version`, so keep the two the same.

**An Audio Unit's identity is the CMake call's, not the descriptor's.** `TYPE`
is `aufx` for an effect and `aumu` for an instrument: an instrument left at
the default is filed as an effect, and no player finds it. `SUBTYPE` and
`MANUFACTURER` are the four-character codes the system registers it by, and
`VENDOR: NAME` is what a host lists. Left out, the codes and the vendor
default to Sonorie's own, which every AU built without them shares (the helper
warns), and `NAME` to the target's name.

**Hosting plugins.** `sonore/host.h` is the other end: it loads CLAP, VST3 and
LV2 plugins (`describeFile`, `scanDirectory`, `loadPlugin`), keeps a scan cache
(`plugin_cache.h`) and runs them in a graph (`graph.h`). CLAP needs only
`sonore_sdk`; VST3 and LV2 hosting compile in where their C headers are found,
which `sonore_enable_hosting(MyHost)` arranges -- without it a host builds,
hosts CLAP, and finds no VST3 or LV2 plugin at all.

**Where the outputs go.** Copy each to the folder hosts scan:

| Output | Windows | macOS | Linux |
| --- | --- | --- | --- |
| `.clap` | `%COMMONPROGRAMFILES%\CLAP` | `~/Library/Audio/Plug-Ins/CLAP` | `~/.clap` |
| `.vst3` bundle | `%COMMONPROGRAMFILES%\VST3` | `~/Library/Audio/Plug-Ins/VST3` | `~/.vst3` |
| `.component` | | `~/Library/Audio/Plug-Ins/Components` | |
| `.lv2/` | `%APPDATA%\LV2` | `~/Library/Audio/Plug-Ins/LV2` | `~/.lv2` |

On Windows and Linux the VST3 helper writes the module, `MyPluginVST3.vst3`,
and the bundle a host scans in a folder of its own beside it:
`vst3/MyPluginVST3.vst3`, ready to copy as it is (its binary is
`Contents/<arch>/MyPluginVST3.vst3` on Windows and `.so` on Linux). The same
bundle is also assembled as `MyPluginVST3.vst3.bundle` next to the module, for
tools that know it by that name. On macOS the target is the bundle itself.

LV2's bundle metadata (manifest.ttl + plugin.ttl) is **generated by a companion
executable built from the same source**: hosts read the TTL to learn the port
layout before loading code, so metadata that disagrees with `connect_port()` is
a crash, and generating both from one descriptor makes disagreement impossible.

The standalone opens the plugin's webview face in its own window with live OS
audio. An effect opens on silence and says how to feed it: `--input <file>`
plays a file through it (at the file's own rate, which must be the device's --
48 kHz in the offline modes), `--input-device <n>` takes a live input (`--inputs`
lists them), and `--test-signal` plays a built-in source (a saw arpeggio,
harmonically rich enough that filters and drive are audible). `--help` lists
every option: devices, ASIO, MIDI ports and files, capture, and the track the
plugin is told it is on. Instruments are played from the
on-screen keyboard the generated page grows for them. And because an app
judged only by ear cannot be tested, it has offline modes that need no device
and no display, which is what makes it CI-testable on all three platforms:

```
MyApp --verify              # render offline, assert health, exit code = verdict
MyApp --render out.wav 2    # two seconds to a float WAV
MyApp --play 2              # live audio, no window
MyApp --shot out.bmp 5      # (Windows) open, wait, screenshot, exit
```

**The same source builds every format.** The CLAP wrapper owns the shared
machinery (the DSP instance, parameters, state, the webview bridge) and the
others adapt it, so a plugin cannot behave differently depending on which format
a host loaded. `sonore_add_plugin` compiles the source once with every format
on. On Linux and macOS each module exports its own format's entry points and
`clap_entry` (a version script, an exported-symbols list); on Windows a module
built this way exports every format's -- the `.clap` also answers
`GetPluginFactory`, which no CLAP host asks for.

| Format | Licence of what we depend on | Status |
| --- | --- | --- |
| CLAP | MIT headers | proven on Windows + Linux |
| VST3 | Steinberg **C ABI** header, BSD-3 | proven on Windows + Linux |
| AU | Apple frameworks | proven on macOS CI: `auval` passes six of the eight example AUs (not yet run on the sampler and reverb), zero warnings |
| LV2 | ISC headers | proven on Windows + Linux |
| AAX | Avid SDK, **not public** | hook only, see below |

VST3 is built against Steinberg's one-file C ABI rather than their C++ SDK: the
header is BSD-3 (not the SDK's dual GPLv3/proprietary), so the wrapper is our
code and the plugin links no framework. Distributing VST3 commercially still
means registering with Steinberg: that is their licence, not the header's.

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
whole suite under WSL. Building on one OS hides real bugs: MSVC leaks
`<cstddef>` and `<cstdlib>` through other headers where gcc does not, and a
Windows-only build once let `nativeWindowApi()` return `"win32"` on Linux, which
would have meant no exported Linux plugin ever showed a UI.

The WSL run covers everything except *window embedding* (Xlib headers and
WebKitGTK are dev packages). For the full Linux GUI proof (a real GTK main
loop, a real X window, a real page driving a parameter), there is a container:

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
plugin cache, MIDI off a cable with its SysEx and MIDI-CI, UMP packets --
every truncation, byte flips, runs of the bytes that break size arithmetic)
at depth, the plugin-state fuzzer (`tests/fuzz_state.cpp`: damaged saved
sessions through the loader every format shares, with the ducker compiled
in -- a refused blob must leave the plugin untouched, a loaded one must play
finite audio and survive its own round trip), the sampler example's sample handover
(`tests/sampler_stress_test.cpp`, a loading thread against a rendering
thread) and the two threads a plugin lives on (`tests/concurrency_test.cpp`:
state loads, preset loads, parameter reads, render-mode flips and interface
edits on the main thread while `process()` runs, against the saturator
compiled into the test) under ASan and then ThreadSanitizer. gcc only:
natively on Linux, through WSL on Windows. The quick passes of all four run
in `verify:sdk` through ctest, and the CLAP host test also feeds every
example's state blob back through its loader under mutation.

Parameters cross threads as relaxed atomics (`SharedParam` in
clap_wrapper.h) snapshotted once per block, which is what lets the
ThreadSanitizer leg be a gate rather than a list of known noise: a report
from it is a bug.

What else is measured rather than trusted (all of it in `verify:sdk` unless
said otherwise):

- **Events under mutation.** Every host test feeds its plugin hundreds of
  blocks of malformed events -- headers whose size lies both ways, unknown
  types and spaces, offsets past the block, undeclared parameter ids, NaN and
  infinite values, notes outside MIDI, sysex whose size and buffer disagree,
  transports with every field random, LV2 atom sequences with lying sizes and
  control ports reading NaN -- and asserts finite audio and in-range
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
  `C:\dev\llvm`, Ninja in `C:\dev\ninja`; skips loudly without them).
- **The editor, opened and closed** twelve times with the process's handle
  count and private bytes read after each (`tests/editor_soak_test.cpp`); a
  cycle counts only once the page has driven a parameter through the bridge.
- **Every example through the browser ABI**, including the sidechain form,
  which the glue could not dispatch before this existed.
- **A real DAW: REAPER** (`npm run verify:reaper`, opt-in; skips and exits 0
  without REAPER or a Release build). Hands the CLAP, VST3 and LV2 build of
  every example to REAPER and checks, by plugin, format and step: the scan (name,
  vendor, pins), offline renders equal to our own host's render of the same
  binary (also at 44.1 and 96 kHz), instruments at the right pitch and the
  arpeggiator's MIDI driving a synth, parameters and their text, an
  automation envelope, a session saved and reopened in a new REAPER (the
  sampler's file included), latency compensation and a live latency change,
  and editors opening and closing. REAPER runs with its own profile in
  `sdk/build/reaper-host-test` and never touches yours; `--only`,
  `--skip-editor`, `--rates`, `--build` and `--work` narrow or redirect it.
  It does not cover playback through a sound card, what an editor looks
  like, other DAWs, AU, or macOS and Linux. Its first full run found
  two bugs nothing else had: CLAP renders reset partway through the bounce,
  and VST3 defaults printed as -0.000 dB.

What it has found that nothing else did: a `Component` that could be copied
over a live one, an SVG path that looped forever on one corrupt byte, the
rasteriser segfault that loop had been hiding, a FLAC channel sum that
overflowed, a host test lying about the length of its own array.

### The Windows-only test, run by hand

The native window peer (HiDPI, keyboard routing, and a real UI Automation
client walking the tree the way a screen reader does) is `native_window_test`,
a ctest, so the native Windows build `verify:sdk` runs covers it. One test is
still run by hand, because it drives a BUILT standalone application from
outside, and what it proves is worth stating because it is provable no other
way:

```
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
fail vaguely: the difference between an actionable bug report and "the editor
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
codebase. They catch a claim that rotted and a capability nobody wrote down:
`unclaimed` was added after a `ProcessorChain` was nearly written a second time,
ten minutes from shipping a duplicate of one already sitting in `shaping.h`.

Neither can see the thing that actually matters. A class the reference has that
nobody
here has ever thought about appears in no header and in no row, so both checks
pass while knowing nothing about it. **"0 gaps" only ever meant "0 among the
rows somebody wrote down."** That was true and repeated for a long time before
it was mechanised, and the first walk (six GUI groups) found
**36 classes with no row at all**, while the map was confidently reporting one
gap.

`verify:parity` closes that. **550 reference classes across 19 modules carry a
decision, with none undecided.** The bar is deliberately low: a class name must
be *decided about*, and *"out of scope, because…"* closes one as well as
building it does. Most honestly are: `MenuBarComponent`, `SidePanel`,
`MultiDocumentPanel`, the plugin chooser a hosting application draws for
itself, and roughly forty core classes that the C++ standard library has done
since C++11. (Hosting itself is built: `sonore/host.h` loads CLAP, VST3 and LV2
plugins, scans for them, caches what it found and runs them in a graph; hosting
an Audio Unit is a named gap.) What the check refuses is silence.

It has already caught things the other two structurally could not:

- A row asserting **"(the reference has no MIDI-CI)"** when it has an entire
  MIDI-CI module. A row that is wrong *about the reference* is invisible to any
  check that only compares this map against this codebase.
- Three capabilities that existed and were **wired into nothing**: parameter
  groups, the resize grip, and the host context menu.
- Two real gaps now named rather than unknown: the macOS `NSAccessibility`
  bridge, and IME/composed text input.

`scripts/reference-index.mjs` holds the class names, taken from the public
documentation index: **names only**, no source, prose, or signatures. It also
lists, by name, every area **not** walked, and when that list is empty the tool
prints a warning rather than a clean bill: an empty pending list means "nothing
is left in the list", which is not the same as "nothing is left". It made
exactly that mistake once, when ten of the twenty-four modules were indexed
and the other fourteen had never been added. Read that list before believing
any parity claim, including one made in this file.

## Versions and compatibility

The SDK follows [Semantic Versioning](https://semver.org) from 1.0.0, and the
version a build has is `SONORE_SDK_VERSION` (`version.h`). What a number
promises:

- **Within 1.x, code that built keeps building.** The public API -- what is
  in `namespace sonore` and its named sub-namespaces, outside any `detail`
  namespace, plus the CMake helpers -- is not removed or changed in a way that
  breaks a caller. A minor version adds; a patch fixes.
- **Nothing is removed without a warning first.** A part of the API on its
  way out is marked `SONORE_DEPRECATED("<version>: use <replacement>")` for at
  least one minor release, keeps working while it is, and goes only in the
  next major. `SONORE_NO_DEPRECATION_WARNINGS` silences the marks in a build
  that cannot act on them yet.
- **What a user saved keeps loading.** Plugin state, presets and sessions
  written by any 1.x build load in every later 1.x build. Where a fix changes
  what a plugin sounds like or how a saved session restores, the CHANGELOG
  lists it under "Changes that can affect an existing plugin", and a fix that
  would change a saved session's sound is a decision for the plugin, not a
  silent default.
- **Not covered:** anything in a `detail` namespace, the tests, the scripts,
  the feature map, and behaviour the documentation calls unspecified.

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
hosts through our own wrappers rather than through another framework: keeping
our code as the plugin core is precisely what stops a framework licence from
governing what anyone may generate with this. What those FORMATS require of
you is separate from any software licence and is listed in the third-party
notices: VST3 needs registration with Steinberg for commercial distribution,
AAX needs Avid's agreement plus PACE signing, and VST2 is deliberately
impossible.
