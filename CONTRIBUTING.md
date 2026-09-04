# Contributing

Thanks for looking. This file is short on ceremony and specific about the two
things this repository is strict on: **every claim is measured**, and **every
public type is accounted for**.

## Before you start

- **Open an issue first for anything large.** A new DSP class or a new plugin
  format is a conversation, not a surprise pull request.
- **Small fixes need no ceremony.** A wrong constant, a missing `<cstddef>`, a
  comment that lies. Just send it.

## The two hard rules

### 1. A claim in a header must be measured in the tests

`sdk/tests/sdk_tests.cpp` is one file, and it is the specification. If your
header says a filter is −3 dB at its cutoff, a test asserts −3.01 dB at that
cutoff. If it says the latency is 24 samples, a test reads 24 back.

"It sounds right" is not a measurement, and neither is agreeing with your own
reference implementation. The K-weighting meter in this project agreed with
its own port perfectly and was 0.26 LU away from the published ITU table. Grade
against the standard, the paper, or the analytic result.

Emulations carry a further rule: cite the paper you implemented in the header,
and implement *that*, not a curve fitted to look like it. Where a model is
structural rather than component-level, the header must say so plainly (see
`passive_eq.h` for the tone).

### 2. Every public type needs a feature-map row

`scripts/feature-map.mjs` names every public class and what it is for;
`npm run verify:unclaimed` fails if a public type has no row. This is not
paperwork; it is what stops the library from growing capabilities nobody can
discover.

## The gate

```
npm run verify:sdk
```

Configures, builds the examples, runs the DSP measurements, drives the built
binaries as a real host, runs the industry validators when they are present,
and compiles the whole thing to WebAssembly. On Windows it also cross-builds
and runs the suite under WSL, because building on one OS hides real bugs. MSVC
leaks `<cstddef>` through other headers where gcc does not, and a Windows-only
run once let a platform guard return `"win32"` on Linux.

Faster loops while you work:

```
npm run verify:features     # the feature map still matches the headers
npm run verify:unclaimed    # no public type is undocumented
npm run verify:headers      # header hygiene (windows.h, include rules)
npm run verify:namespaces   # no duplicate free functions across headers
npm run verify:rt           # real-time-safety rules on audio-thread entry points
```

And the slow one, for a change that touches a wrapper, a parser, or anything
two threads share:

```
npm run verify:sanitize     # every wrapper, the suites and the parser fuzzer under
                            # ASan+UBSan; the sampler handover under TSan (~1 h)
```

It runs under gcc, natively on Linux and through WSL on Windows, and CI runs
it on every push as its own job. `verify:sdk` already runs the quick passes
of the same fuzzer, the sampler stress and the two-thread plugin probe
through ctest; the sanitizers are what turn "did not crash" into "did not
misbehave". Anything that crosses from the main thread to the audio thread
goes through an atomic (`SharedParam`, `SharedFlag`) or the UI queue, and
the TSan leg fails on anything that does not.

Three more rules the new legs enforce. An event is only as big as its header
says: cast a CLAP event through `eventAs<T>()`, never a bare
`reinterpret_cast`, or a short header reads past the host's storage. Every
process() shape the native wrappers accept must also dispatch in
`wasm_abi.h`, and the wasm test compiles every example to prove it. And
anything that changes a process path is worth a `verify:daw-render` run: it
is the one check that compares our render to a host we did not write.

A pull request is expected to be green on `verify:sdk`. If a leg of it cannot
run on your machine (no WSL, no emscripten, no validators), say so in the PR;
those legs skip loudly rather than pretending to pass, and CI runs all three
platforms anyway.

## House style

- **C++17, header-only, no dependencies.** No new third-party code without a
  conversation: the SDK exists partly to not have a framework runtime.
- **No allocation, no locks, no I/O on the audio thread.** Fixed-size storage,
  `flushDenormal` where a filter can decay to zero, and everything sized at
  `prepare()` time. `verify:rt` checks the entry points; the rest is on you.
- **Big objects are members, never locals.** The wasm stack is 64 KB and a
  reverb is hundreds of kilobytes.
- **Comments explain *why*, and record what bit you.** The headers in this
  repository read like a lab notebook on purpose: when a measurement corrected
  an assumption, the correction is written down where the next person will
  look. That is the most valuable thing in the codebase, so please add to it.
- **Report latency.** Anything that delays its signal declares
  `latencySamples()`, or every parallel mix it sits in is silently smeared.

## Certificate of origin (DCO)

This project uses the [Developer Certificate of Origin](https://developercertificate.org/)
rather than a CLA. Sign off your commits:

```
git commit -s -m "your message"
```

which appends `Signed-off-by: Your Name <your@email>`. That line means you
wrote the patch, or otherwise have the right to submit it under Apache-2.0.

Contributions are accepted under Apache-2.0, the same licence as the project,
per section 5 of the License. Please do not paste code from another framework:
this SDK's DSP is written from published mathematics precisely so that its
provenance is clean, and a single adapted function would compromise that for
everybody.

## Security

Do not open a public issue for a vulnerability. See `SECURITY.md`.
