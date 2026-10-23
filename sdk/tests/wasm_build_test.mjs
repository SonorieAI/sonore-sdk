// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: proves the SAME DSP source that ships inside the native CLAP
// also compiles to WebAssembly and runs, which is the property that makes the
// browser preview bit-identical to the product.
//
// It compiles the example plugins' DSP through emscripten against the SDK's
// wasm ABI, instantiates the module in Node, and drives it through the exact C
// ABI the AudioWorklet host speaks (sonore_in_ptr/params/process/out_ptr).
//
//   node sdk/tests/wasm_build_test.mjs
//
// Needs emscripten on PATH (C:\dev\emsdk on the dev box).
import { execFileSync } from "node:child_process";
import { mkdirSync, readFileSync, writeFileSync, rmSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const sdk = resolve(here, "..");
const out = join(sdk, "build", "wasm");

let failures = 0;
let checks = 0;
const check = (ok, what) => {
  checks++;
  if (ok) console.log(`  ok   ${what}`);
  else { failures++; console.log(`  FAIL ${what}`); }
};
const near = (got, want, tol, what) => {
  checks++;
  if (Math.abs(got - want) <= tol) console.log(`  ok   ${what} (${got.toFixed(4)})`);
  else { failures++; console.log(`  FAIL ${what}: got ${got.toFixed(4)}, want ${want} +/- ${tol}`); }
};

console.log("Sonore SDK wasm build test\n");

mkdirSync(out, { recursive: true });

/** Build one plugin's DSP as a standalone wasm module through the SDK ABI. */
function buildWasm(name, sourcePath, extraFlags = []) {
  // The generated pipeline appends the ABI glue after the model's DSP; do the
  // same here so this test exercises the real composition, not a special case.
  const source = readFileSync(sourcePath, "utf8");
  // Drop the native-only tail (descriptor + clap wrapper); wasm needs the DSP
  // and the param count only: exactly what /api/compile sends.
  // An example that wraps its native tail in #if !defined(SONORE_EXAMPLE_DSP_ONLY)
  // (the reverb, the sampler) is cut at that #if: cut at the include inside
  // it, the tail took the #endif with it and left the #if open, which em++
  // refused as an unterminated conditional.
  const dspOnly = source
    .replace(/#if !defined\(SONORE_EXAMPLE_DSP_ONLY\)[\s\S]*$/, "")
    .replace(/#include <sonore\/plugin\.h>[\s\S]*$/, "")
    .replace(/#include <sonore\/formats\.h>[\s\S]*$/, "")
    .replace(/#include <sonore\/clap_wrapper\.h>[\s\S]*$/, "");
  const glued = `${dspOnly}\n#include <sonore/wasm_abi.h>\n`;
  const cpp = join(out, `${name}.cpp`);
  writeFileSync(cpp, glued);

  const wasm = join(out, `${name}.wasm`);
  const exports = [
    "_sonore_init", "_sonore_process", "_sonore_in_ptr", "_sonore_out_ptr",
    "_sonore_params_ptr", "_sonore_num_params", "_sonore_max_frames",
    "_sonore_channels", "_sonore_midi_ptr", "_sonore_midi_count", "_sonore_wants_midi",
    // HOW MUCH THIS DSP DELAYS ITS OUTPUT. The native wrappers have reported it
    // since Limiter and Convolver landed and the browser ABI had nowhere to put
    // it, so a host compensating a look-ahead limiter had to be told the number
    // out of band -- which cannot be right at two sample rates at once.
    "_sonore_latency",
    // How many blocks the output guard has played as silence: the module has
    // no log of its own, so the host reads this and says so.
    "_sonore_output_guard_trips",
    // How far the DSP is turning the signal down, for a compressor's meter.
    // Nothing measured from the output can tell compression from a gain knob,
    // so the DSP reports it, and a DSP that does not reads 0.
    "_sonore_gain_reduction",
  ];
  // The pooled entry points, only when the build asked for slots. Exporting
  // them unconditionally would fail to link every ordinary plugin.
  if (extraFlags.some((f) => String(f).includes("SONORE_MAX_INSTANCES")))
    exports.push("_sonore_latency_at", "_sonore_output_guard_trips_at", "_sonore_gain_reduction_at",
                 "_sonore_new", "_sonore_delete", "_sonore_max_instances",
                 "_sonore_init_at", "_sonore_process_at", "_sonore_in_ptr_at",
                 "_sonore_out_ptr_at", "_sonore_params_ptr_at",
                 "_sonore_midi_ptr_at", "_sonore_midi_count_at");
  execFileSync(
    "em++",
    [
      cpp, "-o", wasm,
      "-O3", "-std=c++17", "--no-entry",
      "-sSTANDALONE_WASM=1", "-sERROR_ON_UNDEFINED_SYMBOLS=1",
      "-sEXPORTED_FUNCTIONS=" + exports.join(","),
      `-I${join(sdk, "include")}`,
      "-fno-exceptions", "-fno-rtti",
      ...extraFlags,
    ],
    { stdio: "pipe", shell: true },
  );
  return wasm;
}

/** Instantiate a standalone wasm the way the worklet host does. */
async function instantiate(path) {
  const bytes = readFileSync(path);
  const module = await WebAssembly.compile(bytes);
  // Permissive import object: stub whatever the module asks for (the host does
  // the same, which is how a WASI clock import never breaks a build).
  const imports = {};
  for (const imp of WebAssembly.Module.imports(module)) {
    imports[imp.module] ??= {};
    imports[imp.module][imp.name] = imp.kind === "function" ? () => 0 : 0;
  }
  const instance = await WebAssembly.instantiate(module, imports);
  const ex = instance.exports;
  ex._initialize?.(); // reactor model: runs global constructors
  return ex;
}

// Every example, because every plugin shape the native wrappers accept must
// also run in the browser: the simple effect, the instrument, and the rich
// ProcessContext ones (sidechain, aux outputs, MIDI out, flexible width, a
// state bag) -- those last take the ABI's third dispatch branch, which two
// plugins never exercised. "note" is a note effect: it wants MIDI and stays
// silent.
// "context" is an effect on the rich signature: without a descriptor the glue
// cannot tell it from a context instrument, so it advertises MIDI (feeding an
// unwanted note to an effect is harmless; hiding a synth's keyboard is not),
// unless the compiler that appended the glue says SONORE_WANTS_MIDI=0 -- the
// studio knows from its spec, and that override is checked below too.
for (const [name, rel, kind] of [
  ["saturator", "examples/saturator/plugin.cpp", "effect"],
  ["synth", "examples/synth/plugin.cpp", "instrument"],
  ["ducker", "examples/ducker/plugin.cpp", "effect"],
  ["trim", "examples/trim/plugin.cpp", "effect"],
  ["splitter", "examples/splitter/plugin.cpp", "context"],
  ["arp", "examples/arp/plugin.cpp", "note"],
  ["sampler", "examples/sampler/plugin.cpp", "instrument"],
  ["reverb", "examples/reverb/plugin.cpp", "effect"],
  ["guiprobe", "examples/guiprobe/plugin.cpp", "effect"],
]) {
  const isInstrument = kind === "instrument";
  const isNoteEffect = kind === "note";
  const isContextEffect = kind === "context";
  console.log(`${name}`);
  let wasmPath;
  try {
    wasmPath = buildWasm(name, join(sdk, rel));
    check(true, "compiles to wasm through the SDK ABI");
  } catch (e) {
    check(false, `compiles to wasm through the SDK ABI: ${String(e.stderr || e).slice(0, 400)}`);
    continue;
  }

  const ex = await instantiate(wasmPath);
  const sr = 48000;
  ex.sonore_init(sr);

  const frames = ex.sonore_max_frames();
  const channels = ex.sonore_channels();
  const nParams = ex.sonore_num_params();
  check(frames === 128 && channels === 2, "the ABI reports the host's buffer geometry");
  check(nParams > 0, "the ABI reports the parameter count");
  // HOW MUCH IT DELAYS, which the browser ABI could not say until now: the
  // native wrappers have reported it since Limiter and Convolver landed, and
  // a host that does not know it has one track sliding against the others.
  // None of these examples delays, so the number to see here is ZERO -- which
  // is the honest default for a DSP that does not declare latencySamples(),
  // rather than a guess. A DSP that DOES declare one is proved where there is
  // a look-ahead limiter to prove it with.
  const lat = typeof ex.sonore_latency === "function" ? ex.sonore_latency() : -1;
  check(lat >= 0 && Number.isInteger(lat),
        `the ABI reports its latency (${lat} samples)`);
  // None of these compresses either, so the reading is exactly 0: the honest
  // answer for a DSP that does not declare gainReduction(). The one that does
  // is proved below, against the number its ratio predicts.
  check(typeof ex.sonore_gain_reduction === "function" && ex.sonore_gain_reduction() === 0,
        "the ABI reports no gain reduction for a DSP that declares none");
  check(
    ex.sonore_wants_midi() === (isInstrument || isNoteEffect || isContextEffect ? 1 : 0),
    isInstrument ? "the instrument advertises wantsMidi"
    : isNoteEffect ? "the note effect advertises wantsMidi"
    : isContextEffect ? "a context-form effect advertises wantsMidi (no descriptor to say otherwise)"
                      : "the effect does not consume MIDI",
  );
  if (isContextEffect) {
    const told = await instantiate(buildWasm(name + "_told", join(sdk, rel), ["-DSONORE_WANTS_MIDI=0"]));
    check(told.sonore_wants_midi() === 0, "...and SONORE_WANTS_MIDI=0 from the compiler turns it off");
  }

  const mem = new Float32Array(ex.memory.buffer);
  const inPtr = ex.sonore_in_ptr() / 4;
  const outPtr = ex.sonore_out_ptr() / 4;
  const parPtr = ex.sonore_params_ptr() / 4;
  const midiPtr = ex.sonore_midi_ptr() / 4;

  // Defaults straight off the parameter table, so the wasm run matches what a
  // freshly-loaded native instance would do.
  const source = readFileSync(join(sdk, rel), "utf8");
  const defaults = [...source.matchAll(/\{"[\w]+",\s*"[^"]*",\s*"[^"]*",\s*([-\d.ef]+),\s*([-\d.ef]+),\s*([-\d.ef]+),/g)]
    .map((m) => parseFloat(m[3]));
  for (let i = 0; i < nParams; i++) mem[parPtr + i] = defaults[i] ?? 0;

  if (isNoteEffect) {
    // Notes in, nothing out: an arpeggiator's product is MIDI, which the
    // preview has no sink for, so the proof here is that it runs and stays
    // silent rather than inventing audio.
    mem[midiPtr + 0] = 0;
    mem[midiPtr + 1] = 0x90;
    mem[midiPtr + 2] = 60;
    mem[midiPtr + 3] = 100;
    ex.sonore_midi_count(1);
    let peak = 0;
    let finite = true;
    for (let b = 0; b < 100; b++) {
      ex.sonore_process(frames);
      for (let i = 0; i < frames; i++) {
        const v = mem[outPtr + i];
        if (!Number.isFinite(v)) finite = false;
        peak = Math.max(peak, Math.abs(v));
      }
    }
    check(finite, "a note effect stays finite");
    check(peak < 1e-6, "...and silent: its product is MIDI, not audio");
  } else if (isInstrument) {
    // Silent at rest.
    let restPeak = 0;
    for (let b = 0; b < 20; b++) {
      for (let i = 0; i < frames * channels; i++) mem[inPtr + i] = 0;
      ex.sonore_process(frames);
      for (let i = 0; i < frames; i++) restPeak = Math.max(restPeak, Math.abs(mem[outPtr + i]));
    }
    check(restPeak < 1e-4, "an instrument is silent at rest in wasm too");

    // Note on through the MIDI inlet: [frameOffset, status, data1, data2].
    mem[midiPtr + 0] = 0;
    mem[midiPtr + 1] = 0x90;
    mem[midiPtr + 2] = 69; // A4
    mem[midiPtr + 3] = 100;
    ex.sonore_midi_count(1);

    let peak = 0;
    let finite = true;
    for (let b = 0; b < 200; b++) {
      ex.sonore_process(frames);
      for (let i = 0; i < frames; i++) {
        const v = mem[outPtr + i];
        if (!Number.isFinite(v)) finite = false;
        peak = Math.max(peak, Math.abs(v));
      }
    }
    check(finite, "the note stays finite");
    check(peak > 1e-3, "note-on sounds in the browser ABI");
  } else {
    // A 1 kHz tone through the effect.
    let peak = 0;
    let energy = 0;
    let finite = true;
    let phase = 0;
    for (let b = 0; b < 100; b++) {
      for (let c = 0; c < channels; c++)
        for (let i = 0; i < frames; i++)
          mem[inPtr + c * frames + i] = 0.25 * Math.sin((2 * Math.PI * 1000 * (phase + i)) / sr);
      phase += frames;
      ex.sonore_process(frames);
      for (let i = 0; i < frames; i++) {
        const v = mem[outPtr + i];
        if (!Number.isFinite(v)) finite = false;
        peak = Math.max(peak, Math.abs(v));
        energy += v * v;
      }
    }
    check(finite, "a second of audio stays finite");
    check(energy > 0, "the effect produces sound");
    check(peak < 10, "the output does not blow up");
  }
  console.log("");
}

// ── many instances in one module ───────────────────────────────────────────
//
// A browser gives a tab about 125 WebAssembly memories -- process-wide, and a
// Worker gets none of its own -- so a host that instantiates a module per
// plugin INSTANCE stops at 125 of them however little each one costs. One
// module holding N DSPs is one reservation. `SONORE_MAX_INSTANCES` is what
// makes that possible and it has to be two things: invisible when off, because
// every plugin ever built compiles against this header, and genuinely separate
// when on.
{
  const saw = buildWasm("pooled", join(sdk, "examples", "saturator", "plugin.cpp"),
                        ["-DSONORE_MAX_INSTANCES=4"]);
  const x = await instantiate(saw);

  check(x.sonore_max_instances() === 4, "the module reports the slots it was built with");
  const handles = [x.sonore_new(), x.sonore_new(), x.sonore_new(), x.sonore_new()];
  check(handles.join(",") === "0,1,2,3", "slots are handed out in order");
  check(x.sonore_new() === -1, "a slot past the last is refused, not wrapped");
  x.sonore_delete(1);
  check(x.sonore_new() === 1, "a released slot comes back");

  // Handle 0 IS the singleton, which is what makes the whole addition
  // backwards compatible: a host that never learned about handles drives it
  // through the original entry points and nothing has moved.
  check(x.sonore_in_ptr_at(0) === x.sonore_in_ptr(),
        "handle 0 is the module's original instance");
  check(x.sonore_in_ptr_at(1) !== x.sonore_in_ptr_at(0),
        "and every other slot has buffers of its own");

  // DRIVEN DIFFERENTLY, because separate storage is not the same as separate
  // behaviour: a shared coefficient cache or a shared filter state would still
  // give each slot its own buffers and the same sound.
  const stride = x.sonore_max_frames();
  const nParams = x.sonore_num_params();
  const mem = () => new Float32Array(x.memory.buffer);
  const level = [];
  for (const h of [0, 1, 2, 3]) {
    x.sonore_init_at(h, 48000);
    const pp = x.sonore_params_ptr_at(h) / 4;
    // Every parameter at its own slot's fraction of the range: whatever the
    // example's controls are, four settings that differ.
    for (let i = 0; i < nParams; ++i) mem()[pp + i] = (h + 1) / 5;
  }
  for (const h of [0, 1, 2, 3]) {
    const ip = x.sonore_in_ptr_at(h) / 4, op = x.sonore_out_ptr_at(h) / 4;
    let e = 0, t = 0, finite = true;
    for (let b = 0; b < 60; ++b) {
      const m = mem();
      for (let i = 0; i < stride; ++i, ++t) {
        const v = 0.3 * Math.sin((2 * Math.PI * 220 * t) / 48000);
        m[ip + i] = v;
        m[ip + stride + i] = v;
      }
      x.sonore_process_at(h, stride);
      const m2 = mem();
      for (let i = 0; i < stride; ++i) {
        const y = m2[op + i];
        if (!Number.isFinite(y)) finite = false;
        e += y * y;
      }
    }
    check(finite, `slot ${h} produces finite audio`);
    level.push(Math.sqrt(e / (60 * stride)));
  }
  check(new Set(level.map((v) => v.toFixed(4))).size > 1,
        `slots driven differently sound different (${level.map((v) => v.toFixed(4)).join(" ")})`);
}

// ── gain reduction through the ABI ─────────────────────────────────────────
//
// A compressor's meter can only show what the DSP reports, so the export is
// proved against a known answer: a 4:1 compressor with a -20 dB threshold and
// a hard knee, fed a level 12 dB over, reduces (1 - 1/4) * 12 = 9 dB, in
// POSITIVE decibels, as every dynamics class here reports it. Silence reads 0.
// And pooled, each slot is its own needle: one driven hot, one silent.
{
  console.log("gain reduction");
  const fixture = join(sdk, "tests", "gr_fixture.cpp");
  const drive = (x, h, level, blocks) => {
    const stride = x.sonore_max_frames();
    const at = h !== undefined;
    const ip = (at ? x.sonore_in_ptr_at(h) : x.sonore_in_ptr()) / 4;
    for (let b = 0; b < blocks; ++b) {
      const m = new Float32Array(x.memory.buffer);
      for (let i = 0; i < stride; ++i) { m[ip + i] = level; m[ip + stride + i] = level; }
      if (at) x.sonore_process_at(h, stride);
      else x.sonore_process(stride);
    }
  };
  const hot = Math.pow(10, -8 / 20);

  const one = await instantiate(buildWasm("gr_fixture", fixture));
  one.sonore_init(48000);
  drive(one, undefined, 0, 40);
  check(one.sonore_gain_reduction() === 0, "a compressor fed silence reports no reduction");
  drive(one, undefined, hot, 40);
  near(one.sonore_gain_reduction(), 9.0, 0.5, "a 4:1 compressor 12 dB over reports 9 dB, positive");

  const pool = await instantiate(buildWasm("gr_fixture_pooled", fixture, ["-DSONORE_MAX_INSTANCES=2"]));
  const a = pool.sonore_new(), b = pool.sonore_new();
  pool.sonore_init_at(a, 48000);
  pool.sonore_init_at(b, 48000);
  drive(pool, a, hot, 40);
  drive(pool, b, 0, 40);
  near(pool.sonore_gain_reduction_at(a), 9.0, 0.5, "the slot driven hot reads its own reduction");
  check(pool.sonore_gain_reduction_at(b) === 0, "...and the silent slot beside it reads none");
  check(pool.sonore_gain_reduction_at(7) === 0, "a handle that is not a slot reads 0, not garbage");
}

console.log(`${checks} checks, ${failures} failure(s)`);
if (failures === 0) console.log("SONORE SDK WASM TEST PASSED");
process.exit(failures === 0 ? 0 : 1);
