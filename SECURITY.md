# Security policy

## Reporting a vulnerability

**Please do not open a public issue.**

Report privately through GitHub's
[private vulnerability reporting](https://github.com/SonorieAI/sonore-sdk/security/advisories/new)
on this repository. If you cannot use that, email **support@sonorie.com** with
`SECURITY` in the subject.

Please include what you need to demonstrate it: the affected header or format
wrapper, a minimal plugin or input that triggers it, and what you observed.

You will get an acknowledgement within a few days. We do not run a bounty
programme, but reports are taken
seriously and fixes land in the open with credit unless you ask otherwise.

## What is in scope

The interesting surface of an audio plugin framework is **untrusted input
reaching a host process**:

- **Audio file parsing** (`audiofile.h`, `wav.h`, and the vendored MP3/Vorbis
  decoders): a malformed file must not read or write out of bounds. This is
  the highest-value area in the repository.
- **Plugin state loading** (`clap_wrapper.h`, `vst3_wrapper.h`, and friends):
  a corrupt or hostile state blob arrives from a session file and must be
  refused, never trusted. Parameter counts are already capped for this reason.
- **The web UI bridge** (`gui.h` and the platform webviews): a plugin's
  interface is model-authored HTML in an OS webview.
- **Anything reached over a network**: the OSC codec and transport, and the
  licence activation path in `license_runtime.h`.
- **The image, font and archive readers** under `sdk/include/sonore/gfx`.

## What is out of scope

- **Vendored third-party code**: report those upstream (see
  `THIRD_PARTY_NOTICES.md`); we will pull the fix.
- **A plugin built with this SDK doing something harmful on purpose.** The SDK
  compiles the DSP it is given; it is not a sandbox.
- **Licence enforcement being defeatable on a machine its owner controls.**
  The offline activation model is documented as clock-rollback-defeatable in
  `license_runtime.h`; that is an accepted trade, not a vulnerability.
- **Denial of service from a plugin misusing its own audio thread**: an
  infinite loop in a DSP is a bug in that DSP.

## Supported versions

The SDK is pre-1.0 and header-only: fixes land on `main`, and there are no
backported release branches yet. Pin a commit if you need reproducibility.
