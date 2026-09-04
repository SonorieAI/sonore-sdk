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
  const dspOnly = source
    .replace(/#include <sonore\/plugin\.h>[\s\S]*$/, "")
    .replace(/#include <sonore\/clap_wrapper\.h>[\s\S]*$/, "");
  const glued = `${dspOnly}\n#include <sonore/wasm_abi.h>\n`;
  const cpp = join(out, `${name}.cpp`);
  writeFileSync(cpp, glued);

  const wasm = join(out, `${name}.wasm`);
  const exports = [
    "_sonore_init", "_sonore_process", "_sonore_in_ptr", "_sonore_out_ptr",
    "_sonore_params_ptr", "_sonore_num_params", "_sonore_max_frames",
    "_sonore_channels", "_sonore_midi_ptr", "_sonore_midi_count", "_sonore_wants_midi",
  ];
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

console.log(`${checks} checks, ${failures} failure(s)`);
if (failures === 0) console.log("SONORE SDK WASM TEST PASSED");
process.exit(failures === 0 ? 0 : 1);
