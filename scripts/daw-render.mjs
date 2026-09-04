// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the same audio through the same plugin in a real DAW and in
// our own host, compared sample for sample.
//
// verify:daw proves REAPER will LOAD our plugins. This proves it hears the
// same thing we do: a file is rendered through each effect example by REAPER
// (a project built by a ReaScript, rendered from the command line, in an
// isolated configuration) and by tests/render_tool.cpp over sonore/host.h,
// and the two renders must agree to the precision of the DAW's output format
// once the plugin's reported latency is accounted for -- REAPER compensates
// it, our host does not. What this catches that nothing else can: a sound
// that depends on the host's block size, a reported latency that is not the
// real one, a parameter default the DAW reads differently from the way the
// wrapper applies it, and any disagreement between the CLAP and VST3 builds
// of one source file, because the DAW renders the VST3 and the reference
// render goes through the CLAP.
//
//   npm run verify:daw-render          (needs REAPER and a Release build)
//
// Isolated the same way daw-probe is: -cfgfile with its own ini, its own
// plugin path, nothing shared with the user's REAPER. The ReaScript reaches
// REAPER as Scripts/__startup.lua under that resource path -- the one script
// REAPER runs by itself on launch -- because a script named on the command
// line was, in practice, not run.
import { spawn, spawnSync } from "node:child_process";
import { cpSync, existsSync, mkdirSync, readdirSync, readFileSync, rmSync, statSync,
         writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const built = join(root, "sdk", "build", "Release");
const log = (s) => process.stdout.write(s + "\n");
let checks = 0, failures = 0;
const check = (ok, what) => { checks++; log(`  ${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) failures++; };

const reaper = "C:\\Program Files\\REAPER (x64)\\reaper.exe";
if (process.platform !== "win32" || !existsSync(reaper)) {
  log("SKIPPED: REAPER is not installed at " + reaper);
  process.exit(0);
}
const renderTool = join(built, "render_tool.exe");
if (!existsSync(built) || !existsSync(renderTool)) {
  log("SKIPPED: no Release build with render_tool. Run: npm run verify:sdk");
  process.exit(0);
}

// ── A scratch folder, and the plugins staged into it ────────────────────────
const work = join(root, "sdk", "build", "daw-render");
rmSync(work, { recursive: true, force: true });
const stage = join(work, "plugins");
mkdirSync(stage, { recursive: true });
let staged = 0;
for (const name of readdirSync(built)) {
  if (name.endsWith(".vst3.bundle")) {
    cpSync(join(built, name), join(stage, name.replace(/\.bundle$/, "")), { recursive: true });
    ++staged;
  }
}
log(`staged ${staged} VST3 bundle(s) into ${stage}`);

// ── The input: deterministic, broadband, with a transient, two seconds ──────
const sr = 48000, seconds = 2, frames = sr * seconds;
const input = new Float32Array(frames * 2);
let seed = 0x9E3779B9;
const rnd = () => { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed / 4294967296 - 0.5; };
for (let i = 0; i < frames; i++) {
  const t = i / sr;
  const tone = 0.25 * Math.sin(2 * Math.PI * 220 * t) + 0.12 * Math.sin(2 * Math.PI * 3130 * t);
  const noise = 0.05 * rnd();
  const click = i >= sr && i < sr + 4 ? 0.6 : 0;
  input[i * 2] = tone + noise + click;
  input[i * 2 + 1] = 0.8 * tone - noise + click;
}
function writeWavFloat(path, data, channels, rate) {
  const bytes = Buffer.alloc(44 + data.length * 4);
  bytes.write("RIFF", 0); bytes.writeUInt32LE(36 + data.length * 4, 4); bytes.write("WAVE", 8);
  bytes.write("fmt ", 12); bytes.writeUInt32LE(16, 16); bytes.writeUInt16LE(3, 20);
  bytes.writeUInt16LE(channels, 22); bytes.writeUInt32LE(rate, 24);
  bytes.writeUInt32LE(rate * channels * 4, 28); bytes.writeUInt16LE(channels * 4, 32);
  bytes.writeUInt16LE(32, 34); bytes.write("data", 36); bytes.writeUInt32LE(data.length * 4, 40);
  for (let i = 0; i < data.length; i++) bytes.writeFloatLE(data[i], 44 + i * 4);
  writeFileSync(path, bytes);
}
const inputWav = join(work, "input.wav");
writeWavFloat(inputWav, input, 2, sr);

/** A WAV reader for what REAPER writes: PCM 16/24/32 or float 32, any chunk order. */
function readWav(path) {
  const b = readFileSync(path);
  if (b.toString("ascii", 0, 4) !== "RIFF" || b.toString("ascii", 8, 12) !== "WAVE") return null;
  let at = 12, fmt = null, data = null;
  while (at + 8 <= b.length) {
    const id = b.toString("ascii", at, at + 4), size = b.readUInt32LE(at + 4);
    if (id === "fmt ") fmt = { tag: b.readUInt16LE(at + 8), channels: b.readUInt16LE(at + 10),
                               rate: b.readUInt32LE(at + 12), bits: b.readUInt16LE(at + 22) };
    if (id === "data") data = b.subarray(at + 8, at + 8 + size);
    at += 8 + size + (size & 1);
  }
  if (!fmt || !data) return null;
  if (fmt.tag === 0xFFFE) fmt.tag = fmt.bits === 32 && data.length % 4 === 0 ? 3 : 1; // extensible: guess by width
  const n = Math.floor(data.length / (fmt.bits / 8));
  const out = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    if (fmt.tag === 3 && fmt.bits === 32) out[i] = data.readFloatLE(i * 4);
    else if (fmt.bits === 16) out[i] = data.readInt16LE(i * 2) / 32768;
    else if (fmt.bits === 24) out[i] = ((data[i * 3] | (data[i * 3 + 1] << 8) | (data[i * 3 + 2] << 16)) << 8 >> 8) / 8388608;
    else if (fmt.bits === 32) out[i] = data.readInt32LE(i * 4) / 2147483648;
  }
  return { ...fmt, samples: out, frames: n / fmt.channels };
}

// ── What to compare: the effect examples, by the name REAPER shows ──────────
const plugins = [
  ["SonoreSaturator", "Sonore Saturator"],
  ["SonoreTrim", "Sonore Trim"],
  ["SonoreSplitter", "Sonore Splitter"],
  ["SonoreReverb", "Sonore Reverb"],
  ["SonoreDucker", "Sonore Ducker"],
];

// ── One ReaScript builds the project and starts the render ──────────────────
const lua = (bundle, fxName, outName) => `
local proj = 0
local function say(s) reaper.ShowConsoleMsg(s .. "\\n") end
reaper.GetSetProjectInfo(proj, "PROJECT_SRATE", ${sr}, true)
reaper.GetSetProjectInfo(proj, "PROJECT_SRATE_USE", 1, true)
reaper.InsertTrackAtIndex(0, false)
local tr = reaper.GetTrack(proj, 0)
reaper.SetOnlyTrackSelected(tr)
reaper.SetEditCurPos(0, false, false)
reaper.InsertMedia(${JSON.stringify(inputWav)}, 0)
local item = reaper.GetTrackMediaItem(tr, 0)
if item then
  reaper.SetMediaItemInfo_Value(item, "D_POSITION", 0)
  reaper.SetMediaItemInfo_Value(item, "D_FADEINLEN", 0)
  reaper.SetMediaItemInfo_Value(item, "D_FADEOUTLEN", 0)
  reaper.SetMediaItemInfo_Value(item, "D_VOL", 1)
end
local fx = reaper.TrackFX_AddByName(tr, "VST3: ${fxName}", false, -1)
if fx < 0 then fx = reaper.TrackFX_AddByName(tr, "${fxName}", false, -1) end
local marker = io.open(${JSON.stringify(join(work, outName + ".fx.txt"))}, "w")
if marker then marker:write(tostring(fx)); marker:close() end
reaper.GetSetProjectInfo_String(proj, "RENDER_FILE", ${JSON.stringify(work)}, true)
reaper.GetSetProjectInfo_String(proj, "RENDER_PATTERN", "${outName}", true)
reaper.GetSetProjectInfo(proj, "RENDER_SETTINGS", 0, true)
reaper.GetSetProjectInfo(proj, "RENDER_BOUNDSFLAG", 1, true)
reaper.GetSetProjectInfo(proj, "RENDER_SRATE", ${sr}, true)
reaper.GetSetProjectInfo(proj, "RENDER_CHANNELS", 2, true)
reaper.GetSetProjectInfo(proj, "RENDER_DITHER", 0, true)
reaper.GetSetProjectInfo(proj, "RENDER_TAILFLAG", 0, true)
reaper.GetSetProjectInfo(proj, "RENDER_ADDTOPROJ", 0, true)
reaper.Main_SaveProjectEx(proj, ${JSON.stringify(join(work, outName + ".rpp"))}, 0)
reaper.Main_OnCommand(42230, 0)
-- No quit here: the render runs on after this command returns, and a quit
-- from the script cut the first file off at 512 KiB. The driver outside ends
-- REAPER once the output has stopped growing.
`;

const ini = join(work, "reaper.ini");
writeFileSync(ini, [
  "[REAPER]", "vstpath64=" + stage, "vstpath=" + stage, "splashmode=1", "tips=0", "showtips=0",
  "reascan=1", "renderclosewhendone=1", "",
].join("\r\n"), "utf8");

const sleep = (ms) => spawnSync("cmd", ["/c", "timeout", "/t", String(Math.ceil(ms / 1000)), "/nobreak", ">nul"], { stdio: "ignore" });
const sizeOf = (f) => (existsSync(f) ? statSync(f).size : -1);
/** True once the WAV's data chunk carries a real size: REAPER writes that last. */
function wavFinished(path) {
  if (!existsSync(path)) return false;
  const b = readFileSync(path);
  if (b.length < 44) return false;
  let at = 12;
  while (at + 8 <= b.length) {
    const id = b.toString("ascii", at, at + 4), size = b.readUInt32LE(at + 4);
    if (id === "data") return size > 0 && size < 0xF0000000 && at + 8 + size <= b.length;
    at += 8 + size + (size & 1);
  }
  return false;
}

function renderInReaper(bundle, fxName, outName) {
  const script = join(work, outName + ".lua");
  writeFileSync(script, lua(bundle, fxName, outName), "utf8");
  mkdirSync(join(work, "Scripts"), { recursive: true });
  writeFileSync(join(work, "Scripts", "__startup.lua"), lua(bundle, fxName, outName), "utf8");
  const outWav = join(work, outName + ".wav");
  rmSync(outWav, { force: true });
  const child = spawn(reaper, ["-cfgfile", ini, "-nosplash", "-nonewinst"], { detached: true, stdio: "ignore" });
  // A fresh profile asks about the audio device before it runs anything, and
  // an unlicensed one shows the evaluation notice: both modal, both in front
  // of the startup script. scripts/daw-dismiss.ps1 answers them for as long
  // as REAPER is being driven.
  // "powershell.exe", not "powershell": without a shell, spawn does not
  // consult PATHEXT, and the first version of this started nothing and said
  // nothing -- every render stalled behind the question and was cut off.
  const dismiss = spawn("powershell.exe", ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                                           join(root, "scripts", "daw-dismiss.ps1"), "200"],
                        { windowsHide: true, stdio: ["ignore", "pipe", "ignore"] });
  let dismissed = "";
  dismiss.on("error", (e) => { dismissed += "dismisser failed to start: " + e.message; });
  dismiss.stdout.on("data", (d) => { dismissed += d.toString(); });
  const deadline = Date.now() + 180_000;
  while (Date.now() < deadline) {
    sleep(2000);
    if (wavFinished(outWav)) break;
    try { process.kill(child.pid, 0); } catch { break; } // REAPER is gone
  }
  sleep(1000);
  spawnSync("taskkill", ["/PID", String(child.pid), "/T", "/F"], { stdio: "ignore" });
  try { process.kill(dismiss.pid); } catch {}
  if (dismissed.trim()) log("    " + dismissed.trim().split(/\r?\n/).join(", "));
  return existsSync(outWav) ? outWav : null;
}

function renderHere(pluginFile, outName) {
  const out = join(work, outName + ".ref.wav");
  const r = spawnSync(renderTool, [pluginFile, inputWav, out, "--block", "512"], { encoding: "utf8" });
  const m = /latency (\d+)/.exec(r.stdout || "");
  return { path: existsSync(out) ? out : null, latency: m ? parseInt(m[1], 10) : 0, stdout: r.stdout, stderr: r.stderr };
}

log("\n── REAPER against our own host, per effect ──────────────────────────────");
for (const [bundle, fxName] of plugins) {
  const clap = join(built, bundle + ".clap");
  if (!existsSync(clap)) { log(`  ${bundle}: no .clap built, skipped`); continue; }
  log(`\n${fxName}`);
  const ref = renderHere(clap, bundle);
  check(ref.path !== null, `our host rendered it (latency ${ref.latency})`);
  if (!ref.path) { log("    " + (ref.stdout || ref.stderr || "").trim()); continue; }
  const daw = renderInReaper(bundle, fxName, bundle);
  const fxMarker = join(work, bundle + ".fx.txt");
  const fxIndex = existsSync(fxMarker) ? parseInt(readFileSync(fxMarker, "utf8"), 10) : -2;
  check(fxIndex >= 0, `REAPER inserted the plugin (TrackFX_AddByName -> ${fxIndex})`);
  check(daw !== null, "REAPER rendered the project");
  if (!daw || fxIndex < 0) continue;
  const a = readWav(daw), b = readWav(ref.path);
  check(a !== null && b !== null, `both renders read back (REAPER wrote ${a ? a.bits + "-bit, tag " + a.tag : "?"})`);
  if (!a || !b) continue;
  check(Math.abs(a.frames - frames) <= sr / 100,
        `REAPER's render is the whole file (${a.frames} of ${frames} frames)`);
  // REAPER compensates the plugin's latency; our host does not. So REAPER's
  // sample i is our sample i + latency.
  const lat = ref.latency;
  const n = Math.min(a.frames, b.frames - lat) - 16;
  let maxDiff = 0, at = -1, energy = 0;
  for (let i = 0; i < n; i++) {
    for (let c = 0; c < 2; c++) {
      const x = a.samples[i * 2 + c], y = b.samples[(i + lat) * 2 + c];
      const d = Math.abs(x - y);
      energy += y * y;
      if (d > maxDiff) { maxDiff = d; at = i; }
    }
  }
  const quant = a.tag === 3 ? 1e-6 : a.bits === 24 ? 1.5e-7 : a.bits === 16 ? 4e-5 : 1e-6;
  log(`    compared ${n} frames: max |diff| ${maxDiff.toExponential(2)} at frame ${at}, ` +
      `reference rms ${Math.sqrt(energy / Math.max(1, n * 2)).toFixed(4)}`);
  check(energy > 1e-3, "the reference render carries signal");
  check(maxDiff <= Math.max(quant * 4, 2e-6), `REAPER's render matches ours to the output format's precision (${quant.toExponential(0)} per sample)`);
}
spawnSync("taskkill", ["/IM", "reaper.exe", "/F"], { stdio: "ignore" });
log(`\n${checks} checks, ${failures} failure(s)`);
log(`scratch: ${work} (input, both renders, the .rpp and the .lua per plugin)`);
if (failures === 0) log("SONORE DAW RENDER PARITY PASSED");
process.exit(failures === 0 ? 0 : 1);
