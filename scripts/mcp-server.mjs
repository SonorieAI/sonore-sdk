// An MCP server for the Sonore SDK, so a coding agent can look the API up
// instead of guessing at it.
//
// WHY THIS EXISTS. The failure mode when a model writes against an unfamiliar
// C++ library is not that it refuses: it invents. It reaches for a class name
// that sounds like it should exist, gives a real class a method from a different
// library, and hands you code that reads perfectly and does not compile. The
// answer is not a better prompt, it is letting the agent ASK. `type` returns the
// real declaration; `search` says whether a name exists at all.
//
// THE ONE PROMISE. "Nothing matches" tells an agent to stop using a name, so it
// must never be said about a name the headers contain. Search therefore looks
// at every declaration, not only classes (dbToGain is a function, ProcessContext
// an alias), matches words in any order ("ladder filter" finds LadderFilter),
// and before giving up reads the code and the comments line by line.
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

// Newest first. A client asking for one of these gets it; any other request
// gets the newest, and the client decides whether it can speak that.
const PROTOCOL_VERSIONS = ["2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"];

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
    text,
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
const matches = (words, s) => {
  const t = s.toLowerCase();
  return words.every((w) => t.includes(w) || t.includes(stem(w))) || t.includes(words.join(""));
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

// ── Tool implementations ────────────────────────────────────────────────────

function toolSearch({ query = "", limit = 30 }) {
  const words = wordsOf(query);
  if (!words.length) return "Give a query: a class or function name, a word from a header, or a concept.";
  const max = Math.min(Math.max(Math.trunc(Number(limit)) || 30, 1), 200);
  const exact = String(query).trim().split("::").pop().toLowerCase();
  const first = (a, b) => (b.toLowerCase() === exact) - (a.toLowerCase() === exact);

  const types = HEADERS.flatMap((h) => h.types.filter((t) => matches(words, t)).map((t) => [t, `${t}  (${h.rel})`]));
  const decls = DECLS.filter((d) => matches(words, d.name))
    .map((d) => [d.name, `${d.name}  ${d.kind}, ${where(d.h, d.line)}  ${snippet(d.h.lines[d.line])}`]);
  const headers = HEADERS.filter((h) => matches(words, h.name) || matches(words, h.purpose))
    .map((h) => [h.name, `${h.name}${h.purpose ? `: ${h.purpose}` : ""}  (${h.rel})`]);

  const sections = [
    ["Types", types.sort((a, b) => first(a[0], b[0]))],
    ["Functions, aliases, enums and constants", decls.sort((a, b) => first(a[0], b[0]))],
    ["Headers", headers],
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
  // the comments: a name the SDK calls but you write (latencySamples), or one
  // its prose explains, is not "nothing".
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
      `It may be something your plugin declares and the SDK looks for, which the "rules" tool explains:\n\n` +
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
    `that assumes it does. Try a broader word, or call the "headers" tool to see the surface.`;
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

function toolType({ name = "" }) {
  const want = String(name).trim().split("::").pop();
  if (!want) return "Give a name, e.g. LadderFilter.";
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
      const more = target && target !== want ? `\n\nThat is an alias: ask "type" for ${target} to see its members.` : "";
      return `${where(d.h, d.line)}  (${d.kind})\n\n${clip(block, 4000)}${more}`;
    }).join("\n\n---\n\n");
  }
  const near = nearMisses(want);
  return `Nothing public is called "${want}".` +
    (near.length ? ` Did you mean: ${near.join(", ")}?` : ` Use "search" to find the real name;`) +
    ` Inventing a name is the most common way code fails to compile here.`;
}

function toolHeader({ name = "" }) {
  const want = String(name).trim().replace(/^<|>$/g, "").replace(/^sonore\//, "");
  if (!want) return `Give a header name, e.g. dsp.h. Call "headers" for the list.`;
  const h = HEADERS.find((x) => x.name === want || x.rel.endsWith(`/${want}`));
  if (!h) return `No header called "${want}". Call "headers" for the list.`;
  return `${h.rel}\n${h.purpose ? `\n${h.purpose}\n` : ""}\n` +
    `Public types (${h.types.length}):\n${h.types.join(", ") || "(none)"}\n\n` +
    `Include as: #include <sonore/${h.rel.split("include/sonore/")[1]}>`;
}

function toolHeaders({ filter = "" }) {
  const words = wordsOf(filter);
  const rows = HEADERS
    .filter((h) => !words.length || matches(words, h.name) || matches(words, h.purpose))
    .map((h) => `${h.rel.split("include/sonore/")[1]}${h.purpose ? `: ${h.purpose}` : ""}`);
  return rows.length ? rows.join("\n") : `No header matches "${filter}".`;
}

// What each kind of plugin is called in sdk/examples.
const EXAMPLE_KINDS = {
  effect: "saturator", instrument: "synth", sidechain: "ducker",
  "aux-outputs": "splitter", "midi-output": "arp", sampler: "sampler",
  "flexible-channels": "trim", reverb: "reverb", "native-editor": "showcase", app: "player",
};

function toolExample({ kind = "effect" }) {
  const dir = EXAMPLE_KINDS[String(kind)] ?? String(kind);
  const folder = join(EXAMPLES, dir);
  const main = ["plugin.cpp", "main.cpp"].find((f) => existsSync(join(folder, f)));
  if (!/^[\w-]+$/.test(dir) || !main) {
    return `No example for "${kind}". Kinds: ${Object.keys(EXAMPLE_KINDS).join(", ")}.`;
  }
  // The example and the local headers it includes, so it is complete; a
  // generated data header is named but not printed.
  const parts = [];
  const visit = (file) => {
    if (parts.some((p) => p.file === file) || !existsSync(join(folder, file))) return;
    const text = readFileSync(join(folder, file), "utf8");
    const generated = /^\/\/.*\bGenerated by\b/m.test(text.split("\n").slice(0, 3).join("\n"));
    parts.push({ file, text: generated ? `// (generated data, ${text.length} characters, not shown)` : text });
    if (!generated) for (const m of text.matchAll(/^\s*#\s*include\s+"([^"]+)"/gm)) visit(m[1]);
  };
  visit(main);
  const body = parts.map((p) => `==> sdk/examples/${dir}/${p.file} <==\n${p.text}`).join("\n\n");
  return clip(body, 60000);
}

function toolRules() {
  const path = join(ROOT, "llms.txt");
  if (!existsSync(path)) return "llms.txt is missing. Run: node scripts/llms-txt.mjs";
  const text = readFileSync(path, "utf8");
  const from = text.indexOf("## The shape of a plugin");
  const to = text.indexOf("## Headers");
  return from < 0 ? text : text.slice(from, to > from ? to : undefined);
}

// Every tool only reads the checkout: hosts that honour the hints can call
// them without asking each time.
const READ_ONLY = { readOnlyHint: true, destructiveHint: false, idempotentHint: true, openWorldHint: false };

const TOOLS = [
  { name: "search", title: "Search the SDK",
    description:
      "Search the SDK for a class, function, alias, header or concept. Use this BEFORE " +
      "naming anything you have not verified. Words match in any order, so \"ladder filter\" " +
      "finds LadderFilter. An empty result means the SDK does not have it, which is a real answer.",
    inputSchema: { type: "object", properties: {
      query: { type: "string", description: "A name (LadderFilter, dbToGain), words (ladder filter) or a concept (oversampling)." },
      limit: { type: "integer", minimum: 1, maximum: 200, description: "Maximum results (default 30)." },
    }, required: ["query"] },
    run: toolSearch },

  { name: "type", title: "Read a declaration",
    description:
      "The real declaration of a public class, struct, function, alias or enum, with the " +
      "comment that explains why it exists. Use this to get exact signatures rather than " +
      "guessing them from another library.",
    inputSchema: { type: "object", properties: {
      name: { type: "string", description: "Exact name, e.g. LadderFilter, dbToGain or ProcessContext." },
    }, required: ["name"] },
    run: toolType },

  { name: "header", title: "Describe a header",
    description: "What one header is for, every public type in it, and the include line.",
    inputSchema: { type: "object", properties: {
      name: { type: "string", description: "Header file name, e.g. dsp.h or gfx/canvas.h." },
    }, required: ["name"] },
    run: toolHeader },

  { name: "headers", title: "List the headers",
    description: "Every header with a one-line purpose. Optionally filtered by words.",
    inputSchema: { type: "object", properties: {
      filter: { type: "string", description: "Optional words, e.g. audio input." },
    } },
    run: toolHeaders },

  { name: "example", title: "Get a working example",
    description:
      "A complete working source from sdk/examples, with the local headers it includes. " +
      `Kinds: ${Object.keys(EXAMPLE_KINDS).join(", ")}.`,
    inputSchema: { type: "object", properties: {
      kind: { type: "string", description: `Which example (default: effect): ${Object.keys(EXAMPLE_KINDS).join(", ")}, or a folder name in sdk/examples.` },
    } },
    run: toolExample },

  { name: "rules", title: "The rules of a plugin",
    description:
      "The shape of a plugin and the rules that are not guessable from the API: " +
      "which process signature makes an instrument, latency reporting, what is " +
      "forbidden on the audio thread, and which objects are too big for the stack.",
    inputSchema: { type: "object", properties: {} },
    run: toolRules },
];

const INSTRUCTIONS =
  "The Sonore SDK, looked up instead of guessed. Before writing plugin code, call `rules` once " +
  "for the shape of a plugin. Before naming any class, function or method you have not seen in " +
  "this SDK, `search` for it: an empty result means it does not exist. Use `type` for exact " +
  "signatures and `example` for a complete plugin of the kind you are writing.";

// ── JSON-RPC 2.0 over stdio ─────────────────────────────────────────────────

const ok = (id, result) => ({ jsonrpc: "2.0", id, result });
const fail = (id, code, message) => ({ jsonrpc: "2.0", id, error: { code, message } });
const send = (msg) => process.stdout.write(JSON.stringify(msg) + "\n");

/** The reply to one message, or null when it takes none. */
function respond(msg) {
  if (!msg || typeof msg !== "object" || Array.isArray(msg) || msg.jsonrpc !== "2.0") {
    return fail(msg?.id ?? null, -32600, "Invalid request: expected a JSON-RPC 2.0 object.");
  }
  const { id, method, params } = msg;
  // A response to a request this server never makes: nothing to answer.
  if (method === undefined && ("result" in msg || "error" in msg)) return null;
  if (typeof method !== "string") return fail(id ?? null, -32600, "Invalid request: no method.");
  // A notification has no id and takes no reply. `notifications/initialized`
  // arrives right after the handshake and answering it is a protocol error.
  if (id === undefined) return null;

  switch (method) {
    case "initialize": {
      const asked = params?.protocolVersion;
      return ok(id, {
        protocolVersion: PROTOCOL_VERSIONS.includes(asked) ? asked : PROTOCOL_VERSIONS[0],
        capabilities: { tools: {} },
        serverInfo: { name: "sonore-sdk", title: "Sonore SDK", version: VERSION },
        instructions: INSTRUCTIONS,
      });
    }
    case "ping":
      return ok(id, {});
    case "tools/list":
      return ok(id, {
        tools: TOOLS.map(({ name, title, description, inputSchema }) =>
          ({ name, title, description, inputSchema, annotations: READ_ONLY })),
      });
    case "tools/call": {
      if (typeof params?.name !== "string") return fail(id, -32602, "tools/call needs the name of a tool.");
      const tool = TOOLS.find((t) => t.name === params.name);
      if (!tool) return fail(id, -32602, `No tool called "${params.name}". Tools: ${TOOLS.map((t) => t.name).join(", ")}.`);
      const args = params.arguments && typeof params.arguments === "object" ? params.arguments : {};
      try {
        return ok(id, { content: [{ type: "text", text: String(tool.run(args)) }] });
      } catch (err) {
        // Reported as tool output rather than a protocol error: the agent can
        // read it and try something else, where a JSON-RPC error just aborts.
        return ok(id, { content: [{ type: "text", text: `Tool failed: ${err?.message ?? err}` }], isError: true });
      }
    }
    default:
      return fail(id, -32601, `Unsupported method "${method}".`);
  }
}

function onLine(line) {
  let msg;
  try { msg = JSON.parse(line); }
  catch { return send(fail(null, -32700, "Parse error: that line is not JSON.")); }
  if (Array.isArray(msg)) {
    // A batch, which protocol versions before 2025-06-18 allow.
    if (!msg.length) return send(fail(null, -32600, "Invalid request: an empty batch."));
    const replies = msg.map(respond).filter(Boolean);
    if (replies.length) send(replies);
    return;
  }
  const reply = respond(msg);
  if (reply) send(reply);
}

// ── npm run verify:mcp ──────────────────────────────────────────────────────
// The one promise, checked: every public type reads back through `type`, and
// every name the shape and rules of a plugin use is found by `search`. A false
// "nothing matches" would tell an agent to delete code that was right.

if (process.argv.includes("--check")) {
  const failures = [];
  for (const h of HEADERS) {
    for (const t of h.types) if (/^Nothing public/.test(toolType({ name: t }))) failures.push(`type ${t} (${h.rel})`);
  }
  const rules = toolRules();
  const used = new Set([
    ...[...rules.matchAll(/\bsonore::([A-Za-z_]\w*)/g)].map((m) => m[1]),
    ...[...rules.matchAll(/\.([a-z]\w*)\(/g)].map((m) => m[1]),
    ...[...rules.matchAll(/`(?:int\s+)?([A-Za-z_]\w*)(?:[<(][^`]*)?`/g)].map((m) => m[1]),
  ]);
  const names = [...used].filter((n) => n.length > 2 && n !== "new");
  for (const name of names) {
    if (/^Nothing in the SDK matches/.test(toolSearch({ query: name }))) failures.push(`search ${name}`);
  }
  for (const kind of Object.keys(EXAMPLE_KINDS)) {
    if (/^No example/.test(toolExample({ kind }))) failures.push(`example ${kind}`);
  }
  const init = respond({ jsonrpc: "2.0", id: 1, method: "initialize", params: { protocolVersion: "1999-01-01" } });
  if (init?.result?.protocolVersion !== PROTOCOL_VERSIONS[0]) failures.push("initialize does not fall back to the newest protocol version");
  if (failures.length) {
    console.error(`✗ the MCP server would answer these wrongly:\n  ${failures.join("\n  ")}`);
    process.exit(1);
  }
  const types = HEADERS.reduce((n, h) => n + h.types.length, 0);
  console.log(`✓ MCP server: ${types} types read back, ${names.length} names from the rules found, ${Object.keys(EXAMPLE_KINDS).length} example kinds resolve.`);
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
