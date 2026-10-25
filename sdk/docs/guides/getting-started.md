# Getting started

From nothing to a CLAP and a VST3 in a DAW. You need CMake 3.21 or newer and a
C++17 compiler (Visual Studio 2022 or newer on Windows), and a checkout of this
repository.

## 1. Generate a project

From the repository root:

```
cmake -DNAME="My Filter" -DVENDOR="My Company" -P sdk/cmake/sonore_new_plugin.cmake
```

This writes a folder `MyFilter/` with `src/plugin.cpp`, a `CMakeLists.txt`, a
README and a `.gitignore`, and never writes into a folder that has anything in
it. `-DTYPE=instrument` starts an eight-voice synth instead of a filter.

Read what it prints. The plugin's id, `com.mycompany.myfilter`, is its identity
in every saved session and every host's plugin list: pass `-DID=` now if that is
not the one to keep, because changing it later orphans every session that used
the plugin. The Audio Unit's four-character codes are derived too: the
manufacturer from the vendor, shared by all your plugins, and the subtype from
the id, so each plugin has its own (`-DAU_SUBTYPE=`, `-DAU_MANUFACTURER=` to
choose your own, and to keep the codes of an Audio Unit already shipped).

## 2. Build it, and let it check itself

```
cmake -S MyFilter -B MyFilter/build
cmake --build MyFilter/build --config Release
ctest --test-dir MyFilter/build -C Release
```

`ctest` runs two checks the project comes with: `MyFilterApp --verify`, which
renders the plugin offline with no audio device and no window and fails on
silence, NaN or a malformed descriptor; and, when the project was generated from
a source checkout, the real-time audit, which fails on any allocation on the
audio thread across sample rates and block sizes.

## 3. The plugin, all of it

A plugin is one file: a `SonoreDsp` struct, a parameter table, a descriptor and
one include at the end. This is the effect the generator writes, with its values
filled in:

```cpp
// test-run
#define SONORE_NUM_PARAMS 2
#include <sonore/dsp.h>

struct SonoreDsp {
  sonore::SVF filter[2]; // one per channel: a filter is its state
  sonore::Smooth cutoffSm, gainSm;
  bool fresh = true;

  // Called before audio starts and whenever the rate or block size changes.
  // It may also run on the audio thread, so it allocates nothing.
  void prepare(const sonore::ProcessSpec& spec) {
    for (auto& f : filter) {
      f.setSampleRate((float) spec.sampleRate);
      f.reset();
    }
    cutoffSm.setup((float) spec.sampleRate, 20.0f);
    gainSm.setup((float) spec.sampleRate, 20.0f);
    fresh = true;
  }

  // The audio thread: no allocation, no locks, no files.
  void process(sonore::AudioBlock<float>& io, const float* p) {
    const float gain = sonore::dbToGain(p[1]);
    if (fresh) { // the first block starts AT the settings, not gliding in
      cutoffSm.snap(p[0]);
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

// POSITIONAL: a parameter's index is its identity in every saved session.
// Once this ships, only ever append rows.
static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    // id, name, unit, min, max, default -- then the knob's skew, which puts
    // 1 kHz at the middle of its travel instead of five per cent along.
    {"cutoff", "Cutoff", "Hz", 20.0f, 20000.0f, 1000.0f, 0, nullptr, nullptr, 0, true, false, false,
     sonore::skewForCentre(20.0f, 20000.0f, 1000.0f)},
    {"gain", "Gain", "dB", -24.0f, 24.0f, 0.0f},
};

static const sonore::PluginDescriptor kDesc = {
    "com.mycompany.myfilter", "My Filter", "My Company", "0.1.0",
    "An effect.",
    "",
    false, // an effect
    kParamTable, SONORE_NUM_PARAMS,
    nullptr, 0, // no factory presets
    "filter",   // the category hosts file it under
};

#include <sonore/formats.h> // CLAP, and every format this target is built as
```

The names are fixed: the DSP is `SonoreDsp`, the descriptor `kDesc`, and
`SONORE_NUM_PARAMS` is the size of the table it points at. `p[i]` in
`process()` is parameter `i`'s plain value, in its own units, held still for the
block. The same file becomes every format: `formats.h` includes the CLAP wrapper
always, and the VST3, AU, LV2 or standalone wrapper when the CMake target asks
for it (`sonore_add_plugin` and `sonore_add_standalone` in the generated
`CMakeLists.txt`).

## 4. Into a DAW

The build puts the outputs in `MyFilter/build/Release` (`MyFilter/build` with a
single-configuration generator):

| Output | Copy it to |
| --- | --- |
| `MyFilter.clap` | Windows `C:\Program Files\Common Files\CLAP` (or, per user, `C:\Users\<you>\AppData\Local\Programs\Common\CLAP`); macOS `~/Library/Audio/Plug-Ins/CLAP`; Linux `~/.clap` |
| `vst3/MyFilterVST3.vst3`, a folder (on macOS, `MyFilterVST3.vst3` itself) | Windows `C:\Program Files\Common Files\VST3` (or `C:\Users\<you>\AppData\Local\Programs\Common\VST3`); macOS `~/Library/Audio/Plug-Ins/VST3`; Linux `~/.vst3` |
| `MyFilterAU.component` (macOS) | `~/Library/Audio/Plug-Ins/Components` |
| `MyFilterLV2.lv2` (a folder) | Windows `C:\Users\<you>\AppData\Roaming\LV2`; macOS `~/Library/Audio/Plug-Ins/LV2`; Linux `~/.lv2` |

Then rescan in the DAW. With no editor of its own the plugin opens the native
editor: a control per parameter, drawn by the SDK. `MyFilterApp` runs the same
plugin without a DAW; `MyFilterApp --help` lists what it can do.

## Where next

- [Parameters and state](parameters-and-state.md): ranges, steps, groups, and
  what a session saves.
- [The native editor](native-editor.md) or [the web editor](web-editor.md): a
  face of its own.
- [Testing your plugin](testing.md): the validators a host vendor runs first.
- [Platforms and formats](platforms-and-formats.md): what runs where.
