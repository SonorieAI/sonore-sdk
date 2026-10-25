# Parameters and state

What a host can automate, and what a saved session brings back.

## The parameter table

A plugin's parameters are a `sonore::ParamInfo` table that the descriptor points
at. Its fields, in the order an aggregate initialiser fills them:

| Field | |
| --- | --- |
| `id`, `label`, `unit` | a stable machine id (never shown), the name a user reads, a unit such as `"dB"` |
| `minValue`, `maxValue`, `defaultValue` | the range and default, in PLAIN units: the numbers the user sees |
| `stepCount` | above 0, a stepped control with that many positions; 0 is continuous |
| `group` | a display group such as `"Filter"`; each format has a place for it |
| `valueNames`, `numValueNames` | what each step is called, so automation reads "Highpass" rather than "2" |
| `automatable`, `hidden`, `readOnly` | whether a host may record it, show it, or write it |
| `skew` | how the range spreads over a knob's travel; `sonore::skewForCentre(min, max, centre)` computes it |

**The table is positional.** A parameter's index is its identity to every host
and in every saved session. A shipped plugin only ever appends rows: reordering
or removing one makes old sessions load values into the wrong controls.

The wrappers and the native editor turn values into text with
`sonore::formatParamValue` and typed text back into values with
`sonore::parseParamValue`, and the web page's `sonore.format` mirrors the same
rule, so the host's readout, the knob and the page never disagree.

## State beyond the parameters

The state every format saves holds the parameter values, the bypass, a few
things of the SDK's own (the preset the user is on, the editor's size) and
whatever the DSP adds. A DSP with more to remember -- which file it loaded, a
curve the user drew -- declares `saveState` and `loadState` over a
`sonore::StateBag`, a list of keys and values. Include `<sonore/state_bag.h>`
before the struct, and make `saveState` `const`: the wrappers look for it on a
const DSP.

A host only knows a session changed when the host changed something. A plugin
that changes its own state where the wrapper cannot see it -- a file opened
from its native editor, a choice the DSP makes by itself -- says so through
`consumeStateDirty()`, which returns true once per change, or the DAW closes
without offering to save. A file a web page picks with `sonore.chooseFile`
already marks the session in CLAP and VST3; the example declares the hook
anyway, which costs nothing.

```cpp
// test-run
#define SONORE_NUM_PARAMS 3
#include <sonore/dsp.h>
#include <sonore/state_bag.h>

#include <atomic>
#include <string>

struct SonoreDsp {
  sonore::SVF filter[2];
  sonore::Smooth cutoffSm, outSm;
  bool fresh = true;

  // Main-thread state: the file the user picked, remembered with the session.
  // process() never touches it.
  std::string curvePath;
  std::atomic<bool> stateDirty{false};

  void prepare(const sonore::ProcessSpec& spec) {
    for (auto& f : filter) {
      f.setSampleRate((float) spec.sampleRate);
      f.reset();
    }
    cutoffSm.setup((float) spec.sampleRate, 20.0f);
    outSm.setup((float) spec.sampleRate, 20.0f);
    fresh = true;
  }

  void process(sonore::AudioBlock<float>& io, const float* p) {
    const int mode = (int) (p[1] + 0.5f); // a stepped parameter arrives on a step
    const float out = sonore::dbToGain(p[2]);
    if (fresh) {
      cutoffSm.snap(p[0]);
      outSm.snap(out);
      fresh = false;
    }
    const size_t channels = io.getNumChannels() < 2 ? io.getNumChannels() : 2;
    for (size_t i = 0; i < io.getNumSamples(); ++i) {
      const float hz = cutoffSm.next(p[0]), g = outSm.next(out);
      for (size_t c = 0; c < channels; ++c) {
        float& x = io.getChannelPointer(c)[i];
        filter[c].set(hz, 0.707f);
        filter[c].process(x);
        x = (mode == 0 ? filter[c].lp : mode == 1 ? filter[c].bp : filter[c].hp) * g;
      }
    }
  }

  // [main thread] The file a web page asked for with sonore.chooseFile("curve");
  // an empty path is a cancel.
  void loadFile(const char* purpose, const char* path) {
    if (std::string(purpose) != "curve" || !*path) return;
    curvePath = path;
    stateDirty.store(true);
  }
  bool consumeStateDirty() { return stateDirty.exchange(false); }

  // The bag is versioned by the plugin itself. A key that was never written
  // reads as the fallback, so a new key needs no migration at all; a key that
  // CHANGED meaning is read by the version that wrote it.
  void saveState(sonore::StateBag& bag) const {
    bag.setInt("stateVersion", 2);
    bag.setString("curvePath", curvePath);
  }
  void loadState(const sonore::StateBag& bag) {
    const int64_t version = bag.getInt("stateVersion", 1);
    // Version 1 called the same thing "file".
    curvePath = version >= 2 ? bag.getString("curvePath") : bag.getString("file");
  }
};

#include <sonore/plugin.h>

static const char* const kModeNames[] = {"Lowpass", "Bandpass", "Highpass"};

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    // A skewed control in a group: 1 kHz sits at the middle of the knob.
    {"cutoff", "Cutoff", "Hz", 20.0f, 20000.0f, 1000.0f, 0, "Filter", nullptr, 0, true, false, false,
     sonore::skewForCentre(20.0f, 20000.0f, 1000.0f)},
    // A stepped control with named steps: 0, 1, 2 are Lowpass, Bandpass, Highpass.
    {"mode", "Mode", "", 0.0f, 2.0f, 0.0f, 3, "Filter", kModeNames, 3},
    {"output", "Output", "dB", -24.0f, 12.0f, 0.0f, 0, "Output"},
};

static const sonore::PluginDescriptor kDesc = {
    "com.mycompany.statefilter", "State Filter", "My Company", "1.0.0",
    "A filter that remembers a file.", "",
    false, kParamTable, SONORE_NUM_PARAMS,
};

#include <sonore/formats.h>
```

## Versioning state

What a later version of the plugin can rely on when it opens an older session:

- **Parameters.** A session saved with fewer parameters loads the ones both
  sides share and leaves the rest at their defaults, so appending a parameter
  never orphans a session. A value out of range is clamped, and a NaN becomes
  the default.
- **The bag.** A missing key reads as the fallback you pass to `getString`,
  `getInt`, `getDouble` or `getBool`. Store a version key of your own, as
  above, for the day a key has to change meaning.
- **The SDK's own blob** is versioned too, and a blob written by a NEWER build
  is refused rather than half-read.

`sonore::StateBag::has()` tells a key that was saved empty from one that was
never saved.

## The other shape: AudioProcessorValueTreeState

Code ported from the reference arrives with a `ParameterLayout` of
`AudioParameterFloat`, `AudioParameterChoice` and `AudioParameterBool`.
`sonore::AudioProcessorValueTreeState` keeps that shape and turns it into the
same `ParamInfo` table: `parameterInfos()` is what the descriptor points at, in
layout order, so the DSP's `p[i]` is layout entry `i`. The showcase example
(`sdk/examples/showcase/plugin.cpp`) is built this way, and its editor binds
widgets with the APVTS attachments.

```cpp
// test-run
#include <sonore/value_tree_state.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

sonore::AudioProcessorValueTreeState::ParameterLayout createLayout() {
  using namespace sonore;
  NormalisableRange<float> cutoff(20.0f, 20000.0f);
  cutoff.setSkewForCentre(1000.0f);
  return AudioProcessorValueTreeState::ParameterLayout(
      std::make_unique<AudioParameterFloat>("cutoff", "Cutoff", cutoff, 1000.0f,
                                            RangedAudioParameter::Attributes().withLabel("Hz")),
      std::make_unique<AudioParameterChoice>(
          "mode", "Mode", std::vector<std::string>{"Lowpass", "Bandpass", "Highpass"}, 0),
      std::make_unique<AudioParameterBool>("bypass", "Bypass", false));
}

int main() {
  // No undo history here; an editor passes one (UndoHistory) to get undo.
  const sonore::AudioProcessorValueTreeState table(nullptr, "Filter", createLayout());
  const sonore::ParamInfo* infos = table.parameterInfos();
  const bool ok = table.numParameters() == 3 && std::strcmp(infos[0].id, "cutoff") == 0 &&
                  infos[0].defaultValue == 1000.0f && infos[1].stepCount == 3;
  for (int i = 0; i < table.numParameters(); ++i)
    std::printf("%d  %-8s %g..%g\n", i, infos[i].id, infos[i].minValue, infos[i].maxValue);
  return ok ? 0 : 1;
}
```
