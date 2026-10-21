// SPDX-License-Identifier: Apache-2.0
// Sonore SDK -- the example plugins inside a real DAW, driven end to end.
//
//   npm run verify:reaper                 (opt-in; needs REAPER and a Release build)
//   node scripts/reaper-host-test.mjs [--build <dir>] [--work <dir>] [--only a,b]
//                                     [--skip-editor] [--rates 44100,96000]
//                                     [--dump-chunks]  (each track's state
//                                     chunk into results.json, to debug)
//
// Every other host check in this repo is a host we wrote (tests/*_host_test,
// render_tool over sonore/host.h) or a validator (clap-validator, pluginval,
// lv2lint). This one hands the CLAP, the VST3 and the LV2 build of each
// example to REAPER and asks REAPER, through its own ReaScript API, what it
// made of them:
//
//   1 scan     REAPER lists each plugin with the right name, vendor, kind and
//              channel layout, and TrackFX_AddByName finds it;
//   2 render   an offline render through it equals our own host's render of
//              the same input (render_tool, the same binary's CLAP build), and
//              an instrument plays a MIDI item at the right pitch;
//   3 params   values set through TrackFX_SetParam land and read back with the
//              plugin's own text, the same in both formats; an envelope on a
//              parameter changes the render exactly where it says;
//   4 state    a project saved in one REAPER and opened in a new one brings
//              every parameter back, and renders the same -- the sampler
//              included, whose file lives in its StateBag;
//   5 latency  what the plugin reports is what REAPER compensates, a latency
//              switched live reaches REAPER, and the compensated render lines
//              up with the dry signal;
//   6 editor   each editor opens and closes in a floating window, repeatedly,
//              at the same size in every format, and the probe's page drives
//              parameters through the bridge;
//   7 rates    the render comparison again at 44.1 and 96 kHz.
//
// ISOLATED from the user's REAPER, which may be open while this runs:
//   - REAPER runs with -cfgfile pointing into the work folder, so that folder
//     is its whole resource path; nothing under %APPDATA%\REAPER is read or
//     written, and -newinst keeps the command line from being handed to a
//     REAPER that is already running;
//   - vstpath64, clap_path_win64 and lv2path_win64 name the staging folders
//     only, and are rewritten before every launch. `vstfullstate` is pre-set
//     because a profile WITHOUT it is a first run, and on a first run REAPER
//     appends %COMMONPROGRAMFILES%\VST3 and %LOCALAPPDATA%\Programs\Common\VST3
//     to vstpath64 and scans every plugin installed on the machine ("Add
//     system LV2 paths" would do the same with %APPDATA%\LV2 and
//     %COMMONPROGRAMFILES%\LV2). Both lists are read back after every
//     session; REAPER reads no LV2_PATH from the environment;
//   - [audioconfig] mode=4 is Dummy Audio, so no sound card is opened and the
//     "select your audio device?" question is never asked;
//   - only the REAPER this script started (found by its -cfgfile on the
//     command line) is ever stopped, and only its dialogs are answered.
// REAPER always scans the plugins in its own install folder; those are not
// touched by anything this script does.
//
// LV2 is judged by what LV2 can carry. It has no call that turns a value
// into text, so REAPER prints a number ("6000.00") where CLAP and VST3 show
// the plugin's "6.00 kHz": the LV2 checks hold the PLAIN value to the
// plugin's and the CLAP build's, and a named step's label to the plugin's
// own text. Its bypass is the designated lv2:enabled port (REAPER shows it
// as "Enabled"), its latency an output port that only run() writes -- so
// REAPER knows it once the plugin has run, not at insert -- and its
// interface is the page, never a native editor (lv2_ui.h). The examples
// without an LV2 target (the trim) are skipped; the LV2 automation lane
// moves the probe's Gain instead.
//
// What it does NOT prove: anything about macOS, Linux or AU; any DAW
// but REAPER; real-time playback through an audio device (the device is
// Dummy Audio and every render is offline); what an editor LOOKS like --
// windows open, close and talk to the host, nobody looks at them; and hosts
// that drive a plugin differently from REAPER (which, for one, owns a CLAP
// plugin's bypass parameter and keeps FX running while stopped).
//
// The REAPER half is scripts/reaper-host-test.lua; the dialog watcher is
// scripts/reaper-host-dialogs.ps1; the reference renders come from
// tests/render_tool.cpp, built with the plugins.
import { spawn, spawnSync } from "node:child_process";
import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { dirname, isAbsolute, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const log = (s = "") => process.stdout.write(s + "\n");
const argv = process.argv.slice(2);
const opt = (name, fallback) => {
  const at = argv.indexOf(name);
  return at >= 0 && at + 1 < argv.length ? argv[at + 1] : fallback;
};

// ── Preconditions: SKIP, loudly and successfully, when there is no DAW ──────
const reaperExe = process.env.SONORE_REAPER || "C:\\Program Files\\REAPER (x64)\\reaper.exe";
if (process.platform !== "win32" || !existsSync(reaperExe)) {
  log(`SKIP -- REAPER is not installed at ${reaperExe} (set SONORE_REAPER to point elsewhere).`);
  log("Nothing was run: this check drives the example plugins inside a real DAW.");
  process.exit(0);
}
const built = resolve(opt("--build", process.env.SONORE_BUILD_DIR || join(root, "sdk", "build", "Release")));
const renderTool = join(built, "render_tool.exe");
if (!existsSync(renderTool)) {
  log(`SKIP -- no render_tool.exe in ${built}. Build the SDK first (npm run verify:sdk), or pass --build.`);
  process.exit(0);
}
const work = resolve(opt("--work", join(root, "sdk", "build", "reaper-host-test")));
// The user's own preset folder (Documents\<Vendor>\<Plugin>\Presets) is not
// this run's to read: REAPER and render_tool get an empty one of their own,
// so the presets on offer are the factory's and nothing else.
const presetRoot = join(work, "presets");
const isolatedEnv = (env) => ({ ...env, SONORE_PRESET_ROOT: presetRoot });
if (!isAbsolute(work) || work.length < 12) {
  log(`refusing a work folder that short: ${work}`);
  process.exit(2);
}

// ── The plugins, and what each is expected to look like from outside ───────
// ins/outs are the pins REAPER shows on a stereo track: the ducker's second
// input pair is its sidechain, the splitter's outputs 3..6 its two aux buses,
// and an instrument has no audio input at all.
const PLUGINS = {
  saturator: { bundle: "SonoreSaturator", ins: 2, outs: 2 },
  trim: { bundle: "SonoreTrim", ins: 2, outs: 2 },
  ducker: { bundle: "SonoreDucker", ins: 4, outs: 2 },
  splitter: { bundle: "SonoreSplitter", ins: 2, outs: 6 },
  synth: { bundle: "SonoreSynth", ins: 0, outs: 2 },
  sampler: { bundle: "SonoreSampler", ins: 0, outs: 2 },
  arp: { bundle: "SonoreArp", ins: 0, outs: 2 },
  reverb: { bundle: "SonoreReverb", ins: 2, outs: 2 },
  guiprobe: { bundle: "SonoreGuiProbe", ins: 2, outs: 2 },
  // The showcase: a native editor of its own, and 24 samples of latency.
  showcase: { bundle: "SonoreShowcase", ins: 2, outs: 2 },
};
const FORMATS = ["clap", "vst3", "lv2"];
const FMT = { clap: "CLAP", vst3: "VST3", lv2: "LV2" };
const only = opt("--only", "") ? new Set(opt("--only", "").split(",")) : null;
const wanted = Object.keys(PLUGINS).filter((k) => !only || only.has(k) ||
  // the arpeggiator is heard through the synth
  (k === "synth" && only.has("arp")));
const rates = opt("--rates", "44100,96000").split(",").filter(Boolean).map(Number);
const LENGTH = 3; // seconds of every render

// ── Checks, each named by plugin, format and step ───────────────────────────
const results = [];
function check(plugin, format, step, ok, what) {
  results.push({ plugin, format, step, ok: !!ok, what });
  log(`  ${ok ? "ok  " : "FAIL"} [${plugin}/${format}/${step}] ${what}`);
}
const fmtNum = (x, d = 3) => (Number.isFinite(x) ? x.toFixed(d) : String(x));

// ── Staging ──────────────────────────────────────────────────────────────────
function stopOurReaper(ini) {
  // Only processes whose command line carries OUR config path.
  const ps = "Get-CimInstance Win32_Process -Filter \"Name='reaper.exe'\" | " +
             "Where-Object { $_.CommandLine -and $_.CommandLine.Contains('" + ini.replace(/'/g, "''") +
             "') } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }";
  spawnSync("powershell.exe", ["-NoProfile", "-NonInteractive", "-EncodedCommand",
                               Buffer.from(ps, "utf16le").toString("base64")],
            { stdio: "ignore", windowsHide: true });
}

const cfgDir = join(work, "cfg");
const ini = join(cfgDir, "reaper.ini");
stopOurReaper(ini); // a REAPER left over from an interrupted run of THIS script
try {
  rmSync(work, { recursive: true, force: true });
} catch (e) {
  log(`cannot clear ${work}: ${e.code || e.message} -- is something still using it?`);
  process.exit(1);
}
const stageClap = join(work, "plugins", "clap"), stageVst3 = join(work, "plugins", "vst3");
const stageLv2 = join(work, "plugins", "lv2");
const mediaDir = join(work, "media"), sessionsDir = join(work, "sessions");
for (const d of [cfgDir, stageClap, stageVst3, stageLv2, mediaDir, sessionsDir]) mkdirSync(d, { recursive: true });

const have = { clap: new Set(), vst3: new Set(), lv2: new Set() };
for (const key of wanted) {
  const p = PLUGINS[key];
  const clap = join(built, p.bundle + ".clap");
  if (existsSync(clap)) {
    cpSync(clap, join(stageClap, p.bundle + ".clap"));
    have.clap.add(key);
  }
  // The bundle folder, as a host expects to find it; the flat DLL beside it
  // in the build folder would be a second copy of the same plugin.
  const bundle = join(built, p.bundle + "VST3.vst3.bundle");
  if (existsSync(bundle)) {
    cpSync(bundle, join(stageVst3, p.bundle + "VST3.vst3"), { recursive: true });
    have.vst3.add(key);
  }
  // An LV2 bundle is a folder already: the DLL and the .ttl files the TTL
  // generator wrote beside it (sonore_add_lv2). REAPER scans the folders
  // named in lv2path_win64 one level deep -- each bundle directly inside.
  const lv2 = join(built, p.bundle + "LV2.lv2");
  if (existsSync(join(lv2, "manifest.ttl"))) {
    cpSync(lv2, join(stageLv2, p.bundle + "LV2.lv2"), { recursive: true });
    have.lv2.add(key);
  }
}
if (have.clap.size === 0) {
  log(`SKIP -- no example plugins built in ${built}.`);
  process.exit(0);
}
log(`REAPER: ${reaperExe}`);
log(`plugins from ${built}: ${have.clap.size} CLAP, ${have.vst3.size} VST3, ${have.lv2.size} LV2`);
log(`work folder (REAPER's resource path, media, renders): ${work}`);

// ── What our own host says about each plugin ────────────────────────────────
function describe(key) {
  const r = spawnSync(renderTool, [join(built, PLUGINS[key].bundle + ".clap"), "--describe"],
                      { encoding: "utf8", env: isolatedEnv(process.env) });
  const d = { params: [], presets: [] };
  for (const line of (r.stdout || "").split(/\r?\n/)) {
    const f = line.split("\t");
    if (f[0] === "plugin") Object.assign(d, { name: f[2], vendor: f[3], instrument: f[4] === "instrument" });
    if (f[0] === "param") d.params.push({ i: +f[1], name: f[2], min: +f[3], max: +f[4], def: +f[5], stepped: f[6] === "1", text: f[7] });
    if (f[0] === "preset") d.presets[+f[1]] = { name: f[2], values: [] };
    if (f[0] === "presetparam" && d.presets[+f[1]]) d.presets[+f[1]].values[+f[2]] = { value: +f[3], text: f[4] };
  }
  return d;
}
const DESC = {};
for (const key of have.clap) DESC[key] = describe(key);
const query = (key, fmt) => `${FMT[fmt]}${DESC[key].instrument ? "i" : ""}: ${DESC[key].name}`;
const norm = (key, index, plain) => {
  const p = DESC[key].params[index];
  return (plain - p.min) / (p.max - p.min);
};
// The controls an LV2 build has: the plugin's own, at the same port index as
// the CLAP build's parameter, and then -- for an effect -- lv2:enabled where
// CLAP has its Bypass parameter, the other way up (1 = processing). REAPER
// lists the freewheel input and the two meter outputs after them, and
// drives freewheel itself while it renders; neither is a control to move.
const lv2Params = (key) => {
  const own = DESC[key].params.filter((p) => p.name !== "Bypass");
  if (DESC[key].instrument) return own;
  return [...own, { i: own.length, name: "Enabled", min: 0, max: 1, def: 1, stepped: true, text: "On", enabled: true }];
};
// A step the plugin NAMES ("Short", "tanh(x)") rather than a number with a
// unit: LV2 carries those names as scale points, and REAPER shows them.
const isName = (text) => !/^[-+]?(\d|\.\d)/.test(text || "");
// What REAPER shows for an LV2 control at plain value `v`: the label of a
// named step, On/Off for a toggle, and otherwise its own rendering of the
// number -- LV2 has no call that turns a value into the plugin's text.
const lv2Shows = (p, got, v) => {
  if (p.enabled) return got.text === (v >= 0.5 ? "On" : "Off");
  if (p.stepped && isName(p.text)) return got.text === p.text;
  return Math.abs(got.value - v) <= 1e-6 * Math.max(1, Math.abs(v)) && Math.abs(parseFloat(got.text) - v) <= 0.0051;
};

// ── Test media ───────────────────────────────────────────────────────────────
function writeWav(path, data, channels, rate) {
  const b = Buffer.alloc(44 + data.length * 4);
  b.write("RIFF", 0); b.writeUInt32LE(36 + data.length * 4, 4); b.write("WAVE", 8);
  b.write("fmt ", 12); b.writeUInt32LE(16, 16); b.writeUInt16LE(3, 20); b.writeUInt16LE(channels, 22);
  b.writeUInt32LE(rate, 24); b.writeUInt32LE(rate * channels * 4, 28); b.writeUInt16LE(channels * 4, 32);
  b.writeUInt16LE(32, 34); b.write("data", 36); b.writeUInt32LE(data.length * 4, 40);
  for (let i = 0; i < data.length; i++) b.writeFloatLE(data[i], 44 + i * 4);
  writeFileSync(path, b);
}
function readWav(path) {
  if (!existsSync(path)) return null;
  const b = readFileSync(path);
  if (b.length < 12 || b.toString("ascii", 0, 4) !== "RIFF" || b.toString("ascii", 8, 12) !== "WAVE") return null;
  let at = 12, fmt = null, data = null;
  while (at + 8 <= b.length) {
    const id = b.toString("ascii", at, at + 4), size = b.readUInt32LE(at + 4);
    if (id === "fmt ") fmt = { tag: b.readUInt16LE(at + 8), channels: b.readUInt16LE(at + 10),
                               rate: b.readUInt32LE(at + 12), bits: b.readUInt16LE(at + 22) };
    if (id === "data") data = b.subarray(at + 8, Math.min(b.length, at + 8 + size));
    at += 8 + size + (size & 1);
  }
  if (!fmt || !data) return null;
  if (fmt.tag === 0xFFFE) fmt.tag = fmt.bits === 32 ? 3 : 1;
  const n = Math.floor(data.length / (fmt.bits / 8));
  const s = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    if (fmt.tag === 3 && fmt.bits === 32) s[i] = data.readFloatLE(i * 4);
    else if (fmt.tag === 3 && fmt.bits === 64) s[i] = data.readDoubleLE(i * 8);
    else if (fmt.bits === 16) s[i] = data.readInt16LE(i * 2) / 32768;
    else if (fmt.bits === 24) s[i] = ((data[i * 3] | (data[i * 3 + 1] << 8) | (data[i * 3 + 2] << 16)) << 8 >> 8) / 8388608;
    else if (fmt.bits === 32) s[i] = data.readInt32LE(i * 4) / 2147483648;
  }
  return { ...fmt, samples: s, frames: Math.floor(n / fmt.channels) };
}
const channel = (w, c) => {
  const out = new Float32Array(w.frames);
  for (let i = 0; i < w.frames; i++) out[i] = w.samples[i * w.channels + c];
  return out;
};

const media = {};
function makeMedia(rate) {
  const n = rate * LENGTH;
  const stereo = (fn) => {
    const d = new Float32Array(n * 2);
    for (let i = 0; i < n; i++) { const [l, r] = fn(i, i / rate); d[i * 2] = l; d[i * 2 + 1] = r; }
    return d;
  };
  let seed = 0x9E3779B9;
  const rnd = () => { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed / 4294967296 - 0.5; };
  const sin = (f, t) => Math.sin(2 * Math.PI * f * t);
  const m = {
    // Broadband, with a transient: a tone pair, noise, and a click at 1 s.
    input: stereo((i, t) => {
      const tone = 0.25 * sin(220, t) + 0.12 * sin(3130, t), noise = 0.05 * rnd();
      const click = i >= rate && i < rate + 4 ? 0.6 : 0;
      return [tone + noise + click, 0.8 * tone - noise + click];
    }),
    // One tone per band of the splitter (crossovers at 200 Hz and 3 kHz).
    tones: stereo((i, t) => { const v = 0.2 * (sin(80, t) + sin(1000, t) + sin(8000, t)); return [v, v]; }),
    // A steady tone for the ducker to duck...
    steady: stereo((i, t) => [0.3 * sin(440, t), 0.3 * sin(440, t)]),
    // ...and the key that ducks it: 100 ms bursts every 500 ms from 0.25 s.
    keys: stereo((i, t) => {
      const ph = (t - 0.25) % 0.5;
      const v = t >= 0.25 && ph < 0.1 ? 0.8 * sin(1000, t) : 0;
      return [v, v];
    }),
    silence: new Float32Array(n * 2),
  };
  const files = {};
  for (const [name, data] of Object.entries(m)) {
    files[name] = join(mediaDir, `${name}_${rate}.wav`);
    writeWav(files[name], data, 2, rate);
    files[name + "Data"] = data;
  }
  media[rate] = files;
}
// The sampler's file: a 1 kHz sine, one second and a half, mono. Loaded at
// root note 60, note 69 plays it 9 semitones up: 1681.8 Hz -- where the
// built-in demo sample would play 440 Hz.
const sampleWav = join(mediaDir, "sample_1k.wav");
{
  const sr = 48000, n = Math.round(sr * 1.5), d = new Float32Array(n);
  for (let i = 0; i < n; i++) d[i] = 0.5 * Math.sin(2 * Math.PI * 1000 * i / sr);
  writeWav(sampleWav, d, 1, sr);
}

// The notes an instrument plays (seconds, key, velocity): overlapping, so
// voices steal and release under each other, at times inside blocks rather
// than on their edges, velocities across the range -- and then one note
// alone, A4, long enough to measure its pitch. Every time is a multiple of
// 0.05 s: a whole number of frames at 44.1, 48 and 96 kHz.
const NOTES = [
  [0.00, 0.35, 60, 100], [0.15, 0.50, 64, 90], [0.25, 0.60, 67, 80],
  [0.50, 1.25, 72, 127], [0.60, 1.00, 48, 1], [1.00, 1.50, 48, 64],
  [1.25, 1.85, 55, 40], [1.50, 1.75, 79, 110], [1.75, 1.85, 79, 70],
  [2.00, 2.90, 69, 100],
];
const CHORD = [[0.0, 2.0, 60, 100], [0.0, 2.0, 64, 100], [0.0, 2.0, 67, 100]];
function notesFile(rate) {
  const f = join(mediaDir, `notes_${rate}.txt`);
  writeFileSync(f, NOTES.map(([on, off, k, v]) => `${Math.round(on * rate)} ${Math.round(off * rate)} ${k} ${v}`).join("\n") + "\n");
  return f;
}

// ── The sampler's state, with a file in its StateBag ────────────────────────
// The SNRS blob every format stores (clap_wrapper.h saveStateBody): header,
// the parameters as float32, bypass byte, preset index, editor size, and the
// DSP's StateBag -- here samplePath and rootNote, as the sampler's
// saveState() writes them. What a session holds after the user picked a file.
function samplerState() {
  const d = DESC.sampler;
  const parts = [];
  const u32 = (v) => { const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0); return b; };
  const i32 = (v) => { const b = Buffer.alloc(4); b.writeInt32LE(v); return b; };
  // Only the DSP's own controls: the plugin's bypass is the byte after them.
  const own = d.params.filter((p) => p.name !== "Bypass");
  parts.push(Buffer.from("SNRS", "ascii"), u32(5), u32(own.length));
  for (const p of own) { const b = Buffer.alloc(4); b.writeFloatLE(p.def); parts.push(b); }
  parts.push(Buffer.from([0]), i32(0), i32(0), i32(0));
  const entries = [["samplePath", sampleWav], ["rootNote", "60"]];
  parts.push(u32(entries.length));
  for (const [k, v] of entries) {
    const kb = Buffer.from(k, "utf8"), vb = Buffer.from(v, "utf8");
    parts.push(u32(kb.length), kb, u32(vb.length), vb);
  }
  return Buffer.concat(parts);
}

// ── Running one REAPER session ──────────────────────────────────────────────
function mergeIni(path, settings) {
  const sections = new Map(), order = [];
  let cur = null;
  if (existsSync(path)) {
    for (const line of readFileSync(path, "utf8").split(/\r?\n/)) {
      const m = line.match(/^\[(.*)\]$/);
      if (m) { cur = m[1]; if (!sections.has(cur)) { sections.set(cur, new Map()); order.push(cur); } continue; }
      const at = line.indexOf("=");
      if (cur && at > 0) sections.get(cur).set(line.slice(0, at), line.slice(at + 1));
    }
  }
  for (const [sec, key, val] of settings) {
    if (!sections.has(sec)) { sections.set(sec, new Map()); order.push(sec); }
    sections.get(sec).set(key, val);
  }
  const out = [];
  for (const s of order) { out.push(`[${s}]`); for (const [k, v] of sections.get(s)) out.push(`${k}=${v}`); out.push(""); }
  writeFileSync(path, out.join("\r\n"), "utf8");
}
function writeIni(rate) {
  // Rewritten before EVERY launch: REAPER writes its ini back on exit.
  mergeIni(ini, [
    ["REAPER", "vstpath64", stageVst3],
    ["REAPER", "vstpath", stageVst3],
    ["REAPER", "clap_path_win64", stageClap],
    ["REAPER", "lv2path_win64", stageLv2],
    ["REAPER", "vstfullstate", "49989"],
    // No "new version available" window, and no request to reaper.fm.
    ["REAPER", "verchk", "0"],
    ["audioconfig", "mode", "4"],
    ["audioconfig", "dummy_srate", String(rate)],
    ["audioconfig", "dummy_blocksize", "512"],
  ]);
}
function iniValue(section, key) {
  if (!existsSync(ini)) return null;
  let cur = null;
  for (const line of readFileSync(ini, "utf8").split(/\r?\n/)) {
    const m = line.match(/^\[(.*)\]$/);
    if (m) { cur = m[1]; continue; }
    if (cur === section && line.startsWith(key + "=")) return line.slice(key.length + 1);
  }
  return null;
}

function toLua(v) {
  if (v === null || v === undefined) return "nil";
  if (typeof v === "boolean") return v ? "true" : "false";
  if (typeof v === "number") return Number.isFinite(v) ? String(v) : "0";
  if (typeof v === "string")
    return '"' + v.replace(/\\/g, "\\\\").replace(/"/g, '\\"').replace(/\n/g, "\\n").replace(/\r/g, "\\r") + '"';
  if (Array.isArray(v)) return "{" + v.map(toLua).join(",") + "}";
  return "{" + Object.entries(v).filter(([, x]) => x !== undefined)
    .map(([k, x]) => `[${toLua(k)}]=${toLua(x)}`).join(",") + "}";
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function runSession(name, cfg, rate, timeoutSec) {
  const dir = join(sessionsDir, name);
  mkdirSync(join(dir, "renders"), { recursive: true });
  const full = { ...cfg, session: name, srate: rate, length: LENGTH, renderDir: join(dir, "renders"),
                 out: join(dir, "results.json"), done: join(dir, "done.txt"),
                 dumpChunks: argv.includes("--dump-chunks") };
  writeFileSync(join(dir, "session.lua"), "return " + toLua(full) + "\n", "utf8");
  const launcher = join(dir, "launch.lua");
  writeFileSync(launcher, `CFG = dofile(${toLua(join(dir, "session.lua"))})\n` +
                          `dofile(${toLua(join(root, "scripts", "reaper-host-test.lua"))})\n`, "utf8");
  writeIni(rate);
  const dialogsLog = join(dir, "dialogs.log");
  rmSync(dialogsLog, { force: true });
  const watcher = spawn("powershell.exe", ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                           join(root, "scripts", "reaper-host-dialogs.ps1"),
                                           String(timeoutSec + 90), ini, dialogsLog],
                        { windowsHide: true, stdio: "ignore" });
  // REAPER starts once the watcher is looking: it creates its log when it is.
  // Started together, a busy machine had REAPER through the first editor's
  // three openings before the watcher had compiled its helper.
  const watchFrom = Date.now();
  while (!existsSync(dialogsLog) && Date.now() - watchFrom < 60000) await sleep(100);
  if (!existsSync(dialogsLog)) log(`  session ${name}: the dialog watcher was not ready after 60 s; starting REAPER anyway`);
  // CLAP_PATH is one of REAPER's default CLAP folders; ours is explicit, and
  // this keeps a CLAP_PATH in the caller's environment from widening it.
  const env = isolatedEnv(process.env);
  for (const k of Object.keys(env)) if (k.toUpperCase() === "CLAP_PATH") delete env[k];
  const t0 = Date.now();
  const child = spawn(reaperExe, ["-cfgfile", ini, "-newinst", "-nosplash", "-new", launcher],
                      { stdio: "ignore", env });
  let exited = false, exitCode = null;
  child.on("exit", (c) => { exited = true; exitCode = c; });
  child.on("error", () => { exited = true; });
  while (!exited && Date.now() - t0 < timeoutSec * 1000) await sleep(250);
  const timedOut = !exited;
  if (timedOut) spawnSync("taskkill", ["/PID", String(child.pid), "/T", "/F"], { stdio: "ignore" });
  stopOurReaper(ini);
  await sleep(600);
  try { watcher.kill(); } catch {}
  let res = null;
  try { res = JSON.parse(readFileSync(full.out, "utf8")); } catch {}
  const dialogs = existsSync(dialogsLog) ? readFileSync(dialogsLog, "utf8").replace(/^\uFEFF/, "").split(/\r?\n/).filter(Boolean) : [];
  const seconds = (Date.now() - t0) / 1000;
  log(`  session ${name}: ${seconds.toFixed(1)} s, REAPER ${timedOut ? "TIMED OUT and was stopped" : "exited " + exitCode}` +
      `, reached stage "${res ? res.stage : "none"}"`);
  return { name, dir, res, dialogs, timedOut, exitCode, renders: full.renderDir, seconds };
}

// Windows a session may legitimately show. Anything else -- an error box, a
// crash report, a question -- is a finding and goes in the report.
const benignWindow = (line) => {
  const [kind, cls, title] = line.split("\t");
  if (kind !== "window") return true;
  // RPTopmostButton is the pin on a floating FX window's title bar.
  if (cls === "REAPERwnd" || cls === "REAPERsplash" || cls === "RPTopmostButton") return true;
  // The evaluation notice ("About REAPER ... REAPER IS NOT FREE") does not
  // block anything; the rest are REAPER's progress windows and the question
  // the dialog watcher answers at the end of a session.
  if (/^(Scanning|Building Peaks|Rendering to file|REAPER Query|About REAPER)/.test(title || "")) return true;
  if (/^(CLAP|VST3|LV2)i?: /.test(title || "")) return true; // an FX's floating editor
  if (cls === "#32768") return true; // a popup menu, logged and closed
  // A tooltip and the drop shadow Windows gives it: a pointer that happens
  // to rest over REAPER's window while the session runs -- the machine's
  // user at work, not something waiting for an answer.
  if (cls === "tooltips_class32" || cls === "SysShadow") return true;
  return false;
};
function sessionSanity(s) {
  const bad = s.dialogs.filter((l) => !benignWindow(l));
  const vstpath = iniValue("REAPER", "vstpath64");
  check("reaper", s.name, "session", !s.timedOut && s.res && s.res.stage === "done",
        `REAPER ran the session to the end (stage "${s.res ? s.res.stage : "none"}", ${s.seconds.toFixed(1)} s)`);
  check("reaper", s.name, "session", !s.res || s.res.errors.length === 0,
        s.res && s.res.errors.length ? "script errors: " + s.res.errors.join("; ") : "no ReaScript errors");
  check("reaper", s.name, "session", bad.length === 0,
        bad.length ? "unexpected windows: " + bad.map((l) => l.split("\t").slice(1).join(" ")).join(" || ")
                   : "no unexpected dialogs");
  check("reaper", s.name, "isolation", vstpath === stageVst3,
        `vstpath64 still names only the staging folder (${vstpath === stageVst3 ? "yes" : vstpath})`);
  // The LV2 list too: "Add system LV2 paths" puts %APPDATA%\LV2 and
  // %COMMONPROGRAMFILES%\LV2 in it, and REAPER rescans whatever it names.
  const lv2path = iniValue("REAPER", "lv2path_win64");
  check("reaper", s.name, "isolation", lv2path === stageLv2,
        `lv2path_win64 still names only the staging folder (${lv2path === stageLv2 ? "yes" : lv2path})`);
}

// ── Reference renders by our own host ───────────────────────────────────────
function reference(key, inputWav, outWav, extra = []) {
  const r = spawnSync(renderTool, [join(built, PLUGINS[key].bundle + ".clap"), inputWav, outWav, "--block", "512", ...extra],
                      { encoding: "utf8" });
  const m = /latency (\d+)/.exec(r.stdout || "");
  return { wav: readWav(outWav), latency: m ? +m[1] : 0, out: (r.stdout || "") + (r.stderr || "") };
}

// ── Signal measurements ─────────────────────────────────────────────────────
function compare(a, b, { channels = 2, aChannels = a.channels, bChannels = b.channels, bOffset = 0, from = 0, to } = {}) {
  const n = Math.min(a.frames, b.frames - bOffset, to ?? Infinity);
  let max = 0, at = -1, energy = 0;
  for (let i = from; i < n; i++)
    for (let c = 0; c < channels; c++) {
      const x = a.samples[i * aChannels + c], y = b.samples[(i + bOffset) * bChannels + c];
      const d = Math.abs(x - y);
      energy += y * y;
      if (!(d <= max)) { max = d; at = i; }
    }
  return { max, at, rms: Math.sqrt(energy / Math.max(1, (n - from) * channels)), frames: n };
}
function rms(x, from, len) {
  let e = 0;
  for (let i = from; i < from + len && i < x.length; i++) e += x[i] * x[i];
  return Math.sqrt(e / Math.max(1, len));
}
function goertzel(x, sr, f, from, len) {
  const w = 2 * Math.PI * f / sr, k = 2 * Math.cos(w);
  let s1 = 0, s2 = 0;
  for (let i = from; i < from + len && i < x.length; i++) { const s0 = x[i] + k * s1 - s2; s2 = s1; s1 = s0; }
  return 2 * Math.sqrt(Math.max(0, s1 * s1 + s2 * s2 - k * s1 * s2)) / len;
}
// YIN: the period whose cumulative-mean-normalised difference first dips
// under a threshold, refined by a parabola. Good to a fraction of a cent on
// the clean tones measured here.
function pitch(x, sr, from, len, fmin = 70, fmax = 4000) {
  const tauMax = Math.floor(sr / fmin), tauMin = Math.max(2, Math.floor(sr / fmax));
  const N = len - tauMax;
  if (N < 64 || from + len > x.length) return NaN;
  const d = new Float64Array(tauMax + 2);
  for (let tau = 1; tau <= tauMax + 1; tau++) {
    let s = 0;
    for (let j = 0; j < N; j++) { const v = x[from + j] - x[from + j + tau]; s += v * v; }
    d[tau] = s;
  }
  const cm = new Float64Array(tauMax + 2);
  let run = 0;
  cm[0] = 1;
  for (let tau = 1; tau <= tauMax + 1; tau++) { run += d[tau]; cm[tau] = run > 0 ? d[tau] * tau / run : 1; }
  let best = -1;
  for (let tau = tauMin; tau <= tauMax; tau++) {
    if (cm[tau] < 0.15) {
      while (tau + 1 <= tauMax && cm[tau + 1] < cm[tau]) tau++;
      best = tau;
      break;
    }
  }
  if (best < 0) return NaN;
  const a = cm[best - 1], b = cm[best], c = cm[best + 1];
  const shift = (a - 2 * b + c) !== 0 ? 0.5 * (a - c) / (a - 2 * b + c) : 0;
  return sr / (best + shift);
}
const midiHz = (k) => 440 * Math.pow(2, (k - 69) / 12);
const cents = (f, ref) => 1200 * Math.log2(f / ref);

// ── Track plans ─────────────────────────────────────────────────────────────
// One REAPER track per (plugin, format, what is being looked at), each named
// for the stem REAPER renders from it. `full` adds the 48 kHz-only tracks:
// moved parameters, automation, sidechain, the sampler's file, the arp, and
// the live latency switch.
function renderTracks(rate, full) {
  const out = [], list = [], meta = {};
  const add = (spec, m) => { out.push(spec); if (m.render !== false) list.push(spec.name); meta[spec.name] = m; };
  const M = media[rate];
  for (const f of FORMATS) {
    const ok = (k) => have[f].has(k) && wanted.includes(k);
    if (ok("saturator")) add({ name: `sat_${f}`, media: M.input, fx: [{ query: query("saturator", f) }] },
                             { key: "saturator", f, kind: "effect", input: "input" });
    if (ok("showcase")) add({ name: `show_${f}`, media: M.input, fx: [{ query: query("showcase", f) }] },
                            { key: "showcase", f, kind: "effect", input: "input" });
    if (ok("saturator") && full)
      add({ name: `satp_${f}`, media: M.input, fx: [{ query: query("saturator", f), set: [
             [0, norm("saturator", 0, 8)], [1, norm("saturator", 1, 3000)], [2, norm("saturator", 2, 50)],
             [3, norm("saturator", 3, -3)]] }] },
          { key: "saturator", f, kind: "effect", input: "input", params: ["0=8", "1=3000", "2=50", "3=-3"], step: "params" });
    if (ok("trim")) add({ name: `trim_${f}`, media: M.input, fx: [{ query: query("trim", f) }] },
                        { key: "trim", f, kind: "effect", input: "input" });
    // The automation lane: 0 dB, -20 dB from 1 s, 0 dB from 2 s, on a plain
    // gain. The trim's Gain where the format has a trim; the probe's Gain
    // (x1 -> x0.1, delayed by the latency it reports, which REAPER
    // compensates) where it does not -- the LV2 build has no trim.
    if (ok("trim") && full)
      add({ name: `trimauto_${f}`, media: M.input, fx: [{ query: query("trim", f), envelope: { param: 0, points: [
             [0, norm("trim", 0, 0)], [1.0, norm("trim", 0, -20)], [2.0, norm("trim", 0, 0)]] } }] },
          { key: "trim", f, kind: "automation" });
    else if (!have[f].has("trim") && ok("guiprobe") && full)
      add({ name: `probeauto_${f}`, media: M.input, fx: [{ query: query("guiprobe", f), envelope: { param: 0, points: [
             [0, norm("guiprobe", 0, 1)], [1.0, norm("guiprobe", 0, 0.1)], [2.0, norm("guiprobe", 0, 1)]] } }] },
          { key: "guiprobe", f, kind: "automation" });
    if (ok("ducker")) add({ name: `ducker_${f}`, media: M.input, fx: [{ query: query("ducker", f) }] },
                          { key: "ducker", f, kind: "effect", input: "input" });
    if (ok("ducker") && full) {
      add({ name: `duckkey_${f}`, channels: 4, media: M.steady, fx: [{ query: query("ducker", f) }] },
          { key: "ducker", f, kind: "sidechain" });
      add({ name: `key_${f}`, media: M.keys, mainSend: false, sendTo: { track: `duckkey_${f}`, dstChan: 2 } },
          { render: false });
    }
    if (ok("splitter")) add({ name: `split_${f}`, channels: 6, media: M.tones, fx: [{ query: query("splitter", f) }] },
                            { key: "splitter", f, kind: "splitter", input: "tones" });
    if (ok("synth")) add({ name: `synth_${f}`, midi: NOTES, fx: [{ query: query("synth", f) }] },
                         { key: "synth", f, kind: "instrument", expectHz: midiHz(69) });
    if (ok("sampler")) add({ name: `sampler_${f}`, midi: NOTES, fx: [{ query: query("sampler", f) }] },
                           { key: "sampler", f, kind: "instrument", expectHz: midiHz(69) });
    if (ok("sampler") && full)
      add({ name: `samplerfile_${f}`, midi: NOTES, fx: [{ query: query("sampler", f), state: samplerStateB64 }] },
          { key: "sampler", f, kind: "instrument", state: true, expectHz: 1000 * Math.pow(2, 9 / 12), step: "state" });
    if (ok("arp") && have[f].has("synth") && full)
      add({ name: `arp_${f}`, midi: CHORD, fx: [{ query: query("arp", f) },
             { query: query("synth", f), set: [[2, 0], [3, 0]] }] },
          { key: "arp", f, kind: "arp" });
    if (ok("reverb")) add({ name: `reverb_${f}`, media: M.input, fx: [{ query: query("reverb", f) }] },
                          { key: "reverb", f, kind: "effect", input: "input" });
    if (ok("guiprobe")) add({ name: `probe_${f}`, media: M.input, fx: [{ query: query("guiprobe", f) }] },
                            { key: "guiprobe", f, kind: "probe" });
    if (ok("guiprobe") && full)
      add({ name: `probebig_${f}`, media: M.input, fx: [{ query: query("guiprobe", f), toggle: [2, 1] }] },
          { key: "guiprobe", f, kind: "probe", big: true, step: "latency" });
  }
  return { tracks: out, list, meta };
}

// ── The sessions ────────────────────────────────────────────────────────────
for (const r of [48000, ...rates]) makeMedia(r);
const samplerStateBlob = have.clap.has("sampler") ? samplerState() : null;
const samplerStateFile = join(mediaDir, "sampler_state.bin");
if (samplerStateBlob) writeFileSync(samplerStateFile, samplerStateBlob);
const samplerStateB64 = samplerStateBlob ? samplerStateBlob.toString("base64") : undefined;

const main = renderTracks(48000, true);
// State tracks: every own control of every plugin moved through
// TrackFX_SetParam to a position no default sits on -- a switch to its other
// end, a knob somewhere off-centre -- then saved, and reopened in session 2.
//
// Except a CLAP plugin's Bypass. REAPER treats a CLAP_PARAM_IS_BYPASS
// parameter as ITS bypass button: it drives it from the FX's own bypass
// state, and a value written to it through the API is overwritten a moment
// later (measured: on at 1 s, off again at 2.5 s, never on if written before
// the plugin first ran). The VST3 build's Bypass is an ordinary parameter to
// REAPER and is moved like the rest.
const reaperOwns = (key, f, i) => f === "clap" && DESC[key].params[i].name === "Bypass";
for (const f of FORMATS)
  for (const key of wanted) {
    if (!have[f].has(key)) continue;
    // LV2's controls are the plugin's own plus lv2:enabled (moved to 0, as
    // the VST3 build's Bypass is moved to on); never REAPER's freewheel.
    const controls = f === "lv2" ? lv2Params(key) : DESC[key].params;
    const set = controls.map((p) => {
      const i = p.i;
      const def = (p.def - p.min) / (p.max - p.min);
      if (p.stepped) return [i, def < 0.5 ? 1 : 0];
      let n = 0.13 + 0.71 * ((i * 0.37) % 1);
      if (Math.abs(n - def) < 0.05) n = n > 0.5 ? n - 0.2 : n + 0.2;
      return [i, n];
    }).filter(([i]) => !reaperOwns(key, f, i));
    main.tracks.push({ name: `state_${key}_${f}`, fx: [{ query: query(key, f), set }] });
    main.meta[`state_${key}_${f}`] = { key, f, kind: "state", render: false };
    if (DESC[key].presets.length > 0) {
      main.tracks.push({ name: `preset_${key}_${f}`, fx: [{ query: query(key, f), lastPreset: true }] });
      main.meta[`preset_${key}_${f}`] = { key, f, kind: "preset", render: false };
    }
  }

// What every render track's reference is, and how its render is judged.
async function judgeRenders(session, rate, meta, step, refCache) {
  const notes = notesFile(rate);
  for (const [name, m] of Object.entries(meta)) {
    if (m.render === false || !m.kind) continue;
    const label = m.step || step;
    const wav = readWav(join(session.renders, name + ".wav"));
    if (!wav) { check(m.key, FMT[m.f], label, false, `${name}: REAPER wrote no render`); continue; }
    const frames = rate * LENGTH;
    if (Math.abs(wav.frames - frames) > rate / 100) {
      check(m.key, FMT[m.f], label, false, `${name}: render is ${wav.frames} frames, wanted ${frames}`);
      continue;
    }
    const refKey = JSON.stringify([m.key, m.input, m.params, m.state, m.kind, rate]);
    const makeRef = () => {
      if (refCache.has(refKey)) return refCache.get(refKey);
      const out = join(session.dir, `ref_${name}.wav`);
      let r;
      if (m.kind === "instrument")
        r = reference(m.key, media[rate].silence, out, ["--notes", notes, ...(m.state ? ["--state", samplerStateFile] : [])]);
      else r = reference(m.key, media[rate][m.input || "input"], out, (m.params || []).flatMap((p) => ["--param", p]));
      refCache.set(refKey, r);
      return r;
    };
    const tol = m.params ? 2e-5 : 1e-5;
    if (m.kind === "effect" || m.kind === "instrument" || m.kind === "splitter") {
      const ref = makeRef();
      if (!ref.wav) { check(m.key, FMT[m.f], label, false, `${name}: our host could not render it: ${ref.out.trim()}`); continue; }
      // REAPER compensates the plugin's latency; render_tool does not. So
      // REAPER's frame i is our frame i + latency.
      const c = compare(wav, ref.wav, { bOffset: ref.latency, to: frames - ref.latency - 1 });
      const what = m.params ? `with ${m.params.join(", ")}` : m.state ? "with the file from its StateBag" : "at defaults";
      check(m.key, FMT[m.f], label, c.rms > 1e-3 && c.max <= tol,
            `${name} ${what} @${rate}: equals our host's render to ${c.max.toExponential(2)} ` +
            `(tolerance ${tol}, reference rms ${c.rms.toFixed(4)}, latency ${ref.latency})`);
      if (m.kind === "instrument") {
        const mono = channel(wav, 0);
        const f0 = pitch(mono, rate, Math.round(2.3 * rate), Math.round(0.25 * rate));
        check(m.key, FMT[m.f], label, Math.abs(cents(f0, m.expectHz)) < 5,
              `${name}: the lone A4 sounds at ${fmtNum(f0, 1)} Hz, expected ${fmtNum(m.expectHz, 1)} (${fmtNum(cents(f0, m.expectHz), 2)} cents)`);
      }
      if (m.kind === "splitter") {
        const mid = Math.round(0.5 * rate), len = 2 * rate;
        const bands = [0, 1, 2].map((b) => [80, 1000, 8000].map((hz) => goertzel(channel(wav, b * 2), rate, hz, mid, len)));
        const okBands = wav.channels === 6 && bands.every((amps, b) => amps.every((a, j) => j === b || amps[b] > 10 * a));
        check(m.key, FMT[m.f], label, okBands,
              `${name}: ${wav.channels} channels; 80 Hz / 1 kHz / 8 kHz by output pair: ` +
              bands.map((a) => a.map((x) => x.toFixed(3)).join("/")).join("  ") + " (each band on its own bus)");
      }
    } else if (m.kind === "automation") {
      const x = channel(wav, 0), dry = channel({ samples: media[rate].inputData, channels: 2, frames }, 0);
      const ratio = (t0, t1) => rms(x, Math.round(t0 * rate), Math.round((t1 - t0) * rate)) /
                                rms(dry, Math.round(t0 * rate), Math.round((t1 - t0) * rate));
      const seg = [ratio(0.1, 0.9), ratio(1.1, 1.9), ratio(2.1, 2.9)];
      // Where the gain actually dropped: the first frame after 0.9 s at which
      // output/input falls under -10 dB in a 2 ms window.
      let edge = -1;
      for (let i = Math.round(0.9 * rate); i < Math.round(1.5 * rate); i += Math.round(rate / 1000)) {
        if (rms(x, i, Math.round(rate / 500)) < 0.316 * rms(dry, i, Math.round(rate / 500))) { edge = i / rate; break; }
      }
      const db = seg.map((g) => 20 * Math.log10(g));
      check(m.key, FMT[m.f], "params", Math.abs(db[0]) < 0.05 && Math.abs(db[1] + 20) < 0.05 && Math.abs(db[2]) < 0.05 &&
            edge > 0.99 && edge < 1.02,
            `${name}: envelope 0 dB -> -20 dB at 1 s -> 0 dB at 2 s; render measured ${db.map((v) => v.toFixed(2)).join(" / ")} dB, ` +
            `the drop lands at ${fmtNum(edge, 4)} s`);
    } else if (m.kind === "sidechain") {
      const x = channel(wav, 0), dry = media[rate].steadyData;
      const dryL = new Float32Array(frames);
      for (let i = 0; i < frames; i++) dryL[i] = dry[i * 2];
      const r = (t0, len) => rms(x, Math.round(t0 * rate), Math.round(len * rate)) / rms(dryL, Math.round(t0 * rate), Math.round(len * rate));
      const ducked = [0.27, 0.77, 1.27, 1.77].map((t) => r(t, 0.06));
      const open = [0.68, 1.18, 1.68, 2.18].map((t) => r(t, 0.06));
      check(m.key, FMT[m.f], "render", ducked.every((g) => g < 0.1) && open.every((g) => g > 0.8),
            `${name}: the key on the sidechain pins (3/4) ducks the main signal -- gain under the key ` +
            `${ducked.map((g) => g.toFixed(3)).join("/")}, between keys ${open.map((g) => g.toFixed(3)).join("/")}`);
    } else if (m.kind === "arp") {
      const x = channel(wav, 0);
      const seq = [60, 64, 67];
      const got = [];
      let good = 0;
      for (let k = 0; k < 15; k++) {
        const f0 = pitch(x, rate, Math.round((k * 0.125 + 0.012) * rate), Math.round(0.045 * rate), 150, 1000);
        const want = midiHz(seq[k % 3]);
        got.push(Number.isFinite(f0) ? Math.round(f0) : "?");
        if (Math.abs(cents(f0, want)) < 15) good++;
      }
      check(m.key, FMT[m.f], "render", good === 15,
            `${name}: the arp's MIDI out plays the synth after it -- steps at 8 Hz measured ${got.join(" ")} Hz ` +
            `(want C4 E4 G4 repeating; ${good}/15 right)`);
    } else if (m.kind === "probe") {
      const L = channel(wav, 0), R = channel(wav, 1);
      const dryL = new Float32Array(frames);
      for (let i = 0; i < frames; i++) dryL[i] = media[rate].inputData[i * 2];
      // Gain 1 and a delay of exactly the latency it reports: once REAPER
      // compensates, the left channel IS the input.
      let max = 0, at = -1;
      for (let i = 0; i < frames - 600; i++) { const d = Math.abs(L[i] - dryL[i]); if (d > max) { max = d; at = i; } }
      check(m.key, FMT[m.f], "latency", max < 1e-6,
            `${name}${m.big ? " (Big Latency switched to Long)" : ""}: compensated output lines up with the dry input ` +
            `(max |diff| ${max.toExponential(2)} at frame ${at})`);
      if (!m.big) {
        // Right channel: tempo*0.001 + beat*0.01 + playing*0.5 + offline*0.125.
        const samples = [0.5, 1.0, 2.0].map((t) => {
          const i = Math.round(t * rate);
          return { t, v: R[i], rest: R[i] - 0.12 - 0.5 - 0.01 * 2 * t };
        });
        const tempoPlaying = samples.every((s) => Math.abs(s.rest) < 0.003 || Math.abs(s.rest - 0.125) < 0.003);
        const offline = samples.every((s) => Math.abs(s.rest - 0.125) < 0.003);
        check(m.key, FMT[m.f], "render", tempoPlaying,
              `${name}: the transport reached the plugin -- 120 BPM, playing, beat = 2 x seconds ` +
              `(readout ${samples.map((s) => s.v.toFixed(4)).join(" / ")}); offline render flag ${offline ? "set" : "NOT set"}`);
      }
    }
  }
}

// LV2's half of the parameter step: every control at its port index, under
// the plugin's name, holding the plugin's default as REAPER shows it; then
// moved, and kept.
function lv2ParamChecks(key, fx) {
  const list = lv2Params(key);
  const wrong = [];
  for (const p of list) {
    const got = fx.params.find((x) => x.i === p.i);
    if (!got) wrong.push(`${p.name}: no port ${p.i}`);
    else if (got.name !== p.name) wrong.push(`port ${p.i}: REAPER calls it "${got.name}", the plugin "${p.name}"`);
    else if (!lv2Shows(p, got, p.def))
      wrong.push(`${p.name}: REAPER shows "${got.text}" (${got.value}), the plugin's default is ${p.def} ("${p.text}")`);
  }
  const named = list.filter((p) => p.stepped && isName(p.text) && !p.enabled).map((p) => `${p.name}="${p.text}"`);
  check(key, "LV2", "params", wrong.length === 0,
        wrong.length ? "defaults differ: " + wrong.join("; ")
                     : `all ${list.length} controls at their ports with the plugin's defaults` +
                       (named.length ? `, named steps by the plugin's own label (${named.join(", ")})` : "") +
                       (DESC[key].instrument ? "" : ", lv2:enabled On"));
  const before = new Map(fx.params.map((x) => [x.i, x.norm]));
  const own = fx.after.filter((x) => list.some((p) => p.i === x.i && p.name === x.name));
  const unmoved = own.filter((x) => Math.abs(x.norm - before.get(x.i)) < 1e-6);
  check(key, "LV2", "params", own.length === list.length && unmoved.length === 0,
        `TrackFX_SetParam moved ${own.length - unmoved.length} of ${list.length} controls` +
        (unmoved.length ? ` (did not move: ${unmoved.map((x) => x.name).join(", ")})` : "") +
        `; read back e.g. ${own.slice(0, 3).map((x) => `${x.name}=${x.text}`).join(", ")}`);
}

// A factory preset picked from REAPER's own menu. The list must be the
// plugin's presets, and the pick must leave every control where our own host
// says that preset puts it -- by the plugin's text in CLAP and VST3, by value
// (or a named step's label) in LV2, which has no call for the text. Then the
// reopened session must still hold the preset's values.
function presetChecks(s1, s2) {
  const byName = (res, name) => res && res.tracks.find((t) => t.name === name);
  for (const key of wanted) {
    const d = DESC[key];
    if (!d.presets.length) continue;
    for (const f of FORMATS) {
      if (!have[f].has(key)) continue;
      const tr = byName(s1.res, `preset_${key}_${f}`);
      const fx = tr && tr.fx[0];
      if (!fx || !(fx.added >= 0)) continue;
      if (f === "clap" && fx.presetCount === 0) {
        // REAPER reads neither CLAP's preset-discovery factory nor any other
        // preset list from a CLAP plugin; clap-validator checks that one.
        log(`  note [${key}/CLAP/params] REAPER lists no presets for a CLAP plugin (${d.presets.length} offered through preset-discovery)`);
        continue;
      }
      check(key, FMT[f], "params", fx.presetCount === d.presets.length,
            `REAPER's preset menu holds ${fx.presetCount} presets, the plugin ships ${d.presets.length}`);
      const preset = d.presets.find((p) => p.name === fx.presetPicked);
      check(key, FMT[f], "params", fx.presetSet === true && !!preset,
            `TrackFX_SetPresetByIndex picked "${fx.presetPicked}"` + (preset ? "" : ", which is none of the plugin's"));
      if (!preset) continue;
      const wrong = [];
      const controls = f === "lv2" ? lv2Params(key).filter((p) => !p.enabled) : d.params;
      for (const p of controls) {
        if (p.name === "Bypass") continue;
        const want = preset.values[p.i];
        const got = fx.after.find((x) => x.i === p.i && x.name === p.name);
        if (!want || !got) { wrong.push(`${p.name}: not read`); continue; }
        const ok = f === "lv2" ? lv2Shows({ ...p, text: want.text }, got, want.value) : got.text === want.text;
        if (!ok) wrong.push(`${p.name}: REAPER shows "${got.text}", the preset sets "${want.text}"`);
      }
      check(key, FMT[f], "params", wrong.length === 0,
            wrong.length ? `"${preset.name}" left controls elsewhere: ` + wrong.join("; ")
                         : `"${preset.name}" set every control to the preset's value (e.g. ${controls.filter((p) => p.name !== "Bypass").slice(0, 3)
                             .map((p) => `${p.name}=${preset.values[p.i] ? preset.values[p.i].text : "?"}`).join(", ")})`);
      const again = byName(s2 && s2.res, `preset_${key}_${f}`);
      const fx2 = again && again.fx[0];
      if (!fx2 || !fx2.after) continue;
      const moved = fx.after.filter((x) => {
        const y = fx2.after.find((z) => z.i === x.i);
        return controls.some((p) => p.i === x.i && p.name !== "Bypass") && (!y || Math.abs(y.norm - x.norm) > 1e-6);
      });
      check(key, FMT[f], "state", moved.length === 0,
            moved.length ? `after reopening, the preset's values moved: ${moved.map((x) => x.name).join(", ")}`
                         : `the preset's values are all there after the session is reopened`);
    }
  }
}

function paramChecks(s1, s2) {
  const byName = (res, name) => res && res.tracks.find((t) => t.name === name);
  for (const key of wanted) {
    const d = DESC[key];
    const read = {};
    for (const f of FORMATS) {
      if (!have[f].has(key)) continue;
      const tr = byName(s1.res, `state_${key}_${f}`);
      const fx = tr && tr.fx[0];
      if (!fx || !(fx.added >= 0)) continue;
      read[f] = fx;
      if (f === "lv2") { lv2ParamChecks(key, fx); continue; }
      // Defaults as REAPER shows them, against the plugin's own text for its
      // declared default (render_tool asks the CLAP build directly).
      const mismatched = [];
      for (const p of d.params) {
        const got = fx.params.find((x) => x.name === p.name);
        if (!got) mismatched.push(`${p.name}: missing`);
        else if (got.text !== p.text) mismatched.push(`${p.name}: REAPER shows "${got.text}", the plugin says "${p.text}"`);
      }
      check(key, FMT[f], "params", mismatched.length === 0,
            mismatched.length ? "default texts differ: " + mismatched.join("; ")
                              : `all ${d.params.length} parameters show the plugin's own text at their defaults`);
      // Moved: every own control took a new value and kept it. (By index as
      // well as name: REAPER's own "Bypass" follows the plugin's.)
      const own = fx.after.filter((x) => x.i < d.params.length && d.params.some((p) => p.name === x.name) &&
                                         !reaperOwns(key, f, x.i));
      const expected = d.params.filter((p, i) => !reaperOwns(key, f, i)).length;
      const before = new Map(fx.params.map((x) => [x.i, x.norm]));
      const unmoved = own.filter((x) => Math.abs(x.norm - before.get(x.i)) < 1e-6);
      check(key, FMT[f], "params", own.length === expected && unmoved.length === 0,
            `TrackFX_SetParam moved ${own.length - unmoved.length} of ${expected} controls` +
            (unmoved.length ? ` (did not move: ${unmoved.map((x) => x.name).join(", ")})` : "") +
            `; read back e.g. ${own.slice(0, 3).map((x) => `${x.name}=${x.text}`).join(", ")}`);
    }
    // The two formats of one source must agree on what a value is called.
    if (read.clap && read.vst3) {
      const diffs = [];
      for (const c of read.clap.after) {
        if (c.i >= d.params.length) continue;
        const v = read.vst3.after.find((x) => x.name === c.name && x.i < d.params.length);
        const cDef = read.clap.params.find((x) => x.i === c.i), vDef = v && read.vst3.params.find((x) => x.i === v.i);
        if (!v || !cDef || !vDef) continue;
        // Only where both formats map value to knob position the same way
        // (a skewed control is linear in CLAP's plain range, curved in VST3's).
        if (Math.abs(cDef.norm - vDef.norm) > 1e-6) continue;
        if (Math.abs(c.norm - v.norm) < 1e-6 && c.text !== v.text) diffs.push(`${c.name}: CLAP "${c.text}" / VST3 "${v.text}"`);
      }
      check(key, "both", "params", diffs.length === 0,
            diffs.length ? "the formats disagree: " + diffs.join("; ") : "CLAP and VST3 show the same text for every moved control");
    }
    // LV2 against CLAP: both map a knob position linearly onto the plain
    // range, so the same position must be the same value -- the TTL's ranges
    // are the descriptor's -- and a named step the same label.
    if (read.clap && read.lv2) {
      const diffs = [];
      for (const p of lv2Params(key)) {
        if (p.enabled) continue;
        const c = read.clap.after.find((x) => x.i === p.i && x.name === p.name);
        const l = read.lv2.after.find((x) => x.i === p.i && x.name === p.name);
        if (!c || !l) diffs.push(`${p.name}: ${c ? "" : "no CLAP value "}${l ? "" : "no LV2 value"}`);
        else if (Math.abs(c.value - l.value) > 1e-5 * Math.max(1, Math.abs(c.value)))
          diffs.push(`${p.name}: CLAP ${c.value} ("${c.text}") / LV2 ${l.value} ("${l.text}")`);
        else if (p.stepped && isName(p.text) && c.text !== l.text) diffs.push(`${p.name}: CLAP "${c.text}" / LV2 "${l.text}"`);
      }
      check(key, "both", "params", diffs.length === 0,
            diffs.length ? "CLAP and LV2 disagree: " + diffs.join("; ")
                         : "CLAP and LV2 hold the same plain value for every moved control (and a named step the same label)");
    }
    // State: the reopened session's values against the saved ones.
    for (const f of FORMATS) {
      if (!read[f] || !s2) continue;
      const tr = byName(s2.res, `state_${key}_${f}`);
      const fx = tr && tr.fx[0];
      if (!fx) { check(key, FMT[f], "state", false, "the reopened project has no such plugin"); continue; }
      check(key, FMT[f], "state", fx.name === read[f].name, `reopened as "${fx.name}"`);
      const lost = [];
      for (const a of read[f].after) {
        const b = fx.after.find((x) => x.i === a.i);
        if (!b || Math.abs(b.norm - a.norm) > 1e-6 || b.text !== a.text)
          lost.push(`${a.name} saved ${a.text} (${a.norm.toFixed(6)}), reopened ${b ? `${b.text} (${b.norm.toFixed(6)})` : "missing"}`);
      }
      check(key, FMT[f], "state", lost.length === 0,
            lost.length ? "values changed across save/reopen: " + lost.join("; ")
                        : `all ${read[f].after.length} values came back in a new REAPER`);
    }
  }
}

function scanChecks(s1) {
  const installed = (s1.res && s1.res.installed) || [];
  for (const key of wanted) {
    const d = DESC[key], p = PLUGINS[key];
    for (const f of FORMATS) {
      if (!have[f].has(key)) continue;
      // REAPER appends "(2->6ch)" to a plugin whose layout is not 2-in
      // 2-out, and "(2 out)" to an LV2 instrument.
      const bare = (n) => n.replace(/ \((\d+->\d+ch|\d+ out)\)/, "");
      // Every format names its vendor. REAPER appends an LV2 plugin's only from
      // a doap:maintainer on the plugin itself, which is where the SDK states
      // it -- one nested under lv2:project was listed with no vendor at all.
      const wants = [`${query(key, f)} (${d.vendor})`];
      const found = installed.find((x) => wants.includes(bare(x.name)));
      check(key, FMT[f], "scan", !!found,
            `REAPER lists "${found ? found.name : wants[0]}"${found ? ` (${found.ident.split(/[\\/]/).pop()})` : ""}`);
      const tr = s1.res && s1.res.tracks.find((t) => t.name === `state_${key}_${f}`);
      const fx = tr && tr.fx[0];
      const name = fx && fx.name ? bare(fx.name) : "";
      check(key, FMT[f], "scan", fx && fx.added >= 0 && wants.includes(name) && fx.type === `${FMT[f]}${d.instrument ? "i" : ""}`,
            `TrackFX_AddByName("${query(key, f)}") -> ${fx ? fx.added : "?"}, named "${fx ? fx.name : ""}", type ${fx ? fx.type : "?"}`);
      check(key, FMT[f], "scan", fx && fx.ins === p.ins && fx.outs === p.outs,
            `pins ${fx ? fx.ins : "?"} in / ${fx ? fx.outs : "?"} out, expected ${p.ins} / ${p.outs}`);
    }
  }
}

function latencyChecks(s1) {
  for (const key of wanted) {
    if (!["saturator", "reverb", "guiprobe", "trim"].includes(key)) continue;
    const ref = spawnSync(renderTool, [join(built, PLUGINS[key].bundle + ".clap"), media[48000].input,
                                       join(s1.dir, `lat_${key}.wav`)], { encoding: "utf8" });
    const m = /latency (\d+)/.exec(ref.stdout || "");
    const ours = m ? +m[1] : NaN;
    const track = { saturator: "sat", reverb: "reverb", guiprobe: "probe", trim: "trim" }[key];
    for (const f of FORMATS) {
      const t = s1.res && s1.res.tracks.find((x) => x.name === `${track}_${f}`);
      const fx = t && t.fx[0];
      if (!fx) continue;
      if (f === "lv2") {
        // lv2:latency is an OUTPUT port, and only run() writes it: REAPER
        // knows the number once it has run the plugin, so it is read after
        // the session settled, not at insert (where it may still be 0).
        check(key, FMT[f], "latency", fx.pdcAfter === ours,
              `REAPER's PDC for it is ${fx.pdcAfter} samples once it has run (${fx.pdc} at insert); the plugin reports ${ours} to our host`);
        continue;
      }
      check(key, FMT[f], "latency", fx.pdc === ours, `REAPER's PDC for it is ${fx.pdc} samples; the plugin reports ${ours} to our host`);
    }
  }
  for (const f of FORMATS) {
    const tr = s1.res && s1.res.tracks.find((t) => t.name === `probebig_${f}`);
    const fx = tr && tr.fx[0];
    if (!fx) continue;
    const final = fx.pdcStopped === 512 ? 512 : fx.pdcPlaying;
    const when = fx.pdcStopped === 512 ? `while stopped, within ${fmtNum(fx.secondsStopped, 2)} s`
                                       : `only once playing (${fmtNum(fx.secondsPlaying, 2)} s into playback; stopped for ` +
                                         `${fmtNum(fx.secondsStopped, 1)} s it still said ${fx.pdcStopped})`;
    check("guiprobe", FMT[f], "latency", fx.pdcBeforeToggle === 64 && final === 512 && fx.toggleText === "Long",
          `Big Latency switched live (${fx.toggleText}): REAPER's PDC went ${fx.pdcBeforeToggle} -> ${final}, ${when}`);
  }
}

function samplerStateChecks(s1, s2) {
  for (const f of FORMATS) {
    if (!have[f].has("sampler") || !wanted.includes("sampler")) continue;
    const t1 = s1.res && s1.res.tracks.find((t) => t.name === `samplerfile_${f}`);
    check("sampler", FMT[f], "state", t1 && t1.fx[0] && t1.fx[0].stateInjected === true,
          `REAPER handed the plugin a state naming ${sampleWav.split(/[\\/]/).pop()} (${t1 && t1.errors.length ? t1.errors.join("; ") : "accepted"})`);
    if (!s2 || !s2.res) continue;
    const t2 = s2.res.tracks.find((t) => t.name === `samplerfile_${f}`);
    const blob = f === "clap" ? t2 && t2.clapState : f === "lv2" ? t2 && t2.lv2State : t2 && t2.fx[0] && t2.fx[0].stateBase64;
    const text = blob ? Buffer.from(blob, "base64").toString("latin1") : "";
    const where = { clap: "CLAP STATE block", vst3: "VST3 chunk", lv2: "LV2 state property urn:sonorie:state:blob" }[f];
    check("sampler", FMT[f], "state", text.includes("samplePath") && text.includes(sampleWav),
          `the state REAPER saved and reopened still names the file (${text.includes(sampleWav) ? "found" : "missing"} in the ${where})`);
  }
}

function reopenRenderChecks(s1, s2, meta) {
  for (const [name, m] of Object.entries(meta)) {
    if (m.render === false || !m.kind) continue;
    const a = readWav(join(s1.renders, name + ".wav")), b = readWav(join(s2.renders, name + ".wav"));
    if (!a || !b) { check(m.key, FMT[m.f], "state", false, `${name}: missing render (${a ? "" : "first "}${b ? "" : "reopened"})`); continue; }
    const c = compare(b, a, { channels: Math.min(a.channels, b.channels) });
    check(m.key, FMT[m.f], "state", c.max <= 1e-6,
          `${name}: renders the same after save and reopen in a new REAPER (max |diff| ${c.max.toExponential(2)})`);
  }
}

function editorChecks(s5) {
  const recs = (s5.res && s5.res.editor) || [];
  // The size each editor came up at, as the dialog watcher saw REAPER's
  // floating window: the view REAPER wraps the plugin in, sized to what the
  // plugin asked for (the last size logged, should it have changed).
  const sizes = {};
  for (const line of s5.dialogs) {
    const [kind, title, inside] = line.split("\t");
    if (kind !== "editor") continue;
    const track = /"(ed_\w+?_(?:clap|vst3|lv2))"/.exec(title || "");
    const view = /(?:^| )1:reaperPluginHostWrapProc:(\d+x\d+)/.exec(inside || "");
    if (track && view) sizes[track[1]] = view[1];
  }
  for (const key of ["guiprobe", "ducker", "showcase"]) {
    for (const f of FORMATS) {
      const cycles = recs.filter((r) => r.track === `ed_${key}_${f}`);
      if (!have[f].has(key) || !wanted.includes(key)) continue;
      const opened = cycles.filter((r) => r.opened).length, closed = cycles.filter((r) => r.closed).length;
      check(key, FMT[f], "editor", cycles.length === 3 && opened === 3 && closed === 3,
            `editor opened ${opened}/3 and closed ${closed}/3 times in a floating window` +
            (cycles.length ? ` (first open after ${fmtNum(cycles[0].openSeconds, 2)} s)` : ""));
      // One plugin, one size: each format's editor as big as the CLAP
      // build's, which every other check here takes as the reference.
      if (f !== "clap" && have.clap.has(key)) {
        const mine = sizes[`ed_${key}_${f}`], ref = sizes[`ed_${key}_clap`];
        check(key, FMT[f], "editor", !!mine && mine === ref,
              `the editor came up at ${mine || "an unmeasured size"} in REAPER's window, the CLAP build's at ${ref || "?"}`);
      }
      if (key === "guiprobe") {
        const first = cycles[0];
        // The page sets Gain when the plugin's state reaches it: to 1.25 when
        // the state names the host, to 0.75 when it does not -- LV2 gives a
        // plugin no host name. Until the LV2 interface spoke in the atoms
        // hosts carry, its state never arrived and nothing here noticed.
        const what = f === "lv2" ? "Probe to 0.777 through its control port and Gain to 0.75 once the plugin's state arrived as atoms"
                                 : "Probe to 0.777 and Gain to 1.25 through the bridge";
        check(key, FMT[f], "editor", first && first.bridge === true,
              first && first.bridge ? `its page drove ${what} in ${fmtNum(first.bridgeSeconds, 2)} s`
                                    : `its page did not reach the host (values ${first && first.bridgeValues ? first.bridgeValues.join(", ") : "?"})`);
      }
    }
  }
}

// ── Run ─────────────────────────────────────────────────────────────────────
log("\n── session 1: scan, parameters, automation, latency, renders at 48 kHz, save ──");
const s1 = await runSession("s1-main-48k", { tracks: main.tracks, render: main.list, saveAs: join(sessionsDir, "saved.rpp"),
                                             settle: 1.5 }, 48000, 240);
sessionSanity(s1);
const refCache = new Map();
if (s1.res) {
  scanChecks(s1);
  await judgeRenders(s1, 48000, main.meta, "render", refCache);
  latencyChecks(s1);
}

log("\n── session 2: the saved project in a NEW REAPER ──");
let s2 = null;
if (existsSync(join(sessionsDir, "saved.rpp"))) {
  s2 = await runSession("s2-reopen-48k", { tracks: [], open: join(sessionsDir, "saved.rpp"), render: main.list, settle: 1.5 }, 48000, 180);
  sessionSanity(s2);
} else {
  check("reaper", "s1-main-48k", "session", false, "session 1 saved no project");
}
if (s1.res) paramChecks(s1, s2);
if (s1.res) presetChecks(s1, s2);
if (s1.res) samplerStateChecks(s1, s2);
if (s2 && s2.res) reopenRenderChecks(s1, s2, main.meta);

for (const rate of rates) {
  log(`\n── session: renders at ${rate / 1000} kHz ──`);
  const plan = renderTracks(rate, false);
  const s = await runSession(`s-rate-${rate}`, { tracks: plan.tracks, render: plan.list, settle: 1.0 }, rate, 180);
  sessionSanity(s);
  if (s.res) await judgeRenders(s, rate, plan.meta, "rates", new Map());
}

if (!argv.includes("--skip-editor")) {
  log("\n── session: editors (windows will open and close) ──");
  const ed = [];
  for (const key of ["guiprobe", "ducker", "showcase"])
    for (const f of FORMATS)
      if (have[f].has(key) && wanted.includes(key)) ed.push({ name: `ed_${key}_${f}`, fx: [{ query: query(key, f) }] });
  const editor = ed.map((t) => ({ track: t.name, cycles: 3,
                                  watchParam: !t.name.startsWith("ed_guiprobe") ? undefined
                                            : t.name.endsWith("_lv2") ? [1, 0.777, 0, 0.375] : [1, 0.777, 0, 0.625] }));
  const s5 = await runSession("s-editor", { tracks: ed, render: [], editor, settle: 0.5 }, 48000, 180);
  sessionSanity(s5);
  editorChecks(s5);
}

// ── Report ──────────────────────────────────────────────────────────────────
const failures = results.filter((r) => !r.ok);
log("\n── summary: plugin x format x step ──");
const steps = ["scan", "render", "params", "state", "latency", "editor", "rates"];
const cell = (c) => (c.length === 0 ? "-" : c.every((r) => r.ok) ? `ok ${c.length}` : `FAIL ${c.filter((r) => !r.ok).length}`).padEnd(9);
log("  " + "plugin/format".padEnd(22) + steps.map((s) => s.padEnd(9)).join(""));
for (const key of wanted)
  for (const f of [...FORMATS.map((x) => FMT[x]), "both"]) {
    const mine = results.filter((r) => r.plugin === key && r.format === f);
    if (mine.length) log("  " + `${key}/${f}`.padEnd(22) + steps.map((s) => cell(mine.filter((r) => r.step === s))).join(""));
  }
log("  " + "REAPER session".padEnd(22) + "session  isolation");
for (const name of [...new Set(results.filter((r) => r.plugin === "reaper").map((r) => r.format))]) {
  const mine = results.filter((r) => r.plugin === "reaper" && r.format === name);
  log("  " + name.padEnd(22) + cell(mine.filter((r) => r.step === "session")) + cell(mine.filter((r) => r.step === "isolation")));
}
log(`\n${results.length} checks, ${failures.length} failure(s)`);
for (const f of failures) log(`  FAIL [${f.plugin}/${f.format}/${f.step}] ${f.what}`);
log(`work folder: ${work} (REAPER's profile, the sessions' scripts, results, renders and dialog logs)`);
log(failures.length === 0 ? "SONORE REAPER HOST TEST PASSED" : "SONORE REAPER HOST TEST FAILED");
process.exit(failures.length === 0 ? 0 : 1);
