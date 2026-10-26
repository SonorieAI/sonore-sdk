# Platforms and formats

What one `plugin.cpp` becomes, where each piece has actually run, and what is
not available.

## Formats

| Format | CMake | Platforms |
| --- | --- | --- |
| CLAP | `sonore_add_clap`, or `CLAP` in `sonore_add_plugin` | Windows, Linux, macOS |
| VST3 | `sonore_add_vst3`, or `VST3` | Windows, Linux, macOS |
| Audio Unit (v2) | `sonore_add_au`, or `AU` (skipped off macOS) | macOS |
| Audio Unit (v3) | `sonore_add_auv3`, or `AUV3` -- an app extension, see below | macOS, iOS and iPadOS |
| LV2 | `sonore_add_lv2`, or `LV2` | Windows, Linux, macOS |
| AAX (Native) | `sonore_add_aax`, or `AAX` -- with Avid's SDK, see below | Windows, macOS |
| Standalone application | `sonore_add_standalone` | Windows, Linux, macOS, iOS and iPadOS (an app), Android (an APK) |
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

- **Android**: built with the NDK for Android 9 (API 28) and later, and run
  in an emulator: every suite, both example standalones' `--verify`, and the
  example apps by hand -- the native editor and a plugin's page in a WebView,
  touch, AAudio, the microphone permission. CI builds it and runs the same
  tests in an emulator, and keeps the APKs. Not yet on a phone.
- **iOS**: built for the simulator by CI, which runs the suites, the AUv3
  self-test and the apps' `--verify` in a booted iPhone simulator, then
  installs and launches both example apps and keeps screenshots: the synth's
  native editor in the safe area, the saturator's page in its WKWebView
  showing the plugin's values, and both apps' AUv3 extensions registered with
  the simulator's pluginkit. Not yet on an iPhone, and not yet inside
  GarageBand or AUM.
- **AUv3 on the Mac**: CI builds the example extensions inside their apps,
  registers them with pluginkit and validates them with `auval`, and drives
  the unit in process (`sdk/tests/auv3_selftest.cpp`). Not yet opened in a
  host's window.

What has not yet run on a Mac with a person at it: the CI job runs
`sdk/tests/mac_host_selftest.cpp` (Audio Unit hosting, audio workgroups), which
reports as a skip what a runner cannot do, and it builds
`sdk/tests/mac_input_selftest.cpp` (input methods, VoiceOver) without running
it. Those two are the first things to run on a Mac.

## AAX, for Pro Tools

Pro Tools loads AAX and nothing else. The format's SDK is Avid's: they give it
to registered AAX developers at developer.avid.com/aax, under their licence or
the GPLv3, and this repository does not carry it. Unpack it anywhere and
configure with its path:

```
cmake -S . -B build -DSONORE_AAX_SDK_ROOT=C:/sdks/aax-sdk-2-9-0
```

A generated project already names an AAX target, filed under the same two
codes as its Audio Unit; in your own CMake it is one more call, or one more
format in `sonore_add_plugin`:

```
sonore_add_aax(MyPluginAAX SOURCES plugin.cpp MANUFACTURER "Manu" PRODUCT "Mypl")
```

`MANUFACTURER` is your four-character code and `PRODUCT` this plugin's; Pro
Tools files the plugin under them and every session saves them, so choose them
once. Left out, `PRODUCT` is derived from the descriptor's id. On Windows the
bundle to install is `aax/MyPluginAAX.aaxplugin`, into
`C:\Program Files\Common Files\Avid\Audio\Plug-Ins`; on macOS,
`MyPluginAAX.aaxplugin` into `/Library/Application Support/Avid/Audio/Plug-Ins`.

What it is: AAX Native, mono and stereo, effects and instruments, with MIDI in
and out, Pro Tools' key input, aux output stems, latency, the master bypass,
the plugin's saved state as its one chunk, and both editors inside Pro Tools'
window. What it is not: AAX DSP (HDX hardware), AudioSuite, the page tables
control surfaces read, or a main bus wider than stereo.

A developer build of Pro Tools, from your Avid account, loads an unsigned
build, which is how to test one. A retail Pro Tools loads only a plugin Avid
has approved and PACE has signed: ask audiosdk@avid.com for the signing tools,
then sign every build with PACE's `wraptool` and your iLok before it ships.

## AUv3, for iPhone, iPad and the Mac App Store

An Audio Unit v3 is an app extension: the plugin ships inside an app, and
installing the app installs the plugin. It is the only kind of plugin iOS and
iPadOS load (GarageBand, AUM, Cubasis, Logic Pro for iPad), and the only kind
the Mac App Store accepts; Mac hosts load it beside an AUv2.

```
sonore_add_auv3(MyPluginAUv3 SOURCES plugin.cpp
  SUBTYPE "Myp3" MANUFACTURER "Manu" TYPE aufx VENDOR "My Company"
  NAME "My Plugin" BUNDLE_ID com.mycompany.myplugin VERSION 1.2.0)
```

or `AUV3 MyPluginAUv3` (with `AUV3_SUBTYPE`) in `sonore_add_plugin`. A product
shipping both formats gives the AUv3 its own subtype, or a Mac host sees one
component twice. On the Mac the extension goes into a small app built beside
it, `MyPluginAUv3.app` -- open it once, or put it in Applications, and every
host lists the plugin. On iOS it goes into the plugin's own app: name it with
`CONTAINER`, a `sonore_add_standalone` target.

Both are signed ad hoc, which a Mac and a simulator run. Shipping needs your
Developer ID (Mac) or a provisioning profile (iOS), and the App Store's review.

What it does: the parameters as the host's tree, with begin and end gestures
for every edit an editor makes; automation as timed events, sample-accurate
where the descriptor asks (`automationGranularity`); MIDI 1.0 and MIDI 2.0 in,
MIDI out; the host's tempo and transport; the same saved state as every other
format, in the AUv2 dictionary layout; factory presets; the bypass; the track's
name; and either editor, the page or the native one.

## iPhone and iPad apps

On iOS `sonore_add_standalone` builds `MyPluginApp.app`: the editor full screen
inside the safe area, sound through RemoteIO while the app is in front, and a
MIDI keyboard through CoreMIDI. An effect asks for the microphone once and hears
it only while the sound goes to headphones or an interface -- through the
speaker the microphone would hear the effect and howl. Configure for the
simulator or a device with CMake's iOS support:

```
cmake -G Ninja -S . -B build-ios -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
```

iOS 15 and later. Not yet: the software keyboard for a text field in the native
editor, and VoiceOver.

## Android apps

Android has no plugin format a host loads, so a plugin there is an app, as it
is with other frameworks. With the NDK's toolchain, `sonore_add_standalone`
builds `MyPluginApp.apk` beside the command-line program:

```
cmake -G Ninja -S . -B build-android \
  -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28
cmake --build build-android --target MyPluginAppInstall
```

`MyPluginAppInstall` puts the APK on the attached phone or emulator and starts
it. Packaging uses the Android SDK's own tools and a JDK -- no Gradle -- so set
`ANDROID_HOME` and `JAVA_HOME` if they are not found; the APK is signed with
the debug key, and a store needs yours. Android 9 (API 28) and later; MIDI from
Android 10. The app shows the plugin's page in a WebView, or the native
editor; plays through AAudio while it is in front; takes every MIDI keyboard
plugged in; and, for an effect, the microphone, by the same rule as on iOS.

## Not available, and why

- **VST2.** Steinberg ended VST2 licensing in October 2018 and no longer
  distributes the SDK, so a plugin licensed after that date cannot legally ship
  as VST2. `sonore_add_vst2()` is a configure error that says so. Every host that
  still loads VST2 also loads VST3 or CLAP.
- **Plugins on Android.** There is no plugin format Android hosts share, so
  an Android build is an app (above).
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
