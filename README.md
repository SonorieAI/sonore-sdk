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
npm run verify:reaper     # every example, CLAP, VST3 and LV2, inside REAPER (if installed):
                          # scan, renders against our own host, automation, a saved
                          # session reopened, latency, editors
npm run verify:clang      # Windows: clang's front end over every translation unit, with
                          # only Visual Studio installed -- errors and header warnings fail
npm run verify:llms       # llms.txt and llms-full.txt still match the headers
```

The three map checks are how "as many features as the frameworks people know"
stays a measured claim instead of a remembered one;
`scripts/reference-index.mjs` explains the method and
names its own blind spots.

The industry validators (`clap-validator`, `pluginval`) are fetched binaries,
not committed. On Windows, drop `clap-validator.exe` and `pluginval.exe` into
`sdk/tools/` and the gate uses them; without them it skips those legs loudly
rather than silently. On Linux and macOS the gate does not run them; CI fetches
both for its Linux and Windows jobs.

## Writing plugins with a coding agent

Three things in this repository exist for language models rather than for people,
because the way an unfamiliar C++ library usually fails an agent is not refusal,
it is invention: a class name that sounds like it should exist, a real class
given a method from a different library, and code that reads perfectly and does
not compile.

**[llms.txt](llms.txt)** is the map, in the [llmstxt.org](https://llmstxt.org)
convention: what this is, the shape of a plugin, the rules that cannot be
guessed from the API, and one line per header.
**[llms-full.txt](llms-full.txt)** adds every class and struct in every header,
which is the part that settles whether a class exists. Both are generated from
the headers by `scripts/llms-txt.mjs`, so they cannot describe an API the code
does not have, and `npm run verify:llms` fails if they have drifted.

**An MCP server** lets an agent ask instead of guess. No dependencies, no
install: it is plain JSON-RPC over stdio, and every tool only reads the checkout.

| Tool | What it answers |
| --- | --- |
| `search` | Does this class, function, alias or concept exist here, and where. Words match in any order ("ladder filter" finds `LadderFilter`). An empty result is a real answer. |
| `type` | The actual declaration of a class, function, alias or enum, with the comment explaining why it exists. |
| `header` | What one header is for, its public types, and the include line. |
| `headers` | Every header with a one-line purpose. |
| `example` | A complete working plugin and the local headers it includes: effect, instrument, sidechain, aux outputs, MIDI output, sampler, flexible channels, reverb, native editor, or the player app. |
| `rules` | The shape of a plugin, which `process` signature makes an instrument, latency reporting, and what is forbidden on the audio thread. |

"An empty result is a real answer" holds only if search never misses a name the
headers have, so `npm run verify:mcp` checks it: every public type reads back
through `type`, and every name the rules of a plugin use is found by `search`.

**[A skill](skills/sonore-sdk/SKILL.md)** puts the same map in the agent's own
context, in the [Agent Skills](https://agentskills.io) format that Claude Code,
Codex, Cursor and Antigravity read, with the part a map does not have: look a
name up before writing it, start from a generated project, and do not call a
plugin done until the validators have run. The same script generates it, so it
cannot drift either.

### Installing both for your agent

The MCP server runs from a checkout, so clone the SDK first; `"$HOME"` reads the
same in bash, zsh and PowerShell.

```
git clone https://github.com/SonorieAI/sonore-sdk "$HOME/sonore-sdk"
```

**Claude Code**

```
npx skills add SonorieAI/sonore-sdk --skill sonore-sdk -a claude-code -g
claude mcp add --scope user sonore-sdk -- node "$HOME/sonore-sdk/scripts/mcp-server.mjs"
```

**Codex** (the CLI, the IDE extension and the ChatGPT desktop app share this)

```
npx skills add SonorieAI/sonore-sdk --skill sonore-sdk -a codex -g
codex mcp add sonore-sdk -- node "$HOME/sonore-sdk/scripts/mcp-server.mjs"
```

**Cursor** and **Antigravity**: the skill with `-a cursor` or `-a antigravity`,

```
npx skills add SonorieAI/sonore-sdk --skill sonore-sdk -a cursor -g
```

then the server in `~/.cursor/mcp.json` (Cursor) or
`~/.gemini/config/mcp_config.json` (Antigravity), with the full path to your
clone; JSON does not expand `$HOME`. Any other MCP client takes the same:

```json
{
  "mcpServers": {
    "sonore-sdk": {
      "command": "node",
      "args": ["/path/to/sonore-sdk/scripts/mcp-server.mjs"]
    }
  }
}
```

Without the `skills` CLI, copy `skills/sonore-sdk` into `~/.claude/skills/`
(Claude Code), `~/.agents/skills/` (Codex, Cursor) or
`~/.gemini/config/skills/` (Antigravity).

## Status

Windows and Linux are built and tested continuously: unit tests and our own
host tests, with clap-validator and pluginval (strictness 5, advisory) run on
CI over the saturator and the synth. The VST3s have also been exercised in a
real DAW (REAPER, 2026-08-23). macOS is built and tested by CI too: the same
unit and host tests, plus Apple's `auval` over six of the eight example Audio
Units with zero warnings (since 2026-09-01). What no CI runner can
exercise is a real audio device, a MIDI device or an on-screen window, so the
CoreAudio, CoreMIDI and Cocoa window paths are compiled but have not yet been
run by a person.

## Getting help

**[Discord](https://discord.gg/4bvsn8r8CG)**: questions about writing a
plugin, a wrapper that behaves oddly in some host, or a measurement that
disagrees with yours. That last one is the most useful thing you can bring:
every claim in this repository is meant to be reproducible, so a number that
does not reproduce is a bug report.

Bugs and feature requests belong in the issue tracker, where they can be
found again. Security reports go through [`SECURITY.md`](SECURITY.md).

## Licence

Apache License 2.0 (see [`LICENSE`](LICENSE)). Use it in closed-source and
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
the licence grant: build what you like with it, just don't call your fork the
official one.

Licensing or naming questions: **legal@sonorie.com**.
