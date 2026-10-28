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
    return body.replace(/^Sonore SDK[:,.]?\s*/i, "").replace(/^[.\s]+/, "").replace(/\.$/, "");
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
    // A name given to a class template listed above -- a processor's float
    // form, `using Biquad = BiquadT<float>;`, or a lane type, `using Vec8i16 =
    // VecIntT<int16_t>;` -- is the name a plugin writes, so it is listed beside
    // the template rather than vanishing from a list that says "not here means
    // not in the SDK".
    const alias = line.match(/^\s*using\s+([A-Z]\w*)\s*=\s*([A-Z]\w*)\s*</);
    if (alias && depth === 0 && names.includes(alias[2]) && !namespaces.some((n) => /detail$/i.test(n)))
      names.push(alias[1]);
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

  ["A meter or an analyzer shows what the plugin measured.",
   "A web page reads `onLevel`, `onVU`, `onGR` and `onSpectrum` from the " +
   "bridge; a native editor reads an `AudioScopeBuffer` or a `SpectrumFeed` " +
   "that `process()` fills. Gain reduction comes from the DSP declaring " +
   "`float gainReduction() const`, in positive dB. A display animated with " +
   "random numbers looks alive and tells the user nothing."],
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

If a class or struct is not in this list, the SDK does not have it. Reaching for
a class that sounds like it should exist is the most common way generated code
fails to compile against this library. The names given to a listed class template
are listed beside it (Biquad beside BiquadT, Vec8i16 beside VecIntT); functions,
methods and other aliases are not (dbToGain is a function): look for those in
the header itself, or ask the MCP server's \`sonore_search\`, which covers them.

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
    if (!main) return [];
    // a test fixture lives here too; it is not a project to start from
    if (/test fixture/i.test(readFileSync(join(dir, d, main), "utf8").slice(0, 400))) return [];
    return [`\`${d}/${main}\``];
  });
}

// The generated project's own "Before you ship" list, so the skill and the
// README every new project gets cannot disagree. Its placeholders become the
// names a reader of the skill can look for.
function beforeYouShip() {
  const text = readFileSync(join(ROOT, "sdk", "cmake", "templates", "README.md.in"), "utf8");
  const from = text.indexOf("## Before you ship");
  if (from < 0) throw new Error("sdk/cmake/templates/README.md.in has no \"## Before you ship\" section for the skill");
  const rest = text.slice(from).split("\n").slice(1);
  const end = rest.findIndex((l) => l.startsWith("## "));
  const section = rest.slice(0, end < 0 ? undefined : end).join("\n").trim()
    .replaceAll("`@ID@`", "The id")
    .replaceAll("`@AU_SUBTYPE@`", "`AU_SUBTYPE`")
    .replaceAll("`@AU_MANUFACTURER@`", "`AU_MANUFACTURER`")
    .replaceAll("@TARGET@", "MyFilter");
  const left = section.match(/@[A-Z_]+@/);
  if (left) throw new Error(`the template's "Before you ship" has a placeholder the skill does not name: ${left[0]}`);
  return section;
}

function skillMd(headers) {
  for (const rel of [NEW_PLUGIN, MCP_SERVER]) {
    if (!existsSync(join(ROOT, rel))) throw new Error(`the skill points at ${rel}, which does not exist`);
  }
  const typeCount = headers.reduce((n, h) => n + h.types.length, 0);
  const server = `/path/to/sonore-sdk/${MCP_SERVER}`;
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
\`.component\`, an \`.lv2\` bundle and a standalone app.

A project reaches the SDK in one of two ways. Through \`add_subdirectory\` it
is a checkout: the folder with \`llms.txt\` at its root, the headers in
\`sdk/include/sonore/\` and the MCP server in \`${MCP_SERVER}\`. Through
\`find_package(SonoreSDK)\` it is an install, which has the headers (in
\`<prefix>/include/sonore/\`) and the CMake helpers but neither of the others;
for those, clone https://github.com/SonorieAI/sonore-sdk.

## Workflow

Copy this into your answer and tick it off as you go:

\`\`\`
- [ ] Read the rules: sonore-sdk:sonore_rules, or "The shape of a plugin" below
- [ ] Looked up every SDK name before using it
- [ ] Release build succeeds
- [ ] ctest passes
- [ ] clap-validator and pluginval (strictness 10) pass, or are reported as not run
\`\`\`

When a build, a test or a validator fails, fix what it names and run that step
again before moving on.

## Look a name up before you write it

Sonore's DSP is written from the published mathematics, not adapted from
another framework, so its names are its own: a name that sounds right from
another library is usually wrong here. Before writing any class, function or
method you have not seen in this SDK:

- **With the \`sonore-sdk\` MCP server connected**, ask it.
  \`sonore-sdk:sonore_search\` finds classes, functions, aliases and headers,
  with words in any order, and an empty result is a real answer.
  \`sonore-sdk:sonore_declaration\` gives the exact declaration of any of them,
  \`sonore-sdk:sonore_header\` the include line, \`sonore-sdk:sonore_example\`
  a complete working plugin of a given kind, and \`sonore-sdk:sonore_rules\`
  the rules below.
- **Without it**, read \`llms-full.txt\` at the SDK root for the classes and
  structs: every one in every header (${headers.length} headers, ${typeCount}
  types), so a class that is not in it does not exist. Functions, methods and
  aliases of anything but a listed class are not listed there; search the
  headers for those before deciding one is missing.

## Start from a generated project

From the SDK checkout:

\`\`\`
cmake -DNAME="My Filter" -DVENDOR="My Company" -P ${NEW_PLUGIN}
cmake -S MyFilter -B MyFilter/build
cmake --build MyFilter/build --config Release
ctest --test-dir MyFilter/build -C Release
\`\`\`

The plugin is \`MyFilter/src/plugin.cpp\`. \`-DTYPE=instrument\` starts an
eight-voice synth instead of a filter. The id the generator derives
(\`com.mycompany.myfilter\`) is the plugin's identity in every saved session:
pass \`-DID=\` now if that is not the one to keep. To add a plugin to an
existing CMake project instead, see Building below.

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
2. Run the format owners' validators on what was built, from the folder the
   build put it in (\`MyFilter/build\`, or \`MyFilter/build/Release\` with Visual
   Studio and Xcode):

   \`\`\`
   clap-validator validate MyFilter.clap
   pluginval --strictness-level 10 --validate vst3/MyFilterVST3.vst3
   \`\`\`

   On macOS, copy the \`.component\` into \`~/Library/Audio/Plug-Ins/Components\`
   and run \`auval -v aufx <subtype> <manufacturer>\` (\`aumu\` for an instrument).
   Neither validator ships with the SDK. If one is not installed, say so and
   leave its box unticked rather than skipping it quietly; both publish
   binaries on their GitHub releases (github.com/free-audio/clap-validator,
   github.com/Tracktion/pluginval).
3. Report what ran and what it printed. A step that was skipped is said to be
   skipped.

## Before it ships

${beforeYouShip()}

## Where the rest is

- \`sdk/README.md\`: the full documentation.
${guides().join("\n")}
- \`sdk/examples/\`, complete projects: ${examples().join(", ")}.

## Connecting the MCP server

\`${MCP_SERVER}\` in the SDK checkout is plain JSON-RPC over stdio, with no
dependencies and nothing to install. Register it as \`sonore-sdk\`, the name
this skill uses for its tools (Claude Code lists them as
\`mcp__sonore-sdk__sonore_search\` and so on), with the checkout's full path.

- Claude Code: \`claude mcp add --scope user sonore-sdk -- node ${server}\`
- Codex (CLI, IDE extension and the ChatGPT desktop app):
  \`codex mcp add sonore-sdk -- node ${server}\`
- Cursor, in \`~/.cursor/mcp.json\`, and Antigravity, in
  \`~/.gemini/config/mcp_config.json\`; any other MCP client takes the same:

  \`\`\`json
  { "mcpServers": { "sonore-sdk": {
      "command": "node", "args": ["${server}"] } } }
  \`\`\`

This skill installs into any of them with
\`npx skills add SonorieAI/sonore-sdk --skill sonore-sdk -a <agent> -g\`, where
\`<agent>\` is \`claude-code\`, \`codex\`, \`cursor\` or \`antigravity\`.
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
