// SPDX-License-Identifier: Apache-2.0
// The API reference and the guides, built with Doxygen into sdk/build/docs.
//
//   npm run docs          (doxygen from sdk/tools/doxygen, or on PATH)
//   npm run docs:fetch    (Windows: fetch that doxygen first)
//
// Open sdk/build/docs/html/index.html afterwards. Nothing it writes is
// committed: sdk/build is ignored.
//
// ── What fails the run ──────────────────────────────────────────────────────
//
// A Doxygen warning about the public API or the guides: a comment Doxygen
// cannot parse (an unknown \command, an HTML tag it does not know, a #word it
// tries to link) is a comment that renders wrong in the API reference, so it is
// fixed rather than tolerated. Two kinds are passed over, and listed as
// passed over rather than hidden: the preprocessor not finding an #include
// (a vendored C source the API reference never documents), and a Markdown link
// to a .md file that exists but is not part of the API reference (the README's
// links to the repository's own files, which work on GitHub).
//
// Also failing: a top-level header with no area below; a relative link in
// the guides or the README that points at a file that is not there; and a
// `sonore::...` name in a guide that the API reference does not document --
// Doxygen leaves such a name unlinked without a word, so the generated pages
// are read back for it.
//
// ── What is a report, not a failure ─────────────────────────────────────────
//
// Public classes and members with no comment. Doxygen documents them anyway
// (EXTRACT_ALL, see the Doxyfile): a class a plugin uses is API whether or
// not it has a comment yet. They are counted and listed in
// sdk/build/docs/undocumented.txt, so the number can only be argued about
// with the list in hand.
//
// And comments Doxygen JOINS: a doc comment followed directly by another one
// is merged into the next declaration's documentation, which is right when
// the first is an introduction and wrong when it is a comment whose
// declaration moved away -- the ParamInfo comment once documented EditorKind.
// Listed in sdk/build/docs/joined-comments.txt for a person to judge.
//
// ── The areas ───────────────────────────────────────────────────────────────
//
// The API reference is grouped by area. Areas are not written into ~270 headers:
// AREAS below assigns each top-level header (a header under gfx/ is the
// native GUI unless GFX_ELSEWHERE says otherwise), and this script writes one
// Doxygen @file block per header into sdk/build/docs/areas.dox, filed under
// its area, with the header's own top comment as the file's description --
// the comment that says what the file is and why it is built that way, which
// is where the SDK keeps its design notes.
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const sdk = join(root, "sdk");
const docsDir = join(sdk, "docs");
const includeDir = join(sdk, "include", "sonore");
const out = join(sdk, "build", "docs");
const log = (s) => process.stdout.write(s + "\n");

/** Pinned beside scripts/fetch-doxygen.mjs: the version CI's apt installs. */
const EXPECTED_DOXYGEN = "1.9.8";

// ── The areas ────────────────────────────────────────────────────────────────

const AREAS = [
  {
    id: "plugin",
    title: "Plugin model",
    brief: "What a plugin declares and keeps: the DSP's types, parameters, state, presets, undo.",
    guides: ["getting-started.md", "parameters-and-state.md", "presets.md"],
    headers: [
      "audio.h", "plugin.h", "state_bag.h", "presets.h", "user_presets.h", "preset_file.h",
      "value_tree_state.h", "undo.h", "version.h", "plugin_host_type.h", "output_guard.h",
      "license.h", "license_runtime.h",
    ],
  },
  {
    id: "dsp",
    title: "DSP",
    brief: "The toolkit dsp.h pulls in, and the maths under it: filters, dynamics, reverbs, synthesis, analysis.",
    guides: ["getting-started.md", "midi-and-mpe.md"],
    headers: [
      "dsp.h", "effects.h", "transport.h", "fft.h", "resample.h", "shaping.h", "fir.h", "metering.h",
      "dynamics.h", "lofi.h", "fdn.h", "pitch.h", "stft.h", "synth.h", "tone.h", "delay.h", "tape.h",
      "tube.h", "crossover.h", "va_filters.h", "granular.h", "phase_vocoder.h", "spring.h", "vocoder.h",
      "compressors.h", "eq.h", "clipper.h", "hilbert.h", "modulation.h", "multiband.h",
      "reflections.h", "pitch_correct.h", "distortion.h", "circuit.h", "fuzz.h", "passive_eq.h",
      "dynamic_eq.h", "limiting.h", "plate.h", "screamer.h", "filter_design.h", "random.h", "simd.h",
      "fast_math.h", "denormals.h", "interpolation.h", "phase.h", "polynomial.h", "matrix.h",
      "special.h", "statistics.h", "channel_matrix.h", "sampler.h", "impulse.h", "scope_buffer.h",
      "load.h",
    ],
  },
  {
    id: "formats",
    title: "Formats and wrappers",
    brief: "One source, every format: CLAP, VST3, AU, LV2, the standalone application and the browser ABI.",
    guides: ["getting-started.md", "platforms-and-formats.md"],
    headers: [
      "formats.h", "clap_wrapper.h", "vst3_wrapper.h", "vst3_uid.h", "au_wrapper.h", "au_view.h",
      "lv2_wrapper.h", "lv2_ports.h", "lv2_atom.h", "lv2_ui.h", "standalone.h", "wasm_abi.h",
    ],
  },
  {
    id: "hosting",
    title: "Hosting plugins",
    brief: "Loading other people's plugins: scanning, the scan cache, CLAP/VST3/LV2/LADSPA/AU hosts, a graph.",
    guides: ["hosting.md"],
    headers: ["host.h", "host_au.h", "plugin_cache.h", "graph.h", "turtle.h", "ladspa_abi.h"],
  },
  {
    id: "gui_native",
    title: "Native GUI",
    brief: "The SDK-drawn interface: components, widgets, LookAndFeel, text, images, windows, the plugin editor.",
    guides: ["native-editor.md"],
    headers: [], // everything under gfx/ not in GFX_ELSEWHERE
  },
  {
    id: "gui_web",
    title: "Web editor bridge",
    brief: "A page as the plugin's face: the window.sonore bridge and the three OS webviews.",
    guides: ["web-editor.md"],
    headers: ["gui.h", "webview_win32.h", "webview_gtk.h", "webview_cocoa.h"],
  },
  {
    id: "files",
    title: "Files and codecs",
    brief: "Paths, streams and folders; WAV, FLAC, MP3, Ogg and the OS's decoders; images; zip; waveforms.",
    guides: [],
    headers: [
      "files.h", "path_io.h", "file_types.h", "streams.h", "file_dialog.h", "wav.h", "audiofile.h",
      "audiofile_codecs.h", "audiostream.h", "flac_encode.h", "lame_encoder.h",
      "system_audio_format.h", "sample_stream.h", "record.h", "zip.h", "waveform.h", "thumbnail.h",
      "thumbnail_cache.h",
    ],
  },
  {
    id: "midi",
    title: "MIDI and OSC",
    brief: "MIDI beyond a plugin's note input: files, sequences, UMP and MIDI-CI, devices, clock, OSC.",
    guides: ["midi-and-mpe.md"],
    headers: [
      "midi_ci.h", "midi_clock.h", "midi_collector.h", "midi_file.h", "midi_input.h", "midi_output.h",
      "midi_sequence.h", "ump.h", "ump128.h", "sysex.h", "osc.h", "bluetooth_midi.h",
    ],
  },
  {
    id: "system",
    title: "System, devices and network",
    brief: "Audio devices per OS, threads, processes, IPC, sockets and HTTP, what machine this is.",
    guides: ["platforms-and-formats.md"],
    headers: [
      "audio_wasapi.h", "audio_wasapi_input.h", "audio_alsa.h", "audio_alsa_input.h",
      "audio_coreaudio.h", "audio_coreaudio_input.h", "audio_jack.h", "audio_asio.h", "asio_format.h",
      "audio_input.h", "audio_linux.h", "audio_ring.h", "audio_workgroup.h", "audio_cd.h",
      "system_volume.h", "net.h", "process.h", "ipc.h", "thread_pool.h", "system_stats.h",
      "apple_block.h", "win_com.h",
    ],
  },
  {
    id: "app",
    title: "Applications",
    brief: "What a desktop application built on the SDK needs: the message thread, settings, documents, logs, tests.",
    guides: ["testing.md"],
    headers: [
      "application.h", "application_properties.h", "file_document.h", "recent_files.h",
      "user_settings.h", "commands.h", "message_manager.h", "multi_timer.h", "localisation.h",
      "analytics.h", "broadcaster.h", "logger.h", "leak_detector.h", "unit_test.h", "audio_source.h",
    ],
  },
  {
    id: "data",
    title: "Data, text and crypto",
    brief: "JSON, XML, expressions, a JavaScript interpreter, text diffs, numbers as text, hashes, RSA, UUIDs.",
    guides: [],
    headers: [
      "json.h", "xml.h", "expression.h", "javascript.h", "text_diff.h", "number_text.h", "time.h",
      "hash.h", "crypto.h", "bigint.h", "uuid.h",
    ],
  },
];

/** gfx/ headers that belong to another area than the native GUI. */
const GFX_ELSEWHERE = {
  "gfx/png.h": "files",
  "gfx/jpeg.h": "files",
  "gfx/gif.h": "files",
  "gfx/inflate.h": "files",
  "gfx/image_format.h": "files",
  "gfx/plugin_list.h": "hosting",
};

function everyHeader(dir) {
  const found = [];
  for (const name of readdirSync(dir)) {
    const full = join(dir, name);
    if (statSync(full).isDirectory()) found.push(...everyHeader(full));
    else if (name.endsWith(".h")) found.push(full);
  }
  return found;
}

/** The header's top comment: the // lines after the SPDX line, up to the
 *  first line that is not a comment. */
function topComment(text) {
  const lines = text.split(/\r?\n/);
  const body = [];
  let i = 0;
  if (/SPDX-License-Identifier/.test(lines[0] || "")) i = 1;
  for (; i < lines.length; ++i) {
    const line = lines[i];
    if (/^\s*#pragma once/.test(line)) continue; // license_runtime.h has it first
    if (!/^\s*\/\//.test(line)) break;
    body.push(line.replace(/^\s*\/\/ ?/, ""));
  }
  while (body.length && body[0].trim() === "") body.shift();
  while (body.length && body[body.length - 1].trim() === "") body.pop();
  return body;
}

/** Text for a Doxygen @brief: Doxygen's special characters escaped outside
 *  `code spans`, so a header's sentence cannot become a command or a tag. */
function briefText(lines) {
  const para = [];
  for (const l of lines) {
    if (l.trim() === "") break;
    para.push(l.trim());
  }
  let s = para.join(" ").replace(/^Sonore SDK\s*(\u2014|--|-|:)\s*/, "");
  s = s.replace(/\*\//g, "* /");
  s = s.charAt(0).toUpperCase() + s.slice(1);
  return s
    .split(/(`[^`]*`)/)
    .map((part, i) => (i % 2 ? part : part.replace(/([\\@<>#%&$])/g, "\\$1")))
    .join("");
}

function writeAreas(headers) {
  const areaOf = new Map();
  const problems = [];
  for (const area of AREAS)
    for (const h of area.headers) {
      if (areaOf.has(h)) problems.push(`${h} is in two areas: ${areaOf.get(h)} and ${area.id}`);
      areaOf.set(h, area.id);
    }
  for (const [h, id] of Object.entries(GFX_ELSEWHERE)) areaOf.set(h, id);

  const present = new Set(headers.map((f) => relative(includeDir, f).replace(/\\/g, "/")));
  for (const h of areaOf.keys())
    if (!present.has(h)) problems.push(`${h} is in AREAS (scripts/docs.mjs) but not in sdk/include/sonore`);

  let dox = "/* Generated by scripts/docs.mjs -- the areas of the API reference. Not committed. */\n\n";
  for (const area of AREAS) {
    dox += `/** @defgroup ${area.id} ${area.title}\n *  @brief ${area.brief}\n`;
    if (area.guides.length) {
      // Relative to this file, which is where Doxygen resolves a link to a
      // page from.
      const toGuides = relative(out, join(docsDir, "guides")).replace(/\\/g, "/");
      dox += " *\n *  Guides: " +
             area.guides.map((g) => `[${g.replace(/\.md$/, "").replace(/-/g, " ")}](${toGuides}/${g})`).join(", ") +
             ".\n";
    }
    dox += " */\n\n";
  }
  for (const file of headers) {
    const rel = relative(includeDir, file).replace(/\\/g, "/");
    let id = areaOf.get(rel);
    if (!id && rel.startsWith("gfx/")) id = "gui_native";
    if (!id) {
      problems.push(`${rel} has no area: add it to AREAS in scripts/docs.mjs`);
      continue;
    }
    // No leading " * " on the lines: inside @verbatim Doxygen keeps it.
    const comment = topComment(readFileSync(file, "utf8"));
    dox += `/** @file sonore/${rel}\n@ingroup ${id}\n`;
    if (comment.length) {
      dox += `@brief ${briefText(comment)}\n\n@verbatim\n`;
      for (const l of comment)
        dox += l.replace(/\*\//g, "* /").replace(/@endverbatim|\\endverbatim/g, "end-verbatim").trimEnd() + "\n";
      dox += "@endverbatim\n";
    }
    dox += "*/\n\n";
  }
  writeFileSync(join(out, "areas.dox"), dox);
  return problems;
}

// ── Doxygen ──────────────────────────────────────────────────────────────────

function findDoxygen() {
  const candidates = [];
  if (process.env.SONORE_DOXYGEN) candidates.push(process.env.SONORE_DOXYGEN);
  candidates.push(join(sdk, "tools", "doxygen", process.platform === "win32" ? "doxygen.exe" : "doxygen"));
  candidates.push("doxygen");
  for (const c of candidates) {
    if (c !== "doxygen" && !existsSync(c)) continue;
    const r = spawnSync(c, ["--version"], { encoding: "utf8" });
    if (r.status === 0) return { path: c, version: r.stdout.trim().split(/\s+/)[0] };
  }
  return null;
}

function sdkVersion() {
  const text = readFileSync(join(includeDir, "version.h"), "utf8");
  const m = text.match(/#define SONORE_SDK_VERSION_STRING "([^"]+)"/);
  return m ? m[1] : "unknown";
}

// ── The reports ──────────────────────────────────────────────────────────────

function xmlText(s) {
  return s.replace(/<[^>]+>/g, "").replace(/&[a-z]+;|&#\d+;/g, " ").trim();
}

/** Public classes and members with neither a brief nor a detailed comment,
 *  from Doxygen's XML: what EXTRACT_ALL documented with nothing to say. */
function undocumented() {
  const xmlDir = join(out, "xml");
  const index = readFileSync(join(xmlDir, "index.xml"), "utf8");
  const classes = { total: 0, bare: [] };
  const members = { total: 0, bare: [] };
  const seenMembers = new Set();
  const compoundRe = /<compound refid="([^"]+)" kind="(class|struct|union|namespace|file)">/g;
  let m;
  while ((m = compoundRe.exec(index))) {
    const [, refid, kind] = m;
    const xml = readFileSync(join(xmlDir, refid + ".xml"), "utf8");
    const name = (xml.match(/<compoundname>([^<]*)<\/compoundname>/) || [])[1] || refid;
    if (kind === "class" || kind === "struct" || kind === "union") {
      // The compound's own description is the one outside every sectiondef.
      const own = xml.replace(/<sectiondef[\s\S]*?<\/sectiondef>/g, "");
      const brief = xmlText((own.match(/<briefdescription>([\s\S]*?)<\/briefdescription>/) || [])[1] || "");
      const detail = xmlText((own.match(/<detaileddescription>([\s\S]*?)<\/detaileddescription>/) || [])[1] || "");
      classes.total++;
      if (!brief && !detail) classes.bare.push(`${kind} ${name.replace(/&lt;/g, "<").replace(/&gt;/g, ">")}`);
    }
    // Members are counted where they are defined: a class's own, and a
    // namespace's functions, variables, enums and typedefs. A file compound
    // repeats its namespaces' members, and macros are not API here.
    if (kind === "file") continue;
    const sectionRe = /<sectiondef kind="([^"]+)">([\s\S]*?)<\/sectiondef>/g;
    let s;
    while ((s = sectionRe.exec(xml))) {
      // Friends and macros are not API here. A private member is skipped by
      // its own prot, not by its section's name: a ///@{ group makes a
      // "user-defined" section, public or private, and a test of the
      // section's name for "define" matched that too, so a documented group
      // of public fields left the count instead of joining it.
      if (s[1] === "friend" || s[1] === "define") continue;
      const memberRe = /<memberdef kind="([^"]+)" id="([^"]+)" prot="([^"]+)"[\s\S]*?<name>([^<]*)<\/name>([\s\S]*?)<\/memberdef>/g;
      let md;
      while ((md = memberRe.exec(s[2]))) {
        const [, mkind, id, prot, mname, rest] = md;
        if (prot === "private") continue;
        if (seenMembers.has(id)) continue;
        seenMembers.add(id);
        const brief = xmlText((rest.match(/<briefdescription>([\s\S]*?)<\/briefdescription>/) || [])[1] || "");
        const detail = xmlText((rest.match(/<detaileddescription>([\s\S]*?)<\/detaileddescription>/) || [])[1] || "");
        const inbody = xmlText((rest.match(/<inbodydescription>([\s\S]*?)<\/inbodydescription>/) || [])[1] || "");
        members.total++;
        if (!brief && !detail && !inbody) members.bare.push(`${mkind} ${name}::${mname}`);
      }
    }
  }
  return { classes, members };
}

/** A doc comment that ends on the line before another begins: Doxygen gives
 *  both to the next declaration. */
function joinedComments(headers) {
  const found = [];
  for (const file of headers) {
    const lines = readFileSync(file, "utf8").split(/\r?\n/);
    for (let i = 1; i < lines.length; ++i)
      if (/\*\/\s*$/.test(lines[i - 1]) && /^\s*\/\*\*/.test(lines[i]))
        found.push(`${relative(includeDir, file).replace(/\\/g, "/")}:${i + 1}`);
  }
  return found;
}

/** Relative links in the guides, the docs index and the README that point
 *  at nothing. */
function brokenLinks() {
  const files = [join(sdk, "README.md")];
  const walk = (d) => {
    for (const name of readdirSync(d)) {
      const full = join(d, name);
      if (statSync(full).isDirectory()) walk(full);
      else if (name.endsWith(".md")) files.push(full);
    }
  };
  walk(docsDir);
  const broken = [];
  for (const f of files) {
    const text = readFileSync(f, "utf8").replace(/```[\s\S]*?```/g, "");
    const linkRe = /\]\(([^)\s]+)\)/g;
    let m;
    while ((m = linkRe.exec(text))) {
      const target = m[1].split("#")[0];
      if (!target || /^[a-z]+:/i.test(target) || target.startsWith("@ref")) continue;
      if (!existsSync(resolve(dirname(f), decodeURIComponent(target))))
        broken.push(`${relative(root, f).replace(/\\/g, "/")}: ${m[1]}`);
    }
  }
  return broken;
}

/** Every `sonore::...` the guides name in running text must be something the
 *  reference documents. Doxygen links such a name when it finds it and leaves
 *  plain text when it does not -- without a warning -- so the generated pages
 *  are read back: a name with no link is a name the API does not have. */
function unknownNames() {
  const html = join(out, "html");
  const linked = new Set();
  for (const name of readdirSync(html)) {
    if (!name.endsWith(".html") || !(name.startsWith("md_") || name === "index.html")) continue;
    const text = readFileSync(join(html, name), "utf8");
    const re = /<a class="el" href="[^"]*"[^>]*>([^<]*)<\/a>/g;
    let m;
    while ((m = re.exec(text))) linked.add(m[1]);
  }
  const unknown = [];
  const walk = (d) => {
    for (const name of readdirSync(d)) {
      const full = join(d, name);
      if (statSync(full).isDirectory()) {
        walk(full);
        continue;
      }
      if (!name.endsWith(".md")) continue;
      const text = readFileSync(full, "utf8").replace(/```[\s\S]*?```/g, "");
      const re = /`(sonore::[A-Za-z0-9_:]+(?:\(\))?)`/g;
      let m;
      while ((m = re.exec(text)))
        if (!linked.has(m[1])) unknown.push(`${relative(root, full).replace(/\\/g, "/")}: ${m[1]}`);
    }
  };
  walk(docsDir);
  return unknown;
}

// ── The run ──────────────────────────────────────────────────────────────────

const failures = [];
const doxygen = findDoxygen();
if (!doxygen) {
  log("docs: doxygen not found. On Windows run `npm run docs:fetch` (it goes to sdk/tools/doxygen);");
  log("      elsewhere install it (apt install doxygen, brew install doxygen) or set SONORE_DOXYGEN.");
  process.exit(1);
}
log(`docs: doxygen ${doxygen.version} (${doxygen.path})`);
if (doxygen.version !== EXPECTED_DOXYGEN)
  log(`docs: note -- CI runs doxygen ${EXPECTED_DOXYGEN}; another version can warn differently.`);

rmSync(join(out, "html"), { recursive: true, force: true });
rmSync(join(out, "xml"), { recursive: true, force: true });
mkdirSync(out, { recursive: true });

const headers = everyHeader(includeDir).sort();
for (const p of writeAreas(headers)) failures.push(p);

// Doxygen runs from an empty folder four levels deep: it resolves a Markdown
// link against its working directory before the page's own folder, and from
// here no link of up to four ../ can reach a real file (see the Doxyfile).
const cwd = join(out, ".cwd", "a", "b", "c");
rmSync(join(out, ".cwd"), { recursive: true, force: true });
mkdirSync(cwd, { recursive: true });
const slashes = (p) => p.replace(/\\/g, "/");
const r = spawnSync(doxygen.path, [join(docsDir, "Doxyfile")], {
  cwd,
  encoding: "utf8",
  env: {
    ...process.env,
    SONORE_SDK_DIR: slashes(sdk),
    SONORE_SDK_VERSION: sdkVersion(),
    SONORE_DOCS_AREAS: slashes(join(out, "areas.dox")),
  },
});
rmSync(join(out, ".cwd"), { recursive: true, force: true });
if (r.status !== 0) {
  log(r.stdout || "");
  log(r.stderr || "");
  log(`docs: doxygen exited with ${r.status}`);
  process.exit(1);
}
if (r.stderr && r.stderr.trim()) log(r.stderr.trim());

const warningLines = existsSync(join(out, "warnings.log"))
  ? readFileSync(join(out, "warnings.log"), "utf8").split(/\r?\n/).filter((l) => l.trim())
  : [];
// One warning per line that names a place; the lines after it that do not
// are its continuation.
const warnings = [];
for (const line of warningLines) {
  if (/:\d+: (warning|error):/.test(line) || /^(warning|error):/.test(line) || !warnings.length) warnings.push(line);
  else warnings[warnings.length - 1] += "\n    " + line.trim();
}
// Passed over, by kind:
//  - the preprocessor not finding a vendored #include (a C source the
//    reference never documents);
//  - a Markdown link to a .md file that exists but is not part of the
//    reference (README's link to the repository's THIRD_PARTY_NOTICES.md):
//    it works on GitHub, where those files are read, and is plain text here.
//    brokenLinks() below is what catches a link to a file that is not there.
const passOver = [
  (w) => /warning: include file .* not found/.test(w),
  (w) => {
    const m = w.match(/warning: unable to resolve reference to '([^']+\.md)' for \\ref command/);
    return !!m && (existsSync(m[1]) || existsSync(resolve(root, m[1])));
  },
];
// Doxygen reports some places twice; each is counted once.
const passedOver = [...new Set(warnings.filter((w) => passOver.some((test) => test(w))))];
const matter = [...new Set(warnings.filter((w) => !passedOver.includes(w)))];

const { classes, members } = undocumented();
writeFileSync(
  join(out, "undocumented.txt"),
  `# Public classes with no comment (${classes.bare.length} of ${classes.total})\n` +
    classes.bare.sort().join("\n") +
    `\n\n# Public members with no comment (${members.bare.length} of ${members.total})\n` +
    members.bare.sort().join("\n") +
    "\n",
);
const joined = joinedComments(headers);
writeFileSync(join(out, "joined-comments.txt"), joined.join("\n") + "\n");
for (const b of brokenLinks()) failures.push(`broken link: ${b}`);
for (const n of unknownNames()) failures.push(`a name the API reference does not document: ${n}`);

log(`docs: ${headers.length} headers in ${AREAS.length} areas`);
const shown = (w) => w.split(root.replace(/\\/g, "/") + "/").join("").split(root + "\\").join("");
log(`docs: ${matter.length} warning(s) that matter, ${passedOver.length} passed over:`);
for (const w of passedOver) log("  (passed over) " + shown(w));
for (const w of matter) log("  " + shown(w));
log(`docs: undocumented public classes: ${classes.bare.length} of ${classes.total}; ` +
    `members: ${members.bare.length} of ${members.total} -- listed in sdk/build/docs/undocumented.txt`);
log(`docs: ${joined.length} doc comment(s) Doxygen joins onto the next declaration -- ` +
    "sdk/build/docs/joined-comments.txt");
for (const f of failures) log("  " + f);
log(`docs: ${join(out, "html", "index.html")}`);

if (matter.length || failures.length) {
  log("\nDOCS FAILED");
  process.exit(1);
}
log("\nDOCS OK");
