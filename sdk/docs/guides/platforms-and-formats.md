# Platforms and formats

What one `plugin.cpp` becomes, where each piece has actually run, and what is
not available.

## Formats

| Format | CMake | Platforms |
| --- | --- | --- |
| CLAP | `sonore_add_clap`, or `CLAP` in `sonore_add_plugin` | Windows, Linux, macOS |
| VST3 | `sonore_add_vst3`, or `VST3` | Windows, Linux, macOS |
| Audio Unit (v2) | `sonore_add_au`, or `AU` (skipped off macOS) | macOS |
| LV2 | `sonore_add_lv2`, or `LV2` | Windows, Linux, macOS |
| Standalone application | `sonore_add_standalone` | Windows, Linux, macOS |
| Browser (WebAssembly) | emscripten over `wasm_abi.h` | a browser, driven from an AudioWorklet (the worklet host is Sonorie's studio, not this repository) |

The CLAP wrapper owns the shared machinery: the DSP instance, parameters,
state and the page bridge. Every other format adapts it, so a plugin cannot
behave differently depending on which format a host loaded. `sonore_add_plugin`
compiles the source once and links that object into each format's bundle.

VST3 is built against Steinberg's one-file C API header (VST 3.8.1,
BSD-3-Clause), so the plugin links no Steinberg SDK. Since VST 3.8 (October
2025) Steinberg asks for no agreement to distribute a VST3, commercially or
not. The VST name and logo are optional; using them means following
Steinberg's usage guidelines.

The browser build takes the DSP half of the source, everything before
`#include <sonore/plugin.h>`, so the preview runs the same DSP code as the
shipped plugin. The SDK's [README](../../README.md) has the emscripten command
line and the functions the worklet calls.

## What has run where

- **Windows**: everything. The whole test suite with MSVC (Release and Debug)
  and clang-cl, the plugins under clap-validator and pluginval, the examples
  inside REAPER (`npm run verify:reaper`), and the native window with a real UI
  Automation client walking it.
- **Linux**: built and tested in CI on Ubuntu 24.04: the same test suite, the
  GUI tests under a virtual X display, and the validators as an advisory step.
  Also under WSL by `npm run verify:sdk` on Windows. Window embedding is proven
  in a container (`sdk/tests/linux-gui.Dockerfile`).
- **macOS**: built and tested in CI on macOS 14: the same test suite, with the
  example Audio Units installed so the hosting tests load them. The Audio Unit
  wrapper passes Apple's `auval` on six of the nine example AUs (not yet run on
  the sampler, the reverb and the showcase).

What has not yet run on a Mac with a person at it: the CI job runs
`sdk/tests/mac_host_selftest.cpp` (Audio Unit hosting, audio workgroups), which
reports as a skip what a runner cannot do, and it builds
`sdk/tests/mac_input_selftest.cpp` (input methods, VoiceOver) without running
it. Those two are the first things to run on a Mac.

## Not available, and why

- **AAX.** Avid's SDK is distributed only under their developer agreement, so
  there is no public header to build against, and shipping AAX binaries needs
  PACE signing. `sonore_add_aax()` exists and says why it is skipping. Once the
  SDK is obtained, `SONORE_AAX_SDK_ROOT` points at it; the wrapper itself is
  still to be written.
- **VST2.** Steinberg ended VST2 licensing in October 2018 and no longer
  distributes the SDK, so a plugin licensed after that date cannot legally ship
  as VST2. `sonore_add_vst2()` is a configure error that says so. Every host that
  still loads VST2 also loads VST3 or CLAP.
- **AUv3.** An app extension shipped inside a containing application, which iOS
  and App Store distribution require. It needs an Xcode extension target and a
  host application, not CMake alone, and can only be checked on a Mac.
  `sonore_add_auv3()` skips loudly. AUv2 is what every macOS desktop host loads.
- **ARA** is a hook too (`sonore_add_ara()`): its SDK is Apache-2.0, but an ARA
  plugin can only be proven inside an ARA host, and none is in the loop.

## Which format, which host, at run time

A DSP that declares `setHostInfo` is told once, when the plugin is created,
which host it is in. (An Audio Unit is told when its host names itself, which
an AU host may not do; LV2 never says.) `sonore::PluginHostType` turns that
into a known host and version, and `PluginHostType::getPluginLoadedAs()` says
which format's entry point the host used.

Use it for a support log or an About box, not to change what the DSP does.
Branching the DSP on a host's name grows a code path per DAW that nobody can
test, and every host difference the SDK knows about is asked of the host as a
capability instead.

```cpp
// test-run
#define SONORE_NUM_PARAMS 1
#include <sonore/dsp.h>
#include <sonore/plugin_host_type.h>

#include <string>

struct SonoreDsp {
  std::string hostLine; // [main thread] for the About box

  void setHostInfo(const sonore::HostInfo& info) {
    using Format = sonore::PluginHostType::PluginFormat;
    const sonore::PluginHostType host(info);
    const Format format = sonore::PluginHostType::getPluginLoadedAs();
    hostLine = std::string(host.getHostDescription()) + " " + host.getHostVersion() + ", as " +
               (format == Format::Clap         ? "CLAP"
                : format == Format::Vst3       ? "VST3"
                : format == Format::AudioUnit  ? "Audio Unit"
                : format == Format::Lv2        ? "LV2"
                : format == Format::Standalone ? "the standalone application"
                                               : "an unknown format");
  }

  void prepare(const sonore::ProcessSpec&) {}

  void process(sonore::AudioBlock<float>& io, const float* p) {
    const float g = sonore::dbToGain(p[0]);
    for (size_t c = 0; c < io.getNumChannels(); ++c)
      for (size_t i = 0; i < io.getNumSamples(); ++i) io.getChannelPointer(c)[i] *= g;
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"gain", "Gain", "dB", -24.0f, 24.0f, 0.0f},
};

static const sonore::PluginDescriptor kDesc = {
    "com.mycompany.hostaware", "Host Aware", "My Company", "1.0.0",
    "A gain that knows where it is.", "",
    false, kParamTable, SONORE_NUM_PARAMS,
};

#include <sonore/formats.h>
```

(A gain with no smoothing, as here, zippers under automation; the
[getting started](getting-started.md) plugin shows the smoothed form.)
