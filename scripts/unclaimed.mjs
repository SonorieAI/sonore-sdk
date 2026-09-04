// SPDX-License-Identifier: Apache-2.0
// Public classes in the headers that the feature map never mentions.
//
//   npm run verify:unclaimed
//
// ── Why ─────────────────────────────────────────────────────────────────────
//
// feature-map.mjs answers one direction: everything the map CLAIMS is still in
// the headers, and a claim that rots fails the run. Seven audits have hardened
// that direction, including a regex that fails when something declared ABSENT
// turns out to be present.
//
// It cannot answer the other direction, and the other direction bites too. I
// set out to write a ProcessorChain and found one already in shaping.h,
// complete, with a ProcessorDuplicator beside it -- neither mentioned anywhere
// in the map. Ten minutes from shipping a second one.
//
// A map that under-claims is a map that makes you rewrite what you have. It
// also makes "how far from the reference" wrong in the flattering direction's opposite,
// which is a strange way to be wrong but is still being wrong.
//
// ── What counts as public ───────────────────────────────────────────────────
//
// A class or struct declared at namespace scope in a header, whose name does
// not start with a lowercase letter and which is not inside a `detail`-ish
// namespace. Nested types are skipped: they are reached through their owner,
// and the owner is what a map row would name.
//
// The allow-list below is for types that are genuinely not capabilities --
// options structs, tag types, platform shims. Every entry is a decision, so
// each carries the reason it is not a row.
import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, relative } from "node:path";

const root = new URL("..", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1");
const includeDir = join(root, "sdk", "include", "sonore");
const mapPath = join(root, "scripts", "feature-map.mjs");

/**
 * Types that are not capabilities. Each line is a decision about what a map
 * row is FOR: a row describes something a plugin author would look for by
 * name, not every type the implementation needed on the way there.
 */
const NOT_A_CAPABILITY = new Set([
  // ── Plain data, passed to or returned from something that IS a row ──
  //
  // A row describes something a plugin author looks for BY NAME. Nobody goes
  // looking for the struct a function takes; they go looking for the function.
  "ProcessSpec", "ParamInfo", "TransportInfo", "MidiMessage", "RpnMessage", "MpeZone",
  "EditorHost", "EditorChoice", "CommandInfo", "FileEntry", "DateTime", "Uuid", "TreeNode",
  "TableColumn", "AccessibleInfo", "AccessibleNode", "CpuFeatures", "Display", "WidgetState",
  "MouseEvent", "KeyPress", "Shortcut", "StrokeStyle", "PremulColour", "Colour", "Point",
  "Rect", "PixelRect", "Transform", "Line", "ZipEntry", "FlexItem", "GridItem", "GridTrack",
  "PopupItem", "ScopeFrame", "TiledImage", "ColourStop", "Margin", "TrackSize", "SvgShape",
  "PngImage", "WavData", "MidiFileData", "MidiFileEvent", "MidiTempoEvent", "MidiTrack",
  "SequenceEvent", "SampleData", "FileStamp", "FirWindowParams", "DeviceIdentity",
  "BridgeMessage", "MeterState", "BlockLevel", "HostInfo", "TrackInfo", "AuxBusInfo",
  "PluginDescriptor", "PluginDescription", "HostedParam", "ParamEdit", "FileFilter",
  "NoteExpressionSpec", "Options", "Triple", "Preset",

  // ── Compile-time detectors ──
  //
  // The SFINAE traits the wrappers use to ask "does this DSP take MIDI".
  // Machinery for one decision each, reached by nobody.
  "TakesMidi", "TakesSidechain", "TakesContext", "TakesDouble", "TakesDoubleCtx",
  "WantsTransport", "WantsHostInfo", "WantsTrackInfo", "WantsFile", "HasVoiceCapacity",
  "HasActiveVoices", "HasStateBag", "HasLatency", "HasTail", "HasPrepare", "HasReset",
  "SonoreTakesMidi", "SonoreTakesContext", "SonoreTakesSidechain",

  // ── The licence check's internals ──
  //
  // "Licensing" is the row somebody looks for. These are what it is built from:
  // an arbitrary-precision integer written for one modular exponentiation, the
  // three lines of a .lic file, and the object holding the atomic flag the
  // audio thread reads. Nobody goes looking for any of them by name.
  "BigInt", "LicFile", "Gate",

  // ── One format's plumbing ──
  //
  // The COM/vtable structs a wrapper must present, and the host-side mirrors of
  // them. The format rows -- "VST3 wrapper", "LV2 wrapper", "AudioUnit (AUv2)
  // wrapper" -- are what somebody looks for; these are what those are made of.
  "Instance", "Plugin", "Unit", "Factory", "View", "ViewFactoryRegistrar",
  "AudioComponentPlugInInstance", "ComponentIface", "ProcessorIface", "ControllerIface",
  "ViewIface", "ViewScaleIface", "UnitInfoIface", "MidiMappingIface", "NoteExpressionIface",
  "InfoListenerIface", "ContextRequirementsIface", "ComponentHandler", "PlugFrame",
  "ParameterChanges", "ParamQueue", "EventList", "StateHeader", "BypassState",
  "Steinberg_IPlugView", "Steinberg_ViewRect", "Steinberg_Vst_Event",
  "Steinberg_Vst_NoteExpressionTypeInfo",
  "Lv2Feature", "Lv2UiDescriptor", "Lv2UiIdleInterface", "Lv2UiResize", "UiInstance",
  "Lv2WorkerInterface", "Lv2WorkerSchedule", "HostLv2Feature", "HostLv2UiDescriptor",
  "HostLv2UiIdle", "Lv2Plugin", "Lv2Port", "Lv2Preset", "ClapPlugin", "HostedPlugin",
  "Document",

  // ── Steinberg's ASIO ABI, mirrored so we can talk to it ──
  //
  // Declarations of somebody else's interface. The "ASIO output" and "ASIO
  // driver enumeration" rows are the capability; these are the shape of the
  // SDK we are forbidden to ship.
  "IAsio", "AsioCallbacks", "AsioBufferInfo", "AsioChannelInfo", "AsioClockSource",
  "AsioTimeStamp", "AsioSamples",

  // ── Per-platform halves of one row ──
  //
  // "audio input capture" and "MidiInput" are the capabilities; a caller names
  // the platform-neutral type and the right one is selected for them.
  "AlsaInput", "WasapiInput", "CoreAudioInput", "NullAudioInput", "Device", "Sink",
  "FileSink", "WebViewHost", "ComCallback", "XdndAtoms",

  // ── Internal machinery of something that IS a row ──
  "HuffmanTable", "Inflater", "Rasteriser", "Edge", "Contour", "SysexAssembler",
  "XlibApi", "CoreGraphicsApi", "Api", "LinearKernel", "ZeroOrderHoldKernel",
  "ThumbnailBucket", "WaveformBucket", "HalfBandFilter", "ChebyshevSection",
  "Chebyshev2Section", "SampleStage", "Overlay", "PopupContent", "GroupTable",
  "Sysex7Assembler", "RpnDetector", "LambdaChangeListener",
]);

function everyHeader(dir) {
  const out = [];
  for (const name of readdirSync(dir)) {
    const full = join(dir, name);
    if (statSync(full).isDirectory()) out.push(...everyHeader(full));
    else if (name.endsWith(".h")) out.push(full);
  }
  return out;
}

const mapText = readFileSync(mapPath, "utf8");
const declared = [];

for (const file of everyHeader(includeDir)) {
  const shown = relative(includeDir, file).replace(/\\/g, "/");
  const text = readFileSync(file, "utf8")
    .replace(/\/\*[\s\S]*?\*\//g, "")
    .replace(/\/\/[^\n]*/g, "");

  // Brace depth, so a type nested inside a class or struct is skipped: it is
  // reached through its owner, and the owner is what a row would name.
  let depth = 0;
  let namespaceDepth = 0;
  const namespaces = [];

  for (const line of text.split("\n")) {
    const ns = line.match(/\bnamespace\s+([A-Za-z_]\w*)\s*\{/);
    if (ns) {
      namespaces.push(ns[1]);
      namespaceDepth = namespaces.length;
      depth += 1;
      continue;
    }

    // Only at namespace scope -- depth equal to the namespaces we are inside.
    const decl = line.match(/^\s*(?:template\s*<[^>]*>\s*)?(class|struct)\s+([A-Z]\w*)\b/);
    if (decl && depth === namespaceDepth) {
      const name = decl[2];
      const inDetail = namespaces.some((n) => /detail$/i.test(n));
      // A forward declaration is not a definition and names nothing new.
      if (!inDetail && !/;\s*$/.test(line)) declared.push({ name, file: shown });
    }

    for (const c of line) {
      if (c === "{") depth += 1;
      else if (c === "}") {
        depth -= 1;
        if (namespaces.length > 0 && depth < namespaces.length) {
          namespaces.pop();
          namespaceDepth = namespaces.length;
        }
      }
    }
  }
}

const seen = new Set();
const unclaimed = [];
for (const entry of declared) {
  if (seen.has(entry.name)) continue;
  seen.add(entry.name);
  if (NOT_A_CAPABILITY.has(entry.name)) continue;
  // Word-boundary, so "Reverb" is not matched by "ReverbTail".
  const mentioned = new RegExp(`\\b${entry.name}\\b`).test(mapText);
  if (!mentioned) unclaimed.push(entry);
}

console.log("── public types the map does not mention ──\n");
console.log(`  ${seen.size} public types across the headers`);

if (unclaimed.length > 0) {
  console.log("");
  for (const entry of unclaimed) console.log(`  FAIL ${entry.name}  (${entry.file})`);
  console.log(
    `\n${unclaimed.length} type(s) nobody claimed. Either add a row -- the map is how "how far ` +
    `from the reference" is answered, and something absent from it is something the next person will ` +
    `rewrite -- or add it to NOT_A_CAPABILITY with the reason it is not one.`);
  process.exit(1);
}

console.log("  and every one of them appears in the feature map.");
