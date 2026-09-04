# Sonore SDK

A header-only C++17 audio plugin framework. One plugin source file builds a
`.clap`, a `.vst3`, an AUv2 `.component`, an `.lv2` bundle and a standalone
application, with a software-rasterised native UI. No framework runtime, no
framework licence.

**The framework itself lives in [`sdk/`](sdk/), and so does
[its real README](sdk/README.md)**: what's in it, how to write a plugin, how
everything is verified. This file only explains the repository around it.

## Layout

```
sdk/        the framework: headers, examples, tests, vendored third-party
scripts/    the verification instruments (run from the repo root, see below)
.github/    three-platform CI
```

The `sdk/` prefix is kept rather than hoisted to the root: the framework sits
beside the verification instruments in `scripts/`, and every script and
workflow refers to paths under `sdk/`.

## Verifying

```
npm run verify:sdk        # the whole gate: build, DSP measurements, host tests,
                          # validators, wasm round-trip, Linux leg under WSL
npm run verify:sanitize   # slow, opt-in: every wrapper, the suites and the parser
                          # fuzzer under ASan+UBSan, the sampler handover under TSan
npm run verify:features   # every capability the feature map claims still exists
npm run verify:unclaimed  # every public type in the headers appears in the map
npm run verify:parity     # every audited reference class carries a decision
npm run verify:daw        # ask REAPER (if installed) what it makes of the plugins
```

The three map checks are how "as many features as the frameworks people know"
stays a measured claim instead of a remembered one;
`scripts/reference-index.mjs` documents the method.

The industry validators (`clap-validator`, `pluginval`) are fetched binaries,
not committed. Drop them into `sdk/tools/` and the gate uses them; when they
are absent it says so explicitly.

## Status

Windows and Linux are built and tested continuously (unit tests, our own host
tests, clap-validator and pluginval strictness 10), and the VST3s have been
exercised in a real DAW (REAPER, 2026-08-23). macOS is built and tested by CI
too: the same unit and host tests, plus Apple's `auval` over the six example
Audio Units with zero warnings (since 2026-09-01).

## Getting help

**[Discord](https://discord.gg/4bvsn8r8CG)** for questions about writing a
plugin, a wrapper that behaves oddly in some host, or a measurement that
disagrees with yours. That last one is the most useful thing you can bring:
every claim in this repository is meant to be reproducible, so a number that
does not reproduce is a bug report.

Bugs and feature requests belong in the issue tracker, where they can be
found again. Security reports go through [`SECURITY.md`](SECURITY.md).

## Licence

Apache License 2.0; see [`LICENSE`](LICENSE). Use it in closed-source and
commercial products; the patent grant and its termination clause come with it.

Two files are worth reading before you ship something built with this:

- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md): the vendored headers
  (CLAP, LV2, VST3 C API, minimp3, stb_vorbis) and their licences, plus the
  format obligations no software licence can grant you: VST3 requires
  registration with Steinberg for commercial distribution, AAX requires Avid's
  agreement and PACE signing, and VST2 is deliberately impossible.
- [`CONTRIBUTING.md`](CONTRIBUTING.md): the two rules this repository is
  strict about (every claim is measured; every public type is in the feature
  map), and the DCO sign-off contributions use.

Security reports go through [`SECURITY.md`](SECURITY.md), not the issue
tracker.

The Sonore SDK is built and maintained as the plugin framework behind
[Sonorie](https://sonorie.com). "Sonore" and "Sonorie" are names, not part of
the licence grant. Build what you like with it, just don't call your fork the
official one.

Licensing or naming questions: **legal@sonorie.com**.
