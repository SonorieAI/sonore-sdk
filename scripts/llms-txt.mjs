// Generates llms.txt and llms-full.txt from the headers themselves.
//
// WHY GENERATED. A hand-written index of an API drifts from the API, and the
// failure is worse than having no file: an agent that reads a class which no
// longer exists writes code that cannot compile, and it writes it confidently.
// Everything below is derived from `sdk/include/sonore`, so the map is wrong
// only when the code is.
//
// WHAT THE TWO FILES ARE FOR. llms.txt is the map: what this is, the shape of a
// plugin, the rules that are not guessable, and one line per header. It is meant
// to be read whole. llms-full.txt adds every public type per header, which is
// the part that stops a model inventing a class name that sounds plausible.
//
// The convention is llmstxt.org: a markdown file at the repository root that a
// model can read instead of crawling the source.
//
// AND THE SKILL. skills/sonore-sdk/SKILL.md carries the same shape and rules
// into the agent's own context, in the Agent Skills format (agentskills.io)
// that Claude Code, Codex and Cursor read, with the part llms.txt does not
// have: what to do, in order. It is generated here for the same reason the map
// is: a skill copied by hand would be the first thing in the repository to
// describe an SDK that has moved on.
//
// Run: node scripts/llms-txt.mjs        (writes all three files)
//      node scripts/llms-txt.mjs --check (fails if any is out of date)

import { readFileSync, writeFileSync, readdirSync, statSync, existsSync, mkdirSync } from "node:fs";
import { join, relative, dirname, sep } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const INC = join(ROOT, "sdk", "include", "sonore");
const REPO = "https://github.com/SonorieAI/sonore-sdk/blob/main";

// ── Reading the headers ─────────────────────────────────────────────────────

function headerFiles(dir, out = []) {
  for (const name of readdirSync(dir).sort()) {
    const full = join(dir, name);
    if (statSync(full).isDirectory()) headerFiles(full, out);
    else if (name.endsWith(".h")) out.push(full);
  }
  return out;
}

/** The one-line purpose a header states about itself. Every header opens with
 *  an SPDX line and then says what it is; that sentence is the summary, and
 *  taking it from the file means it cannot contradict the file. */
function purposeOf(text) {
  for (const raw of text.split("\n").slice(0, 12)) {
    const line = raw.trim();
    if (!line.startsWith("//")) continue;
    const body = line.replace(/^\/+\s?/, "").trim();
    if (!body || /^SPDX/i.test(body)) continue;
    return body.replace(/^Sonore SDK[:,]?\s*/i, "").replace(/\.$/, "");
  }
  return "";
}

/**
 * Public types: a class or struct declared at NAMESPACE scope whose name starts
 * with a capital, skipping anything inside a `detail` namespace and anything
 * nested inside another type. Same rule as scripts/unclaimed.mjs, deliberately:
 * two different answers to "what is public" would be one answer too many.
 */
function publicTypes(text) {
  const clean = text.replace(/\/\*[\s\S]*?\*\//g, "");
  const names = [];
  const namespaces = [];
  let depth = 0;
  for (const line of clean.split("\n")) {
    const ns = line.match(/\bnamespace\s+([A-Za-z_]\w*)\s*\{/);
    if (ns) { namespaces.push(ns[1]); continue; }
    const decl = line.match(/^\s*(?:template\s*<[^>]*>\s*)?(class|struct)\s+([A-Z]\w*)\b/);
    if (decl && depth === 0 && !/;\s*$/.test(line)) {
      const inDetail = namespaces.some((n) => /detail$/i.test(n));
      if (!inDetail) names.push(decl[2]);
    }
    for (const ch of line) {
      if (ch === "{") depth++;
      else if (ch === "}") { if (depth > 0) depth--; else namespaces.pop(); }
    }
  }
  return [...new Set(names)];
}

// ── The parts a parser cannot derive ────────────────────────────────────────
//
// Everything above comes from the headers. What follows is the knowledge that
// is NOT in any single header: the shape a plugin takes, and the handful of
// rules that are invisible until they bite. These are the paragraphs worth
// writing by hand, and they are short on purpose.

const SHAPE = `\`\`\`cpp
// One file is a whole plugin. The same source builds every format: the build
// defines SONORE_BUILD_<FORMAT> for each one it makes (sonore_add_plugin does),
// and formats.h at the bottom includes what each of them needs.

#define SONORE_NUM_PARAMS 2
#include <sonore/dsp.h>

struct SonoreDsp {
  sonore::Smooth gainSm;

  void prepare(const sonore::ProcessSpec& spec) {
    gainSm.setup((float) spec.sampleRate, 12.0f);
    gainSm.snap(1.0f);
  }

  // p[] is the parameter array, in the order of kParamTable below.
  void process(sonore::AudioBlock<float>& io, const float* p) {
    for (size_t i = 0; i < io.getNumSamples(); ++i) {
      const float g = gainSm.next(sonore::dbToGain(p[0]));
      for (size_t c = 0; c < io.getNumChannels(); ++c)
        io.getChannelPointer(c)[i] *= g;
    }
  }
};

#include <sonore/plugin.h>

// id, label, unit, min, max, default, stepCount
static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"gain", "Gain", "dB", -24.0f, 12.0f, 0.0f, 0},
    {"mix",  "Mix",  "%",    0.0f, 100.0f, 100.0f, 0},
};

static const sonore::PluginDescriptor kDesc = {
    "com.example.gain", "Example Gain", "Vendor", "1.0.0",
    "A gain stage.", "https://example.com",
    false,                       // true for an instrument
    kParamTable, SONORE_NUM_PARAMS,
    nullptr, 0,                  // factory presets, if any
    "utility",                   // category
};

#include <sonore/formats.h> // CLAP, and every format this target is built as
\`\`\``;

const RULES = [
  ["Pick the process signature you need; the wrapper detects it at compile time.",
   "`process(AudioBlock<float>&, const float* p)` is an effect. Add a third " +
   "`sonore::MidiBuffer&` and the plugin becomes an INSTRUMENT: note ports, no " +
   "audio input bus. Add a second `AudioBlock<float>&` before `p` for a " +
   "sidechain input. Take `sonore::ProcessContext&` instead when you need aux " +
   "outputs, MIDI output or per-note expression. Writing the signature is how " +
   "you ask; there is no flag to set."],

  ["Declare latency or the host cannot align you.",
   "Anything that delays its signal (look-ahead limiter, convolver, " +
   "oversampler) must define `int latencySamples() const`. A plugin that " +
   "delays and reports zero silently smears every parallel path it sits in. " +
   "It is detected the same way as the process signature."],

  ["prepare() is where you learn the sample rate, and the only place to allocate.",
   "`process()` runs on the audio thread: no allocation, no locks, no file or " +
   "network access, no `new`, no `std::string`. Size every buffer in " +
   "`prepare()`. The rate is not fixed: a plugin that assumes 44100 or 48000 " +
   "is a plugin that breaks at 96k."],

  ["Rebuild filter coefficients only when a control moved.",
   "Recomputing a biquad or a ladder every block costs more than the filter " +
   "does. Keep the last value and compare, as the examples do."],

  ["Big objects are members, never locals.",
   "`FdnReverb<>` is about 1.1 MB, `StereoDelay<>` about 1.5 MB, " +
   "`Convolver<256,32>` about 140 KB. Emscripten's default stack is 64 KB, so " +
   "one of these as a local variable crashes the browser build while the " +
   "native one looks fine."],

  ["Parameters are plain values, in the order of the table.",
   "`p[0]` is the first row of `kParamTable`. Not normalised: if the row says " +
   "-24 to 12 dB, `p[0]` is decibels. The wrappers convert to and from each " +
   "format's own convention."],
];

// ── Emitting ────────────────────────────────────────────────────────────────

function collect() {
  return headerFiles(INC).map((file) => {
    const text = readFileSync(file, "utf8");
    const rel = relative(ROOT, file).split(sep).join("/");
    return {
      rel,
      name: rel.split("/").pop(),
      group: rel.includes("/gfx/") ? "gfx" : "core",
      purpose: purposeOf(text),
      types: publicTypes(text),
    };
  });
}

function preamble(headers) {
  const typeCount = headers.reduce((n, h) => n + h.types.length, 0);
  return `# Sonore SDK

> A header-only C++17 audio plugin framework. One source file builds a \`.clap\`,
> a \`.vst3\`, an AUv2 \`.component\`, an \`.lv2\` bundle and a standalone
> application. No framework runtime is linked into the result and there is no
> separate framework licence to obtain. Apache-2.0.

This file exists for language models and coding agents. It is generated from the
headers by \`scripts/llms-txt.mjs\`, so it cannot describe an API the code does
not have. ${headers.length} headers, ${typeCount} public types.

Two things are worth knowing before writing any code against it. Every claim in
this repository has a measurement behind it in \`sdk/tests\`, so if you are
unsure whether something behaves as documented, the test says. And the DSP is
implemented from published mathematics rather than adapted from another
framework, so names will not always match what a similar library calls them:
check the inventory below rather than assuming.

## The shape of a plugin

${SHAPE}

## Rules that are not guessable from the API

${RULES.map(([t, b]) => `- **${t}** ${b}`).join("\n")}

## Building

\`\`\`cmake
add_subdirectory(sdk)
sonore_add_plugin(MyPlugin SOURCES plugin.cpp)   # CLAP, VST3, AU, LV2, standalone
\`\`\`

\`npm run verify:sdk\` builds the examples, runs the DSP measurements, drives the
built binaries as a host, and compiles the same DSP to WebAssembly.
`;
}

function llmsTxt(headers) {
  const group = (g) => headers.filter((h) => h.group === g);
  const line = (h) =>
    `- [${h.name}](${REPO}/${h.rel})${h.purpose ? `: ${h.purpose}` : ""}`;
  return `${preamble(headers)}
## Headers

${group("core").map(line).join("\n")}

### Native UI toolkit (gfx/)

${group("gfx").map(line).join("\n")}

## Optional

- [llms-full.txt](${REPO}/llms-full.txt): the same, plus every public type in
  each header. Read this one before naming a class you have not verified exists.
- [sdk/README.md](${REPO}/sdk/README.md): the full prose documentation.
- [examples](${REPO}/sdk/examples): nine working plugins, including an
  instrument, a sidechain effect and one with multiple output buses.
`;
}

function llmsFullTxt(headers) {
  const body = headers
    .map((h) => {
      const types = h.types.length ? h.types.join(", ") : "(no public types)";
      return `### ${h.name}\n\n${h.purpose ? h.purpose + "\n\n" : ""}\`${h.rel}\`\n\n${types}`;
    })
    .join("\n\n");
  return `${preamble(headers)}
## Every public type, by header

If a name is not in this list, the SDK does not have it. Reaching for a class
that sounds like it should exist is the most common way generated code fails to
compile against this library.

${body}
`;
}

// ── The skill ───────────────────────────────────────────────────────────────

// What the skill points at has to exist, or the first instruction an agent
// follows is a dead end. These are checked rather than trusted.
const NEW_PLUGIN = "sdk/cmake/sonore_new_plugin.cmake";
const MCP_SERVER = "scripts/mcp-server.mjs";

function guides() {
  const dir = join(ROOT, "sdk", "docs", "guides");
  return readdirSync(dir).filter((f) => f.endsWith(".md")).sort().map((f) => {
    const title = readFileSync(join(dir, f), "utf8").match(/^# (.+)$/m);
    return `- \`sdk/docs/guides/${f}\`${title ? `: ${title[1]}` : ""}`;
  });
}

function examples() {
  const dir = join(ROOT, "sdk", "examples");
  return readdirSync(dir).sort().flatMap((d) => {
    const main = ["plugin.cpp", "main.cpp"].find((f) => existsSync(join(dir, d, f)));
    return main ? [`\`${d}/${main}\``] : [];
  });
}

function skillMd(headers) {
  for (const rel of [NEW_PLUGIN, MCP_SERVER]) {
    if (!existsSync(join(ROOT, rel))) throw new Error(`the skill points at ${rel}, which does not exist`);
  }
  const typeCount = headers.reduce((n, h) => n + h.types.length, 0);
  return `---
name: sonore-sdk
description: Writes, extends and debugs audio plugins on the Sonore SDK, the header-only C++17 framework where one plugin.cpp builds CLAP, VST3, AU, LV2 and a standalone app. Use when a project includes <sonore/...> headers or calls sonore_add_plugin, or when asked for an audio effect, instrument or DSP plugin built on Sonore.
license: Apache-2.0
metadata:
  author: Sonorie
  homepage: https://sonore.dev
---

<!-- Generated by scripts/llms-txt.mjs. Edit the script, not this file: npm run verify:llms fails when they differ. -->

# Sonore SDK

One \`plugin.cpp\` is a whole plugin: a DSP struct, a parameter table and a
descriptor. The build turns it into a \`.clap\`, a \`.vst3\`, an AUv2
\`.component\`, an \`.lv2\` bundle and a standalone app. The SDK checkout is the
folder with \`llms.txt\` at its root; a project reaches it through
\`add_subdirectory\` or \`find_package(SonoreSDK)\`.

## Look a name up before you write it

Code against an unfamiliar C++ library rarely fails by refusing. It fails by
invention: a class that sounds right, or a real class given a method from
another framework. Sonore's DSP is written from the published mathematics, not
adapted from another library, so its names are its own. Before writing any type
or method you have not seen in this SDK:

- **With the \`sonore-sdk\` MCP server connected**, ask it. \`search\` says
  whether a name exists, and an empty result is a real answer. \`type\` gives
  the exact declaration, \`header\` the include line, \`example\` a complete
  working plugin of a given kind, and \`rules\` the rules below.
- **Without it**, read \`llms-full.txt\` at the SDK root: every public type in
  every header (${headers.length} headers, ${typeCount} types). A name that is
  not in it does not exist. The headers are in \`sdk/include/sonore/\`.

## Start from a generated project

From the SDK checkout:

\`\`\`
cmake -DNAME="My Filter" -DVENDOR="My Company" -P ${NEW_PLUGIN}
cmake -S MyFilter -B MyFilter/build
cmake --build MyFilter/build --config Release
ctest --test-dir MyFilter/build -C Release
\`\`\`

\`-DTYPE=instrument\` starts an eight-voice synth instead of a filter. The id
the generator derives (\`com.mycompany.myfilter\`) is the plugin's identity in
every saved session: pass \`-DID=\` now if that is not the one to keep. To add a
plugin to an existing CMake project instead, see Building below.

## The shape of a plugin

${SHAPE}

## Rules that are not guessable from the API

${RULES.map(([t, b]) => `- **${t}** ${b}`).join("\n")}

## Building

\`\`\`cmake
add_subdirectory(sdk)
sonore_add_plugin(MyPlugin SOURCES plugin.cpp)   # CLAP, VST3, AU, LV2, standalone
\`\`\`

## Done means built and validated

Compiling is not finished. Before calling a plugin done:

1. Build Release and run \`ctest\`. A generated project's tests include its
   self-checks; built against a checkout, they include the real-time audit.
2. Run the format owners' validators on what was built:
   \`clap-validator validate MyFilter.clap\` and
   \`pluginval --strictness-level 10 --validate vst3/MyFilterVST3.vst3\`.
   On macOS, \`auval -v aufx <subtype> <manufacturer>\` after copying the
   \`.component\` into \`~/Library/Audio/Plug-Ins/Components\`.
3. Report what ran and what it printed. A step that was skipped is said to be
   skipped.

## Where the rest is

- \`sdk/README.md\`: the full documentation.
${guides().join("\n")}
- \`sdk/examples/\`, complete projects: ${examples().join(", ")}.

## Connecting the MCP server

\`${MCP_SERVER}\` in the SDK checkout is plain JSON-RPC over stdio, with no
dependencies and nothing to install.

- Claude Code: \`claude mcp add sonore-sdk -- node /path/to/sonore-sdk/${MCP_SERVER}\`
- Codex, in \`~/.codex/config.toml\`:

  \`\`\`toml
  [mcp_servers.sonore-sdk]
  command = "node"
  args = ["/path/to/sonore-sdk/${MCP_SERVER}"]
  \`\`\`

- Cursor (\`.cursor/mcp.json\`) and other MCP clients:

  \`\`\`json
  { "mcpServers": { "sonore-sdk": {
      "command": "node", "args": ["/path/to/sonore-sdk/${MCP_SERVER}"] } } }
  \`\`\`
`;
}

// ── Main ────────────────────────────────────────────────────────────────────

const headers = collect();
const files = [
  ["llms.txt", llmsTxt(headers)],
  ["llms-full.txt", llmsFullTxt(headers)],
  ["skills/sonore-sdk/SKILL.md", skillMd(headers)],
];

const check = process.argv.includes("--check");
let stale = 0;
for (const [name, content] of files) {
  const path = join(ROOT, name);
  let current = "";
  try { current = readFileSync(path, "utf8"); } catch { /* not written yet */ }
  if (current === content) continue;
  if (check) {
    console.error(`✗ ${name} is out of date. Run: node scripts/llms-txt.mjs`);
    stale++;
  } else {
    mkdirSync(dirname(path), { recursive: true });
    writeFileSync(path, content);
    console.log(`✓ wrote ${name} (${content.length} chars)`);
  }
}
if (check) {
  if (stale) process.exit(1);
  console.log(`✓ llms.txt, llms-full.txt and the skill match the headers.`);
}
