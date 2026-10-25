# Versions and compatibility

The SDK follows Semantic Versioning from 1.0.0. What a version number promises
-- code that built keeps building within 1.x, nothing is removed without a
deprecation first, and what a user saved keeps loading -- is written down once,
in the SDK's [README](../../README.md), section *Versions and compatibility*.
What has changed, and which changes can affect an existing plugin's sound or a
saved session, is in `sdk/CHANGELOG.md`.

## In code

`<sonore/version.h>` says which SDK a build has: `SONORE_SDK_VERSION_MAJOR`,
`_MINOR`, `_PATCH`, `SONORE_SDK_VERSION_STRING`, and `SONORE_SDK_VERSION`, one
number to compare (major * 10000 + minor * 100 + patch). `audio.h` includes
it, so `dsp.h`, `plugin.h` and every plugin have it; anywhere else, include it
yourself before testing the macros, as the example below does, since an `#if`
reads a macro that was never defined as 0.

A part of the API on its way out is marked
`SONORE_DEPRECATED("<version>: use <replacement>")`: it keeps working and warns
at every use for at least one minor release, and goes only in the next major. A build that cannot act on the
warnings yet defines `SONORE_NO_DEPRECATION_WARNINGS`.

```cpp
// test-run
#include <sonore/version.h>

#include <cstdio>

#if SONORE_SDK_VERSION < 10000
#error "this plugin needs Sonore SDK 1.0 or later"
#endif

// How the SDK marks what is going away; a call to oldGain() warns at compile time.
SONORE_DEPRECATED("1.0: use gain()") inline float oldGain() { return 1.0f; }
inline float gain() { return 1.0f; }

int main() {
  std::printf("built against Sonore SDK %s (%d)\n", SONORE_SDK_VERSION_STRING, SONORE_SDK_VERSION);
  return gain() == 1.0f ? 0 : 1;
}
```

## What is not covered

Anything in a `detail` namespace, the tests, the scripts and the feature map,
and behaviour the documentation calls unspecified. The API reference leaves the
`detail` namespaces out for that reason.

## Your own plugin's versions

The SDK's promise covers the SDK. A plugin keeps its own sessions loading by the
rules in [parameters and state](parameters-and-state.md): append parameters,
never reorder them, and version the plugin's own `StateBag` keys.
