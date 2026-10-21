# Testing your plugin

From a unit test of one filter to a DAW driving the built binary.

## What a generated project already runs

`ctest` in a project from `sonore_new_plugin.cmake` runs:

- **`MyFilterApp --verify`**: the standalone build renders the plugin offline,
  with no audio device and no window, and fails on a NaN or an infinity, on
  silence where there should be sound, on a render that blows up, and on a
  malformed descriptor. An effect is also fed a file it writes itself; an
  instrument must sound on a held note and fall silent after its release.
- **The real-time audit** (`sonore_add_rt_test`, with a source checkout of the
  SDK): your plugin's source compiled into a program that counts every
  allocation made on the audio thread, across sample rates and block sizes.
  Allocations only; locks are not measured.

`MyFilterApp --render out.wav 2` writes two seconds of the plugin to a WAV, for
listening or for a comparison of your own.

## Unit tests

`sonore/unit_test.h` is a small framework in the shape code from the reference
expects: a `sonore::UnitTest` per area, `beginTest` and the `expect` family
inside `runTest()`, a static instance that registers itself, and a
`sonore::UnitTestRunner`. Each test gets a reproducible random generator
(`getRandom()`), and the runner logs the seed so a failure can be replayed.

```cpp
// test-run
#include <sonore/dsp.h>
#include <sonore/unit_test.h>

#include <algorithm>
#include <cmath>

struct LowpassTests : sonore::UnitTest {
  LowpassTests() : UnitTest("Lowpass", "dsp") {}

  void runTest() override {
    beginTest("DC passes");
    sonore::SVF f;
    f.setSampleRate(48000.0f);
    f.set(1000.0f, 0.707f);
    for (int i = 0; i < 48000; ++i) f.process(1.0f);
    expectWithinAbsoluteError(f.lp, 1.0f, 1e-3f);

    beginTest("Nyquist does not");
    f.reset();
    float peak = 0.0f;
    for (int i = 0; i < 48000; ++i) {
      f.process(i % 2 ? -1.0f : 1.0f);
      if (i >= 4800) peak = (std::max)(peak, std::fabs(f.lp)); // after the transient
    }
    expectLessThan(peak, 0.01f, "Nyquist leaks through the lowpass");
  }
};
static LowpassTests lowpassTests;

int main() {
  sonore::UnitTestRunner runner;
  runner.runAllTests();
  return runner.getTotalFailures() == 0 ? 0 : 1;
}
```

Register it like any program: `add_executable`, `target_link_libraries(... sonore_sdk)`,
`add_test`. Grade against a number you can defend -- the standard, the paper,
the analytic result -- not against what the code printed last time.

## The format owners' validators

What a host vendor or a store runs first:

- **clap-validator** (github.com/free-audio/clap-validator):
  `clap-validator validate MyFilter.clap`.
- **pluginval** (github.com/Tracktion/pluginval):
  `pluginval --strictness-level 10 --validate vst3/MyFilterVST3.vst3`. The SDK's
  examples pass at strictness 10, editor tests included on Windows.
- **auval** on macOS, for the Audio Unit: `auval -v aufx <subtype> <manufacturer>`
  after copying the `.component` into `~/Library/Audio/Plug-Ins/Components`.

On Windows, clap-validator refuses a preset location that does not begin with
`/`, which every drive-letter path fails, so its preset-discovery checks fail
on the default preset folder. Run it with `SONORE_PRESET_ROOT` set to a folder
spelled `//?/C:/...`: Windows opens that like any other path, and the
validator accepts it.

## A real DAW

`npm run verify:reaper` (from the repository root, with REAPER installed and a
Release build of the SDK) hands the CLAP, VST3 and LV2 build of every SDK
example to REAPER and checks what REAPER made of them: the scan, offline renders
against the SDK's own host, automation, a session saved and reopened, latency
compensation, editors opening and closing. It runs REAPER with a profile of its
own and never touches yours; `--only`, `--skip-editor` and `--rates` narrow it.
It is the check to run after changing anything in a process path, state,
parameters, presets or latency in the SDK itself.

## The SDK's own tests

For a change to the SDK, each area has a standalone test program,
`sdk/tests/<area>_test.cpp`: a main function, a `check(bool, const char*)` helper
printing `  ok   message` or `  FAIL message`, real numbers rather than "it
compiles" (round trips, published test vectors, reference outputs), a last line
`<AREA> TESTS PASSED` or `FAILED`, and a non-zero exit on any failure. Parsers
of untrusted input also get a fuzzer, `sdk/tests/fuzz_<area>.cpp`, run under
AddressSanitizer. `npm run verify:sdk` builds and runs all of it with the
validators and the browser build; `npm run verify:sanitize` runs the slow
sanitizer legs. CONTRIBUTING.md at the repository root has the rules.
