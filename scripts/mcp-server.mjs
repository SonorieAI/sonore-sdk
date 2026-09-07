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
// NO DEPENDENCIES, on purpose. This repository has none, and an MCP server that
// dragged in a package tree to speak a line-delimited JSON protocol would be a
// strange first exception. It is plain JSON-RPC 2.0 over stdio, which is all the
// stdio transport is.
//
// Register it with any MCP client, for example in Claude Code:
//   claude mcp add sonore-sdk -- node /path/to/sonore-sdk/scripts/mcp-server.mjs
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
    return body.replace(/^Sonore SDK[:,]?\s*/i, "").replace(/\.$/, "");
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

const HEADERS = walk(INC).map((file) => {
  const text = readFileSync(file, "utf8");
  return {
    file,
    rel: relative(ROOT, file).split(sep).join("/"),
    name: file.split(sep).pop(),
    purpose: purposeOf(text),
    types: publicTypes(text),
    text,
  };
});

// ── Tool implementations ────────────────────────────────────────────────────

const clip = (s, n) =>
  s.length > n ? `${s.slice(0, n)}\n\n[...truncated at ${n} characters]` : s;

function toolSearch({ query = "", limit = 30 }) {
  const q = String(query).toLowerCase().trim();
  if (!q) return "Give a query: a class name, a word from a header, or a concept.";
  const hits = [];
  for (const h of HEADERS) {
    for (const t of h.types) {
      if (t.toLowerCase().includes(q)) hits.push(`${t}  (${h.rel})`);
    }
    if (h.name.toLowerCase().includes(q) || h.purpose.toLowerCase().includes(q)) {
      hits.push(`${h.name}  ${h.purpose ? `: ${h.purpose}` : ""}  (${h.rel})`);
    }
  }
  if (!hits.length) {
    return `Nothing in the SDK matches "${query}".\n\nThat is an answer, not a ` +
      `failure: the SDK does not have it, so do not write code that assumes it ` +
      `does. Try a broader word, or call the "headers" tool to see the surface.`;
  }
  return [...new Set(hits)].slice(0, limit).join("\n");
}

function toolType({ name = "" }) {
  const want = String(name).trim();
  if (!want) return "Give a type name.";
  for (const h of HEADERS) {
    if (!h.types.includes(want)) continue;
    const lines = h.text.split("\n");
    const start = lines.findIndex((l) =>
      new RegExp(`^\\s*(?:template\\s*<[^>]*>\\s*)?(?:class|struct)\\s+${want}\\b`).test(l));
    if (start < 0) continue;
    // Walk back over the doc comment that introduces it: the comment is usually
    // the part that says WHY, which is what a caller actually needs.
    let from = start;
    while (from > 0 && /^\s*(\/\/|\/\*|\*)/.test(lines[from - 1])) from--;
    let depth = 0, end = start, seen = false;
    for (let i = start; i < lines.length; i++) {
      for (const ch of lines[i]) {
        if (ch === "{") { depth++; seen = true; }
        else if (ch === "}") depth--;
      }
      end = i;
      if (seen && depth <= 0) break;
    }
    return `${h.rel}\n\n${clip(lines.slice(from, end + 1).join("\n"), 6000)}`;
  }
  return `No public type called "${want}". Use "search" to find the real name; ` +
    `inventing one is the most common way code fails to compile here.`;
}

function toolHeader({ name = "" }) {
  const want = String(name).trim().replace(/^sonore\//, "");
  const h = HEADERS.find((x) => x.name === want || x.rel.endsWith(`/${want}`));
  if (!h) return `No header called "${want}". Call "headers" for the list.`;
  return `${h.rel}\n${h.purpose ? `\n${h.purpose}\n` : ""}\n` +
    `Public types (${h.types.length}):\n${h.types.join(", ") || "(none)"}\n\n` +
    `Include as: #include <sonore/${h.rel.split("include/sonore/")[1]}>`;
}

function toolHeaders({ filter = "" }) {
  const f = String(filter).toLowerCase();
  const rows = HEADERS
    .filter((h) => !f || h.name.toLowerCase().includes(f) || h.purpose.toLowerCase().includes(f))
    .map((h) => `${h.name}${h.purpose ? `: ${h.purpose}` : ""}`);
  return rows.length ? rows.join("\n") : `Nothing matches "${filter}".`;
}

function toolExample({ kind = "effect" }) {
  const map = {
    effect: "saturator", instrument: "synth", sidechain: "ducker",
    "aux-outputs": "splitter", "midi-output": "arp", sampler: "sampler",
    "flexible-channels": "trim", reverb: "reverb",
  };
  const dir = map[String(kind)] || String(kind);
  const file = join(EXAMPLES, dir, "plugin.cpp");
  if (!existsSync(file)) {
    return `No example for "${kind}". Available: ${Object.keys(map).join(", ")}.`;
  }
  return `sdk/examples/${dir}/plugin.cpp\n\n${clip(readFileSync(file, "utf8"), 24000)}`;
}

function toolRules() {
  const path = join(ROOT, "llms.txt");
  if (!existsSync(path)) return "llms.txt is missing. Run: node scripts/llms-txt.mjs";
  const text = readFileSync(path, "utf8");
  const from = text.indexOf("## The shape of a plugin");
  const to = text.indexOf("## Headers");
  return from < 0 ? text : text.slice(from, to > from ? to : undefined);
}

const TOOLS = [
  { name: "search",
    description:
      "Search the SDK for a class, a header or a concept. Use this BEFORE naming " +
      "any type you have not verified: an empty result means the SDK does not " +
      "have it, which is a real answer.",
    inputSchema: { type: "object", properties: {
      query: { type: "string", description: "Class name, header name or concept." },
      limit: { type: "number", description: "Maximum results (default 30)." },
    }, required: ["query"] },
    run: toolSearch },

  { name: "type",
    description:
      "The real declaration of a public type, with the comment that explains " +
      "why it exists. Use this to get exact method signatures rather than " +
      "guessing them from another library.",
    inputSchema: { type: "object", properties: {
      name: { type: "string", description: "Exact type name, e.g. LadderFilter." },
    }, required: ["name"] },
    run: toolType },

  { name: "header",
    description: "What one header is for, every public type in it, and the include line.",
    inputSchema: { type: "object", properties: {
      name: { type: "string", description: "Header file name, e.g. dsp.h." },
    }, required: ["name"] },
    run: toolHeader },

  { name: "headers",
    description: "Every header with a one-line purpose. Optionally filtered.",
    inputSchema: { type: "object", properties: {
      filter: { type: "string", description: "Optional substring." },
    } },
    run: toolHeaders },

  { name: "example",
    description:
      "A complete working plugin source. Kinds: effect, instrument, sidechain, " +
      "aux-outputs, midi-output, sampler, flexible-channels, reverb.",
    inputSchema: { type: "object", properties: {
      kind: { type: "string", description: "Which example (default: effect)." },
    } },
    run: toolExample },

  { name: "rules",
    description:
      "The shape of a plugin and the rules that are not guessable from the API: " +
      "which process signature makes an instrument, latency reporting, what is " +
      "forbidden on the audio thread, and which objects are too big for the stack.",
    inputSchema: { type: "object", properties: {} },
    run: toolRules },
];

// ── JSON-RPC 2.0 over stdio ─────────────────────────────────────────────────

const send = (msg) => process.stdout.write(JSON.stringify(msg) + "\n");
const ok = (id, result) => send({ jsonrpc: "2.0", id, result });
const fail = (id, code, message) => send({ jsonrpc: "2.0", id, error: { code, message } });

function handle(msg) {
  const { id, method, params } = msg;
  // A notification has no id and takes no reply. `notifications/initialized`
  // arrives right after the handshake and answering it is a protocol error.
  if (id === undefined) return;

  switch (method) {
    case "initialize":
      return ok(id, {
        protocolVersion: params?.protocolVersion ?? "2024-11-05",
        capabilities: { tools: {} },
        serverInfo: { name: "sonore-sdk", version: "1.0.0" },
      });
    case "ping":
      return ok(id, {});
    case "tools/list":
      return ok(id, {
        tools: TOOLS.map(({ name, description, inputSchema }) =>
          ({ name, description, inputSchema })),
      });
    case "tools/call": {
      const tool = TOOLS.find((t) => t.name === params?.name);
      if (!tool) return fail(id, -32602, `No tool called "${params?.name}".`);
      try {
        const text = tool.run(params.arguments ?? {});
        return ok(id, { content: [{ type: "text", text: String(text) }] });
      } catch (err) {
        // Reported as tool output rather than a protocol error: the agent can
        // read it and try something else, where a JSON-RPC error just aborts.
        return ok(id, {
          content: [{ type: "text", text: `Tool failed: ${err?.message ?? err}` }],
          isError: true,
        });
      }
    }
    default:
      return fail(id, -32601, `Unsupported method "${method}".`);
  }
}

let buffer = "";
process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => {
  buffer += chunk;
  let nl;
  while ((nl = buffer.indexOf("\n")) >= 0) {
    const line = buffer.slice(0, nl).trim();
    buffer = buffer.slice(nl + 1);
    if (!line) continue;
    try {
      handle(JSON.parse(line));
    } catch {
      // A malformed line has no id to answer against, so there is nobody to
      // tell. Dropping it keeps the stream alive for the next message.
    }
  }
});
process.stdin.on("end", () => process.exit(0));
