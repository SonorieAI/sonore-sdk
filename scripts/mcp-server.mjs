// An MCP server for the Sonore SDK, so a coding agent can look the API up
// instead of guessing at it.
//
// WHY THIS EXISTS. The failure mode when a model writes against an unfamiliar
// C++ library is not that it refuses: it invents. It reaches for a class name
// that sounds like it should exist, gives a real class a method from a different
// library, and hands you code that reads perfectly and does not compile. The
// answer is not a better prompt, it is letting the agent ASK. `sonore_search`
// says whether a name exists at all; `sonore_declaration` returns the real one.
//
// THE ONE PROMISE. "Nothing matches" tells an agent to stop using a name, so it
// must never be said about a name the headers contain. Search therefore looks
// at every declaration, not only classes (dbToGain is a function, ProcessContext
// an alias), matches words in any order and by stem ("ladder filter" finds
// LadderFilter, "oversampling" OversamplerT), and before giving up reads the
// code and the comments line by line. `npm run verify:mcp` holds it to that.
//
// BOTH ERAS OF THE PROTOCOL. A request carrying `_meta` with a protocol version
// is served statelessly, as MCP 2026-07-28 defines; an `initialize` handshake
// selects the legacy protocol (2025-11-25 and earlier) for the life of the
// process. Clients of either kind work, which the spec calls dual-era.
//
// NO DEPENDENCIES, on purpose. This repository has none, and an MCP server that
// dragged in a package tree to speak a line-delimited JSON protocol would be a
// strange first exception. It is plain JSON-RPC 2.0 over stdio, which is all the
// stdio transport is.
//
// Register it with any MCP client, for example in Claude Code:
//   claude mcp add --scope user sonore-sdk -- node /path/to/sonore-sdk/scripts/mcp-server.mjs
//
// Or by hand, in a client's config:
//   { "mcpServers": { "sonore-sdk": {
//       "command": "node",
//       "args": ["/path/to/sonore-sdk/scripts/mcp-server.mjs"] } } }

import { readFileSync, readdirSync, statSync, existsSync } from "node:fs";
import { join, relative, dirname, sep } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
const INC = join(ROOT, "sdk", "include", "sonore");
const EXAMPLES = join(ROOT, "sdk", "examples");

// The SDK's own version, from the one place it is written.
const VERSION = (() => {
  try {
    return readFileSync(join(ROOT, "sdk", "CMakeLists.txt"), "utf8").match(/project\(SonoreSDK VERSION ([\d.]+)/)?.[1] ?? "0.0.0";
  } catch { return "0.0.0"; }
})();
const SERVER_INFO = { name: "sonore-sdk", title: "Sonore SDK", version: VERSION };

// Newest first. Modern versions are per-request; legacy ones are negotiated
// once, by `initialize`.
const MODERN_VERSIONS = ["2026-07-28"];
const LEGACY_VERSIONS = ["2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"];
const META_VERSION = "io.modelcontextprotocol/protocolVersion";
const META_CAPABILITIES = "io.modelcontextprotocol/clientCapabilities";

// ── The index, built once at startup ────────────────────────────────────────

function walk(dir, out = []) {
  for (const name of readdirSync(dir).sort()) {
    const full = join(dir, name);
    if (statSync(full).isDirectory()) walk(full, out);
    else if (name.endsWith(".h")) out.push(full);
  }
  return out;
}

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

/** Same publicness rule as scripts/unclaimed.mjs and scripts/llms-txt.mjs. */
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
      if (!namespaces.some((n) => /detail$/i.test(n))) names.push(decl[2]);
    }
    for (const ch of line) {
      if (ch === "{") depth++;
      else if (ch === "}") { if (depth > 0) depth--; else namespaces.pop(); }
    }
  }
  return [...new Set(names)];
}

/** The text with comments and string literals blanked out, newlines kept, so
 *  line numbers still match the file. */
function codeOnly(text) {
  return text
    .replace(/\/\*[\s\S]*?\*\//g, (m) => m.replace(/[^\n]/g, " "))
    .replace(/"(?:[^"\\\n]|\\.)*"/g, (m) => " ".repeat(m.length))
    .replace(/\/\/.*$/gm, "");
}

const NOT_A_NAME = new Set(["if", "for", "while", "switch", "return", "sizeof", "alignof", "decltype", "static_assert", "catch", "operator", "noexcept", "requires", "defined"]);
const DECLARATIONS = [
  ["alias", /\busing\s+([A-Za-z_]\w*)\s*=/],
  ["enum", /\benum\s+(?:class\s+|struct\s+)?([A-Za-z_]\w*)/],
  ["constant", /\bconstexpr\s+(?:[\w:]+(?:<[^;=]*>)?[\s*&]+)+([A-Za-z_]\w*)\s*(?:=|\{|\[)/],
  ["function", /^\s*(?:template\s*<[^>]*>\s*)?(?:(?:static|inline|constexpr|consteval|virtual|explicit|friend|extern|const|\[\[nodiscard\]\])\s+)*(?:[\w:]+(?:<[^;()]*>)?[\s*&]+)+([A-Za-z_]\w*)\s*\(/],
  ["macro", /^\s*#\s*define\s+([A-Za-z_]\w*)/],
];

const HEADERS = walk(INC).map((file) => {
  const text = readFileSync(file, "utf8");
  return {
    file,
    rel: relative(ROOT, file).split(sep).join("/"),
    name: file.split(sep).pop(),
    purpose: purposeOf(text),
    types: publicTypes(text),
    lines: text.split("\n"),
    code: codeOnly(text).split("\n"),
  };
});

// Everything else the headers declare: functions, aliases, enums, constants,
// macros. The first declaration of each name in each header, which is the one
// that carries the comment. Only at namespace or class scope: the headers hold
// function bodies, and a local variable is not part of the API.
const DECLS = [];
for (const h of HEADERS) {
  const seen = new Set();
  const scopes = [];
  h.code.forEach((line, i) => {
    const inBody = scopes.includes("block");
    let opened = false;
    for (const ch of line) {
      if (ch === "{") {
        scopes.push(opened ? "block" : /\bnamespace\b/.test(line) ? "ns" : /\b(class|struct|union|enum)\b/.test(line) ? "class" : "block");
        opened = true;
      } else if (ch === "}") scopes.pop();
    }
    if (inBody) return;
    for (const [kind, re] of DECLARATIONS) {
      const name = line.match(re)?.[1];
      if (!name || NOT_A_NAME.has(name) || seen.has(name) || h.types.includes(name)) continue;
      seen.add(name);
      DECLS.push({ name, kind, h, line: i });
      break;
    }
  });
}

const ALL_NAMES = [...new Set([...HEADERS.flatMap((h) => h.types), ...DECLS.map((d) => d.name)])];

// ── Matching ────────────────────────────────────────────────────────────────

/** "sonore::LadderFilter", "ladder filter", "ladder_filter" and "LadderFilter"
 *  all reduce to the same words. */
function wordsOf(query) {
  const last = String(query).trim().split("::").pop() ?? "";
  return last.replace(/([a-z0-9])([A-Z])/g, "$1 $2").toLowerCase().split(/[^a-z0-9]+/).filter(Boolean);
}
// A word also matches by its stem, so "oversampling" finds OversamplerT and
// "compressors" finds Compressor.
const stem = (w) => (w.length > 5 ? w.replace(/(?:ings?|ers?|ed|ions?|es|s)$/, "") : w);
// The SDK spells the British way (SpectrumAnalyser, Colour, normalise), and an
// agent asking for "spectrum analyzer" or "color" means the same thing; both
// sides are folded to one spelling before they are compared.
const fold = (s) => s.replace(/colour/g, "color").replace(/([iy])z/g, "$1s");
const matches = (words, s) => {
  const t = fold(s.toLowerCase());
  const ws = words.map(fold);
  return ws.every((w) => t.includes(w) || t.includes(stem(w))) || t.includes(ws.join(""));
};

function distance(a, b) {
  const d = Array.from({ length: a.length + 1 }, (_, i) => [i]);
  for (let j = 1; j <= b.length; j++) d[0][j] = j;
  for (let i = 1; i <= a.length; i++) {
    for (let j = 1; j <= b.length; j++) {
      d[i][j] = Math.min(d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
    }
  }
  return d[a.length][b.length];
}

/** Names a typo away from the query, for "did you mean". */
function nearMisses(query) {
  const q = String(query).trim().split("::").pop().toLowerCase();
  if (q.length < 3) return [];
  return ALL_NAMES
    .map((n) => [n, distance(q, n.toLowerCase())])
    .filter(([, d]) => d > 0 && d <= Math.max(1, Math.floor(q.length / 4)))
    .sort((a, b) => a[1] - b[1])
    .slice(0, 5)
    .map(([n]) => n);
}

const clip = (s, n) =>
  s.length > n ? `${s.slice(0, n)}\n\n[...truncated at ${n} characters]` : s;
const snippet = (s) => clip(s.trim().replace(/\s+/g, " "), 110).split("\n")[0];
const where = (h, i) => `${h.rel}:${i + 1}`;

// ── Arguments ───────────────────────────────────────────────────────────────

/** A mistake in how a tool was called. It goes back as a tool result with
 *  isError set, worded so the model can correct the call, rather than as a
 *  protocol error that would only abort it. */
class UsageError extends Error {}

function text(args, key, { required = false, max = 200 } = {}) {
  const v = args[key];
  if (v === undefined || v === null || v === "") {
    if (required) throw new UsageError(`"${key}" is required.`);
    return "";
  }
  if (typeof v !== "string") throw new UsageError(`"${key}" must be a string, not ${typeof v}.`);
  if (v.length > max) throw new UsageError(`"${key}" is ${v.length} characters; keep it under ${max}.`);
  return v.trim();
}

// ── Tool implementations ────────────────────────────────────────────────────

function toolSearch(args) {
  const query = text(args, "query", { required: true });
  const words = wordsOf(query);
  if (!words.length) throw new UsageError(`"query" has no letters or digits: give a name, words or a concept.`);
  let max = 30;
  if (args.limit !== undefined) {
    if (!Number.isInteger(args.limit) || args.limit < 1 || args.limit > 200) throw new UsageError(`"limit" must be a whole number from 1 to 200.`);
    max = args.limit;
  }
  const exact = query.split("::").pop().toLowerCase();
  const first = (a, b) => (b.toLowerCase() === exact) - (a.toLowerCase() === exact);

  const sections = [
    ["Types", HEADERS.flatMap((h) => h.types.filter((t) => matches(words, t)).map((t) => [t, `${t}  (${h.rel})`])).sort((a, b) => first(a[0], b[0]))],
    ["Functions, aliases, enums and constants", DECLS.filter((d) => matches(words, d.name))
      .map((d) => [d.name, `${d.name}  ${d.kind}, ${where(d.h, d.line)}  ${snippet(d.h.lines[d.line])}`]).sort((a, b) => first(a[0], b[0]))],
    ["Headers", HEADERS.filter((h) => matches(words, h.name) || matches(words, h.purpose))
      .map((h) => [h.name, `${h.name}${h.purpose ? `: ${h.purpose}` : ""}  (${h.rel})`])],
  ].filter(([, rows]) => rows.length);
  if (sections.length) {
    let left = max;
    return sections.map(([title, rows]) => {
      const shown = rows.slice(0, Math.max(left, 0)).map(([, r]) => r);
      left -= shown.length;
      return shown.length ? `${title}:\n${shown.join("\n")}` : "";
    }).filter(Boolean).join("\n\n");
  }

  // Not declared under that name. Before saying so, read the code itself, then
  // the comments: a name the SDK calls but you write (latencySamples on your
  // own DSP), or one its prose explains, is not "nothing".
  const word = new RegExp(`\\b${words.join("\\W*")}\\b`, "i");
  const inCode = [], inComments = [];
  for (const h of HEADERS) {
    h.lines.forEach((line, i) => {
      if (word.test(h.code[i])) inCode.push(`${where(h, i)}  ${snippet(line)}`);
      else if (word.test(line)) inComments.push(`${where(h, i)}  ${snippet(line)}`);
    });
  }
  if (inCode.length) {
    return `The SDK declares nothing called "${query}", but its code uses the name. ` +
      `It may be something your plugin declares and the SDK looks for, which sonore_rules explains:\n\n` +
      inCode.slice(0, max).join("\n");
  }
  if (inComments.length) {
    return `The SDK declares nothing called "${query}"; its comments mention it, so it is a concept here, not a name to call:\n\n` +
      inComments.slice(0, max).join("\n");
  }
  const near = nearMisses(query);
  return `Nothing in the SDK matches "${query}".\n\n` +
    (near.length ? `Did you mean: ${near.join(", ")}?\n\n` : "") +
    `That is an answer, not a failure: the SDK does not have it, so do not write code ` +
    `that assumes it does. Try a broader word, or sonore_headers to see the surface.`;
}

/** The lines that declare something: its doc comment above, any template line,
 *  then down to the end of the body (or the signature, for a function). */
function declarationBlock(h, start, { body }) {
  let from = start;
  while (from > 0 && /^\s*(template\b|\/\/|\/\*|\*)/.test(h.lines[from - 1])) from--;
  let depth = 0, end = start, seen = false;
  for (let i = start; i < h.lines.length; i++) {
    for (const ch of h.code[i]) {
      if (ch === "{") { depth++; seen = true; }
      else if (ch === "}") depth--;
    }
    end = i;
    if (!body && (seen || h.code[i].includes(";"))) break;
    if (seen && depth <= 0) break;
    if (!seen && h.code[i].includes(";")) break;
  }
  return h.lines.slice(from, end + 1).join("\n");
}

function toolDeclaration(args) {
  const want = text(args, "name", { required: true }).split("::").pop();
  if (!/^[A-Za-z_]\w*$/.test(want)) throw new UsageError(`"name" must be one C++ identifier, e.g. LadderFilter; for words, use sonore_search.`);
  for (const h of HEADERS) {
    if (!h.types.includes(want)) continue;
    const start = h.lines.findIndex((l) =>
      new RegExp(`^\\s*(?:template\\s*<[^>]*>\\s*)?(?:class|struct)\\s+${want}\\b`).test(l) && !/;\s*$/.test(l));
    if (start < 0) continue;
    return `${where(h, start)}\n\n${clip(declarationBlock(h, start, { body: true }), 8000)}`;
  }
  // Not a class or struct: a function, an alias, an enum or a constant.
  const decls = DECLS.filter((d) => d.name === want);
  if (decls.length) {
    return decls.slice(0, 4).map((d) => {
      const block = declarationBlock(d.h, d.line, { body: d.kind === "enum" });
      const target = d.kind === "alias" ? d.h.code[d.line].match(/=\s*(?:typename\s+)?([A-Z]\w*)/)?.[1] : null;
      const more = target && target !== want ? `\n\nThat is an alias: ask sonore_declaration for ${target} to see its members.` : "";
      return `${where(d.h, d.line)}  (${d.kind})\n\n${clip(block, 4000)}${more}`;
    }).join("\n\n---\n\n");
  }
  const near = nearMisses(want);
  return `Nothing public is called "${want}".` +
    (near.length ? ` Did you mean: ${near.join(", ")}?` : ` Use sonore_search to find the real name;`) +
    ` Inventing a name is the most common way code fails to compile here.`;
}

function toolHeader(args) {
  const want = text(args, "name", { required: true }).replace(/^<|>$/g, "").replace(/^sonore\//, "");
  const h = HEADERS.find((x) => x.name === want || x.rel.endsWith(`/${want}`));
  if (!h) throw new UsageError(`No header called "${want}". sonore_headers lists them all.`);
  return `${h.rel}\n${h.purpose ? `\n${h.purpose}\n` : ""}\n` +
    `Public types (${h.types.length}):\n${h.types.join(", ") || "(none)"}\n\n` +
    `Include as: #include <sonore/${h.rel.split("include/sonore/")[1]}>`;
}

function toolHeaders(args) {
  const filter = text(args, "filter");
  const words = wordsOf(filter);
  const rows = HEADERS
    .filter((h) => !words.length || matches(words, h.name) || matches(words, h.purpose))
    .map((h) => `${h.rel.split("include/sonore/")[1]}${h.purpose ? `: ${h.purpose}` : ""}`);
  return rows.length ? rows.join("\n") : `No header matches "${filter}". Call it without a filter for all ${HEADERS.length}.`;
}

// What each kind of plugin is called in sdk/examples.
const EXAMPLE_KINDS = {
  effect: "saturator", instrument: "synth", sidechain: "ducker",
  "aux-outputs": "splitter", "midi-output": "arp", sampler: "sampler",
  "flexible-channels": "trim", reverb: "reverb", "native-editor": "showcase", app: "player",
};

/** An example's main source, and the local headers it pulls in (generated
 *  data headers are listed but marked, never printed). */
function exampleFiles(dir) {
  const folder = join(EXAMPLES, dir);
  const main = ["plugin.cpp", "main.cpp"].find((f) => existsSync(join(folder, f)));
  if (!main) return null;
  const files = [];
  const visit = (file) => {
    if (files.some((f) => f.file === file) || !existsSync(join(folder, file))) return;
    const body = readFileSync(join(folder, file), "utf8");
    const generated = /^\/\/.*\bGenerated by\b/m.test(body.split("\n").slice(0, 3).join("\n"));
    files.push({ file, body, generated });
    if (!generated) for (const m of body.matchAll(/^\s*#\s*include\s+"([^"]+)"/gm)) visit(m[1]);
  };
  visit(main);
  return files;
}

function toolExample(args) {
  const kind = text(args, "kind") || "effect";
  const dir = Object.hasOwn(EXAMPLE_KINDS, kind) ? EXAMPLE_KINDS[kind] : kind;
  const files = /^[\w-]+$/.test(dir) ? exampleFiles(dir) : null;
  if (!files) throw new UsageError(`No example for "${kind}". Kinds: ${Object.keys(EXAMPLE_KINDS).join(", ")}.`);
  // One file per call, so a large example never floods the context: the main
  // source by default, any of its local headers by name.
  const want = text(args, "file") || files[0].file;
  const f = files.find((x) => x.file === want);
  if (!f) throw new UsageError(`sdk/examples/${dir} has no file "${want}" in this example. Files: ${files.map((x) => x.file).join(", ")}.`);
  if (f.generated) return `sdk/examples/${dir}/${f.file} is generated data (${f.body.length} characters), not source to read.`;
  const others = files.filter((x) => x !== f)
    .map((x) => `${x.file} (${Math.round(x.body.length / 1024)} KB${x.generated ? ", generated data" : ""})`);
  return `sdk/examples/${dir}/${f.file}\n\n${clip(f.body, 40000)}` +
    (others.length ? `\n\nAlso part of this example: ${others.join(", ")}. Ask again with "file" to read one.` : "");
}

function toolRules() {
  const path = join(ROOT, "llms.txt");
  if (!existsSync(path)) return "llms.txt is missing. Run: node scripts/llms-txt.mjs";
  const all = readFileSync(path, "utf8");
  const from = all.indexOf("## The shape of a plugin");
  const to = all.indexOf("## Headers");
  return from < 0 ? all : all.slice(from, to > from ? to : undefined);
}

// Every tool only reads the checkout: hosts that honour the hints can call
// them without asking each time.
const READ_ONLY = { readOnlyHint: true, destructiveHint: false, idempotentHint: true, openWorldHint: false };
const schema = (properties, required = []) =>
  ({ type: "object", properties, ...(required.length ? { required } : {}), additionalProperties: false });

const TOOLS = [
  { name: "sonore_search", alias: "search", title: "Search the SDK",
    description:
      "Search the Sonore SDK for a class, function, alias, header or concept. Use it BEFORE " +
      "naming anything you have not verified. Words match in any order and by stem, so " +
      "\"ladder filter\" finds LadderFilter. An empty result means the SDK does not have it, " +
      "which is a real answer.",
    inputSchema: schema({
      query: { type: "string", maxLength: 200, description: "A name (LadderFilter, dbToGain), words (ladder filter) or a concept (oversampling)." },
      limit: { type: "integer", minimum: 1, maximum: 200, description: "Maximum results (default 30)." },
    }, ["query"]),
    run: toolSearch },

  { name: "sonore_declaration", alias: "type", title: "Read a declaration",
    description:
      "The real declaration of a public class, struct, function, alias or enum in the Sonore SDK, " +
      "with the comment that explains why it exists. Use it for exact signatures instead of " +
      "guessing them from another library.",
    inputSchema: schema({
      name: { type: "string", maxLength: 200, description: "One exact name, e.g. LadderFilter, dbToGain or ProcessContext." },
    }, ["name"]),
    run: toolDeclaration },

  { name: "sonore_header", alias: "header", title: "Describe a header",
    description: "What one Sonore SDK header is for, every public type in it, and the include line.",
    inputSchema: schema({
      name: { type: "string", maxLength: 200, description: "Header file name, e.g. dsp.h or gfx/canvas.h." },
    }, ["name"]),
    run: toolHeader },

  { name: "sonore_headers", alias: "headers", title: "List the headers",
    description: "Every Sonore SDK header with a one-line purpose, optionally filtered by words.",
    inputSchema: schema({
      filter: { type: "string", maxLength: 200, description: "Optional words, e.g. audio input." },
    }),
    run: toolHeaders },

  { name: "sonore_example", alias: "example", title: "Read a working example",
    description:
      "A complete working source from the SDK's examples, one file per call: the main source " +
      "by default, then any local header it includes by name. " +
      `Kinds: ${Object.keys(EXAMPLE_KINDS).join(", ")}.`,
    inputSchema: schema({
      kind: { type: "string", maxLength: 200, description: `Which example (default: effect): ${Object.keys(EXAMPLE_KINDS).join(", ")}, or a folder name in sdk/examples.` },
      file: { type: "string", maxLength: 200, description: "One of the example's files, as listed at the end of the first answer (default: the main source)." },
    }),
    run: toolExample },

  { name: "sonore_rules", alias: "rules", title: "The rules of a plugin",
    description:
      "The shape of a Sonore plugin and the rules that are not guessable from the API: which " +
      "process signature makes an instrument, latency reporting, what is forbidden on the audio " +
      "thread, and which objects are too big for the stack. Read it once before writing a plugin.",
    inputSchema: schema({}),
    run: toolRules },
];

const INSTRUCTIONS =
  "The Sonore SDK, looked up instead of guessed. Before writing plugin code, call sonore_rules once " +
  "for the shape of a plugin. Before naming any class, function or method you have not seen in this " +
  "SDK, call sonore_search: an empty result means it does not exist. Use sonore_declaration for exact " +
  "signatures and sonore_example for a complete plugin of the kind you are writing.";

const listedTools = () => TOOLS.map(({ name, title, description, inputSchema }) =>
  ({ name, title, description, inputSchema, annotations: { title, ...READ_ONLY } }));

// Generous for an agent, which calls a few times a turn; a runaway loop is
// told to stop rather than served forever.
const RATE = { calls: 120, perMs: 10_000 };
const recent = [];

function callTool(params) {
  if (typeof params?.name !== "string") return { error: [-32602, "tools/call needs the name of a tool."] };
  // The names from before the tools were namespaced still work.
  const tool = TOOLS.find((t) => t.name === params.name || t.alias === params.name);
  if (!tool) return { error: [-32602, `Unknown tool "${params.name}". Tools: ${TOOLS.map((t) => t.name).join(", ")}.`] };
  if (params.arguments !== undefined && (typeof params.arguments !== "object" || params.arguments === null || Array.isArray(params.arguments))) {
    return { result: { content: [{ type: "text", text: "\"arguments\" must be an object." }], isError: true } };
  }
  const now = Date.now();
  while (recent.length && now - recent[0] > RATE.perMs) recent.shift();
  if (recent.length >= RATE.calls) {
    return { result: { content: [{ type: "text", text: `More than ${RATE.calls} calls in ${RATE.perMs / 1000} seconds. Wait a moment, then call again.` }], isError: true } };
  }
  recent.push(now);
  try {
    return { result: { content: [{ type: "text", text: String(tool.run(params.arguments ?? {})) }] } };
  } catch (err) {
    const message = err instanceof UsageError ? err.message : `Tool failed: ${err?.message ?? err}`;
    return { result: { content: [{ type: "text", text: message }], isError: true } };
  }
}

// ── JSON-RPC 2.0 over stdio ─────────────────────────────────────────────────

const ok = (id, result) => ({ jsonrpc: "2.0", id, result });
const fail = (id, code, message, data) => ({ jsonrpc: "2.0", id, error: { code, message, ...(data ? { data } : {}) } });
const send = (msg) => process.stdout.write(JSON.stringify(msg) + "\n");
const answer = (id, r, extra = {}) => (r.error ? fail(id, ...r.error) : ok(id, { ...extra, ...r.result }));

// Set by an `initialize`: from then on, requests without `_meta` are legacy.
let legacy = null;

/** The 2026-07-28 protocol: every request says its version and capabilities. */
function respondModern(id, method, params, meta) {
  const version = meta[META_VERSION];
  if (typeof version !== "string" || !meta[META_CAPABILITIES] || typeof meta[META_CAPABILITIES] !== "object") {
    return fail(id, -32602, `params._meta needs "${META_VERSION}" and "${META_CAPABILITIES}".`);
  }
  if (!MODERN_VERSIONS.includes(version)) {
    return fail(id, -32022, "Unsupported protocol version", { supported: MODERN_VERSIONS, requested: version });
  }
  const done = { resultType: "complete" };
  // The tools never change while the process runs: a client may cache them.
  const cache = { ttlMs: 3_600_000, cacheScope: "public" };
  switch (method) {
    case "server/discover":
      return ok(id, {
        ...done, supportedVersions: MODERN_VERSIONS, capabilities: { tools: {} }, instructions: INSTRUCTIONS, ...cache,
        _meta: { "io.modelcontextprotocol/serverInfo": SERVER_INFO },
      });
    case "tools/list":
      return ok(id, { ...done, tools: listedTools(), ...cache });
    case "tools/call":
      return answer(id, callTool(params), done);
    default:
      return fail(id, -32601, `Unsupported method "${method}".`);
  }
}

/** The legacy protocol: one `initialize`, then plain requests. */
function respondLegacy(id, method, params) {
  switch (method) {
    case "initialize": {
      const asked = params?.protocolVersion;
      legacy = LEGACY_VERSIONS.includes(asked) ? asked : LEGACY_VERSIONS[0];
      return ok(id, { protocolVersion: legacy, capabilities: { tools: {} }, serverInfo: SERVER_INFO, instructions: INSTRUCTIONS });
    }
    case "ping":
      return ok(id, {});
    case "tools/list":
      return ok(id, { tools: listedTools() });
    case "tools/call":
      return answer(id, callTool(params));
    default:
      return fail(id, -32601, `Unsupported method "${method}".`);
  }
}

/** The reply to one message, or null when it takes none. */
function respond(msg) {
  if (!msg || typeof msg !== "object" || Array.isArray(msg) || msg.jsonrpc !== "2.0") {
    return fail(msg?.id ?? null, -32600, "Invalid request: expected a JSON-RPC 2.0 object.");
  }
  const { id, method, params } = msg;
  // A response to a request this server never makes: nothing to answer.
  if (method === undefined && ("result" in msg || "error" in msg)) return null;
  if (typeof method !== "string") return fail(id ?? null, -32600, "Invalid request: no method.");
  // A notification takes no reply: notifications/initialized (legacy) and
  // notifications/cancelled (both eras; every call here answers at once).
  if (id === undefined) return null;

  const meta = params?._meta;
  if (meta && typeof meta === "object" && META_VERSION in meta) return respondModern(id, method, params, meta);
  if (method === "initialize" || legacy) return respondLegacy(id, method, params);
  if (method === "server/discover") return respondModern(id, method, params, meta ?? {});
  return fail(id, -32602,
    `No protocol version for "${method}": send "initialize" first (protocol ${LEGACY_VERSIONS[0]} and earlier), ` +
    `or put "${META_VERSION}" in params._meta (${MODERN_VERSIONS[0]}).`);
}

function onLine(line) {
  let msg;
  try { msg = JSON.parse(line); }
  catch { return send(fail(null, -32700, "Parse error: that line is not JSON.")); }
  if (Array.isArray(msg)) {
    // A batch, which protocol 2025-03-26 allowed.
    if (!msg.length) return send(fail(null, -32600, "Invalid request: an empty batch."));
    const replies = msg.map(respond).filter(Boolean);
    if (replies.length) send(replies);
    return;
  }
  const reply = respond(msg);
  if (reply) send(reply);
}

// ── npm run verify:mcp ──────────────────────────────────────────────────────
// The one promise, checked: every public type reads back through
// sonore_declaration, and every name the shape and rules of a plugin use is
// found by sonore_search. A false "nothing matches" would tell an agent to
// delete code that was right. The protocol is checked in both eras too.

if (process.argv.includes("--check")) {
  // the check calls every tool hundreds of times; the limit is for clients
  RATE.calls = Infinity;
  const failures = [];
  const call = (name, args) => callTool({ name, arguments: args }).result;
  const textOf = (r) => r?.content?.[0]?.text ?? "";
  for (const h of HEADERS) {
    for (const t of h.types) if (/^Nothing public/.test(textOf(call("sonore_declaration", { name: t })))) failures.push(`sonore_declaration ${t} (${h.rel})`);
  }
  const rules = toolRules();
  const used = new Set([
    ...[...rules.matchAll(/\bsonore::([A-Za-z_]\w*)/g)].map((m) => m[1]),
    ...[...rules.matchAll(/\.([a-z]\w*)\(/g)].map((m) => m[1]),
    ...[...rules.matchAll(/`(?:int\s+)?([A-Za-z_]\w*)(?:[<(][^`]*)?`/g)].map((m) => m[1]),
  ]);
  const names = [...used].filter((n) => n.length > 2 && n !== "new");
  for (const name of names) {
    if (/^Nothing in the SDK matches/.test(textOf(call("sonore_search", { query: name })))) failures.push(`sonore_search ${name}`);
  }
  for (const kind of Object.keys(EXAMPLE_KINDS)) {
    if (call("sonore_example", { kind }).isError) failures.push(`sonore_example ${kind}`);
  }
  for (const t of TOOLS) {
    if (t.inputSchema.additionalProperties !== false) failures.push(`${t.name}: inputSchema allows unknown arguments`);
    if (!/^[A-Za-z0-9_.-]{1,128}$/.test(t.name)) failures.push(`${t.name}: not a valid tool name`);
    if (call(t.alias, {}).isError === undefined && t.inputSchema.required) failures.push(`${t.alias}: alias accepts a call missing its required argument`);
  }
  if (!call("sonore_search", {}).isError) failures.push("a missing required argument is not reported with isError");

  const meta = { [META_VERSION]: MODERN_VERSIONS[0], [META_CAPABILITIES]: {} };
  const expect = (label, reply, test) => { if (!test(reply)) failures.push(`${label}: ${JSON.stringify(reply).slice(0, 160)}`); };
  expect("request with no version before initialize", respond({ jsonrpc: "2.0", id: 1, method: "tools/list" }), (r) => r?.error?.code === -32602);
  expect("server/discover", respond({ jsonrpc: "2.0", id: 2, method: "server/discover", params: { _meta: meta } }),
    (r) => r?.result?.resultType === "complete" && r.result.supportedVersions?.includes(MODERN_VERSIONS[0]) && typeof r.result.ttlMs === "number" && r.result.cacheScope && r.result._meta?.["io.modelcontextprotocol/serverInfo"]?.name === "sonore-sdk");
  expect("server/discover without _meta", respond({ jsonrpc: "2.0", id: 3, method: "server/discover" }), (r) => r?.error?.code === -32602);
  expect("modern tools/list", respond({ jsonrpc: "2.0", id: 4, method: "tools/list", params: { _meta: meta } }), (r) => r?.result?.tools?.length === TOOLS.length && r.result.resultType === "complete");
  expect("modern tools/call", respond({ jsonrpc: "2.0", id: 5, method: "tools/call", params: { _meta: meta, name: "sonore_search", arguments: { query: "dbToGain" } } }), (r) => r?.result?.resultType === "complete" && /dbToGain/.test(textOf(r.result)));
  expect("unsupported modern version", respond({ jsonrpc: "2.0", id: 6, method: "tools/list", params: { _meta: { ...meta, [META_VERSION]: "2099-01-01" } } }),
    (r) => r?.error?.code === -32022 && r.error.data?.supported?.length && r.error.data.requested === "2099-01-01");
  expect("missing client capabilities", respond({ jsonrpc: "2.0", id: 7, method: "tools/list", params: { _meta: { [META_VERSION]: MODERN_VERSIONS[0] } } }), (r) => r?.error?.code === -32602);
  expect("legacy initialize falls back to the newest legacy version", respond({ jsonrpc: "2.0", id: 8, method: "initialize", params: { protocolVersion: "1999-01-01" } }), (r) => r?.result?.protocolVersion === LEGACY_VERSIONS[0]);
  expect("legacy tools/call by old name", respond({ jsonrpc: "2.0", id: 9, method: "tools/call", params: { name: "search", arguments: { query: "ladder filter" } } }), (r) => /LadderFilter/.test(textOf(r?.result)));
  expect("American spelling finds the British name", respond({ jsonrpc: "2.0", id: 11, method: "tools/call", params: { _meta: meta, name: "sonore_search", arguments: { query: "spectrum analyzer" } } }), (r) => /SpectrumAnalyser/.test(textOf(r?.result)));
  expect("unknown tool", respond({ jsonrpc: "2.0", id: 10, method: "tools/call", params: { name: "nope" } }), (r) => r?.error?.code === -32602);

  if (failures.length) {
    console.error(`✗ the MCP server would answer these wrongly:\n  ${failures.join("\n  ")}`);
    process.exit(1);
  }
  const types = HEADERS.reduce((n, h) => n + h.types.length, 0);
  console.log(`✓ MCP server: ${types} types read back, ${names.length} names from the rules found, ` +
    `${Object.keys(EXAMPLE_KINDS).length} example kinds resolve, protocol ${MODERN_VERSIONS[0]} and legacy answer correctly.`);
  process.exit(0);
}

let buffer = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => {
  buffer += chunk;
  let nl;
  while ((nl = buffer.indexOf("\n")) >= 0) {
    const line = buffer.slice(0, nl).trim();
    buffer = buffer.slice(nl + 1);
    if (line) onLine(line);
  }
});
process.stdin.on("end", () => process.exit(0));
