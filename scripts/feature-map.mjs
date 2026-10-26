// SPDX-License-Identifier: Apache-2.0
// What this SDK has, against what the reference has, and a check that the answer is
// still true.
//
//   npm run verify:features
//
// The reason this is a script and not a table in a readme: the question "how
// far are we from the reference" has been answered from memory in this project twice,
// and both times the memory was wrong: a state-variable filter and a
// partitioned convolver were both reported missing while sitting in the
// headers. A list that nothing verifies is a list that drifts the moment
// somebody renames a class.
//
// So every row names a SYMBOL, and every symbol is looked up. A row whose
// symbol has gone is a failure, not a stale line in a document.
//
// What it deliberately does NOT do is compare by name. the reference's names are not
// ours and never will be: SmoothedValue is Smooth here, Synthesiser is
// VoiceManager, FloatVectorOperations is simd.h. Matching by name produced a
// sweep that reported forty false gaps. Each row is a CAPABILITY, matched by
// hand once and then held in place by its symbol.
import { existsSync, readFileSync, readdirSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const inc = join(root, "sdk", "include", "sonore");
const log = (s) => process.stdout.write(s + "\n");


// ── Every the reference module, accounted for ────────────────────────────────────────
//
// The rows below are capabilities. This is the level above them: the reference's own
// module list, taken from the framework's published module list, with each module either COVERED by a
// bucket of rows or explicitly out of scope with a reason.
//
// It exists because the row list alone was measuring itself. It reported "0
// unaccounted for" for weeks while never mentioning AudioDeviceManager or
// AudioDeviceSelectorComponent -- not because they were judged unnecessary,
// but because nobody had ever written them down. A list of things I thought
// of cannot tell me what I did not think of.
//
// So the authority here is the reference's list, not mine. A module absent from this
// table is a failure, exactly like a symbol that has gone.
const MODULES = [
  // name, bucket of rows below, or null + why it is out of scope
  // Was out of scope as "telemetry. A generated plugin that phoned home would
  // be a liability, not a feature". Still true, and it is now the design
  // rather than the reason for absence: the module is built with nothing
  // sent by default and no network code of its own (see the row).
  ["analytics", "analytics", "nothing is sent by default: no destination ships, and the send is the plugin's own code"],
  // This said "the interface is a web page: CSS transitions and
  // requestAnimationFrame animate it", and was written off as out of scope on
  // that basis. True when the webview WAS the editor; false since the native
  // toolkit became the default -- the identical rot the opengl note had,
  // found by re-reading the module table rather than the rows under it.
  //
  // gfx/animator.h existed the whole time. It moved component BOUNDS and
  // nothing else, which is not most of what an editor animates.
  ["animation", "gui", null],
  ["audio_basics", "basics", null],
  ["audio_devices", "devices", null],
  ["audio_formats", "formats", null],
  ["audio_plugin_client", "client", null],
  ["audio_processors", "processors", null],
  ["audio_processors_headless", "processors",
   "the same wrappers: a plugin with no uihtml reports no GUI extension at all"],
  ["audio_utils", "utils", null],
  // Was out of scope as "a 2D physics engine, shipped with the reference for
  // historical reasons". The engine still is not shipped; the renderer is
  // built against the plugin's own copy.
  ["box2d", "box2d", "the renderer, against the plugin's own Box2D: nothing vendored"],
  ["core", "core", null],
  ["cryptography", "core", null],
  ["data_structures", "core", null],
  ["dsp", "dsp", "audited class by class against the reference's own list"],
  ["events", "core", null],
  ["graphics", "graphics", null],
  ["gui_basics", "gui", null],
  ["gui_extra", "extras", null],
  // This was out of scope, on the argument that a preset able to carry code
  // is a preset able to carry anything. The argument was about a script with
  // no limits, and it holds for one: so the engine that closes the module is
  // built around the limits -- time, stack, memory, string and array length,
  // stop() from another thread -- every one of which ends a hostile script in
  // an error rather than a hang or a crash, and none of which a script can
  // catch. It is opt-in: nothing in the SDK runs a script unless a plugin
  // constructs a JavascriptEngine and hands it one.
  ["javascript", "javascript", null],
  ["midi_ci", "extras", null],
  // The note here read "WebGL, in the same webview, without a second context to
  // manage", and then, when the native toolkit became the default, "software
  // rasterisation, deliberately". The second note is still the answer for an
  // editor of knobs and meters -- identical pixels on every machine, nothing
  // to fail to initialise in a VM -- and it is still the default.
  //
  // What it was not an answer for is a plugin whose editor IS a GPU picture: a
  // 3D waterfall, a 60 fps scope, a shader visualiser. Those had only the
  // webview's WebGL. The module is covered now, as an opt-in a component asks
  // for, with its own bucket of rows; nothing else in the editor changes.
  ["opengl", "opengl", null],
  ["osc", "osc", null],
  ["product_unlocking", "core", null],
  // Was out of scope as "not an audio plugin concern; a page can carry a
  // <video> if it must" -- true only of the webview editor, the same rot the
  // opengl and animation notes had once the native toolkit became the default.
  ["video", "video", "frames as Bitmaps through Graphics; Windows run, macOS compiled"],
];

// module, the reference's name for the capability, ours, and the symbol that proves it.
//
// Three states, and the difference between the last two matters:
//   "symbol"  present, and looked up every run
//   null      deliberately not built, with the reason in the note
//   false     a REAL GAP -- something the reference has, that belongs here, not yet
//             written. Not a failure, and not to be quietly filed under
//             "deliberate" either. Work that is queued, named out loud.
//
// A row may carry a FIFTH element: a regex that would prove the capability is
// actually there. It applies only to the absent kinds, and it fails the run if
// the pattern IS found.
//
// That direction was missing until something absent turned out not to be. Two
// capabilities were declared missing on the strength of a grep written as
// `grep -E "a\|b"` -- which in extended regex is not alternation at all but a
// search for the literal text `a|b`. Both greps found nothing, both times
// because they were looking for something that could not exist.
//
// One of the two was genuinely absent and the work was real. The other was
// not, and nothing in this file would have caught it: every check here asks
// whether a claimed capability is still present, and none of them asked
// whether a claimed ABSENCE is still absent.
const MAP = [
  // ── audio_basics ────────────────────────────────────────────────────
  ["basics", "ADSR", "ADSR", "class ADSR"],
  ["basics", "AudioBuffer / AudioBlock", "AudioBlock", "class AudioBlock"],
  ["basics", "AudioChannelSet", "channel roles + masks", "channelLayoutName"],
  ["basics", "Decibels", "dbToGain / gainToDb", "inline float dbToGain"],
  ["basics", "ScopedNoDenormals", "denormals.h, on every DSP entry point", "class ScopedNoDenormals"],
  ["basics", "AudioProcessLoadMeasurer", "LoadMeasurer, pushed to the editor", "class LoadMeasurer"],
  ["basics", "MidiRPNGenerator", "sendRpn + sendPitchBendRange", "inline void sendRpn"],
  ["basics", "AudioWorkgroup",
   "audio_workgroup.h: AudioWorkgroup holds a macOS 11 os_workgroup_t -- retained " +
   "on copy, released on destruction, no allocation, so a copy is safe on the " +
   "audio thread -- and join() makes the calling thread a member through a " +
   "WorkgroupToken; maxParallelThreadCount() and isCancelled() ask the OS. Where " +
   "one comes from: CoreAudioOutput::ioWorkgroup() reads " +
   "kAudioDevicePropertyIOThreadOSWorkgroup, and the standalone hands it to the " +
   "DSP before audio starts; an AU plugin offers " +
   "kAudioUnitProperty_RenderContextObserver (au_wrapper.h, a block built by hand " +
   "in apple_block.h) and the DSP's workgroupChanged() hook hears the render " +
   "thread's group. CLAP, VST3 and LV2 give a plugin no way to learn its host's " +
   "workgroup, so there the hook is never called, and the header says so.\n" +
   "      Written blind, and built not to be silent about it: os_workgroup_* are " +
   "found with dlsym, so an older macOS gets a workgroup that says why, and every " +
   "failed join says which errno. COMPILED for macOS against tests/au_shim " +
   "(mac_syntax_mac_host.cpp), NEVER RUN -- no Mac here; mac_host_selftest.cpp " +
   "joins the default device's group from a real-time thread and prints each " +
   "answer. Off macOS RUN on Windows (mac_host_test.cpp): every join fails, the " +
   "token is inactive and describe() says why",
   "class AudioWorkgroup"],
  ["basics", "MidiMessage system messages", "one policy: deliverableToDsp()", "inline bool deliverableToDsp"],
  ["basics", "(the reference has no MIDI clock follower)", "MidiClock: tempo from the wire", "class MidiClock"],
  ["basics", "MidiDataConcatenator (SysEx reassembly)", "SysexAssembler", "class SysexAssembler"],
  ["basics", "SysEx in MidiBuffer", "an arena beside the fixed events", "bool addSysex"],
  ["client", "SysEx input (CLAP, VST3, LV2)", "copied, never referenced", "CLAP_EVENT_MIDI_SYSEX"],
  ["client", "SysEx input (the standalone's MIDI ports)",
   "winmm MIM_LONGDATA, ALSA byte stream, CoreMIDI packets -- all through one " +
   "SysexAssembler. UNVERIFIED against hardware: there is no MIDI device on " +
   "this machine, so the queue half is tested and the driver half is not",
   "midiInPrepareHeader"],
  ["client", "SysEx input (AU)",
   "MusicDeviceSysEx through the shared SysexAssembler. Compiled on macOS CI, " +
   "unrun: auval sends no SysEx and the runner has no MIDI device -- " +
   "what is checked is that the format HAS a SysEx path, by the same rule " +
   "that would have caught it being the only one without",
   "kMusicDeviceSysExSelect"],
  ["client", "SysEx output (CLAP, VST3, LV2)",
   "from the block's arena, so the pointer outlives the call", "CLAP_EVENT_MIDI_SYSEX"],
  ["processors", "MIDI effect passthrough",
   "a MIDI effect forwards what it does not consume", "inline void forwardSysex"],
  ["basics", "FloatVectorOperations", "simd.h", "peakAbs"],
  ["basics", "IIRFilter", "Biquad", "class Biquad"],
  ["basics", "LagrangeInterpolator", "Lagrange3Kernel", "struct Lagrange3Kernel"],
  ["basics", "CatmullRomInterpolator", "CatmullRomKernel", "struct CatmullRomKernel"],
  ["basics", "WindowedSincInterpolator", "WindowedSincKernel", "struct WindowedSincKernel"],
  ["basics", "MidiBuffer", "MidiBuffer", "class MidiBuffer"],
  ["basics", "MidiMessage", "MidiMessage", "class MidiMessage"],
  ["basics", "MidiFile", "readMidiFile / writeMidiFile", "inline bool readMidiFile"],
  ["basics", "MidiMessageSequence", "MidiSequence", "class MidiSequence"],
  ["basics", "MidiKeyboardState", "NoteState", "class NoteState"],
  ["basics", "MidiRPNDetector", "RpnMessage + parser (audio.h)", "struct RpnMessage"],
  ["basics", "MPEInstrument / MPEZoneLayout", "MpeDecoder / MpeZone", "class MpeDecoder"],
  ["basics", "NormalisableRange (skew)", "ParamInfo::skew", "inline float skewForCentre"],
  ["basics", "Reverb", "Reverb", "class Reverb"],
  ["basics", "SmoothedValue", "Smooth / LogSmooth", "class Smooth"],
  ["basics", "Synthesiser", "VoiceManager", "class VoiceManager"],
  ["basics", "AudioPlayHead::PositionInfo", "TransportInfo", "struct TransportInfo"],

  // ── dsp ─────────────────────────────────────────────────────────────
  ["dsp", "BallisticsFilter", "BallisticsFilter", "class BallisticsFilter"],
  ["dsp", "Chorus", "Chorus", "class Chorus"],
  ["dsp", "Compressor", "Compressor", "class Compressor"],
  ["dsp", "Convolution", "partitioned convolver, over an ImpulseResponse loaded from a " +
   "file -- the IR is a separate type because loading one is a disk read and convolving " +
   "with it is not", "class Convolver"],
  ["dsp", "Convolution::loadImpulseResponse", "impulse.h: ImpulseResponse, which owns the " +
   "samples and their normalisation. Normalised on LOAD rather than per block, because an " +
   "IR that changed the output level when it was swapped would make every comparison " +
   "between two of them a comparison of their gains", "class ImpulseResponse"],
  ["dsp", "DelayLine", "DelayLine", "class DelayLine"],
  ["dsp", "DryWetMixer", "DryWetMixer", "class DryWetMixer"],
  ["dsp", "FFT", "Fft", "class Fft"],
  ["dsp", "FIR::Filter + FilterDesign", "fir.h", "designLowpassToSpec"],
  ["dsp", "IIR coefficient design", "filter_design.h", "class CascadedIir"],
  ["dsp", "LadderFilter", "LadderFilter", "class LadderFilter"],
  ["dsp", "LinkwitzRileyFilter", "LinkwitzRileyN", "class LinkwitzRileyN"],
  ["dsp", "LookupTable", "LookupTable", "class LookupTable"],
  ["dsp", "NoiseGate", "NoiseGate", "class NoiseGate"],
  ["dsp", "Oscillator", "Oscillator", "class Oscillator"],
  ["dsp", "Oversampling", "OversamplerT", "class OversamplerT"],
  ["dsp", "Panner", "Panner", "class Panner"],
  ["dsp", "Phaser", "Phaser", "class Phaser"],
  ["dsp", "StateVariableTPTFilter", "SVF", "class SVF"],
  ["dsp", "WaveShaper", "softClip / tanhClip / hardClip", "inline float softClip"],
  ["dsp", "WindowingFunction", "fft.h windows", "BlackmanHarris"],
  ["dsp", "SIMDRegister", "simd.h -- Vec4f, four floats wide on SSE and NEON and a plain " +
   "struct everywhere else, so the same DSP source compiles for wasm", "class Vec4f"],

  // ── audio_formats ───────────────────────────────────────────────────
  ["formats", "WavAudioFormat", "wav.h", "inline bool writeWav"],
  ["formats", "AiffAudioFormat", "audiostream.h", "parseAiff"],
  ["formats", "FlacAudioFormat", "own encoder + decoder", "class BitWriter"],
  ["formats", "MP3AudioFormat", "minimp3, vendored", "MINIMP3_IMPLEMENTATION"],
  ["formats", "OggVorbisAudioFormat", "stb_vorbis, vendored", "STB_VORBIS_NO_STDIO"],
  ["formats", "AudioFormatReader", "AudioFileReader", "class AudioFileReader"],
  ["formats", "AudioFormatWriter::ThreadedWriter", "WavRecorder", "class WavRecorder"],
  ["formats", "AudioThumbnail", "WaveformPeaks", "class WaveformPeaks"],

  // ── audio_devices ───────────────────────────────────────────────────
  ["devices", "AudioIODevice (WASAPI)", "audio_wasapi.h", "class WasapiOutput"],
  // Found by a seventh audit. The Linux standalone was ALSA-only, which reaches
  // the speakers and is not what a Linux STUDIO runs.
  ["devices", "AudioIODeviceType JACK",
   "audio_jack.h, dlopened -- and PipeWire's libjack has the same soname, so " +
   "this reaches both without knowing about either. JACK CALLS the client from " +
   "its own real-time thread rather than being written to, so there is no " +
   "render loop here and being late xruns the whole graph, not one program. " +
   "VERIFIED against a live server: jack_test.cpp registers a client on a real " +
   "jackd, and measures the frames the server's own thread asks for against the " +
   "wall clock -- 0.49 seconds of audio for 0.5 seconds of elapsed time, at the " +
   "server's rate and channel count rather than ours, and zero further " +
   "callbacks after stop(). The gate starts a dummy server to do it, but only " +
   "from an unpacked-in-home copy: on a box with a system JACK it connects to " +
   "whatever is already running and starts nothing, for the same reason the " +
   "backend passes JackNoStartServer",
   "class JackOutput"],
  ["devices", "AudioDeviceManager (choosing at runtime)",
   "audio_linux.h: both Linux backends behind one type, because whether JACK " +
   "is worth using is a fact about the machine that RUNS the plugin. Never " +
   "starts a server -- a standalone that did would reconfigure the machine's " +
   "audio because somebody opened a plugin", "class LinuxOutput"],
  ["devices", "AudioIODevice (CoreAudio)", "audio_coreaudio.h", "CoreAudioOutput"],
  ["devices", "AudioIODevice (ALSA)", "audio_alsa.h", "AlsaOutput"],
  ["devices", "audio input capture", "audio_input.h", "class AudioRing"],
  ["devices", "MidiInput", "midi_input.h", "namespace midiin"],
  ["devices", "MidiInput::getAvailableDevices / MidiDeviceInfo",
   "listDevices + --midi-input, in the same picker as the audio device",
   "inline void rememberMidi"],
  ["devices", "MidiDeviceListConnection (hot-plug)",
   "polled from the UI tick and pushed only when the list CHANGES -- one rule " +
   "rather than three platform notification APIs plus a fallback",
   "bool changedSinceLastPush"],
  ["devices", "universal_midi_packets (MIDI 2.0 UMP)",
   "ump.h: sizing, MIDI 1.0 both ways, MIDI 2.0 narrowed, SysEx7", "namespace ump"],
  ["devices", "UMP Flex Data / Stream / SysEx8",
   "ump128.h: the 128-bit packets ump.h left out, each encoded and decoded " +
   "to the bit positions of the UMP specification's field tables. Flex Data: " +
   "setTempo in its ten-nanosecond units (tempoFromBpm), TimeSignature, " +
   "Metronome, KeySignature and ChordName with its ChordAlteration fields, " +
   "read back with their FlexHeader; metadata text and lyrics split by " +
   "toFlexText and rejoined by FlexTextAssembler. UMP Stream: endpoint " +
   "discovery, EndpointInfo, device identity (into MIDI-CI's own " +
   "DeviceIdentity), StreamConfiguration request and notification, function " +
   "block discovery and FunctionBlockInfo, start and end of clip, and the " +
   "three names -- endpoint, product instance, function block -- rejoined by " +
   "StreamTextAssembler. SysEx8 through toSysex8 and Sysex8Assembler, keyed " +
   "by group AND stream ID, because letting streams interleave is what the " +
   "stream ID is for; the Mixed Data Set through MixedDataSetHeader, " +
   "toMixedDataSetChunk and MixedDataSetAssembler, delivered per chunk.\n" +
   "      Every assembler refuses rather than guesses: a start inside an open " +
   "message drops it, a continue or end with nothing open is an orphan, a " +
   "message too long for its buffer is dropped and never truncated, and each " +
   "is counted. Only the Mixed Data Set allocates -- a chunk can be 64 KB " +
   "and 256 can be open at once -- so it is assembled off the audio thread. " +
   "Tested in midi_osc_test against packets worked out by hand from the " +
   "field tables (Set Tempo at 120 BPM is D0100000 02FAF080), and with " +
   "interleaved, interrupted and malformed sequences; fuzz_midi_osc merges " +
   "random messages, mutates the word stream, and requires every unmutated " +
   "merge back exactly, 48,000 cases clean under ASan. Run on Windows (MSVC, " +
   "x64); the headers have no platform code, and the test and fuzz programs " +
   "were parsed as macOS (au_shim) and Linux (stub headers), not run there",
   "class Sysex8Assembler"],
  ["devices", "MidiOutput", "midi_output.h", "namespace midiout"],
  ["devices", "ASIO driver enumeration", "audio_asio.h", "inline std::vector<DriverInfo> listDrivers"],
  ["devices", "ASIO output", "AsioOutput + standalone --asio", "class AsioOutput"],
  ["devices", "ASIO input (duplex)", "one createBuffers for both directions", "bool hasInput"],
  ["devices", "ASIO sample formats", "asio_format.h, tested off-Windows", "inline float readSample"],
  ["devices", "ASIO control panel", "the driver's own settings dialog", "bool showControlPanel"],
  ["devices", "AudioDeviceManager", "enumerate + choose + remember, across backends", "inline void rememberDevice"],
  ["devices", "AudioDeviceSelectorComponent",
   "in-window picker across backends, live", "inline void installDeviceBridge"],
  ["devices", "live device switching", "stop, open, prepare, run -- without restarting", "bool select"],

  // ── audio_processors ────────────────────────────────────────────────
  ["processors", "AudioProcessor", "the DSP contract + wrappers", "struct ProcessContextT"],
  ["processors", "AudioProcessorParameter", "ParamInfo", "struct ParamInfo"],
  ["processors", "AudioProcessorGraph", "PluginGraph", "class PluginGraph"],
  ["processors", "KnownPluginList", "PluginCache", "class PluginCache"],
  ["processors", "AudioPluginFormat (hosting)", "host.h", "class Vst3Plugin"],
  ["processors", "PluginDirectoryScanner",
   "host.h scanDirectory(); plugin_cache.h scanDirectoryCached() keeps what it found",
   "std::vector<PluginDescription> scanDirectory("],
  ["processors", "AudioUnitPluginFormat (hosting an Audio Unit)",
   "host_au.h, included by host.h: AudioUnitPluginFormat scans the AU registry by " +
   "type -- aufx, aumu, aumf, aumi, augn: .component bundles, Apple's own units and " +
   "AUv3 extensions alike -- and describes a .component from its Info.plist, so " +
   "scanDirectory() and PluginCache see AUs too; loadPlugin() takes format \"AU\" " +
   "and returns an AuPlugin. AudioComponentInstanceNew for AUv2, " +
   "AudioComponentInstantiate for AUv3 and units that require it (its completion " +
   "through a hand-built block, the main run loop running while it waits); " +
   "non-interleaved float formats negotiated against SupportedNumChannels; " +
   "AudioUnitRender with an input render callback; MIDI in by MusicDeviceMIDIEvent " +
   "and SysEx, out through the MIDI output callback; parameters with clumps and " +
   "value strings, and the unit's own edits and gestures through an AUEventListener; " +
   "state as ClassInfo written as an XML plist, which is an .aupreset; presets, " +
   "latency, tail, bypass; the editor from kAudioUnitProperty_CocoaUI or an AUv3's " +
   "requested view controller, in the caller's NSView. AudioToolbox, CoreFoundation " +
   "and libobjc are opened, not linked, so no host gains a framework to name.\n" +
   "      COMPILED for macOS against tests/au_shim, NEVER RUN: there is no Mac " +
   "here. Every Apple call's status is checked and reported by name (lastError(), " +
   "the loader's error string); mac_host_selftest.cpp loads Apple's AUDelay and " +
   "DLSMusicDevice and is the run a Mac owes it. The portable half -- ids, the " +
   "parameter mapping, channel negotiation, emitted MIDI, error names -- RUN on " +
   "Windows by mac_host_test.cpp",
   "class AudioUnitPluginFormat"],
  ["processors", "AudioProcessorEditor", "webview editor", "inline std::string bridgeScript"],
  ["processors", "WebBrowserComponent's TypeScript interop (the reference's npm package)",
   "sdk/include/sonore/sonore.d.ts: the types of window.sonore, installed with the headers, " +
   "held to the bridge gui.h generates by verify:bridge-types", "interface Sonore {"],
  ["processors", "GenericAudioProcessorEditor", "gfx/plugin_editor.h, built from ParamInfo",
   "class GenericEditor"],
  ["processors", "SliderParameterAttachment", "gfx/plugin_editor.h", "class SliderAttachment"],
  ["processors", "ComboBoxParameterAttachment", "gfx/plugin_editor.h", "class ComboAttachment"],
  ["processors", "editor host context menu", "FileDialog + host menu", "showHostContextMenu"],
  ["processors", "AudioProcessorValueTreeState",
   "value_tree_state.h: AudioProcessorValueTreeState, built from the four " +
   "separate pieces that remain separate -- ParamInfo, StateBag, UndoHistory " +
   "and the plugin_editor.h attachments -- so code ported from the reference has " +
   "a direct target. A ParameterLayout of AudioParameterFloat (a " +
   "NormalisableRange with interval and skew), AudioParameterInt, " +
   "AudioParameterBool and AudioParameterChoice, nestable in an " +
   "AudioProcessorParameterGroup, all RangedAudioParameter with ParameterID, " +
   "attribute builders and text conversion. Each produces a ParamInfo; " +
   "parameterInfos() is the table a PluginDescriptor points at, in layout " +
   "order, so p[i] is entry i, and text defaults to formatParamValue and " +
   "parseParamValue, the functions every wrapper uses. layoutProblem() names " +
   "what ParamInfo cannot carry (a symmetric skew) instead of every host " +
   "drawing it linear.\n" +
   "      getRawParameterValue() is one std::atomic<float> per parameter; the " +
   "audio thread reads it, and its one write, updateFromHost(p), is relaxed " +
   "stores plus a bit in an atomic mask -- listeners run on the message thread " +
   "from dispatchPendingChanges()/syncFromHost(), never on the audio thread as " +
   "the reference's do. A HostLink (the four calls gfx::EditorHost carries) " +
   "sends edits to the real parameter; gestures bracket them; UndoHistory " +
   "records one snapshot per gesture, and only if something moved; undo goes " +
   "back to the host inside gestures of its own. copyState()/replaceState() " +
   "are a StateBag (\"param.<id>\" plus the free-form `state`), restored to " +
   "defaults for anything a session lacks, and fit a DSP's saveState/" +
   "loadState. gfx/value_tree_state_attachments.h: the nested " +
   "SliderAttachment, ButtonAttachment and ComboBoxAttachment, each the SDK's " +
   "own attachment over editorHostFor(apvts) plus a listener so code, undo and " +
   "the host move the widget at once.\n" +
   "      Tested in tests/plugin_state_test.cpp, RUN on Windows (MSVC /W4, and " +
   "under ASan): the layout's table field by field, interval snapping, the " +
   "raw atomics against setValue and against a concurrent audio thread, a real " +
   "drag through MouseRouter reaching a host as one gesture and one undo step, " +
   "undo/redo moving widgets and the host, and state round trips through a " +
   "DSP shaped as the wrappers drive one; tests/fuzz_plugin_state.cpp feeds " +
   "replaceState and the undo path 50,000 mutated states under ASan, each one " +
   "that loads left in range, on its steps, and a fixed point. Linux and " +
   "macOS: compiled only",
   "class AudioProcessorValueTreeState"],


  // Added by the class-by-class pass over dsp's 52 classes. Everything
  // below was either already here and unlisted, or is named as absent with a
  // reason. Matching was by CAPABILITY, not by name -- the first sweep called
  // FFT and FIR missing because ours are Fft and FirFilter.
  ["dsp", "dsp::LogRampedValue", "LogRamp", "class LogRamp"],
  ["dsp", "dsp::Panner", "Panner", "class Panner"],
  ["dsp", "dsp::LinkwitzRileyFilter", "LinkwitzRiley", "LinkwitzRiley"],
  ["dsp", "dsp::FirstOrderTPTFilter", "OnePole", "class OnePole"],
  ["dsp", "dsp::LookupTable", "shaping.h tables", "LookupTable"],
  ["dsp", "dsp::Chorus / flanger", "Chorus (cubic-interpolated)", "class Chorus"],
  ["dsp", "dsp::Phaser", "Phaser", "class Phaser"],
  ["dsp", "dsp::Limiter", "Limiter", "class Limiter"],
  ["dsp", "dsp::NoiseGate", "NoiseGate", "NoiseGate"],
  ["dsp", "dsp::LadderFilter", "LadderFilter", "LadderFilter"],
  ["dsp", "dsp::BallisticsFilter", "Ballistics", "Ballistics"],
  ["dsp", "dsp::DelayLineInterpolationTypes::Thiran",
   "dsp.h: DelayLine::readThiran -- a first-order Thiran allpass read beside " +
   "read() (linear) and readCubic(). Its magnitude is flat at every fraction " +
   "(measured within 5e-8 of unity across the band, where a half-sample delay " +
   "through the linear read is -16 dB at 21.6 kHz and through the cubic and " +
   "Lagrange ones -12.7 dB) and its delay is exact at DC (to 4e-6 samples, " +
   "float). The fraction is kept in [0.618, 1.618), so the pole is never " +
   "nearer the unit circle than 0.236; each head's state is a ThiranState " +
   "(the line keeps one and reset() clears it), and a delay that JUMPS a " +
   "sample or more reseeds it at the new delay -- first outputs within 4e-5 " +
   "of a settled head, against 0.2 if the old state were kept.\n" +
   "      The trade is dispersion, measured: a 10.5-sample request is 10.33 " +
   "samples at 5 kHz. So readCubic() stays the read for a swept delay, and " +
   "this is the one for a fractional delay inside a feedback loop -- a " +
   "waveguide, a tuned comb -- where a lowpass per pass would detune it. " +
   "dsp_maths_test: impulse responses at ten delays against the analytic " +
   "group delay, a swept sine, jumps, 400k samples of swept and hostile " +
   "delays (NaN, negative, past the end), reset, zero allocations. RUN on " +
   "Windows (MSVC /W4, and under ASan); standard C++ with no platform " +
   "branch, not separately built on macOS or Linux",
   "readThiran("],
  ["dsp", "dsp::Matrix",
   "matrix.h: Matrix<T> -- dense, row-major, sized at construction: " +
   "identity, toeplitz and hankel, row and column access, transpose, sum, " +
   "difference, scalar and matrix products, the Hadamard product, compare; " +
   "solve() by Gaussian elimination with partial pivoting (several " +
   "right-hand sides at once), determinant() and inverse() by the same " +
   "elimination. What returns a matrix allocates; multiply() into a sized " +
   "result, solveInPlace() and the compound operators do not, and the test " +
   "counts zero allocations through a thousand blocks of them -- which is " +
   "the line between a prepare() tool and something usable per block.\n" +
   "      Tested against the textbook 3x3 system, a zero and a 1e-20 pivot, " +
   "Hilbert(3)'s exact integer inverse, the determinants of Hilbert(3) and " +
   "(4), A inv(A) = I to 1e-14 for 60 random matrices up to 12x12, and " +
   "Yule-Walker recovering an AR(2) model through toeplitz(). RUN on " +
   "Windows (MSVC /W4, ASan); standard C++ only",
   "class Matrix {"],
  ["dsp", "dsp::Polynomial",
   "polynomial.h: Polynomial<T> -- coefficients lowest power first, as the " +
   "reference orders them; Horner evaluation, which does not allocate; " +
   "withGain, getSumWith, getProductWith, getDerivative, getIntegral. The " +
   "per-sample polynomials elsewhere (the BLEP residual, the sine, the dB " +
   "curve) stay inline; this is for the ones a design BUILDS. Tested: " +
   "(x-1)(x-2)(x-3) and (1+x)^10 against their expanded coefficients, Horner " +
   "against the direct sum, products against products of values, and T7 " +
   "built by the Chebyshev recurrence against cos 7t. RUN on Windows (MSVC " +
   "/W4, ASan)",
   "class Polynomial {"],

  // ── The 2026-08-29 round: what the reference never had ──
  //
  // Eight gaps the generation pipeline kept walking into -- every "make it
  // louder", "de-ess it", "a bitcrusher", "a plate", "a supersaw" was written
  // from scratch by the model, and the hand-written version is where the knee
  // comes out discontinuous and the fold aliases. None of these has a
  // counterpart in the reference's list; the row exists so unclaimed.mjs knows
  // they were decided, and so the symbol is held in place.
  ["dsp", "(the reference has no loudness meter)",
   "metering.h: KWeighting -- the BS.1770 pre-filter pair, designed for the host's rate " +
   "from the parameters that regenerate the published 48 kHz table (NOT the RBJ shelf, " +
   "which is 0.26 dB low at 1 kHz)", "class KWeighting"],
  ["dsp", "(the reference has no LUFS / LRA)",
   "LoudnessMeter: momentary, short-term, integrated with the two-stage gate, and loudness " +
   "range -- gating blocks held as a 0.1 LU histogram, so the integrated measurement is " +
   "fixed memory for a programme of any length", "class LoudnessMeter"],
  ["dsp", "(the reference has no true-peak meter)",
   "TruePeakMeter: 4x windowed-sinc oversampling, the taps computed; reads the over a " +
   "sample peak cannot see (a 45-degree quarter-rate sine: -3 dBFS sample, 0 dBTP true)",
   "class TruePeakMeter"],
  ["dsp", "(the reference has no correlation meter)",
   "CorrelationMeter: the goniometer's needle, -1..+1 over a stated window", "class CorrelationMeter"],
  ["dsp", "(the reference has no expander)",
   "dynamics.h: Expander -- downward, with range; the Giannoulis quadratic knee, same " +
   "EnvFollower as the compressor", "class Expander"],
  ["dsp", "(the reference has no upward compressor)",
   "UpwardCompressor: lifts what is under the threshold, capped, because uncapped it lifts " +
   "the noise floor by 75 dB", "class UpwardCompressor"],
  ["dsp", "(the reference has no transient shaper)",
   "TransientShaper: SPL's differential-envelope trick -- two followers differing only in " +
   "attack detect an onset, two differing only in release detect a decay; no threshold",
   "class TransientShaper"],
  ["dsp", "(the reference has no de-esser)",
   "DeEsser: a third-octave detector into the compressor's gain computer, wideband or split " +
   "over the crossover", "class DeEsser"],
  ["dsp", "(the reference has no bit crusher)",
   "lofi.h: BitCrusher, fractional bits, mid-tread so silence stays silent", "class BitCrusher"],
  ["dsp", "(the reference has no sample-rate reducer)",
   "Decimator: sample-and-hold at any rate, including one that does not divide the host's; " +
   "deliberately NOT oversampled -- its aliasing is the effect", "class Decimator"],
  ["dsp", "(the reference has no wavefolder)",
   "WaveFolder: the closed-form triangle fold, one fmod per sample however hard it is driven",
   "class WaveFolder"],
  ["dsp", "(the reference has no oversampled-shaper wrapper)",
   "Oversampled<Shaper, Stages>: any per-sample shape run through the half-band cascade, " +
   "latency reported -- the folder's 3 kHz alias measured 26 dB lower through it",
   "class Oversampled"],
  ["dsp", "(the reference has only the Schroeder reverb)",
   "fdn.h: FdnReverb -- Jot's feedback delay network over a Hadamard, prime lengths, per-line " +
   "decay gains so the declared T60 is the measured one, modulated reads, two taps per line " +
   "for a balanced decorrelated pair", "class FdnReverb"],
  ["dsp", "(the reference has no pitch detector)",
   "pitch.h: PitchDetector -- YIN, normalised from lag 1 (the band-start shortcut reports " +
   "the octave on a missing fundamental), parabolic refinement", "class PitchDetector"],
  ["dsp", "(the reference has no pitch shifter)",
   "PitchShifter: two heads on one delay line, equal-power crossfade, latency half the window",
   "class PitchShifter"],
  ["dsp", "(the reference has an FFT but no processable STFT)",
   "stft.h: Stft<Size, Overlap> -- window, transform, hand the bins to a functor, mirror, " +
   "overlap-add; reconstruction exact by per-position normalisation, latency exactly Size",
   "class Stft"],
  ["dsp", "(the reference has no wavetable oscillator)",
   "synth.h: Wavetable (mip-mapped by octave, one per plugin) + WavetableOscillator (one " +
   "per voice) -- C7's folded 15th harmonic at -107 dB against -24 raw", "class Wavetable"],
  ["dsp", "(the reference has no wavetable oscillator)",
   "WavetableOscillator, the per-voice reader", "class WavetableOscillator"],
  ["dsp", "(the reference has no unison / supersaw)",
   "UnisonOscillator: N polyBLEP saws spread in cents and in the field, RMS-normalised",
   "class UnisonOscillator"],
  ["dsp", "(the reference has no oscillator sync)",
   "HardSyncOscillator: the reset BLEPped with the height of the drop -- 41 dB off the alias " +
   "floor against the naive reset", "class HardSyncOscillator"],
  ["dsp", "(the reference has no FM operator)",
   "FmOperator: phase modulation in radians with feedback; the carrier vanishes at index " +
   "2.405, asserted", "class FmOperator"],
  ["dsp", "(the reference has an ADSR, not a DAHDSR)",
   "Dahdsr: delay and hold stages, same -60 dB convention as ADSR", "class Dahdsr"],
  ["dsp", "(the reference has no physical model)",
   "KarplusStrong: the plucked string, the loop filter's delay compensated so 220 Hz reads " +
   "220.00", "class KarplusStrong"],
  ["dsp", "(the reference has no modal synthesis)",
   "ModalResonator: a bank of two-pole modes with exact amplitude and -60 dB decay",
   "class ModalResonator"],
  ["dsp", "(the reference has no tilt EQ)",
   "tone.h: TiltEq -- FERRIC's hand-written tone control, done once with the Q that reads " +
   "0 dB at the pivot", "class TiltEq"],
  ["dsp", "(the reference has no Baxandall)",
   "Baxandall: the 1952 bass/treble pair at Q 0.5", "class Baxandall"],
  ["dsp", "(the reference has no velvet noise)",
   "random.h: VelvetNoise -- one +/-1 pulse per grid cell, the cheap diffusion element of " +
   "modern reverbs", "class VelvetNoise"],

  // ── The second 2026-08-29 round: the machines a plugin is NAMED after ──
  //
  // A delay, a tape, an amp stage, a multiband split, the 303 and MS-20
  // filters, a granular engine, a phase vocoder, a spring, a vocoder. Every
  // one had been rebuilt by the model from primitives, and every rebuild is
  // where the time knob zips, the tape is a tanh and the crossover dips.
  // Where a paper exists the header cites it and the test measures the
  // paper's property, not the code's.
  ["dsp", "(the reference has no delay machine)",
   "delay.h: StereoDelay -- two lines, filtered feedback, ping-pong, modulation, tempo sync; " +
   "a time change is a queued equal-power crossfade or a slew-capped glide, never a click",
   "class StereoDelay"],
  ["dsp", "(the reference has no hysteresis)",
   "tape.h: JilesAtherton -- the ferromagnetic loop, RK4 per sample at 2x with sub-steps, " +
   "the series Langevin and the well-posedness bound the paper does not state",
   "class JilesAtherton"],
  ["dsp", "(the reference has no tape machine)",
   "TapeSaturator: the loop at 2x, Kadis/Chowdhury head loss as a linear-phase FIR that slides " +
   "with the speed, wow and flutter on a reported transport delay, drive compensated",
   "class TapeSaturator"],
  ["dsp", "(the reference has no valve model)",
   "tube.h: TubeStage -- Koren's triode solved once into a table with the load line and cathode " +
   "bias in the loop (bisection), grid conduction, unity and upright by default",
   "class TubeStage"],
  ["dsp", "(the reference has no tone stack)",
   "ToneStack: the '59 Bassman network from Yeh & Smith's symbolic solution, bilinear into a " +
   "third-order section in double, equal to the circuit at every pre-warped frequency",
   "class ToneStack"],
  ["dsp", "(the reference has only a two-way crossover)",
   "crossover.h: MultibandSplitter -- 2..8 bands that SUM FLAT, every band carrying the " +
   "allpass of every later crossover", "class MultibandSplitter"],
  ["dsp", "(the reference has no diode ladder)",
   "va_filters.h: DiodeLadder -- the 303's coupled four-pole, Zavalishin's ZDF solution in " +
   "Pirkle's form", "class DiodeLadder"],
  ["dsp", "(the reference has no Sallen-Key)",
   "SallenKeyFilter: the Korg35 lowpass and highpass, ZDF, exactly 1/(s^2 + (2-K)s + 1) at " +
   "the pre-warped frequency", "class SallenKeyFilter"],
  ["dsp", "(the reference has no granular engine)",
   "granular.h: GrainEngine -- scheduled Hann grains from a live or loaded buffer, cubic " +
   "reads, per-grain pitch, pan and reverse, freeze by not writing", "class GrainEngine"],
  ["dsp", "(the reference has no phase vocoder)",
   "phase_vocoder.h: PhaseVocoder -- Laroche & Dolson peak shifting with identity phase " +
   "locking: no resampler, exact overlap-add, latency of exactly one frame at every ratio",
   "class PhaseVocoder"],
  ["dsp", "(the reference has no spring)",
   "spring.h: SpringReverb -- Abel/Parker dispersive waveguide: second-order allpass chain " +
   "peaking at the transition frequency inside a transit-delay loop, two coils",
   "class SpringReverb"],
  ["dsp", "(the reference has no vocoder)",
   "vocoder.h: ChannelVocoder -- constant-Q bands of cascaded SVFs, each normalised to unity " +
   "peak, modulator envelopes on carrier bands", "class ChannelVocoder"],
  ["dsp", "(the reference has no formant filter)",
   "FormantFilter: five formants per vowel from the Csound/CHANT soprano table, morphed " +
   "geometrically between vowels", "class FormantFilter"],
  ["dsp", "(the reference has no opto compressor)",
   "compressors.h: OptoCompressor -- the LA-2A's T4 cell: a 10 ms panel and a photocell with a " +
   "fast state and a slow memory, so half the release goes in 60 ms and the rest in seconds, " +
   "longer after longer compression", "class OptoCompressor"],
  ["dsp", "(the reference has no FET compressor)",
   "FetCompressor: the 1176 as a FEEDBACK compressor whose loop is solved each sample (a " +
   "20 us attack without a limit cycle), a FET shunt attenuator whose drain-dependent " +
   "conductance is the even-harmonic grain, the four ratios and all buttons in",
   "class FetCompressor"],
  ["dsp", "(the reference has no VCA compressor)",
   "VcaCompressor: dbx/SSL -- an exactly exponential gain law (the ratio IS the ratio), true-RMS " +
   "or peak detection, OverEasy knee, the two-capacitor auto release", "class VcaCompressor"],


  // The 2026-08-30 library round: the pieces every audio plugin rewrites.
  ["dsp", "(the reference EQ is bilinear)",
   "eq.h: ParametricEq -- 8 bands of Vicanek matched biquads (impulse-invariant poles, zeros " +
   "fit at DC / f0 / Nyquist), parameters smoothed at control rate, and an optional " +
   "linear-phase FIR realisation of the same curve", "class ParametricEq"],
  ["dsp", "(the reference has no diode clipper)",
   "clipper.h: DiodeClipper -- the diode-pair clipper as its circuit ODE (Yeh/Abel/Smith " +
   "2007), trapezoidal + Newton, diode counts per side for asymmetry", "class DiodeClipper"],
  ["dsp", "(the reference has no diode clipper)",
   "OversampledDiodeClipper: the same circuit at 8x with the latency reported",
   "class OversampledDiodeClipper"],
  ["dsp", "(the reference has no Hilbert pair)",
   "hilbert.h: HilbertTransformer -- elliptic half-band allpass pair rotated to quadrature: " +
   "90 degrees within one from 300 Hz to 20 kHz", "class HilbertTransformer"],
  ["dsp", "(the reference has no frequency shifter)",
   "FrequencyShifter: single-sideband shift in hertz (not semitones), image 110 dB down",
   "class FrequencyShifter"],
  ["dsp", "(the reference has no LFO object)",
   "modulation.h: Lfo -- six shapes, phase offset in turns, seedable sample-and-hold and " +
   "smoothed random", "class Lfo"],
  ["dsp", "(the reference has no tremolo)",
   "Tremolo: depth against unity with a stereo phase spread", "class Tremolo"],
  ["dsp", "(the reference has no auto-panner)",
   "AutoPan: constant-power panning under the LFO", "class AutoPan"],
  ["dsp", "(the reference has no vibrato)",
   "Vibrato: depth stated in CENTS and measured as cents, whatever the rate",
   "class Vibrato"],
  ["dsp", "(the reference has no ring modulator)",
   "RingModulator: two sidebands at half amplitude, carrier and signal suppressed",
   "class RingModulator"],
  ["dsp", "(the reference has no flanger)",
   "Flanger: the chorus engine at flange depths with feedback", "class Flanger"],
  ["dsp", "(the reference has no multiband dynamics)",
   "multiband.h: MultibandDynamics -- downward + upward (OTT) compression per band over the " +
   "phase-matched splitter; unity when idle, per-band solo", "class MultibandDynamics"],
  ["dsp", "(the reference has no early reflections)",
   "reflections.h: EarlyReflections -- Allen-Berkley image sources: room geometry, 1/distance, " +
   "wall absorption per bounce, two listening points so the geometry pans",
   "class EarlyReflections"],
  ["dsp", "(the reference has no pitch correction)",
   "pitch_correct.h: PitchCorrector -- YIN to the nearest allowed scale note in cents, PSOLA " +
   "moves the voice there, every harmonic by the ratio, retune-speed glide", "class PitchCorrector"],
  ["dsp", "(the reference has no distortion object)",
   "distortion.h: Distortion -- the whole drive chain in one object: curve at 8x, DC blocker, " +
   "equal-RMS auto gain, tone, mix, latency reported", "class Distortion"],
  ["dsp", "(the reference has no circuit engine)",
   "circuit.h: NodalCircuit -- the nodal DK method (Yeh 2010): MNA with trapezoidal " +
   "companions, the linear part inverted once, Newton per sample on the nonlinear ports " +
   "only; diodes and Ebers-Moll BJTs, gmin and junction limiting like SPICE",
   "class NodalCircuit"],
  ["dsp", "(the reference has no Fuzz Face)",
   "fuzz.h: FuzzFace -- the two-germanium-PNP circuit solved whole on the engine: the bias, " +
   "the 70 dB max-fuzz gain and the volume-knob cleanup all fall out of the netlist",
   "class FuzzFace"],
  ["dsp", "(the reference has no passive program EQ)",
   "passive_eq.h: PassiveEq -- Pultec-style: independent boost and cut whose corners differ " +
   "(the trick dip), bandwidth-coupled bell gain, panel-position frequencies",
   "class PassiveEq"],


  // The 2026-08-30 second round: the four gaps named after the library round.
  ["dsp", "(the reference has no dynamic EQ)",
   "dynamic_eq.h: DynamicEq -- per band, a detector on the band's own slice of the spectrum " +
   "(or a sidechain key) drives a Vicanek matched bell through the Giannoulis gain computer; " +
   "downward and upward with a range cap, unity at rest", "class DynamicEq"],
  ["dsp", "(the reference limiter is sample-peak)",
   "limiting.h: TruePeakLimiter -- the look-ahead limiter with an 8x (64 taps a phase) inter-sample " +
   "peak estimator as its detector, so the ceiling is a dBTP ceiling; latency reported " +
   "including the interpolator's centre", "class TruePeakLimiter"],
  ["dsp", "(the reference has no plate)",
   "plate.h: PlateReverb -- Dattorro's 1997 figure-of-eight plate: the paper's delays and " +
   "seven-tap stereo outputs scaled by sr/29761, modulated tank allpasses, damping and " +
   "bandwidth in hertz, T60 from the figure's own decay^2-per-lap arithmetic",
   "class PlateReverb"],
  ["dsp", "(the reference has no Tube Screamer)",
   "screamer.h: TubeScreamer -- the TS-808 drive stage as its netlist on the nodal engine " +
   "(ideal op-amp, 4.7k+47n leg, 51k+pot feedback with the diode pair): the clean-rides-on-" +
   "top clipping and the 720 Hz voicing fall out; tone stage structural and says so",
   "class TubeScreamer"],

  // The AudioSource family, from a corrected sweep of audio_basics. The
  // first pass reported several of these absent on the strength of a grep
  // whose alternation was written `a\|b` -- literal text in extended regex,
  // matching nothing anywhere. They were never listed either way, which is
  // the same silence this map exists to break.
  ["basics", "AudioData (sample format conversion)",
   "audiofile.h, audiostream.h, asio_format.h", "inline float readSample"],
  ["basics", "MixerAudioSource / channel remapping",
   "audio_source.h: MixerAudioSource (its list swapped by pointer, no lock in " +
   "the callback) and ChannelRemappingAudioSource; graph.h's processor graph " +
   "does both for plugins, and routing besides",
   "class MixerAudioSource"],
  ["basics", "ToneGeneratorAudioSource",
   "audio_source.h: ToneGeneratorAudioSource; the standalone keeps its own " +
   "TestSource", "class ToneGeneratorAudioSource"],
  ["basics", "AudioTransportSource",
   "audio_source.h: AudioTransportSource -- start, stop, seek and gain ramped, " +
   "read-ahead and rate correction in its chain, waitForNextAudioBlockReady " +
   "for a render that may block; SampleStreamer is the sampler's own streaming",
   "class AudioTransportSource"],
  ["basics", "MemoryMappedAudioFormatReader / BufferingAudioReader",
   "SampleStream: a worker thread reads ahead, the audio thread never touches the file",
   "class SampleStreamer"],
  ["basics", "MidiMessageCollector",
   "midi_collector.h: MidiMessageCollector. Any thread queues a message " +
   "stamped on one steady clock (addMessageToQueue, addSysexToQueue); the " +
   "audio thread's removeNextBlockOfMessages puts each at the frame its " +
   "stamp implies. The interval since the previous block maps onto this one: " +
   "one-to-one when the callbacks keep time, laid against the END of the " +
   "block when one comes early, so arrival-to-sound stays one constant " +
   "block, and squeezed in order when one is late. Past 32 blocks of stall " +
   "the oldest land on frame 0 -- KEPT, where the reference drops them, " +
   "because one of them may be a note-off. samplePosition() is that mapping " +
   "as a pure function.\n" +
   "      The hand-over is a bounded multi-producer ring with a sequence number " +
   "per slot: a producer claims a slot with one compare-and-swap and never " +
   "waits, the audio thread never waits at all, nothing allocates, and an " +
   "event a full block cannot take stays queued for the next. The reference " +
   "takes a lock in the audio callback; this cannot. midiin::MessageQueue " +
   "still feeds the standalone's device input at frame 0, as before. Tested: " +
   "known positions for on-time, early, late and stalled callbacks; zero " +
   "allocations counted around the drain; three producer threads racing the " +
   "consumer with 60,000 messages, none lost or reordered; fuzzed with NaN, " +
   "infinite and backward stamps. Run on Windows; parsed as macOS and Linux",
   "class MidiMessageCollector"],
  ["devices", "SystemAudioVolume",
   "system_volume.h: SystemAudioVolume -- getGain/setGain/isMuted/setMuted on the " +
   "default output, all static as the reference's. Windows through the default " +
   "render endpoint's IAudioEndpointVolume; macOS through the default output " +
   "device's kAudioHardwareServiceDeviceProperty_VirtualMainVolume and " +
   "kAudioDevicePropertyMute (by four-character code, so it builds against SDKs " +
   "before and after the Master-to-Main rename); Linux through ALSA's Master " +
   "(else PCM) simple-mixer control, libasound dlopened exactly as the ALSA " +
   "backend does it, so a machine without ALSA reads isAvailable() == false.\n" +
   "      Nothing in the SDK calls the setters: the volume is the user's, for " +
   "everything on the machine, and is here for an application that is asked " +
   "to change it. RUN on Windows: audio_io_test always reads it, and only with " +
   "SONORE_TEST_SYSTEM_VOLUME=1 (never in CI) changes gain and mute, reads " +
   "both back, and restores them exactly -- on every exit path. The macOS and " +
   "Linux branches are COMPILED only (tests/mac_syntax_audio_io.cpp against " +
   "au_shim; Linux against stubs) and have not been run",
   "class SystemAudioVolume"],

  // ── audio_plugin_client ─────────────────────────────────────────────
  ["client", "clap.preset-discovery",
   "clap_wrapper.h: PresetProvider. How a CLAP host finds a plugin's factory " +
   "presets WITHOUT loading the plugin -- it reads them from an index the " +
   "plugin declares, so a browser can list ten thousand presets across forty " +
   "plugins without instantiating any of them. Two FILE locations: the factory " +
   "presets written out as files (syncFactoryPresetFiles, a cache the binary " +
   "rebuilds) and the user's own folder; preset-load takes a factory index or " +
   "any preset file.\n" +
   "The user's presets themselves -- the reference has no single class for " +
   "them; plugins built on it each write a \"preset manager\" over its program " +
   "calls and its state -- are user_presets.h: PresetMetadata and " +
   "PresetFileContents (a versioned, checksummed container around the plugin's " +
   "state blob, the host save path's bytes exactly), UserPresetStore (one " +
   "folder per plugin under Documents / ~/Library/Audio/Presets / " +
   "$XDG_DATA_HOME, file names legal on every filesystem, atomic writes, save, " +
   "overwrite, rename, delete to the Recycle Bin / Trash, duplicate, import " +
   "including .vstpreset, export, rescan) and PresetManager (factory and user " +
   "as one list, next and previous, a modified marker, the selection saved in " +
   "the session as state v6); loads land through the host's own restore path " +
   "in CLAP, VST3 and AU. gfx/preset_bar.h: PresetBar, in the generic native " +
   "editor and the showcase, a load being one APVTS undo step. RUN on Windows: " +
   "tests/user_presets_test.cpp (146 checks, ASan clean, the real Recycle Bin " +
   "with SONORE_TEST_TRASH=1) and fuzz_user_presets (ASan, 50,000 iterations); " +
   "clap-validator 0.4.1 on all ten examples with preset-discovery-crawl/load " +
   "included -- on Windows under a //?/C:/ preset root, because the validator " +
   "refuses a FILE location that does not begin with '/'. The macOS " +
   "branch (Trash, Finder) is COMPILED only (au_shim), the Linux one " +
   "(freedesktop trash, xdg-open) parsed against POSIX stubs here and run only " +
   "where CI runs Linux",
   "PresetProvider"],
  ["client", "state dirty (the host's save prompt)",
   "clap_wrapper.h: a DSP that reports whether its state has changed since the " +
   "last save, so the host can ask before closing. Detected rather than " +
   "required -- a plugin that does not answer is treated as always-dirty, which " +
   "is the safe direction: an unnecessary prompt costs a click and a missing " +
   "one costs the work",
   "HasStateDirty"],
  ["client", "VST3 preset files (.vstpreset)",
   "preset_file.h: Vst3PresetFile, the format Steinberg's own browser reads. A " +
   "plugin whose presets are only in its own format is one whose presets do not " +
   "appear where the user looks for them",
   "struct Vst3PresetFile"],
  ["client", "VST3 wrapper", "vst3_wrapper.h", "vst3_c_api"],
  ["client", "AudioUnit (AUv2) wrapper", "au_wrapper.h", "AudioComponentPlugInInterface"],
  ["client", "the macOS code, read by a compiler",
   "au_wrapper, CoreAudio in and out, CoreMIDI -- against a shim, on both legs",
   "SONORE_APPLE_SYNTAX_CHECK"],
  ["client", "LV2 wrapper", "lv2_wrapper.h", "LV2_Descriptor"],
  ["client", "Standalone wrapper", "standalone.h", "inline int runInteractive"],
  ["client", "(the reference has no CLAP)", "clap_wrapper.h", "clap_plugin_entry"],
  ["client", "AUv3 app extension",
   "an app EXTENSION: needs a containing app and an Xcode target. Not fabricated " +
   "without a Mac to build one on",
   // AudioUnitViewController is the class an AUv3 must supply and
   // NSExtensionPrincipalClass the plist key that names it: neither can appear
   // in prose, which the first attempt at this regex did -- "AUv3" matches a
   // comment in host.h that merely says the format exists.
   null, "AudioUnitViewController|NSExtensionPrincipalClass"],
  // AAX was a guarded ABSENCE until Avid's SDK was in hand: the rule was not
  // to write a wrapper against a guessed ABI, and the row failed the build if
  // one appeared. It was then written against the real SDK (2.9.0), which the
  // repository still does not carry -- each developer brings their own copy.
  ["client", "AAX wrapper",
   "aax_wrapper.h: AAX Native for Pro Tools, mono and stereo, effects and " +
   "instruments, with MIDI in and out, the key input and aux output stems, the " +
   "SNRS state as the one chunk, latency, master bypass, and both editors in " +
   "Pro Tools' window. Built by sonore_add_aax() against the developer's own " +
   "copy of Avid's SDK (SONORE_AAX_SDK_ROOT), never vendored. Not AAX DSP, " +
   "AudioSuite or page tables. Every example builds on Windows against SDK " +
   "2.9.0; shipping still needs Avid's approval and a PACE signature",
   "AAX_CEffectParameters"],
  ["client", "VST2 wrapper",
   "deliberately never: Steinberg ended VST2 licensing in 2018, and shipping one " +
   "without a licence is the kind of risk this project rejected the reference to avoid",
   // The same kind of guard the AAX row now carries, and for a stronger
   // reason: this absence is a LEGAL position, not an engineering one, and a
   // legal position held only in prose is one that survives exactly as long as
   // the person who remembers it.
   null, "audioMasterCallback|VstInt32|AudioEffectX|kVstVersion"],

  // ── audio_utils ─────────────────────────────────────────────────────
  ["utils", "MidiKeyboardComponent", "sonore.notes + the page's own keys", "api.__notes"],
  ["utils", "AudioVisualiserComponent", "sonore.level / db / vu", "api.__meter"],
  ["utils", "AudioDeviceSelectorComponent", "the in-window picker", "inline void installDeviceBridge"],
  ["utils", "AudioAppComponent", "standalone App + processBlock", "inline void processBlock"],
  ["utils", "AudioThumbnail", "peak envelopes for a waveform display", "class AudioThumbnail"],

  // ── core, events, data_structures, cryptography ──────
  // ── core: a second audit, after the GUI one ─────────────────────────
  //
  // The first pass over this module listed nine rows and then read "0 real
  // gaps", which is the same flattery the GUI audit already caught once: a
  // count of zero only ever means zero among the things somebody wrote down.
  // Going back through the reference's core against what a PLUGIN actually needs found
  // three that were never named.
  ["core", "Random",
   "random.h: xoshiro128+ seeded through splitmix64, sixteen bytes of state and " +
   "no allocation, so it runs on the audio thread. Deterministic from its seed " +
   "-- a plugin whose noise differed between runs could not be nulled against a " +
   "reference render", "class Random"],
  ["dsp", "dsp::ProcessorChain",
   "shaping.h. A signal path written as a TYPE -- ProcessorChain<OnePole, " +
   "Saturation, Limiter> -- so adding a stage is one edit rather than four, and " +
   "the day somebody adds a fourth, three of the four places do not get updated " +
   "and one does. Expanded at compile time: no virtual dispatch and nothing " +
   "allocated, so a chain costs exactly what writing the stages out by hand " +
   "would have. Bypass per stage, and a bypassed stage still gets prepare and " +
   "reset -- one that did not would hold its old state and deliver it as a " +
   "click the moment it came back",
   "class ProcessorChain"],
  ["dsp", "dsp::ProcessorDuplicator",
   "shaping.h. One MONO processor per channel, so a design written for a single " +
   "signal becomes multichannel without being rewritten. Each channel gets its " +
   "own INSTANCE, which is the entire point: sharing one filter's state across " +
   "channels is the bug that collapses a stereo signal towards mono as soon as " +
   "the two sides differ. Settings are applied to all of them through one call, " +
   "because \"must be kept identical\" stops being true the first time somebody " +
   "sets one and forgets the other",
   "class ProcessorDuplicator"],
  ["dsp", "dsp::Bias", "shaping.h: a constant added before a waveshaper, which is how an " +
   "asymmetric transfer curve is reached from a symmetric one -- and asymmetry is where even " +
   "harmonics come from", "class Bias"],
  ["dsp", "DC blocking",
   "dsp.h: DcBlocker, a one-pole highpass at a few hertz. Not a tone control -- " +
   "an asymmetric waveshaper produces a DC offset, and DC eats headroom on every " +
   "bus downstream while being completely inaudible where it was made",
   "class DcBlocker"],
  ["dsp", "envelope following",
   "dsp.h: EnvFollower, separate attack and release. What every compressor, gate " +
   "and meter in this SDK is built on, exposed rather than hidden inside them so " +
   "a plugin can follow a signal without also compressing it",
   "class EnvFollower"],
  ["dsp", "comb and allpass sections",
   "dsp.h: Comb and Allpass, the two things a Schroeder reverb is made of. " +
   "Exposed because they are also how a flanger, a resonator and a diffuser are " +
   "made, and a Reverb that kept them private would make all three start again",
   "class Comb"],
  ["dsp", "gain-compensated dry/wet",
   "dsp.h: CompensatedDryWetMixer. A plain dry/wet crossfade DROPS three " +
   "decibels at the halfway point when the two signals are uncorrelated, which " +
   "is most of the time -- so an effect at 50% sounds quieter than at either " +
   "end and people reach for the mix knob to fix a problem the mix knob made. " +
   "Equal-power, so it does not",
   "class CompensatedDryWetMixer"],
  ["dsp", "stereo width",
   "effects.h: StereoWidener, mid/side. The width control every mastering chain " +
   "has, and the one place a plugin can accidentally destroy mono compatibility " +
   "-- so it is a named stage rather than something each plugin re-derives",
   "class StereoWidener"],
  ["dsp", "spectrum analysis",
   "fft.h: SpectrumAnalyser, a windowed FFT with averaging, fed from the audio " +
   "thread and read by the editor. The analyser half of every EQ that draws what " +
   "it is doing",
   "class SpectrumAnalyser"],
  ["dsp", "ResamplingAudioSource (rate conversion)",
   "resample.h: Resampler, plus PolyphaseIirHalfBand for the two-to-one steps; " +
   "audio_source.h's ResamplingAudioSource is the streaming form, its ratio " +
   "changeable while it plays. " +
   "A sample recorded at 44.1 played in a 48 kHz session is the ordinary case, " +
   "and the alternative to converting it is playing it a semitone flat",
   "class Resampler"],
  ["dsp", "Interpolators (one interface over the kernels)",
   "interpolation.h: Interpolator, over the Lagrange, Catmull-Rom, windowed-sinc " +
   "and linear kernels the map lists separately. One type so a delay line can be " +
   "given its interpolation as a SETTING rather than as a template argument " +
   "chosen when the plugin was written",
   "class Interpolator"],
  ["dsp", "tempo-synced LFO",
   "transport.h: SyncedLfo, phase derived from the host's ppqPosition rather " +
   "than free-running. Which is the whole difference: a free LFO at \"1/4\" " +
   "drifts against the grid within a bar and lands somewhere different every " +
   "time the transport is restarted, where this one is at the same place in the " +
   "bar every pass -- so a bounce matches what was monitored",
   "class SyncedLfo"],
  ["dsp", "note expression, per voice",
   "audio.h: NoteExpressionBuffer. Per-note pitch, timbre and pressure, kept " +
   "beside the MIDI rather than folded into it, because a note expression event " +
   "addresses a VOICE and a MIDI message addresses a channel -- and flattening " +
   "the first into the second is what makes MPE sound like ordinary MIDI",
   "class NoteExpressionBuffer"],
  ["basics", "channel routing matrix",
   "channel_matrix.h: ChannelMatrix. What a plugin uses when its input layout " +
   "and its output layout are not the same one -- a mono-to-stereo, a " +
   "5.1-to-stereo fold-down -- rather than each plugin inventing the " +
   "arithmetic",
   "class ChannelMatrix"],
  ["basics", "sampler voice",
   "sampler.h: SampleVoice, over SampleStream's read-ahead. The voice half of a " +
   "sampler: pitch, loop points and the interpolation between them, kept apart " +
   "from the streaming so a plugin can have one without the other",
   "class SampleVoice"],
  ["dsp", "white and pink noise, and TPDF dither",
   "random.h. Pink is Voss-McCartney with sixteen rows, every octave from " +
   "20 Hz to 20 kHz within 0.6 dB; " +
   "dither is TRIANGULAR, which is the difference between a noise floor that " +
   "sits still and one that breathes with the music. Three types rather than " +
   "one function: WhiteNoise, PinkNoise and Dither each carry their own state, " +
   "and a shared generator would correlate the dither on two channels into a " +
   "centred image that is audible where two independent ones are not",
   "class PinkNoise"],
  ["dsp", "white noise and dither, as objects",
   "random.h: WhiteNoise and Dither beside PinkNoise. Named separately because " +
   "the map is what somebody searches when they want one, and until this row " +
   "existed the header contained both and the map mentioned neither -- which " +
   "is the direction of wrongness this file was not previously able to detect",
   "class WhiteNoise"],
  ["core", "var / JSON",
   "json.h: RFC 8259, with a depth cap because a state file may come from " +
   "anywhere. Missing keys give a fallback rather than throwing -- reading state " +
   "written by an older version of the same plugin is the NORMAL case. Object " +
   "keys are sorted, so the same values always write identical bytes",
   "class JsonValue"],
  ["core", "Base64",
   "json.h, checked against the RFC 4648 vectors rather than against itself",
   "struct Base64"],

  ["core", "File / paths", "userDataRoot + ensureDirectory", "inline std::string userDataRoot"],
  ["core", "SystemStats",
   "system_stats.h. The first line of the log a user sends you: \"it crashes on " +
   "my machine\" and \"it crashes on Windows 10.0 build 26200, Ryzen 7, 63 GB, " +
   "avx2\" are different bug reports, and only one can be acted on without a " +
   "conversation.\n" +
   "      RUNTIME feature detection, which is a different question from " +
   "simd.h's. That one decides at COMPILE time what the DSP is built with; this " +
   "asks the CPU what it actually is, and a binary built for SSE2 running on a " +
   "machine with AVX2 is a fact worth knowing when somebody reports the CPU " +
   "meter reading double. AVX checks OSXSAVE and XCR0 as well as the CPU bit -- " +
   "a plugin trusting the CPU bit alone faults on a kernel that does not save " +
   "the wide registers.\n" +
   "      On Windows through RtlGetVersion, never GetVersionEx. That one LIES " +
   "by design: since 8.1 it reports 6.2 to any process without a matching " +
   "compatibility manifest, and a plugin is loaded into a HOST whose manifest " +
   "is not ours -- so a plugin using it reports \"Windows 8\" on every machine " +
   "forever and the bug report is worthless. Anything a platform will not say " +
   "comes back empty, because a fabricated version sends whoever reads it " +
   "looking for a bug on a system nobody was using",
   "inline std::string machineDescription"],
  ["core", "MultiTimer",
   "multi_timer.h: a meter at 30 Hz, a blinking light at 2 Hz and a settings " +
   "nudge every ten seconds, in one object rather than three subscriptions to " +
   "keep in step with each other's lifetimes.\n" +
   "      It takes the elapsed time as an ARGUMENT, the same decision the " +
   "Animator and the TooltipManager already made: a timer that read a clock " +
   "could only be tested by sleeping, and a sleeping test is slow, flaky on a " +
   "loaded machine, and unable to check the interesting cases at all. A timer " +
   "more than one interval behind fires ONCE -- a host that stalls for two " +
   "seconds leaves a 30 Hz timer sixty ticks behind, and catching up means " +
   "sixty repaints back to back and, for a blinking light, sixty state changes " +
   "nobody sees. That is a decision rather than an oversight and it is the one " +
   "a caller most needs to know",
   "class MultiTimer"],
  ["core", "Logger / FileLogger",
   "logger.h. An application that misbehaves can be run from a terminal; a " +
   "plugin cannot. It lives in somebody else's process on somebody else's " +
   "machine, and the only channel back is the user -- \"it crashes sometimes " +
   "when I load a preset\" is the whole bug report AND the whole evidence.\n" +
   "      Two doors, because the interesting failures are in process() and that " +
   "is exactly where a logger must not be used: opening a file, formatting a " +
   "string or taking a mutex is each enough to blow a buffer deadline, so the " +
   "dropout would be caused by the diagnostic rather than by the bug. the reference's " +
   "Logger has no answer to that -- it is a virtual call that writes a file, " +
   "and the one place you most want a trace is the one place you cannot have " +
   "one. writeFromAudioThread copies at most 120 bytes into a fixed ring: no " +
   "allocation, no lock, no syscall, MEASURED with the same counting operator " +
   "new the RT audit uses, at zero for a hundred writes.\n" +
   "      A message arriving into a full ring is dropped and COUNTED, and the " +
   "count is written out -- dropped with a number attached is a fact, dropped " +
   "silently is a lie, and blocking until there is room is the dropout the " +
   "whole arrangement exists to avoid. Timestamps are UTC so lines from two " +
   "machines sort into the order they happened. The file is capped, and " +
   "rotation keeps the most recent lines that fit in half the cap -- the first " +
   "version wrote back every line it held in memory and produced a 3329-byte " +
   "file for a 200-byte cap, which the test caught",
   "class Logger"],
  ["core", "ThreadPool",
   "thread_pool.h. Reading a hundred-megabyte sample cannot happen on the AUDIO " +
   "thread -- it allocates and blocks on a disk -- and cannot happen on the UI " +
   "thread either, because that one belongs to the host and a plugin spending " +
   "four seconds in a message handler freezes the DAW while the user blames the " +
   "DAW.\n" +
   "      Two threads by default, capped at four, and that is the one place " +
   "this deliberately differs from the reference. The reference defaults to the core count, which " +
   "is right for an APPLICATION because there is one of it. A session with " +
   "forty plugin instances would create forty pools -- six hundred threads on a " +
   "sixteen-core machine, all idle, all costing a stack -- and they would buy " +
   "nothing, because what a plugin does in the background is read files. Two " +
   "threads saturate a disk queue as well as sixteen, and on a network share " +
   "more make it slower.\n" +
   "      Cancellation is not optional: a user who opens forty thousand samples " +
   "and immediately opens a different folder must not wait for the first scan. " +
   "Every job is handed a flag through a generation counter rather than a " +
   "resettable bool, so a job started before a cancel stops and one added after " +
   "it does not, with no window between. A throwing job does not take its " +
   "worker with it -- one bad sample loader would otherwise reduce the pool to " +
   "nothing and the symptom is that background work silently stops happening. " +
   "And the destructor joins a RUNNING job, timed in the tests, because that is " +
   "the one that hangs a DAW on plugin removal",
   "class ThreadPool"],
  ["core", "ZipFile",
   "zip.h. A preset pack, a sample pack, an expansion somebody downloaded -- " +
   "and without it the choices are to launch a shell command from inside a " +
   "plugin, on a thread the host owns, or to ask the user to unpack it and " +
   "explain where. Almost all of it already existed: gfx/inflate.h is a " +
   "complete DEFLATE decoder written by hand for PNG, so what is here is the " +
   "container and nothing else.\n" +
   "      READING only. Writing needs a COMPRESSOR, which is a different piece " +
   "of work and one nothing here needs -- a plugin reads packs, it does not " +
   "make them -- and a half-implemented writer producing zips that only this " +
   "reader accepts is worse than no writer.\n" +
   "      Every size, offset and name length comes from the file, which came " +
   "from the internet, so half of it is refusal: truncated downloads, a missing " +
   "central directory, Zip64 sentinels, methods that are not stored or " +
   "deflated, and a caller's limit that overrides whatever the header claims " +
   "about its own size -- the classic bomb is a few kilobytes claiming to be a " +
   "few gigabytes. The CRC is VERIFIED and a mismatch clears the buffer, " +
   "because a caller ignoring the return value would otherwise be holding " +
   "plausible garbage. Tested against an archive written by PYTHON rather than " +
   "by anything here: a reader tested against its own writer proves only that " +
   "the two share a misreading of the spec.\n" +
   "      isSafeEntryName is the zip-slip check. Nothing in this file writes to " +
   "disk so nothing here is vulnerable, but a caller that does needs it, and it " +
   "is named so that not calling it looks like an omission",
   "class ZipFile"],
  ["core", "Time / RelativeTime",
   "time.h. files.h reported a modification time and nothing could show it -- a " +
   "Date column, a sampler saying a file is 3:24 long, a \"last used\" ordering, " +
   "all of them the same two conversions. ISO dates, so a Date column sorted as " +
   "TEXT is sorted as a date, where \"23/08/2026\" sorts by day and " +
   "\"08/23/2026\" by month. Durations ROUNDED rather than truncated -- a " +
   "3.7-second sample reading 0:03 and playing for four is the small wrongness " +
   "that makes people distrust a readout -- and a coarse \"2 hours ago\", which " +
   "is what somebody scanning for the preset they made this morning is actually " +
   "looking for. Through localtime_r and localtime_s, never localtime, which " +
   "returns a pointer to a STATIC struct: two threads formatting a date at once " +
   "get each other\'s answers, and the bug looks like a date that is " +
   "occasionally somebody else\'s",
   "inline std::string formatDuration"],
  ["core", "Uuid",
   "uuid.h. A preset needs a name a user can change and an id that never does, " +
   "or renaming one breaks every project that referenced it.\n" +
   "      Deliberately NOT built on random.h, which is xoshiro128+ and " +
   "deterministic on purpose -- a plugin whose noise differed between runs " +
   "could not be nulled against a reference render. Which makes it exactly " +
   "wrong here: two instances seeded identically because they were constructed " +
   "identically would mint identical \"unique\" ids, and the failure is " +
   "invisible until somebody has two of the plugin in one project and their " +
   "presets overwrite each other. std::random_device is mixed with the " +
   "high-resolution clock and a stack address, because the standard PERMITS a " +
   "deterministic random_device and some MinGW builds shipped one. Version and " +
   "variant bits per RFC 4122, and parsing accepts an id however it arrived -- " +
   "braces, no hyphens, any case",
   "inline Uuid makeUuid"],
  ["core", "ChangeBroadcaster / ChangeListener",
   "broadcaster.h, and the half worth having is the COALESCING: mark a change " +
   "as often as you like and the listeners are told ONCE on the next UI tick. " +
   "That is the difference between a preset load notifying forty parameter " +
   "listeners forty times -- each one repainting -- and notifying them once, " +
   "which is the classic reason loading a preset takes a visible second. " +
   "sendChangeMessage sets one atomic flag and touches nothing else, so a " +
   "worker thread finishing a sample load can say so without the listener list " +
   "needing a lock it does not otherwise want. Notification runs over a COPY " +
   "and re-checks membership, because a panel closing in response to the very " +
   "change it was told about is the ordinary case and a listener removed during " +
   "the notify must not still be called",
   "class ChangeBroadcaster"],
  ["core", "File::findChildFiles / RangedDirectoryIterator",
   "files.h. There was ONE directory scan in this SDK -- pluginFilesIn, " +
   "hard-coded to .clap, .vst3 and .lv2 -- so a sampler could not show its " +
   "samples and a preset browser could not list its presets.\n" +
   "      Two things that are easy to get wrong and silent when they are. " +
   "ORDER: readdir and FindNextFile return entries in whatever order the " +
   "filesystem feels like -- creation order on ext4, hash order on some " +
   "network mounts -- so a browser built straight on them reshuffles between " +
   "machines and sometimes between runs; this sorts, always, folders first and " +
   "names compared case-insensitively without tolower, which is locale " +
   "dependent. ENCODING: the Windows narrow API is not UTF-8, and " +
   "FindFirstFileA on a folder called \"Percussión\" returns mojibake that " +
   "then fails to open -- sample libraries have accented names constantly -- so " +
   "the Windows path here is entirely wide and converted at the edges. And " +
   "stat rather than d_type, which is DT_UNKNOWN on XFS and several network " +
   "mounts, where a library is most likely to live",
   "inline std::vector<FileEntry> listDirectory"],
  ["core", "MemoryInputStream / MemoryOutputStream",
   "streams.h: MemoryInputStream and MemoryOutputStream; and host.h's " +
   "MemoryStream, which is what a plugin's state actually travels " +
   "through -- every format hands over a blob and expects one back, and a " +
   "stream over a buffer is the shape that makes reading it back " +
   "version-tolerant rather than a struct cast",
   "class MemoryStream"],
  ["core", "AudioVisualiser feed (the RT-safe scope buffer)",
   "scope_buffer.h: AudioScopeBuffer, written from process() and read by the " +
   "editor. Owned by the WRAPPER rather than the editor, so a window opened " +
   "mid-session shows the last second of audio instead of starting blank -- and " +
   "rt_safety arms a counting operator new around process() so \"allocates " +
   "nothing\" is measured rather than claimed",
   "class AudioScopeBuffer"],
  ["extras", "MIDI-CI messages",
   "midi_ci.h: Message, the property-exchange and profile envelopes. The half " +
   "of MIDI-CI that goes on the wire, kept as a type so a plugin can answer an " +
   "enquiry it does not otherwise implement",
   "class Message"],
  ["devices", "the standalone's device hub",
   "standalone.h: DeviceHub, which owns the audio and MIDI devices and the " +
   "choosing between them, plus FileSource for playing a file through the " +
   "plugin instead of a live input -- which is how a plugin is tested when the " +
   "machine has no interface attached",
   "DeviceHub"],
  ["core", "InputStream / OutputStream",
   "streams.h: InputStream and OutputStream with the reference's typed reads and " +
   "writes, BufferedInputStream, SubregionStream and InputSource; " +
   "file_types.h's FileInputStream and FileOutputStream. Audio still travels " +
   "through AudioFileReader and writeWav, which stream a file in blocks",
   "class InputStream"],
  ["core", "Thread + lock-free FIFO", "UiEventQueue, AudioRing", "class UiEventQueue"],
  ["core", "Atomic parameter value (the reference's parameters hold an atomic float)",
   "SharedParam, SharedFlag: relaxed atomics behind float/bool conversions, snapshotted " +
   "once per block so the DSP sees one parameter set for the whole call",
   "struct SharedParam"],
  ["core", "Timer / AsyncUpdater", "the webview tick", "onTick"],
  ["core", "ValueTree", "StateBag", "class StateBag"],
  ["core", "UndoManager", "UndoHistory (state snapshots, not commands)", "class UndoHistory"],
  ["core", "SHA-256", "hash.h, against the FIPS vectors", "class Sha256"],
  ["core", "MD5 / RSA / Blowfish",
   "hash.h: MD5 beside Sha256 (md5Hex, md5File); crypto.h: RSAKey and BlowFish, with " +
   "Primes and bigint.h's BigInteger under them. One implementation, described " +
   "in full -- vectors, caveats, platforms -- by the \"MD5 / Whirlpool\", " +
   "\"BlowFish / RSAKey / Primes\" and \"BigInteger\" rows. SHA-256 is still the " +
   "hash this SDK picks for itself; these are here so a format or a key that " +
   "already names them can be read",
   "class RSAKey"],
  ["core", "product unlocking",
   "not built, and not the SDK's job: licensing belongs to the service that " +
   "generates plugins, where a key can actually be checked against an account. " +
   "A licence check compiled into a downloadable binary is one that runs on the " +
   "attacker's machine, which is the argument against putting it there at all",
   null, "OnlineUnlockStatus|KeyGeneration|KeyFileUtils|TracktionMarketplace"],

  // ── graphics ────────────────────────────────────────────────────────
  //
  // Being BUILT, not out of scope any more. The decision that the page is the
  // widget set has been reversed: native becomes the default face of a
  // generated plugin and the web UI becomes opt-in, because a webview is a
  // browser process per instance and a session holds thirty plugins.
  ["graphics", "Point / Rectangle / Line", "gfx/geometry.h", "struct Rect"],
  ["graphics", "AffineTransform", "gfx/geometry.h", "struct Transform"],
  ["graphics", "Colour", "gfx/colour.h", "struct Colour"],
  ["graphics", "PixelARGB (premultiplied compositing)", "gfx/colour.h", "struct PremulColour"],
  ["graphics", "Path", "gfx/path.h: lines, quads, cubics, arcs, winding rules", "class Path"],
  ["graphics", "LowLevelGraphicsSoftwareRenderer", "gfx/bitmap.h: 16 sub-rows, exact horizontal coverage", "class Rasteriser"],
  ["graphics", "Image / PixelARGB buffer", "gfx/bitmap.h", "class Bitmap"],
  ["graphics", "Graphics", "gfx/graphics.h: state stack, clip, transform", "class Graphics"],
  ["graphics", "Graphics::reduceClipRegion (a Path)",
   "gfx/graphics.h: clipToPath -- antialiased, the path's coverage per pixel " +
   "multiplied into whatever is drawn under it, fills, strokes and images " +
   "alike, until the next restore(). A rounded panel clipping an image keeps " +
   "a smooth corner rather than a staircase", "void clipToPath"],
  ["graphics", "Graphics::beginTransparencyLayer / endTransparencyLayer",
   "gfx/graphics.h: beginLayer(opacity) / endLayer -- drawn into a layer the " +
   "size of the clip, put down through the clip as one picture, so a faded " +
   "group's overlaps do not show through each other. And beginMask / " +
   "endMask, which make what was drawn into a clip -- by alpha or by " +
   "luminance: an SVG's <mask>, a fade at the end of a list", "void beginLayer"],
  ["graphics", "PathStrokeType::createDashedStroke",
   "gfx/stroke.h: StrokeStyle::dashes and dashOffset, cut by Stroker::dash in " +
   "device space after flattening, so dash lengths are exact along curves. " +
   "SVG's rules: an odd list repeats, a negative or all-zero one is solid. A " +
   "pattern too fine to show along the path draws solid rather than cutting " +
   "a hundred million pieces", "static bool dash"],
  ["graphics", "PathStrokeType (joins, caps, miter limit)",
   "gfx/stroke.h: a quad per segment, unioned -- no self-intersecting outline",
   "class Stroker"],
  ["graphics", "ColourGradient",
   "gfx/gradient.h -- linear and radial, any number of stops, interpolated in " +
   "STRAIGHT alpha so a fade to transparent does not pass through black. Pad, " +
   "reflect and repeat past the ends (GradientSpread); a radial gradient's " +
   "focal point, a cone when it lies outside the circle as a canvas draws " +
   "one; and any affine map, so a radial gradient squashed by its box is an " +
   "ellipse rather than staying a circle",
   "class ColourGradient"],
  ["graphics", "ColourGradient (spread past the ends)",
   "gfx/gradient.h: GradientSpread -- Pad, Reflect, Repeat; what SVG's " +
   "spreadMethod names", "enum class GradientSpread"],
  ["graphics", "Typeface", "gfx/truetype.h: glyf and CFF outlines parsed to Paths -- .ttf, .otf " +
   "and collections of either, CID-keyed CJK fonts included", "class Typeface"],
  ["graphics", "Variable fonts (font axes: weight, width, slant, optical size)",
   "gfx/truetype.h: fvar's axes, avar's bend, gvar's deltas -- listed and " +
   "inferred along each contour -- with composite offsets and phantom " +
   "points, and HVAR's advances; Font::withVariation(\"wght\", 650). " +
   "Matched against fontTools on every glyph of Bahnschrift, Segoe UI " +
   "Variable, Cascadia Code and Sitka at several locations, and checked on " +
   "a fixture built by make_variable.py. CFF2 -- variable CFF outlines -- is " +
   "not read, and GPOS kerning stays the default design's",
   "struct FontAxis"],
  ["graphics", "Font", "gfx/font.h: metrics, kerning, UTF-8, justification", "class Font"],
  

  // ── gui_basics ──────────────────────────────────────────────────────
  //
  // No longer out of scope: native is becoming the default face of a
  // generated plugin, so the widget set has to exist here rather than in a
  // browser.
  ["gui", "Component", "gfx/component.h: tree, z-order, clipping, damage", "class Component"],
  ["gui", "MouseEvent / MouseListener", "MouseEvent + MouseRouter", "class MouseRouter"],
  ["gui", "mouse capture (dragging outside a component)", "MouseRouter::captured", "captured_"],
  ["gui", "Slider (rotary, linear, and bar)",
   "gfx/widgets.h: five styles -- Rotary, LinearHorizontal, LinearVertical, " +
   "LinearBar, LinearBarVertical. The bars are the reference's LinearBar pair: a filled " +
   "proportion with no thumb, which is the shape most modern plugins draw a " +
   "level as, because a column of them compares by eye without finding each " +
   "thumb first. A bar's click arithmetic spans the WHOLE rectangle -- no " +
   "thumb means no thumb radius stolen from either end, and using the thumbed " +
   "arithmetic would make a bar reach 1.0 seven pixels before its own edge.\n" +
   "      RotaryDrag decides which way a knob reads a drag: Vertical (the " +
   "default, what nearly every plugin does), Horizontal, or Both -- which sums " +
   "the axes so the diagonal drag people actually make works, where a " +
   "vertical-only knob silently discards half the gesture.\n" +
   "      Linear sliders JUMP to the click position and rotaries do not, each " +
   "with its stated reason; double-click returns to the default without " +
   "recording a drag gesture; shift is fine-adjust at a quarter speed",
   "class Slider"],
  ["gui", "Slider::setScrollWheelEnabled (as WheelMode)",
   "gfx/widgets.h: WheelMode on Widget -- Never, Always, DeferToScroll -- " +
   "where the reference has only the on/off pair. The third value is the default and " +
   "the reason the setting exists here at all: inside a scrolling editor, a " +
   "user reaching for a parameter further down scrolls, the pointer crosses a " +
   "knob, and the next notch used to move that knob instead of the list -- an " +
   "accidental edit, reported to the host as automation, made while only " +
   "navigating.\n" +
   "      DeferToScroll gives the wheel to the control unless an ancestor " +
   "would actually scroll right now AND no drag is in progress. Both halves " +
   "matter: a Viewport that fits its content refuses the wheel (so small " +
   "editors behave exactly as before), and a wheel mid-drag always belongs to " +
   "the control -- the list must not scroll out from under the hand adjusting " +
   "it, which is the rule that was already written into the old comment",
   "enum class WheelMode"],
  ["gui", "Button / ToggleButton", "gfx/widgets.h", "class Button"],
  ["gui", "Label", "gfx/widgets.h", "class Label"],
  ["gui", "ComboBox", "gfx/widgets.h", "class ComboBox"],
  ["gui", "LookAndFeel", "gfx/lookandfeel.h, swappable per component subtree", "class LookAndFeel"],
  ["gui", "PopupMenu",
   "gfx/popup.h: a top-level window above everything, holding the mouse until " +
   "dismissed. WS_POPUP with a capture on Windows, override-redirect with a " +
   "pointer grab on X11", "class PopupMenu"],
  ["gui", "ComboBox drop-down",
   "the box asks for a menu and never opens one itself -- a widget knows " +
   "nothing about windows -- and cycles where nobody is listening",
   "std::function<void(ComboBox&)> onOpenMenu"],
  ["gui", "ComponentPeer (Windows)", "gfx/window_win32.h: window, blit, input", "class NativeWindow"],
  ["gui", "ComponentPeer (Linux)",
   "gfx/window_x11.h: window, blit, input. libX11 is OPENED, never linked, so " +
   "a plugin still loads on a Wayland-only session and says why it has no " +
   "native window", "class NativeWindowX11"],
  ["gui", "ComponentPeer (macOS)",
   "gfx/window_cocoa.h: an NSView registered against the Objective-C runtime, " +
   "so no .mm file and no mixed-language build. COMPILED on macOS by CI since " +
   "2026-09-01, never RUN: no macOS test opens a window, and there is no Mac " +
   "in this loop. The Win32 and X11 peers are checked against real windows; " +
   "this one is not",
   "class NativeWindowCocoa"],
  ["gui", "the Cocoa files, read by a compiler",
   "tests/cocoa_syntax.cpp against an Objective-C runtime shim. webview_cocoa.h " +
   "is every Mac plugin's editor today and no compiler had ever looked at it. " +
   "au_view.h is still not covered -- clap_wrapper.h selects _WIN32 first, so " +
   "the chain would compile the Windows branches and report success",
   "SonoreNativeView"],
  ["gui", "the host's event loop (Linux)",
   "clap.posix-fd-support for the X socket, clap.timer-support as the fallback. " +
   "X gives a plugin no clock and no loop of its own; a wrapper that ignored " +
   "both would show a window that never responds",
   "CLAP_EXT_POSIX_FD_SUPPORT"],
  ["gui", "Timer (the editor clock)",
   "NativeWindow::onTick -- 33 ms, so automation and preset loads reach the " +
   "controls without the mouse moving", "std::function<void()> onTick"],

  // ── gui_extra / the editor a host opens ─────────────────────────────
  //
  // the reference's AudioProcessorEditor is where a plugin's face meets its parameters,
  // and GenericAudioProcessorEditor is the floor under a plugin that has none.
  // Both are here now, and the choice between native and web is the part the reference
  // does not have to make.
  ["gui", "AudioProcessorEditor (the format-facing editor)",
   "gfx/native_editor.h: window, clock and content as one object each wrapper " +
   "opens. A plugin's OWN face derives from gfx::AudioProcessorEditor " +
   "(plugin_editor.h: editorTick on the window's clock, openComboMenu) and its " +
   "DSP's createEditor(host) hands it to the CLAP, VST3 and AU wrappers, which " +
   "show it with the generic editor's tooltips, resize border and drop-downs; " +
   "examples/showcase is one. Tested in native_window_test (RUN on Windows: a " +
   "real window, its clock, a host resize, the drop-down, the fallback) and " +
   "showcase_test (the example's editor rendered and driven headless)",
   "class NativeEditor"],
  ["gui", "GenericAudioProcessorEditor",
   "gfx/plugin_editor.h -- a row per parameter, so no plugin is faceless",
   "class GenericEditor"],
  ["gui", "SliderParameterAttachment",
   "gfx/plugin_editor.h, on the host's own value curve", "class SliderAttachment"],
  ["gui", "ComboBoxParameterAttachment", "gfx/plugin_editor.h", "class ComboAttachment"],
  ["gui", "(the reference picks its editor at compile time)",
   "chooseEditorBackend: native by default, web when a plugin supplied a page, " +
   "and a named reason whenever it is neither", "inline EditorChoice chooseEditorBackend"],
  ["gui", "Typeface::createSystemTypefaceFor",
   "gfx/system_font.h -- found on the machine, never shipped, so no font " +
   "licence rides along in every generated plugin. A character the face lacks " +
   "-- a preset named in Japanese, a folder in Korean -- is drawn from the " +
   "faces the desktop falls back to (fallbackTypeface), each asked only about " +
   "the scripts it is for and read the first time one of their characters " +
   "needs it", "inline std::shared_ptr<Typeface> systemTypeface"],

  // ── gui_basics: the rows this map did not have ──────────────────────
  //
  // Added because "0 real gaps" was flattering itself. Every row in this file
  // is one somebody chose to write down, so a count of zero only ever meant
  // zero among the things already listed -- and the reference has a keyboard, text
  // entry, scrolling and layout that nothing here had even claimed to be
  // missing. An instrument that measures only what it already knows about is
  // not an instrument.
  ["gui", "KeyPress / keyboard focus",
   "gfx/component.h: focus on the router, an unhandled key walking UP the tree, " +
   "and Tab traversal in tree order. Delivered by all three peers -- WM_KEYDOWN " +
   "plus WM_CHAR, XLookupString, and keyDown: -- three shapes, one KeyPress",
   "struct KeyPress"],
  ["gui", "TextEditor",
   "gfx/text_editor.h: caret, selection, character filtering, length limit, " +
   "read-only. Stores UTF-32 because a caret moves over CHARACTERS, so an " +
   "accent is one backspace and not two", "class TextEditor"],
  ["gui", "TextEditor::setMultiLine (a text field of several lines)",
   "gfx/text_area.h: TextArea, a separate class, as text_editor.h argues a " +
   "multi-line field must be: lines wrapped where Unicode's line breaking " +
   "algorithm allows (Font::wrapLines, the WrappedLine positions wrapText " +
   "copies from), up and down keeping the caret's x, Home and End by line, " +
   "Ctrl+Home and Ctrl+End, Page Up and Down, a vertical scroll that follows " +
   "the caret and the wheel, a selection across lines, a double click for a " +
   "word and a triple for a paragraph; line breaks stored as LF whatever " +
   "the clipboard brought", "class TextArea"],
  ["gui", "Viewport (scrolling)",
   "gfx/viewport.h. Scrolls by putting the content at a NEGATIVE position and " +
   "letting the tree's own translate-and-clip do the rest, so there is no " +
   "second set of clipping rules to keep in step", "class Viewport"],
  ["gui", "ScrollBar",
   "gfx/viewport.h: proportional thumb with a minimum grabbable size, and a " +
   "click on the track pages rather than jumping", "class ScrollBar"],
  ["gui", "FlexBox",
   "gfx/layout.h, with CSS's real size resolution -- freeze an item that hits " +
   "a limit, share what is left among the rest. GenericEditor's rows are laid " +
   "out with it rather than by subtraction", "class FlexBox"],
  ["gui", "Grid",
   "gfx/layout.h: pixel, fr and auto tracks, gaps, and spans that reach across " +
   "the gaps they cover -- missing that leaves a spanning cell exactly one gap " +
   "short", "class Grid"],
  ["gui", "TooltipWindow",
   "gfx/tooltip.h. The timing takes the clock as an ARGUMENT, so appear-after, " +
   "hold-then-go and move-resets-the-clock are exact tests rather than sleeps. " +
   "The window is the same platform popup with the grab turned OFF -- a tooltip " +
   "that grabbed would block the control it describes", "class TooltipManager"],
  ["gui", "DragAndDropContainer / DragAndDropTarget",
   "on the MouseRouter, not a separate container: a drag is per-window state " +
   "exactly like capture, hover and focus, and the router already owns those " +
   "three. A target is found by walking UP from the pointer, so a panel with a " +
   "knob in it is still droppable", "void startDrag"],
  ["gui", "FileDragAndDropTarget",
   "Win32 WM_DROPFILES and X11 XDND, both routed to the same component " +
   "interface. XDND is a handshake -- the source waits for an answer BEFORE " +
   "the user may release -- so the X11 peer lights a target on hover where the " +
   "Win32 one cannot. macOS is written but never run",
   "isInterestedInFileDrag"],
  ["gui", "ResizableCornerComponent / ComponentBoundsConstrainer",
   "gfx/tooltip.h: a grip that ASKS for a size rather than taking one -- a " +
   "plugin editor does not own its window -- and a constrainer whose aspect " +
   "ratio is re-clamped so it can never return a size violating its own limits",
   "class ResizableCorner"],

  // ── audio_utils: the components that are specific to audio ──────────
  ["gui", "MidiKeyboardComponent",
   "gfx/midi_keyboard.h: real piano geometry (the black keys are NOT evenly " +
   "spaced), velocity from where the key was struck, glissando on a drag, and " +
   "keys lit by what the DSP is sounding rather than by what was clicked",
   "class MidiKeyboard"],
  // Claimed as a gap for about four minutes, until the proof-of-absence regex
  // on the row said "that symbol IS in the headers". It has been there all
  // along -- min/max/RMS reduction, in thumbnail.h. This is exactly the check
  // that exists because a grep written to find nothing will always find
  // nothing, and it works in the direction that catches a wrong claim of
  // ABSENCE rather than a wrong claim of presence.
  ["gui", "AudioThumbnail",
   "thumbnail.h: min/max/RMS reduction, so a sampler can draw what it loaded",
   "class AudioThumbnail"],
  ["gui", "AudioVisualiserComponent",
   "gfx/audio_display.h over scope_buffer.h. The buffer is fed from process() " +
   "and is owned by the wrapper, not the editor, so a window opened mid-session " +
   "shows the last second of audio instead of starting blank -- and rt_safety " +
   "arms a counting operator new around process(), so 'allocates nothing' is " +
   "measured rather than claimed", "class AudioVisualiser"],
  ["gui", "AudioThumbnail, drawn",
   "gfx/audio_display.h: WaveformView with a playhead and a drag-selection, " +
   "normalised to the peak so a quiet recording still fills its box",
   "class WaveformView"],

  // ── graphics: the same audit ────────────────────────────────────────
  ["graphics", "ImageFileFormat (PNG)",
   "gfx/png.h over gfx/inflate.h -- DEFLATE written rather than vendored, " +
   "because every length and distance in the stream is attacker-controlled and " +
   "the bounds checks are the point. All five row filters, palettes at 1/2/4/8 " +
   "bits, 16-bit reduced to 8, tRNS, and multiple IDAT chunks. JPEG and GIF " +
   "are decoded beside it (gfx/jpeg.h, gfx/gif.h; see their row), and " +
   "gfx/image_format.h tells the three apart by their first bytes",
   "class PngDecoder"],
  ["graphics", "Graphics::drawImageTransformed (rotation and shear)",
   "graphics.h. drawImage honours translation and scale and its own comment " +
   "said so; that is not enough for a needle on a VU meter or a knob cap that " +
   "turns.\n" +
   "      Destination pixels are mapped BACK through the inverse and sampled " +
   "there. Walking the source and scattering forwards leaves holes wherever " +
   "the mapping stretches, which is the speckled look of every hand-rolled " +
   "rotation -- so the test counts the holes in the middle of a rotated fill " +
   "and there are none. BILINEAR, because a rotated edge sampled with nearest " +
   "is a staircase and the staircase is the thing people notice; 68 pixels " +
   "along one edge come out partly covered, which nearest gives none of.\n" +
   "      Interpolated in PREMULTIPLIED alpha, which is what makes the plain " +
   "weighted average correct: a straight-alpha resampler pulls a fading edge " +
   "towards whatever the transparent texels' colour channels hold, and for a " +
   "cleared bitmap that is black -- the dark halo around every naively " +
   "resampled logo. Checked by rotating a WHITE square onto WHITE and counting " +
   "the pixels that darkened, which is zero. A transform with no inverse draws " +
   "nothing rather than filling the clip with NaN",
   "void drawImageTransformed"],
  ["graphics", "FillType::setTiledImage",
   "graphics.h and bitmap.h: a bitmap repeated across the plane, for a panel " +
   "texture or a grid. Through the SAME per-pixel hook in the rasteriser that " +
   "a gradient uses, so a tiled fill clips and antialiases exactly like every " +
   "other fill rather than being a special blit with its own edges -- a tiled " +
   "ELLIPSE leaves its corners empty, which is the cheapest proof of it.\n" +
   "      Nearest sampling, deliberately: a texture at natural size wants to " +
   "stay crisp and bilinear at 1:1 blurs every pixel with its neighbours for " +
   "nothing. The origin is given in USER space and transformed once, so a " +
   "texture scrolls with the component it is painted on rather than staying " +
   "nailed to the window. And the wrap is a TRUE modulo -- C's % returns a " +
   "negative answer for a negative operand, so a texture built on it mirrors " +
   "everywhere left of or above its origin",
   "struct TiledImage"],
  ["graphics", "Graphics::drawImage",
   "graphics.h: translate and scale, sampled at pixel centres with NEAREST, " +
   "composited with the same premultiplied `over` as every other fill. Kept " +
   "beside drawImageTransformed rather than replaced by it: this walks the " +
   "destination rectangle with two scale factors and never inverts anything, " +
   "which is what a logo drawn square wants -- crisp, and cheap. The general " +
   "one is for when the transform actually turns", "void drawImage"],
  ["graphics", "Drawable / SVG",
   "gfx/svg.h: what Figma, Illustrator, Inkscape and the icon sets write -- " +
   "the seven shapes, groups, nested viewports, <defs>, <symbol> and <use>; " +
   "linear and radial gradients with every unit, transform, spread and focal " +
   "attribute; clip-path and mask; group opacity as a layer; stroke caps, " +
   "joins, miter limits and dashes; presentation attributes, style= and " +
   "<style> sheets with type, class and id selectors; every CSS colour " +
   "spelling and currentColor, which draw() can be handed so an icon takes " +
   "its colour from the widget. The one piece of real geometry is the arc -- " +
   "SVG states it endpoint-first and a renderer needs centre-first, " +
   "including the spec's rule that radii too small to span the endpoints are " +
   "scaled UP. Checked against resvg: 332 icons and logos found on a " +
   "development machine, and a file per feature, agree to within a pixel's " +
   "antialiasing. <text> and <tspan> are drawn too, as DrawableText runs, " +
   "their faces matched by FontMatcher (see the DrawableText row). Images, " +
   "patterns, markers, filters and blend modes are named as unsupported rather " +
   "than silently dropped; a filter is skipped and the element drawn without it", "class Drawable"],

  // ── a fifth audit, of gui_basics again ───────────────────────────────────
  //
  // Reading down the reference's own class list rather than checking the rows already
  // written. Four more, and the first is not a nicety -- it is a control that
  // is BROKEN without it.
  ["gui", "SystemClipboard, and copy/paste in TextEditor",
   "gfx/clipboard.h. Windows opens the global object WITH RETRIES, because a " +
   "single attempt fails whenever anything else on the desktop touches it -- " +
   "which is the \"sometimes copy does not work\" complaint nobody can " +
   "reproduce. X11 has no clipboard at all: it owns a SELECTION and answers " +
   "requests for it, so copying means agreeing to serve data later. A paste " +
   "obeys the field's own filter and length limit",
   "struct Clipboard"],
  ["gui", "Desktop::Displays (more than one monitor)",
   "gfx/displays.h, filled by all three peers -- EnumDisplayMonitors, Xinerama, " +
   "NSScreen. Placement takes the screen the ANCHOR is on, as a RECTANGLE " +
   "rather than a size: a monitor left of or above the primary starts at " +
   "NEGATIVE coordinates, and every 0..width test is wrong there and right on " +
   "one monitor, which is why it survived. Work areas too, so a menu does not " +
   "go under the taskbar", "class Displays"],
  ["gui", "ApplicationCommandManager / ApplicationCommandTarget",
   "commands.h. Undo, save preset, bypass -- a plugin has a handful of actions " +
   "that are not parameters, and without a command each is a lambda hanging off " +
   "a button, invisible to the keyboard and impossible to rebind. A command has " +
   "a name, an identity and a state, so a button, a menu item and a shortcut " +
   "are three ways to one thing and grey out together.\n" +
   "      Two rules decide whether it is usable. A shortcut belongs to ONE " +
   "command, and assigning it to a second TAKES it from the first -- two " +
   "commands firing on one key is worse than either being unbound. And a " +
   "DISABLED command does not consume the key: a swallowed key that does " +
   "nothing is indistinguishable from a broken keyboard, where an unconsumed " +
   "one falls through to whatever else wanted it.\n" +
   "      The component tree gets first refusal, through " +
   "MouseRouter::onUnhandledKey. A text field must have Ctrl+A for select-all " +
   "rather than a global command stealing it from under the caret, and that " +
   "order is tested both ways round",
   "class CommandManager"],
  ["gui", "KeyPressMappingSet",
   "commands.h: Shortcut, and the save/load either side of it. A user who wants " +
   "Ctrl+S somewhere else can move it and it survives a restart. Only the " +
   "MAPPINGS are written, never the commands -- those are the plugin's own and " +
   "are registered in code every load, so a stale settings file cannot " +
   "resurrect a command a later version removed. A mapping naming a command " +
   "this build no longer has is SKIPPED rather than failing the load, because " +
   "removing one command in an update must not lose every other binding the " +
   "user ever set. And a shortcut can describe itself -- \"Ctrl+Shift+Z\" -- " +
   "with its modifiers in a fixed order, because a shortcut nobody can see is " +
   "one nobody uses",
   "struct Shortcut"],
  ["gui", "keyboard operation of every control",
   "gfx/widgets.h. Focus was only half of it: every control could be focused by " +
   "nothing and operated by nothing, so Tab walked past the knobs and the arrow " +
   "keys did nothing once you were on one -- a plugin made by this SDK could " +
   "not be used without a mouse AT ALL. A slider now takes arrows (1%), " +
   "shift-arrows (0.1%), page keys (10%) and Home/End; a button takes Space and " +
   "Return; a combo box takes arrows and Home/End, and stops at the ends rather " +
   "than wrapping, because a parameter that wrapped would jump from loudest to " +
   "quietest on one keystroke. Each keystroke is ONE gesture bracket, so the " +
   "host records an edit undo can take back, and it balances even at the end of " +
   "the range where the value cannot move. The tests that matter are the two " +
   "that are easy to skip: that Tab visits exactly the three controls of a " +
   "three-parameter editor and not the hidden fields inside its value boxes, " +
   "and that focusing something changes 232 pixels -- because a focus ring a " +
   "LookAndFeel forgot to draw is keyboard support that is present and useless",
   "bool keyPressed"],
  ["gui", "Component (the widget base)",
   "gfx/widgets.h: Widget, between Component and every control -- it owns the " +
   "LookAndFeel lookup, the font, the enabled and hovered state and the focus " +
   "ring. A control that inherited Component directly would answer \"what does " +
   "this look like\" for itself, and forty of them would answer it forty ways",
   "class Widget"],
  ["gui", "DragAndDropContainer::startDragging",
   "gfx/component.h: DragSource, the description of what is being dragged. On " +
   "the ROUTER rather than in a container, because a drag is per-window state " +
   "exactly like capture and focus, and the router already owns those",
   "struct DragSource"],
  ["gui", "PopupWindow (the platform half of a menu)",
   "gfx/native_editor.h: the top-level window a PopupMenu is drawn into, with " +
   "the grab that keeps it up. Separate from PopupMenu because a menu is a " +
   "Component and a window is not something a Component can know about",
   "class PopupWindow"],
  ["gui", "ComponentBoundsConstrainer",
   "gfx/tooltip.h: BoundsConstrainer. Minimum and maximum size with an aspect " +
   "ratio, re-clamped so it can never return a size that violates its own " +
   "limits -- which a naive one does the moment the ratio and the minimum " +
   "disagree",
   "class BoundsConstrainer"],
  ["gui", "SelectedItemSet",
   "gfx/selection.h. A vector rather than a set, because a selection is a " +
   "handful of items and ORDER matters -- a caller asking for \"the first one\" " +
   "means the one they clicked first, not whatever a hash produced. One " +
   "notification per CHANGE rather than per item, so a lasso covering forty " +
   "things repaints once.\n" +
   "      The click rule worth having is the subtle one: a plain press on " +
   "something ALREADY selected must not collapse the selection, or a group of " +
   "four becomes impossible to drag -- the press deselects three of them before " +
   "the drag begins. So it returns \"decide again on release\", and the collapse " +
   "happens there and only if nothing was dragged. Both halves are tested, " +
   "including that a release AFTER a drag keeps the group",
   "class SelectedItemSet"],
  ["gui", "LassoComponent / LassoSource",
   "gfx/selection.h: the rubber band, and only the rubber band. WHAT it covers " +
   "is the owner's business -- this has no idea what the items are or how to " +
   "compare them, and a version that did would need a model interface for every " +
   "kind of thing anybody ever lassoes. Normalised, so dragging up-and-left " +
   "gives the same rectangle as down-and-right; a lasso that forgets that " +
   "selects nothing in three of the four directions. Transparent to the mouse, " +
   "because the press that started it went to whatever is underneath and the " +
   "release has to reach there too",
   "class LassoComponent"],
  ["gui", "ComponentDragger",
   "gfx/selection.h, working in ROOT coordinates, which is the whole trick. A " +
   "dragger using the event's LOCAL position moves the component, which moves " +
   "the coordinate system the next event arrives in, which moves it again -- " +
   "the classic runaway where a dragged thing accelerates away from the " +
   "pointer. The test that proves the difference is the one that makes the same " +
   "total travel in two steps and checks it lands in exactly the same place as " +
   "in one. Confined to a rectangle, because a node dragged outside its panel " +
   "is one the user cannot reach again",
   "class ComponentDragger"],
  ["gui", "StretchableLayoutManager / StretchableLayoutResizerBar",
   "gfx/splitter.h. A browser down the left and its detail on the right; a " +
   "preset list above its parameters. The moment an editor has two regions " +
   "whose right proportion depends on what the user is doing, a fixed split is " +
   "wrong for somebody -- usually the one with a long preset name or a deep " +
   "folder tree.\n" +
   "      the reference\'s size convention, because it is genuinely the clearest: " +
   "POSITIVE is pixels, NEGATIVE is a fraction of the total, so a bar stays six " +
   "pixels while the panes either side stay a third and two thirds through " +
   "every resize. The same freeze loop FlexBox uses, for the same reason -- " +
   "distribute-then-clamp leaves the clamped items\' share unspent and the row " +
   "comes up short by exactly what its constrained members refused.\n" +
   "      The property that makes it a splitter rather than a decoration is " +
   "the one checked hardest: a boundary the USER moved survives the next " +
   "layout. A bar takes its OWN index and moves the panes either side of it -- " +
   "pairing item N with item N+1 pairs a pane with the bar, so a hundred-pixel " +
   "drag moves the boundary by the six pixels the bar can spare, which is what " +
   "the first version did",
   "class StretchableLayoutManager"],
  ["gui", "ImageComponent",
   "gfx/widgets2.h. Four lines anybody can write and four lines everybody " +
   "writes slightly differently -- usually without the placement rule, so the " +
   "logo distorts the first time a host resizes the editor. Contained by " +
   "default, measured: a 40x20 image in a 100x100 box covers exactly 100x50. " +
   "The bitmap is NOT owned, because a plugin\'s artwork is decoded once and " +
   "drawn by however many components want it",
   "class ImageComponent"],
  ["gui", "HyperlinkButton",
   "gfx/widgets2.h -- and it does NOT open the URL itself, which is a decision " +
   "rather than an omission. Launching a browser means a shell call from a " +
   "process the user did not start on a thread the host owns, and a host " +
   "mid-render when that blocks is a host dropping audio. The owner gets the " +
   "URL and decides: a standalone opens it, a plugin can ask first. Underlined " +
   "on hover only, which is the convention that distinguishes a link from " +
   "coloured text, and a reader hears where it goes rather than having to " +
   "follow it to find out",
   "class HyperlinkButton"],
  ["gui", "FileBrowserComponent / FileTreeComponent / DirectoryContentsList",
   "gfx/file_browser.h -- TreeView over listDirectory, so a hundred-thousand-" +
   "file library costs one readdir per folder the user actually opens. NOT the " +
   "same thing as file_dialog.h, which opens the OS chooser and is right for " +
   "\"load an impulse response\": a browser that lives IN the editor stays open " +
   "while you audition, keeps its place between plugins, and does not take " +
   "focus off the host every time you look at another file. Every sampler worth " +
   "using has one and none of them use the system dialog for it.\n" +
   "      With no start folder the root is SYNTHETIC -- one child per drive on " +
   "Windows, \"/\" elsewhere -- because there is no single top on Windows and a " +
   "library on D: cannot be reached from C: by going up. selectPath opens every " +
   "folder down to a saved selection, and compares on a separator boundary so " +
   "\"/lib/Drums2\" is not treated as being inside \"/lib/Drums\". Refresh " +
   "reopens what was open and restores the selection, because a refresh that " +
   "collapsed the tree would be worse than no button",
   "class FileBrowser"],
  ["gui", "TableListBox / TableHeaderComponent",
   "gfx/table.h: sortable, resizable columns over virtual rows. It does NOT " +
   "reorder your data -- clicking a header reports which column and which " +
   "direction and the OWNER re-sorts. That is the reference\'s contract and the only one " +
   "without an ambiguity: a table keeping its own permutation has to decide " +
   "whether getCellText(3) means the fourth row of the model or the fourth on " +
   "screen, and every bug in every such table is that question answered " +
   "differently in two places. It also means a numeric column sorts " +
   "numerically, which a table sorting its own cell TEXT cannot do -- \"10\" " +
   "before \"9\", and nobody notices until the list is long enough to matter.\n" +
   "      The same header again reverses; a DIFFERENT one starts forwards, " +
   "because \"sort by author\" almost always means A to Z the first time. A " +
   "column can refuse to be sorted by. Dragging a boundary resizes it and " +
   "cannot take it below its minimum, which is what stops a column becoming a " +
   "line nobody can grab again",
   "class TableListBox"],
  ["gui", "TreeView / TreeViewItem",
   "gfx/treeview.h. LAZY, because the alternative is unusable: a sample " +
   "library is a hundred thousand files, and building the tree to draw twelve " +
   "rows means an editor that does not appear -- on a network share, one that " +
   "never appears. A node says it MIGHT have children before anyone knows " +
   "whether it does, which is what lets a folder show a twisty without being " +
   "read; the children are built once, on first open, and a folder that turns " +
   "out to be empty gives its twisty back rather than leaving something to " +
   "click that does nothing forever.\n" +
   "      VIRTUAL, like ListBox: open nodes are flattened into a row list and " +
   "only the rows on screen are drawn. And the keyboard pair that decides " +
   "whether a tree is navigable at all -- Right opens a closed folder and steps " +
   "INTO an open one, Left closes an open folder and steps OUT of anything " +
   "else. Right-always-opens traps a user in a folder they cannot leave without " +
   "a mouse, which is the version most hand-written trees ship",
   "class TreeView"],
  ["gui", "AccessibilityHandler (roles, names and values)",
   "gfx/accessible_info.h and gfx/accessibility.h. Every component answers what " +
   "it IS -- a role from a closed set every platform can map, a name, the value " +
   "it reads, a range where it has one -- and collectAccessible walks the tree " +
   "a reader would want to see. Built on demand rather than stored, because a " +
   "second copy of a knob\'s value is a copy that can be stale at the moment " +
   "somebody asks.\n" +
   "      The checks that matter are about what is NOT said: a generated " +
   "three-parameter editor is exactly four elements -- itself and its three " +
   "controls -- because the name label beside each knob and the value readout " +
   "under it both repeat what the control already says, and a reader that read " +
   "all three would announce every parameter three times. A ListBox reports a " +
   "RANGE rather than four hundred children. A MidiKeyboard is one element, not " +
   "128. Ignoring a group keeps its contents where hiding one does not, which " +
   "is the whole reason those are separate flags",
   "AccessibleInfo accessibleInfo"],
  ["gui", "the accessibility bridge, Windows (UI Automation)",
   "gfx/uia_win32.h -- a COM provider behind WM_GETOBJECT, written by hand " +
   "rather than through ATL, which would have put a second toolchain " +
   "requirement on every generated plugin for the sake of three IUnknown " +
   "methods. RootProvider is the fragment root and the window; ElementProvider " +
   "is one per accessible component, offering Value where there is something " +
   "to read and RangeValue only where there is a genuine range -- offering it " +
   "on a button would give a reader a percentage to announce for a thing with " +
   "no position. Both read a SNAPSHOT of collectAccessible rather than the live " +
   "tree, because UIA calls in on its own thread while the editor repaints on " +
   "another.\n" +
   "      VERIFIED by being the screen reader. Windows ships the UIA CLIENT api " +
   "too, so native_window_test creates the same IUIAutomation object NVDA and " +
   "Narrator create, on a worker thread with the UI thread pumping, points it " +
   "at the editor\'s real HWND and asserts what comes back: slider \"Gain\" " +
   "reading \"-6.0 dB\" over a 0..1 range, slider \"Freq\" reading \"440 Hz\", " +
   "combo box \"Shape\" reading \"Saw\". If that passes a screen reader hears " +
   "it, because that IS what a screen reader does.\n" +
   "      And it LISTENS: the focus moving to another control and the focused " +
   "control's value changing are raised as UIA events from the editor's clock, " +
   "so a value moved by the host's automation is said without being asked for " +
   "-- at most four times a second, the last value always. native_window_test " +
   "registers a client for both and hears the focus land on Gain and -12 then " +
   "-18 dB as the host moves it",
   "class RootProvider"],
  ["gui", "the accessibility bridge, macOS (NSAccessibility)",
   "gfx/accessibility_mac.h: MacAccessibilityRoot, the NSAccessibility bridge " +
   "over the same model the UIA bridge reads (accessibility.h), written " +
   "against the Objective-C runtime as window_cocoa.h is: one " +
   "NSAccessibilityElement subclass registered with objc_allocateClassPair, " +
   "one instance per accessible component, KEPT across rebuilds " +
   "(detail::matchAccessibleNodes, by component and role) because VoiceOver " +
   "identifies an element by the object -- and told AXUIElementDestroyed when " +
   "its component goes. Each answers role and subrole, role description, " +
   "label or title, value (a number for a slider with its text as " +
   "AXValueDescription, 0 or 1 for a toggle, the text for a field or a " +
   "choice) and its range, help, frame on the y-up screen, parent, children " +
   "and window, focused, enabled and selected; and performs AXPress, " +
   "AXIncrement, AXDecrement and AXShowMenu, takes a value and takes the " +
   "focus -- through Component::performAccessibleAction, setAccessibleValue " +
   "and setAccessibleText, i.e. through each control's own key path, one " +
   "bracketed edit each. Which of those apply to which element is " +
   "isAccessibilitySelectorAllowed:, read from the description " +
   "(AccessibleInfo::actions, valueSettable). The roles come from " +
   "accessibleRoleMapping (AccessibleRoleMapping): ONE table, which " +
   "uia_win32.h now reads too -- a ComboBox is an AXPopUpButton, a Table an " +
   "AXList (AXTable promises AXRows as DataGrid promises GridPattern), a " +
   "ToggleButton AXCheckBox/AXToggle, a Dialog an AXGroup still described as " +
   "a dialog. Announced on the view\'s clock while a reader is around " +
   "(isVoiceOverEnabled, or asked in the last 5 s): focus changed, the " +
   "focused value changed (at most four times a second, the last always), " +
   "layout changed, element destroyed -- the UIA bridge\'s rules, in " +
   "detail::AccessibilityAnnouncer. Hooked into window_cocoa.h as " +
   "uia_win32.h is into window_win32.h: the view is an AXGroup named by " +
   "setAccessibleWindowName whose children, hit test and focused element " +
   "come from the bridge, created at open and detached before close. " +
   "AxPoint, AxSize and AxRect mirror NSPoint, NSSize and NSRect; " +
   "AppKitAccessibilityApi is the two AppKit C functions it calls, dlsym\'d.\n" +
   "      RUN on Windows, the half that can be: tests/mac_input_test.cpp " +
   "drives the role table (UIA\'s ids unchanged, the AX answers expected), " +
   "every action through real widgets, the element matching, the " +
   "announcer\'s rate limits and the flip with known numbers; " +
   "native_window_test still reads Gain, Freq and Shape through a real UIA " +
   "client with the table behind it. COMPILED, NOT RUN: the bridge itself, " +
   "with MSVC /W4 and clang against tests/au_shim, for arm64 and x86_64 " +
   "(tests/mac_syntax_mac_input.cpp). No Mac has executed a line of it. " +
   "tests/mac_input_selftest.cpp, macOS only, is what a Mac developer runs: " +
   "it asks AppKit through the attribute API -- the path the accessibility " +
   "server takes for VoiceOver -- for the tree, performs AXIncrement and " +
   "AXPress, sets AXValue and AXFocused, hit-tests, and checks every frame " +
   "against AppKit\'s own conversion. Until somebody has, this is written, " +
   "not known to work",
   "class MacAccessibilityRoot"],
  ["gui", "AlertWindow / DialogWindow",
   "gfx/alert.h: AlertOverlay. A plugin had NO way to tell the user anything -- " +
   "a sample that failed to load, a preset from a newer version, an action " +
   "about to throw away an edit could only fail silently, and \"it just didn\'t " +
   "load\" is the worst bug report to get and the easiest to prevent. An " +
   "OVERLAY rather than a window, deliberately: a modal window owned by a " +
   "plugin can end up behind the host\'s own, where it cannot be seen or " +
   "dismissed while the host waits on a click that can never happen. Modal in " +
   "behaviour and not in implementation -- nothing blocks, the answer arrives " +
   "through a callback, because a plugin that blocked its editor thread would " +
   "stop the host\'s message loop. Return is the default answer, Escape the " +
   "cancel, and the checked claim is the dull one: the knob behind it really " +
   "is dead, mouse and wheel both",
   "class AlertOverlay"],
  ["gui", "CallOutBox",
   "gfx/alert.h: a bubble that POINTS at a control, for what a tooltip is too " +
   "small for and a dialog too much for. Below the target when it fits, above " +
   "when it does not, and always inside the editor -- the same rule PopupMenu " +
   "follows, because a bubble drawn half outside is clipped by the window and " +
   "the half with the buttons on it is simply not there. A click outside " +
   "dismisses it; a click inside belongs to its content",
   "class CallOutBox"],
  ["gui", "GroupComponent",
   "gfx/group.h. A container rather than a decoration: children are positioned " +
   "inside it, contentBounds() says where they may go, and moving the group " +
   "moves what is in it. The border is drawn with a GAP where the caption sits " +
   "rather than the caption drawn over a complete line -- identical over a " +
   "solid ground, and over a gradient or a texture the difference is a " +
   "rectangle of panel colour with the line showing through the letters. " +
   "Measured by painting onto magenta and counting what still shows through",
   "class GroupComponent"],
  ["gui", "Slider text box / Label::setEditable (typing a value in)",
   "gfx/value_box.h, and GenericEditor uses it for every readout -- so this " +
   "reaches every generated plugin rather than being available to one that " +
   "asks. The number under a knob was a Label, which is not editable, so the " +
   "only way to reach a value was to drag: fine for a filter sweep, useless " +
   "for \"exactly -6.0 dB\", which is a thing engineers do constantly and then " +
   "check. It accepts what it PRINTS, suffix and all, through the same " +
   "parseParamValue the host's own value box uses -- two parsers for one " +
   "parameter is two places for \"2.3 kHz\" to mean different things. It is ONE " +
   "gesture, so the DAW records an edit rather than a point and undo takes it " +
   "back. And text it cannot use leaves the field OPEN with the text selected " +
   "rather than reverting, because reverting is indistinguishable from the " +
   "keyboard being ignored",
   "class ValueBox"],
  ["gui", "Desktop::setGlobalScaleFactor / HiDPI",
   "gfx/backing.h -- the one object that knows the difference between a logical " +
   "unit and a device pixel, and the only one. The component tree works in " +
   "logical units and never learns what scale it is drawn at; the backing store " +
   "keeps a bitmap of logical x scale device pixels, paints the root under a " +
   "scale transform, converts damage on the way out and pointer positions on " +
   "the way in. Which means the text is SHARPER at 200%, not bigger and " +
   "blurrier: glyphs are outlines rasterised at device resolution rather than a " +
   "logical-resolution bitmap stretched, and the test that separates those two " +
   "counts the device 2x2 blocks carrying four different coverages -- a " +
   "stretched image has none, and this has eighty. Measured on real windows on " +
   "both peers, at a scale forced rather than waited for: this box reports 96 " +
   "DPI, and a HiDPI bug that only appears on somebody else's laptop is the " +
   "kind a test is for. The drawing spans 177x69 device pixels at 100% and " +
   "350x138 at 200%, the same numbers from Win32 and from X11",
   "class Backing"],
  ["gui", "Displays::getMainDisplay().scale (per-monitor)",
   "gfx/displays.h: scale is per SCREEN, because the mixed setup is the normal " +
   "one -- a 4K laptop panel at 200% beside a 1080p monitor at 100% is what " +
   "most people who own both are looking at, and a single global factor has no " +
   "way to say it. Win32 reads GetDpiForWindow, resolved at RUNTIME because a " +
   "missing import is a load failure with no message on an older Windows; X11 " +
   "reads Xft.dpi from the resource database, which is what the USER chose, and " +
   "falls back to the physical size rounded to a quarter because a server " +
   "reporting a made-up 1000mm gives 96 by accident; Cocoa asks the window for " +
   "its backingScaleFactor",
   "float scale = 1.0f"],
  ["gui", "the host's scale, and macOS being the other way round",
   "clap.gui set_scale, IPlugViewContentScaleSupport and ui:scaleFactor all " +
   "reach NativeEditor::setScale, which keeps the window's LOGICAL size across " +
   "the change -- the editor stays the same physical size on the desk and gains " +
   "pixels. The wrappers used to hand the editor `logical * scale` as though it " +
   "were a layout size, which laid the tree out in DEVICE pixels: the window " +
   "grew and the interface did not. macOS is inverted and says so in the peer: " +
   "every number Cocoa gives is a POINT and the bitmap is multiplied up, so " +
   "nothing there divides a mouse coordinate, and a peer that copied the Win32 " +
   "conversion would halve every click on a Retina display",
   "void setScale"],
  ["gui", "WM_DPICHANGED (dragged between monitors)",
   "gfx/window_win32.h. Windows sends the new DPI and the rectangle it wants " +
   "the window to become; the suggested rectangle is USED rather than " +
   "recomputed, so the window does not jump back across the boundary it was " +
   "just dragged over. A window that ignored the message would keep the old " +
   "scale until something else resized it, which for a standalone is never",
   "WM_DPICHANGED"],
  ["gui", "Slider two-value (a range)",
   "gfx/widgets2.h. The ends cannot cross, which is the whole reason it is one " +
   "control rather than two; dragging the middle moves the pair and keeps its " +
   "width, stopping at the ends rather than being squashed against them",
   "class RangeSlider"],
  ["gui", "ShapeButton / DrawableButton",
   "gfx/widgets2.h: IconButton takes SVG source, because that is how icons " +
   "arrive and because a bitmap goes soft when a host resizes the editor. " +
   "Parsed once at setIcon, never per frame", "class IconButton"],

  // ── a fourth audit, of graphics ─────────────────────────────────────
  //
  // Fifteen rows against a module that has thirty-odd public classes. Reading
  // down the reference's own list found five more, and one of them the SDK had already
  // admitted to in a comment: window_win32.h says the damaged rectangle "is
  // currently the whole window rather than the union the tree reports --
  // correct, and wasteful".
  ["graphics", "RectangleList (and real partial repaint)",
   "gfx/region.h, and all three peers use it. Damage is a LIST now: two " +
   "controls at opposite ends of an editor leave the middle untouched, where " +
   "the united rectangle repainted everything between them. Measured by " +
   "counting which components painted, not by looking at the result",
   "class RectangleList"],
  ["graphics", "DropShadow",
   "gfx/effects2d.h: the shape rasterised into a scratch bitmap, blurred, and " +
   "composited -- which is what a shadow IS", "struct DropShadow"],
  ["graphics", "ImageConvolutionKernel (blur)",
   "gfx/effects2d.h: three box passes, which converge on a Gaussian by the " +
   "central limit theorem, at one add and one subtract per pixel per pass " +
   "REGARDLESS of radius. In 24.8 fixed point, because in 8-bit integers the " +
   "three passes annihilate the signal", "class Blur"],
  ["graphics", "RectanglePlacement",
   "gfx/effects2d.h: contain, cover, stretch and none, with alignment. " +
   "Drawable::draw uses it rather than its own copy", "struct RectanglePlacement"],
  ["graphics", "Colours (the named table)",
   "gfx/effects2d.h: all 148 CSS names and transparent, and the SVG parser " +
   "uses it rather than its own thirteen. " +
   "Returns a bool, because \"not a name\" and \"the name for black\" are " +
   "different answers", "namespace colours"],

  // ── a third audit of gui_basics and graphics ─────────────────────────────
  //
  // Third time this file has read "0 real gaps", and the third audit found six
  // more. The pattern is worth stating rather than repeating: a count of zero
  // measures the LIST, not the library, and the list only grows when somebody
  // goes back to the reference's own module contents and reads down them again.
  ["graphics", "AttributedString / TextLayout (word wrap)",
   "font.h: wrapText, wrappedHeight and drawWrapped. Breaks at spaces, honours " +
   "explicit newlines, and breaks a too-long word mid-word rather than letting " +
   "it overflow -- on a CHARACTER boundary, never a byte one",
   "std::vector<std::string> wrapText"],
  ["gui", "ListBox",
   "gfx/containers.h, and VIRTUAL: it draws rows on demand from a model rather " +
   "than owning a component per row, so a browser with four hundred presets " +
   "draws the five on screen. Arrow keys move the selection and scroll it into " +
   "view", "class ListBox"],
  ["gui", "TabbedComponent",
   "gfx/containers.h. Only the current page is visible, so the others cost " +
   "nothing to paint and cannot be clicked through", "class TabbedComponent"],
  ["gui", "ProgressBar",
   "gfx/containers.h. Reads through a POINTER, so the thread doing the loading " +
   "never has to know about the UI, and reports whether the picture changed so " +
   "it repaints only when it moved", "class ProgressBar"],
  ["gui", "MouseCursor",
   "a small closed set on Component, resolved by the router and set by all " +
   "three peers from the SYSTEM's own cursors. Whatever is captured wins during " +
   "a drag; otherwise it is inherited up the tree. macOS has no public diagonal " +
   "resize cursor and uses the up-down one rather than a private API",
   "enum class MouseCursor"],
  ["gui", "Animator (any value, not just bounds)",
   "gfx/animator.h: ValueAnimator. An opacity fading in, a meter's peak-hold " +
   "falling back, a colour sweeping between two states -- all numbers, none of " +
   "them rectangles, and none of them expressible by the bounds animator next " +
   "door. The clock is an ARGUMENT, so landing exactly on the target, a tick " +
   "longer than the whole animation, and a delay elapsing are exact tests " +
   "rather than sleeps.\n" +
   "      The final value is SET rather than computed: Back and Elastic do not " +
   "evaluate to exactly 1 at t=1, and a panel left a thousandth short is one " +
   "that never quite arrives. tick() reports whether anything MOVED, and an " +
   "animation still inside its delay reports that it has not -- a delay that " +
   "forced repaints would make staggering a row of controls cost more than not " +
   "staggering them",
   "class ValueAnimator"],
  ["gui", "AnimatorSetBuilder (sequence and parallel)",
   "gfx/animator.h, and deliberately NOT a builder. An animation can start " +
   "after a delay, and that one concept expresses both arrangements: two with " +
   "the same delay run together, one delayed by another's duration runs after " +
   "it. A builder would be a second way to say the same thing with its own " +
   "state machine to get wrong, and CSS reached the same conclusion. " +
   "onFinished carries the handle, which is what chains a callback onto the " +
   "end -- and a callback that starts the next animation is tested, because " +
   "that is the ordinary use and it edits the list it was called from",
   "double delay"],
  ["gui", "Easings (including cubic-bezier)",
   "gfx/animator.h: Linear, In, Out, InOut, Back, Elastic and Bounce, plus " +
   "CubicBezierEasing in CSS's own parameterisation -- which is what every " +
   "design tool exports and what a designer actually hands over.\n" +
   "      The awkward part is why it is a class rather than a line: the curve " +
   "is parameterised by an internal t and what is wanted is y as a function of " +
   "X, so x(t) has to be SOLVED before y can be read. Newton, with a bisection " +
   "fallback for the near-vertical curves where Newton wanders -- " +
   "cubic-bezier(1, 0, 0, 1) is a real curve people use. Checked against CSS " +
   "ease's published midpoint of 0.802 rather than against our own output, " +
   "which is the difference between testing the solver and agreeing with it",
   "class CubicBezierEasing"],
  ["gui", "ComponentBoundsConstrainer (editor size limits)",
   "plugin.h: EditorConstraints and applyEditorConstraints, declared on the " +
   "descriptor as editorLimits and honoured by every format.\n" +
   "      These numbers used to be invented. kMinEditorWidth/Height were " +
   "320x200 for EVERY plugin, the maximum 8192 for every plugin, and " +
   "preserve_aspect_ratio was hardcoded false -- so a synth whose interface " +
   "carries a keyboard could be dragged to 320 pixels wide by any host, and a " +
   "plugin drawn as a fixed-proportion skin could be stretched to any shape. " +
   "Silently, because the plugin had no way to say otherwise. Every format has " +
   "somewhere to put this and all of them were being told the same invented " +
   "answer: CLAP's gui_get_resize_hints, can_resize and adjust_size, VST3's " +
   "canResize and checkSizeConstraint, AU's view size, the standalone window.\n" +
   "      The defaults ARE the old constants, which is the only version of " +
   "this change that cannot alter a shipped product -- asserted rather than " +
   "assumed. The rules are a pure function, so each one is a test instead of " +
   "something checked by dragging a window in a host and looking at it: a " +
   "fixed axis pins to the NATURAL size rather than the minimum, a maximum " +
   "below its minimum resolves to the minimum, a four-pixel minimum is " +
   "answered rather than obeyed, and clamping an already-clamped size changes " +
   "nothing so a host that asks twice does not get a window that creeps.\n" +
   "      Where the box and the aspect ratio cannot both be satisfied the BOX " +
   "wins and the ratio is best-effort. A real choice with a defensible " +
   "opposite, so it is asserted rather than left to whatever the arithmetic " +
   "happened to do.\n" +
   "      Proved end to end, not just in the unit tests: the GUI probe fixture " +
   "declares 517x341..1200x800 -- odd numbers no default could produce -- and " +
   "the gate runs clap_host_test against the BUILT .clap asserting the host is " +
   "told exactly those. Asserting 320 would have passed against a wrapper that " +
   "ignored the descriptor entirely, which is the whole bug being looked for.\n" +
   "      the reference's BorderedComponentBoundsConstrainer is the same object with a " +
   "border subtracted first, for a window whose frame is not its content. The " +
   "split does not arise here: every size on every one of these paths is the " +
   "CONTENT size, and the one place a frame has to be added -- the standalone " +
   "answering WM_GETMINMAXINFO -- does it with AdjustWindowRect against the " +
   "style the window was actually created with, which is the platform's own " +
   "answer rather than a second constrainer",
   "struct EditorConstraints"],
  ["graphics", "PNGImageFormat",
   "gfx/png.h: PngDecoder and PngImage. Named here as the reference names it so the " +
   "parity walk can find it -- the row that existed described the decoder and " +
   "never used the word the audit looks for",
   "class PngDecoder"],
  ["graphics", "JPEGImageFormat / GIFImageFormat",
   "gfx/jpeg.h, gfx/gif.h and gfx/image_format.h, written here rather than " +
   "vendored, for the reason inflate.h was: every length in the file is the " +
   "attacker's. The argument against was that a plugin's ART is flat colour; " +
   "what it is handed is not -- a library's cover, a preset's photograph, a " +
   "user's skin, a designer's animated spinner.\n" +
   "      JpegDecoder (into JpegImage): baseline, extended and progressive " +
   "Huffman DCT, any sampling factors 1-4 interleaved or one component per " +
   "scan, restart markers, greyscale, YCbCr, RGB, Adobe CMYK and YCCK, the EXIF " +
   "orientation applied or reported; arithmetic, lossless, hierarchical and " +
   "12-bit refused by name. The arithmetic is libjpeg's -- jidctint's integer " +
   "IDCT, its fancy 2:1 upsampling with the alternating biases, its fixed-point " +
   "colour -- so the 26 fixtures (made by tests/data/images/make_images.py: " +
   "Pillow for the usual files, a small encoder for 4:4:0, 4:1:1, 3x2, mixed " +
   "sampling, separate scans, 16-bit tables and YCCK) decode to Pillow's " +
   "libjpeg-turbo output with a max difference of 0. A cut file keeps what " +
   "arrived and says so in `warning`; a header claiming more blocks than the " +
   "file has bits is refused before allocating. JpegEncoder writes baseline " +
   "JFIF at an IJG quality, 4:2:0 or 4:4:4, alpha flattened over a background; " +
   "Pillow decodes its files to the same bytes this decoder does.\n" +
   "      GifDecoder (into GifImage and GifFrame): 87a and 89a, global, local " +
   "and absent colour tables, LZW at every code size including a full table " +
   "carried on without a clear, interlacing, transparency, the NETSCAPE2.0 " +
   "loop count, and every frame COMPOSITED with its delay and GifDisposal, " +
   "disposal as the browsers do it. Six fixtures, fourteen frames, " +
   "pixel-exact against Pillow's frames. ImageFileFormat sniffs PNG, JPEG and " +
   "GIF by signature into one DecodedImage, and names WebP, BMP, TIFF, HEIC, " +
   "AVIF and ICO in its refusal. tests/images_test.cpp; tests/fuzz_images.cpp " +
   "ran 72,059 mutated cases of the 25 fixtures under ASan with a per-case " +
   "deadline, none crashing or hanging. Run on Windows (MSVC, /W4 clean, " +
   "after a bare windows.h too); macOS and Linux compiled only -- there is no " +
   "platform code in them",
   "class JpegDecoder"],
  ["graphics", "ImageType / NativeImageType / SoftwareImageType",
   "gfx/image.h: ImageType with create, getTypeID and convert, and the two " +
   "types the reference has, as thin types over Bitmap. Everything here is " +
   "software-rasterised into one premultiplied RGBA Bitmap on purpose -- " +
   "identical pixels on every machine, no context to lose -- and what a window " +
   "peer blits IS that bitmap, so NativeImageType and SoftwareImageType make " +
   "the same thing and say so by type ID. They exist so code written against " +
   "the reference's Image(format, w, h, clear, type) has somewhere to land. " +
   "create always clears: memory that held another plugin's pixels is not an " +
   "optimisation. Tested in images_test (cleared, sized, convert is a copy); " +
   "run on Windows, compiled only elsewhere",
   "class SoftwareImageType"],
  ["graphics", "ImageCache",
   "gfx/image.h: shared pointers to decoded Bitmaps by file path (with its " +
   "size and modification time, so an image edited on disk is decoded again), " +
   "by an FNV-1a hash of encoded bytes, or by an id: getFromFile, " +
   "getFromMemory, getFromHashCode, addImageToCache, setCacheTimeout, " +
   "releaseUnusedImages, as the reference names them, plus a byte budget. The " +
   "reason it is real now: an editor is opened and closed many times, and " +
   "several instances show the same art, so the same decode was repeated per " +
   "editor per instance. The reference's rule for letting go -- unheld, and " +
   "neither held nor asked for within the timeout, five seconds by default -- " +
   "and past the budget the least recently used unheld image goes; one still " +
   "held is never evicted, because dropping the cache's pointer frees nothing. " +
   "No timer thread: each call purges, and purge() is there for an editor's " +
   "timer. Decoding happens outside the lock. images_test drives it on an " +
   "injected clock: shared decodes, expiry only when unheld, the budget's " +
   "eviction order. Run on Windows, compiled only elsewhere",
   "class ImageCache"],
  ["graphics", "ScaledImage",
   "gfx/image.h: a Bitmap with the scale it was drawn for -- getImage, " +
   "getScale, getScaledBounds as the reference has them -- drawn at the " +
   "LOGICAL size the art means (drawAt, drawWithin), so @2x art is a device " +
   "pixel per pixel on a 2x Backing and the right size on a 1x one. pickFor " +
   "chooses among renditions for a density, as Backing::scale reports it: the " +
   "smallest at least that dense, else the densest. images_test draws @2x art " +
   "at 1x and under a 2x transform and checks the edges land on the logical " +
   "size. Run on Windows, compiled only elsewhere",
   "class ScaledImage"],
  ["graphics", "EdgeTable / PathFlatteningIterator",
   "gfx/graphics.h and gfx/path.h, as the rasteriser's own internals. Curves " +
   "are flattened to line segments with an error tolerance and scan-converted " +
   "with a coverage accumulator -- which is what those two the reference classes are. " +
   "They are public in the reference so that a custom LowLevelGraphicsContext can reuse " +
   "them; there is one rasteriser here and no second consumer for them to be " +
   "public for",
   "class Path"],
  ["graphics", "BorderSize",
   "gfx/layout.h: Margin, with the four insets the reference's BorderSize carries. One " +
   "name rather than two for the same four numbers -- and FlexItem::withMargin " +
   "is where an inset is actually applied here, so the layout engine's word won",
   "struct Margin"],
  ["graphics", "Parallelogram",
   "gfx/parallelogram.h: three corners, the fourth derived, so it cannot be " +
   "built as anything but a parallelogram. getBottomRight, getWidth, " +
   "getHeight, getBoundingBox, getRelativePoint, transformedBy and isEmpty as " +
   "the reference has them, and what the corners are for here: contains and " +
   "toRelative (half-open, as Rect::contains), area, toPath, and " +
   "transformFrom -- the Transform that puts a source rectangle's corners on " +
   "these, which is how a picture is placed by where its corners LAND and " +
   "handed to drawImageTransformed. DrawableText's box is one. images_test: a " +
   "rotated box's bounds, a sheared one's containment following the lean, " +
   "every corner landing through transformFrom, and an image drawn through it " +
   "filling the shape and nothing else. Run on Windows, compiled only elsewhere",
   "struct Parallelogram"],
  ["graphics", "GlyphArrangement / PositionedGlyph / TypefaceMetrics / FontOptions",
   "gfx/font.h, inside Font rather than beside it. A run of text is walked once " +
   "with kerning and advance widths accumulated, and each glyph appended to a " +
   "Path at its pen position -- which is a glyph arrangement, built and " +
   "consumed in one pass. The reference keeps the arrangement because its text can be " +
   "hit-tested, re-justified and edited in place after layout.\n" +
   "      The one place that genuinely needs positions per character is the " +
   "text editor's caret, and Font::layoutCharacters gives them -- a " +
   "CharacterLayout, each character's extent, direction and whether a caret " +
   "may stand before it, from the same walk that draws -- so measuring and " +
   "drawing cannot disagree, in shaped and right-to-left text as in the rest: " +
   "the failure an arrangement object exists to prevent, prevented here by " +
   "there being one code path",
   "class Font"],
  ["graphics", "line breaking (the reference's word wrap, Unicode's UAX #14)",
   "gfx/line_break.h: findLineBreaks gives each boundary of a text as LineBreak -- " +
   "None, Allowed or Mandatory -- by the Unicode Line Breaking Algorithm, its " +
   "rules LB1 to LB31 with UAX #14's number tailoring; Font::wrapText fills lines " +
   "greedily at those breaks, spaces hanging past the end, and breaks a word " +
   "too long for its line at its clusters. Checked against Unicode's " +
   "LineBreakTest: all 7,654 cases. Thai, Lao, Khmer and Myanmar words are not " +
   "split without a dictionary, which this has not got: they wrap at spaces, " +
   "and a word too long at its clusters",
   "inline std::vector<LineBreak> findLineBreaks"],
  ["graphics", "bidirectional text (the reference's text layout, Unicode's UAX #9)",
   "gfx/bidi.h: BidiParagraph -- the whole algorithm, explicit embeddings, " +
   "overrides and isolates, the weak and neutral rules, bracket pairs, the " +
   "line rules and mirroring. Checked against Unicode's BidiCharacterTest: all " +
   "91,707 paragraphs. Font runs a string through it whenever it holds a " +
   "right-to-left letter, so a preset named in Hebrew reads right to left with " +
   "its numbers and Latin words where a reader expects them",
   "class BidiParagraph"],
  ["graphics", "complex-script shaping: Arabic, Hebrew, Thai, Lao, Hangul, and every script that needs no reordering",
   "gfx/text_shaper.h: shapeRun into ShapedGlyph -- GSUB and GPOS with every " +
   "lookup type, lookup flags, GDEF classes and mark filtering sets; " +
   "HarfBuzz's feature stages, normalisation and mark ordering; Arabic " +
   "joining; Thai and Lao SARA AM and the private-use marks of old Thai " +
   "fonts; Hangul syllables composed where the font has them and jamo " +
   "features where it has not; marks a font cannot place, placed by their " +
   "class. Compared glyph by glyph with HarfBuzz on the system fonts of a " +
   "Windows machine: all of 336,000 random Arabic, Hebrew, Latin, Greek and " +
   "Cyrillic strings, 45,000 on fonts with no GPOS, and 31,500 Thai, Lao and " +
   "Hangul, identical. Font shapes a string that needs it -- right-to-left " +
   "letters, combining marks, joiners, SARA AM, jamo -- per run of one level, " +
   "face and script; anything else is drawn a glyph per character as before",
   "inline std::vector<ShapedGlyph> shapeRun"],
  ["graphics", "complex-script shaping: the Indic, Khmer and Myanmar shapers",
   "gfx/text_shaper.h: Devanagari, Bengali, Gurmukhi, Gujarati, Oriya, Tamil, " +
   "Telugu, Kannada and Malayalam, Khmer and Myanmar, as HarfBuzz shapes them -- " +
   "the syllables found by its grammars, compiled to state machines by " +
   "scripts/unicode/gen_syllables.py; broken clusters given a dotted circle; " +
   "the base consonant, reph, half, below, post and pre-base forms found by " +
   "asking the font's lookups, the syllable reordered, the basic features " +
   "applied a stage each within it, the final reordering, the presentation " +
   "forms. Both Indic specifications, 'dev2' and the old 'deva'. Compared glyph " +
   "by glyph with HarfBuzz: 27,000 random syllables on Nirmala UI across the " +
   "nine scripts, 20,000 Khmer and Myanmar on Leelawadee UI and Myanmar Text, " +
   "all identical, and four fonts built for the tests -- old and new " +
   "Devanagari, Khmer, Myanmar -- whose output sdk_tests checks",
   "void indicInitialSyllable"],
  ["graphics", "complex-script shaping: the Universal Shaping Engine",
   "gfx/text_shaper.h: the eighty-odd scripts HarfBuzz sends to its Universal " +
   "Shaping Engine -- Balinese, Javanese, Tibetan, Sinhala, Mongolian, N'Ko, " +
   "Adlam and the rest -- with its categories (scripts/unicode/gen_use.py, from " +
   "the UCD and Microsoft's additions), its syllables, the repha and pre-base " +
   "forms recorded as the font makes them, its reordering, and the joining " +
   "forms -- Arabic's analysis for the scripts that join, syllable by syllable " +
   "for the rest. Compared glyph by glyph with HarfBuzz on the system fonts " +
   "covering twenty-one of them: 48,000 random syllables identical but for " +
   "Batak's two killers, which Unicode 17 made reordering killers and the UCD 15 " +
   "tables here do not",
   "void useReorderSyllable"],
  ["gui", "HostProvidedContextMenu (right-click a parameter)",
   "EditorHost::showContextMenu in gfx/plugin_editor.h, reaching " +
   "showHostContextMenu in both wrappers -- clap_host_context_menu, and " +
   "IComponentHandler3 for VST3.\n" +
   "      The wrapper end had been wired for a long time and the ONLY thing " +
   "that ever called it was a bridge message from the web editor's " +
   "sonore.contextMenu. So the day the native editor became the default, " +
   "right-clicking a parameter silently stopped doing anything -- and " +
   "right-clicking a knob is how a user reaches MIDI learn, assign automation " +
   "and reset to default in every DAW there is. The native toolkit had no " +
   "concept of a right button at all, so no unit test could have noticed: " +
   "there was nothing to synthesise.\n" +
   "      Component::contextMenu is a SEPARATE dispatch rather than a flag on " +
   "mouseDown, and that is the part worth keeping. Every control here starts " +
   "its gesture in mouseDown, so a right button delivered there means each one " +
   "has to remember to check the flag, and the one that forgets moves the very " +
   "parameter the user was trying to open a menu on. With a separate call " +
   "there is nothing to forget. It takes no capture and moves no focus either " +
   "-- a right-click on a knob while typing in a field should not take the " +
   "keyboard away from the field.\n" +
   "      Proved through a real window, because that is the only place it " +
   "could be: a real WM_RBUTTONDOWN on a real knob asks the host about the " +
   "right parameter and leaves its value alone, while a LEFT click at the very " +
   "same point moves it -- which is what makes \"did not move\" mean " +
   "\"declined\" instead of \"missed\". Control-click is the same gesture on a " +
   "Mac and Cocoa routes it to rightMouseDown: for us, though that half is " +
   "unverified like the rest of that file",
   "virtual void contextMenu"],
  ["gui", "ButtonParameterAttachment",
   "gfx/plugin_editor.h: ButtonAttachment, and GenericEditor builds a toggle " +
   "for any parameter with exactly two positions.\n" +
   "      It was a real gap for about twenty minutes -- the audit found it " +
   "with no row at all, and the first version of this row said so, because " +
   "the alternative was claiming a design reason for an absence that had " +
   "none. It was quicker to close than to justify. A two-state parameter used " +
   "to render as a drop-down with two entries: it worked, it was announced " +
   "correctly, and it was the wrong control -- two clicks and a moment's " +
   "reading to do what one click should, and nothing legible at a glance down " +
   "a column of them. Bypass, invert, sync, mute are switches.\n" +
   "      Two things in it are worth more than the control. The button carries " +
   "the STATE'S OWN NAME where the table gives one, drawn and announced from " +
   "the same string -- a parameter whose positions are \"Pre\" and \"Post\" " +
   "announced as \"on\" makes a listener work out which of the two that is, " +
   "and they cannot, because the mapping belongs to the plugin. And sync " +
   "compares against the MIDPOINT rather than against maxValue: a host " +
   "automating this sends a continuous sweep, and a switch that only turned on " +
   "at exactly 1.0 would flicker off for every intermediate frame of the ramp",
   "class ButtonAttachment"],
  ["gui", "WebSliderParameterAttachment / WebComboBoxParameterAttachment / " +
          "WebToggleButtonParameterAttachment",
   "gui.h: the bridge's sonore.params, sonore.get and sonore.set, which the " +
   "web editor binds controls with. The reference added these in 8 to give its webview " +
   "editors typed per-control attachments; the same job here is done by one " +
   "small API the page uses however it likes, because the page is authored per " +
   "plugin rather than assembled from the reference widgets",
   "sonore.params"],
  ["core", "PluginHostType",
   "plugin_host_type.h: PluginHostType -- which host loaded the plugin, and as " +
   "which format. Capability questions in the wrappers remain the first answer " +
   "to a host difference (does it offer a context menu, does it answer " +
   "gui_request_resize); this exists for the differences that are not " +
   "capabilities -- a host exceeding the block size it reported -- and for " +
   "ported code that asks isReaper(). It classifies, in order, the macOS " +
   "bundle identifier, the executable's name and path (the reference's method, " +
   "and the only one an LV2 instance has, or an AU one whose host does not " +
   "name itself; helper processes such as " +
   "BitwigPluginHost and reaper_host64 still classify), and the name the host " +
   "reported in HostInfo, which is how the SDK's own standalone is known. The " +
   "reference's checks by its names -- isAbletonLive, isBitwigStudio, isCubase, " +
   "isCubase7orLater, isCubaseBridged, isLogic, isProTools, isFruityLoops, " +
   "isStudioOne, isReaper, isWavelabLegacy and the rest -- plus CLAP-era hosts " +
   "it predates (clap-validator, Carla, Audacity, Zrythm). The version comes " +
   "from the Windows version resource (ProductVersion, through version.dll " +
   "loaded at run time, so nobody links version.lib), from " +
   "CFBundleShortVersionString on macOS, else from what the host reported. " +
   "getPluginLoadedAs() reads the format from the MODULE's own path (.clap, " +
   ".vst3, .component, .appex, .lv2, or the executable itself for the " +
   "standalone), because one binary of this SDK carries several entry points.\n" +
   "      Logic's AUHostingService process is not Logic and is not called " +
   "Logic: the AU wrapper reads kAudioUnitProperty_AUHostIdentifier, the name " +
   "and version the host sets, into HostInfo and sendHostInfo(), so it " +
   "classifies by the reported name; a host that never sets the property " +
   "leaves it unknown rather than " +
   "guessed. The wrapper's case is compiled only (au_shim); turning the " +
   "name and NumVersion into HostInfo is RUN in sdk_tests. " +
   "Tested in tests/plugin_state_test.cpp, RUN on Windows: 38 executables, " +
   "bundles and reported names; module paths for every format; the installed " +
   "REAPER's real version resource read (not launched); this process. macOS " +
   "(against tests/au_shim) and Linux: compiled only",
   "class PluginHostType"],
  ["core", "PluginListComponent",
   "gfx/plugin_list.h: PluginListComponent, a view of a PluginCache -- which " +
   "already is the KnownPluginList, saved as a text file with its blacklist -- " +
   "drawn as a gfx/table.h TableListBox of name, format, category and " +
   "manufacturer, with Scan and Remove buttons. Sorting by any column keeps " +
   "blacklisted files listed at the bottom; the category is the first word of " +
   "a format's own vocabulary that is not plumbing (CLAP features, VST3 " +
   "subcategories, LV2 classes). Removing a plugin drops its file's stamp " +
   "(PluginCache::removePlugin) so a rescan finds it again; removing a " +
   "blacklisted row forgives the file; every change is saved at once.\n" +
   "      Scanning is in-process with plugin_cache.h's dead man's switch, now " +
   "for any list of files (scanFilesCached), not only one folder: this SDK " +
   "starts no scanner subprocess (see the ChildProcessCoordinator row), and a " +
   "host that died inside a plugin blacklists it on the next start. Tested " +
   "RUN on Windows: a crash mark left on disk blacklisting a library without " +
   "opening it, save and restore, rendering into a bitmap, header clicks " +
   "sorting both ways, remove/rescan/forgive/clear/removeMissing. macOS " +
   "(against tests/au_shim, which gained dirent.h for it) and Linux: compiled " +
   "only",
   "class PluginListComponent"],
  ["core", "LADSPAPluginFormat",
   "host.h: LADSPAPluginFormat and LadspaPlugin, a HostedPlugin like the other " +
   "three formats, so loadPlugin(), PluginGraph and PluginCache take LADSPA " +
   "descriptions as they take the rest. ladspa_abi.h declares the ABI -- " +
   "ladspa::Descriptor, ladspa::PortRangeHint, the port and hint flags, the " +
   "ladspa_descriptor entry point -- written from the specification rather " +
   "than taken from the LGPL header, with unsigned long kept where the ABI has " +
   "it. Control inputs are parameters with the specification's default rules " +
   "(the geometric low/middle/high under a logarithmic hint, bounds scaled by " +
   "the sample rate, toggles and integers stepped); a control output called " +
   "\"latency\" is the latency, as Linux hosts agree; state is the control " +
   "values keyed by port index. Searched in LADSPA_PATH, else the usual Linux " +
   "and macOS folders; Windows has no convention, so LADSPA_PATH or a folder " +
   "the caller names.\n" +
   "      RUN on Windows against a LADSPA library the test builds as a DLL " +
   "(tests/ladspa_gain_plugin.cpp: a mono dB gain with an invert toggle, and a " +
   "stereo plugin with a latency port, an integer control and rate-relative " +
   "bounds): -6 dB measured through the plugin to 5e-8, a mono plugin heard on " +
   "both sides of a stereo bus, state saved and restored into a second " +
   "instance. The state decoder and the hint rules are fuzzed under ASan -- " +
   "which found a one-sided bound of 1e30 giving an empty range, now treated " +
   "as no bound. The test library builds as a .so too; Linux and macOS: " +
   "compiled only here",
   "class LADSPAPluginFormat"],
  ["core", "VSTPluginFormat",
   "deliberately never: hosting VST2 needs the VST2 SDK, which Steinberg " +
   "stopped licensing in 2018, and a host built on a reconstruction of it " +
   "carries the same risk a VST2 plugin would (see the VST2 wrapper row). The " +
   "rest of plugin hosting is built -- host.h loads CLAP, VST3, LV2 " +
   "(LV2PluginFormat), LADSPA and, on macOS, Audio Units (host_au.h, in its " +
   "own row), scanDirectory() is the PluginDirectoryScanner, PluginCache the " +
   "KnownPluginList, PluginGraph the AudioProcessorGraph",
   null, "class VSTPluginFormat"],
  ["gui", "NSViewComponentWithParent",
   "gfx/nsview_component.h: a component holding any NSView -- a hosted plugin's " +
   "editor, a system control -- inside a parent NSView of its own, which it places " +
   "in the Cocoa peer's view at its root-relative bounds, hides with it, and moves " +
   "to whichever open peer shows its tree (NativeWindowCocoa::viewShowing). The " +
   "child fills the parent, and when the child resizes itself the component " +
   "follows, which is the reference's reason for the class. Re-placed on every " +
   "resize and by a 30 Hz timer, because the tree has no move or visibility " +
   "listener. This row used to say a host's problem; host_au.h made this SDK a " +
   "host whose plugins have NSView editors.\n" +
   "      COMPILED for macOS against tests/au_shim, NEVER RUN: no Mac here; " +
   "mac_host_selftest.cpp embeds a plain NSView in a peer and checks where it " +
   "went. Off macOS the class exists and setView() refuses with a reason, and the " +
   "placement arithmetic is RUN on Windows by mac_host_test.cpp",
   "class NSViewComponentWithParent"],
  ["gui", "FocusTraverser / KeyboardFocusTraverser",
   "gfx/component.h: MouseRouter's tab order, which walks the tree in child " +
   "order and skips anything that does not want keyboard focus. The reference splits " +
   "these two because a traverser can be replaced per-component to impose a " +
   "custom order; the order here is the order things were added, which is the " +
   "order they are drawn and read, and a plugin that wanted a different one " +
   "would be asking for tab order to disagree with the layout",
   "bool focusNext"],
  ["gui", "ModifierKeys",
   "gfx/component.h: shiftDown, ctrlDown and altDown on MouseEvent and " +
   "KeyPress. Three bools rather than a class, because that is the whole set a " +
   "plugin editor acts on -- and each peer fills them from its own platform's " +
   "answer, which is where Command-versus-Control on a Mac is decided",
   "bool shiftDown"],
  ["gui", "CaretComponent / TextEditorKeyMapper",
   "gfx/text_editor.h, inside TextEditor. The reference separates the caret so it can be " +
   "restyled, and the key mapper so the same editing keys can be applied to " +
   "another control. One text editor here, and a second consumer for either " +
   "piece would be the thing that justified splitting them",
   "class TextEditor"],
  ["gui", "TextInputTarget (IME / composed text input)",
   "Built on all three platforms, run on one.\n" +
   "      Component::caretBounds is the portable half -- the reference calls the " +
   "equivalent TextInputTarget -- and TextEditor answers it with the SAME " +
   "arithmetic paint() draws the caret from, so a candidate list cannot end up " +
   "beside a different character than the one being typed.\n" +
   "      WINDOWS: done. window_win32.h answers WM_IME_STARTCOMPOSITION and " +
   "WM_IME_COMPOSITION by placing the composition window at the focused " +
   "component's caret, converted to device pixels. imm32 is loaded at runtime " +
   "rather than linked, so no plugin has to add -limm32 to a link line that " +
   "already works. It positions and then FALLS THROUGH rather than returning: " +
   "DefWindowProc is what actually shows the candidate list, and swallowing " +
   "the message would leave the user with none at all.\n" +
   "      X11 and macOS: built, compiled and NOT run -- see the row \"IME on " +
   "X11 and macOS\": the Cocoa view is an NSTextInputClient and the X11 peer " +
   "opens an XIM input context, both feeding the TextInputSession composition " +
   "model (gfx/text_input.h) that TextEditor and TextArea draw inline. IME is a " +
   "feature whose behaviour is what a user sees while composing, so until " +
   "someone has typed into it on those systems it is code nobody has tested.\n" +
   "      What is proved on Windows is the half that can be: the point handed " +
   "to imm32 is beside the caret of the focused field in device pixels, " +
   "asserted through a real window with a real focused editor -- because the " +
   "failure being guarded against is a point of (0,0) from coordinates nobody " +
   "converted, which looks exactly like not having implemented this. What is " +
   "NOT proved is the visual result, which needs an IME installed and a human " +
   "composing",
   "compositionPoint"],
  ["gui", "IME on X11 and macOS",
   "gfx/text_input.h: TextInputTarget, TextComposition and TextInputSession, " +
   "the composition model all three input-method paths share. A field " +
   "(TextEditor, TextArea) shows an input method\'s composition underlined " +
   "at its caret, the clause being converted heavier, and never in its value " +
   "until it is committed; a click, a key or losing the focus commits it " +
   "first. TextInputSession is NSTextInputClient\'s arithmetic over any " +
   "target -- UTF-16 ranges (TextRange16, NSNotFound) against code points, " +
   "an emoji never split, replacement ranges mapped out of the shown text, " +
   "the first rect widened along one line -- and XIM\'s preedit edits (a " +
   "change at a position, caret directions, the clause from feedback). " +
   "focusedCaretInRoot is the caret conversion all three use; window_win32.h\'s " +
   "compositionPoint now calls it.\n" +
   "      macOS (window_cocoa.h): the view ADOPTS NSTextInputClient " +
   "(class_addProtocol -- without it -[NSView inputContext] is nil) and " +
   "implements insertText:replacementRange:, " +
   "setMarkedText:selectedRange:replacementRange:, unmarkText, " +
   "selectedRange, markedRange, hasMarkedText, " +
   "attributedSubstringForProposedRange:actualRange:, " +
   "validAttributesForMarkedText, firstRectForCharacterRange:actualRange: " +
   "(the caret, on the y-up screen), characterIndexForPoint: and " +
   "doCommandBySelector: (the key, as the KeyPress it always was). keyDown: " +
   "goes through interpretKeyEvents: while a writable text field has the " +
   "focus and no Command or Control shortcut is meant; a click " +
   "mid-composition goes to the input context first; a focus change " +
   "discards the input method\'s state. inputMethodStatus() says whether the " +
   "protocol was adopted.\n" +
   "      X11 (window_x11.h): XOpenIM after XSetLocaleModifiers(\"\"), lazily, " +
   "when a text field first has the focus; XCreateIC preferring on-the-spot " +
   "(XIMPreeditCallbacks, drawn through the model), then over-the-spot " +
   "(XIMPreeditPosition at XNSpotLocation, the caret), then root-window -- " +
   "over-the-spot first when the locale is not UTF-8, because Xlib converts " +
   "the preedit into it; XFilterEvent on every event; Xutf8LookupString for " +
   "committed text, a converted word committed whole in place of the " +
   "preedit; XSetICFocus and XUnsetICFocus with the window\'s focus and a " +
   "text field\'s; XNDestroyCallback and an instantiate callback for a server " +
   "that dies or starts late; SONORE_XIM_STYLE and SONORE_X11_NO_IME to steer " +
   "it. Every failure is in inputMethodStatus() and said once on stderr.\n" +
   "      RUN on Windows: tests/mac_input_test.cpp drives the model against " +
   "real TextEditor and TextArea instances -- marked and selected ranges, " +
   "surrogate pairs, replacement ranges, rectangles on a TextArea\'s second " +
   "line, the field\'s filter and length limit on commit, focus, click, key " +
   "and setText interrupting a composition, XIM preedit draws and caret " +
   "moves -- 113 checks with the accessibility ones, clean under " +
   "AddressSanitizer; native_window_test still places the Windows " +
   "composition point beside the caret. COMPILED, NOT RUN: the macOS half " +
   "(au_shim, arm64 and x86_64, MSVC and clang) and the X11 half (the real " +
   "Xlib.h, MSVC and clang). There is no Mac, no X server and no input " +
   "method here, and the Xvfb x11_window_test runs under has no input " +
   "method server. tests/mac_input_selftest.cpp drives NSTextInputClient " +
   "through the view and a real keyDown:; tests/x11_input_selftest.cpp " +
   "prints what was negotiated, checks the spot is the caret, and echoes " +
   "every composition and commit while a developer types. Until somebody " +
   "has typed into them, both are written, not known to work",
   "class TextInputSession"],
  ["gui", "ModalComponentManager",
   "gfx/alert.h: Overlay and AlertOverlay, which are drawn INSIDE the editor " +
   "and take the mouse while they are up. The reference needs a manager because its " +
   "modal components are real windows with a nested event loop; ours cannot be, " +
   "and must not be -- a nested loop inside a plugin is a loop the host is not " +
   "running, and the transport keeps going while the DAW stops answering",
   "class AlertOverlay"],
  ["gui", "CachedComponentImage / ComponentPaintDiagnostics",
   "gfx/component_listeners.h: CachedComponentImage, the interface a " +
   "component's cache implements; gfx/cached_image.h: " +
   "StandardCachedComponentImage, what Component::setBufferedToImage(true) " +
   "installs. The subtree is rendered into a Bitmap at DEVICE resolution " +
   "with the direct paint's own transform shifted by whole pixels, so its " +
   "antialiasing is the direct paint's -- re-rendered when the scale or the " +
   "sub-pixel phase changes, never stretched. repaint() of the component or " +
   "anything inside it invalidates every cached ancestor, by rectangle, " +
   "BEFORE the is-it-showing test (a change inside a hidden panel must not " +
   "be served stale later); only the invalid rectangles are re-rendered, " +
   "through the same damage walk the window uses; a move by whole device " +
   "pixels keeps the cache.\n" +
   "      Tested at 1x, 1.25x, 1.5x and 2x: an opaque buffered panel is " +
   "pixel-identical to direct painting, translucent within one level (8-bit " +
   "over is associative only up to rounding), a second paint does not call " +
   "paint(), a child's repaint invalidates the parent, and the damage path " +
   "through Backing at 1.25x matches the unbuffered tree.\n" +
   "      gfx/paint_diagnostics.h: ComponentPaintDiagnostics, a " +
   "ComponentPaintObserver (component_listeners.h, one pointer test per " +
   "paint while none is installed) timing each component's own paint() on an " +
   "injectable clock -- count, total, last, max, a text report -- and " +
   "optionally flashing every repainted region with a translucent colour " +
   "that changes each frame. Tested: exact timings on a fake clock; the " +
   "flash tints exactly the damaged region and nothing else. " +
   "Platform-independent; RUN on Windows",
   "class StandardCachedComponentImage"],
  ["gui", "LookAndFeel_V1 / V2 / V3 / V4",
   "gfx/lookandfeel_versions.h: the reference's four generations of default " +
   "look as LookAndFeel subclasses, so a port keeps the face its users know. " +
   "LookAndFeel_V1 grey, square and bevelled (light top-left, dark " +
   "bottom-right, inverted when pressed; pie-segment knob, block thumb on a " +
   "sunken groove); LookAndFeel_V2 glossy -- lavender 0xbbbbff glass lozenge " +
   "buttons, sphere thumbs on a shadowed groove, ring-segment knob, white " +
   "popups with the 0x991111aa highlight; LookAndFeel_V3 V2 with the gloss " +
   "taken off; LookAndFeel_V4 drawn from a nine-colour ColourScheme with the " +
   "reference's dark, midnight, grey and light tables verbatim, arc knobs " +
   "swept +-0.8 pi, round thumbs on line tracks, pill scroll thumbs.\n" +
   "      Every one draws only through the existing LookAndFeel virtuals -- " +
   "sliders, bars, buttons, combo boxes, scroll bars, text fields, tabs, " +
   "popups, tooltips, progress bars, range sliders, icon buttons -- so the " +
   "widgets need nothing new. Shapes are faithful in form (bevel, gloss, " +
   "flatness, arcs), drawn by this SDK's rasteriser rather than " +
   "pixel-identical to a renderer it does not have. Tested: each of V1, V2, " +
   "V3 and V4 in all four schemes renders a standard set of eighteen control " +
   "drawings with tens of thousands of pixels of ink, all eight results " +
   "(with the default) pairwise different; real Slider and Button widgets " +
   "inherit each look from a parent. RUN on Windows; platform-independent " +
   "drawing",
   "class LookAndFeel_V4"],
  ["gui", "BubbleComponent",
   "gfx/alert.h: CallOutBox, which is the same thing -- content in a rounded " +
   "box with a tail pointing at whatever raised it. One name, because two " +
   "components drawing a box with a pointer would be two to keep looking alike",
   "class CallOutBox"],
  ["gui", "DropShadower",
   "gfx/effects2d.h: DropShadow, applied where a shadow is drawn. the reference's " +
   "DropShadower attaches to a Component and follows it, which it needs " +
   "because a framework window may be a real OS window with nothing behind it to " +
   "draw on. Everything here is composited into one bitmap, so a shadow is " +
   "drawn before the thing it falls behind and there is nothing to attach",
   "struct DropShadow"],
  ["gui", "FocusOutline",
   "gfx/widgets.h: Widget::paintFocusRing, drawn by every focusable control. " +
   "the reference makes it a separate component so it can be attached to something " +
   "whose own painting cannot be changed; ours is a call in paint() and every " +
   "widget makes it, which is what keeps the ring identical across controls",
   "paintFocusRing"],
  ["gui", "DrawableComponent / OwningDrawableComponent",
   "gfx/drawable_component.h: DrawableComponent shows a Drawable (gfx/svg.h) " +
   "it REFERENCES, as ImageComponent references its Bitmap -- an icon set " +
   "parsed once is drawn by many components; OwningDrawableComponent holds " +
   "its own, set directly or parsed from an SVG string (setSvg, refusing one " +
   "with nothing visible in it). Placement is RectanglePlacement " +
   "(effects2d.h), the same rules ImageComponent uses, against the viewBox " +
   "-- Contain, Cover, None and a real Stretch, drawn through a non-uniform " +
   "scale so the file's own aspect rule cannot letterbox it -- or the SVG's " +
   "own preserveAspectRatio. currentColor can be set. Clicks pass through, " +
   "as for any picture.\n" +
   "      Tested on a 100x50 red viewBox in a 200x200 component: Contain gives " +
   "rows 50..150, Cover and Stretch fill, None is 100x50 centred, None " +
   "aligned left/bottom lands at 0..100 x 150..200. RUN on Windows; " +
   "platform-independent",
   "class OwningDrawableComponent"],
  ["gui", "MouseInputSource / PenDetails",
   "gfx/component.h: MouseEvent gains, additively and defaulted to a plain " +
   "mouse, sourceType (Mouse/Touch/Pen), sourceIndex, pressure (negative " +
   "when unreported), orientation, PenDetails (rotation, tiltX, tiltY, " +
   "eraser, barrel button) and eventComponent; a peer that knows sets them " +
   "on the router with setPointerInput (a PointerInput) for the length of " +
   "one call, so every existing caller and test still delivers mouse events " +
   "unchanged. gfx/mouse_input.h: MouseInputSource per pointer and " +
   "MouseInputSources, the process's list, which dispatches PointerSample " +
   "reports -- the PRIMARY pointer through the router (capture, focus, " +
   "hover, drag-and-drop all as before), each SECONDARY finger with its own " +
   "capture, no focus, guarded against its component being destroyed; double " +
   "taps counted.\n" +
   "      Windows: WM_POINTERDOWN/UPDATE/UP/LEAVE/CAPTURECHANGED for pen and " +
   "touch, handled in window_win32.h and returning 0 so Windows makes no " +
   "legacy mouse messages from them; the mouse stays on the legacy path (no " +
   "EnableMouseInPointer -- process-wide, the host's). The pointer functions " +
   "are looked up from user32 at run time and can be substituted, so the " +
   "test sends REAL WM_POINTER messages to a real window with synthesised " +
   "POINTER_PEN_INFO/POINTER_TOUCH_INFO, through every line the window runs: " +
   "512/1024 pressure arrives as 0.5, 45/-18 degree tilt as 0.5/-0.2, 90 " +
   "degrees rotation as pi/2, the eraser flag; a pen drag keeps capture; two " +
   "fingers hold two components and keep them when they cross; a finger on a " +
   "destroyed component is forgotten. RUN on Windows as far as a mouse-only " +
   "machine allows -- the OS query is the one thing substituted.\n" +
   "      macOS: NSEvent subtype/pressure/tilt/rotation on the ordinary mouse " +
   "events a tablet sends, and tabletProximity: for the eraser, wired into " +
   "window_cocoa.h -- compiled against the shim, never run. X11: NOT " +
   "available -- the X11 peer reads core ButtonPress/MotionNotify, which " +
   "carry no pressure, and does not open XInput2; X11 events stay mouse " +
   "events",
   "struct PenDetails"],
  ["gui", "MouseWheelDetails",
   "gfx/component.h: MouseRouter::mouseWheel takes the delta directly. The reference " +
   "wraps it because its struct also carries smooth-scroll and inertia flags " +
   "from the platform; ours is one number, and the peers already turn each " +
   "platform's wheel message into it",
   "mouseWheel"],
  ["gui", "MouseInactivityDetector",
   "gfx/mouse_input.h: MouseInactivityDetector watches a component and " +
   "everything inside it through a MouseListener (component_listeners.h: " +
   "listeners attached to a component, optionally hearing every nested " +
   "child's events, told BEFORE the component so a handler destroying it " +
   "cannot starve them) and tells a Listener, or " +
   "onBecameActive/onBecameInactive, when the mouse has been still for " +
   "setDelay milliseconds (1000 by default, as in the reference) and when it " +
   "moves again past setMouseMoveTolerance (50); a click or a wheel wakes it " +
   "at once. The clock is injectable and tick() belongs in the editor's own " +
   "tick, as with every timed thing here -- and it is for dimming an " +
   "overlay, never for hiding a host's cursor. Tested on a fake clock: " +
   "inactive exactly at the delay, told once, a drift within tolerance does " +
   "not wake it, a move past it does, a click restarts the wait, the wheel " +
   "wakes it. Platform-independent; RUN on Windows",
   "class MouseInactivityDetector"],
  ["gui", "SettableTooltipClient",
   "gfx/widgets.h: setTooltip on Widget, read by TooltipManager. The reference splits " +
   "the interface from the settable implementation so a component can compute " +
   "its tip; a std::string that anything may overwrite covers both, and the " +
   "manager takes the clock as an argument so every tooltip rule is an exact " +
   "test rather than a wait",
   "setTooltip"],
  ["gui", "TextDragAndDropTarget",
   "gfx/component_listeners.h: TextDragAndDropTarget, which a component opts " +
   "into by answering asTextDragAndDropTarget() (the asWidget pattern: no " +
   "RTTI). Inside the editor a drag started with MouseRouter::startTextDrag " +
   "rides the router's ordinary drag machinery, and Component's default " +
   "item-drag handlers hand a text drag (DragSource::isText) to the target, " +
   "with the position it ENTERED at. From outside, gfx/text_drag.h's " +
   "TextDragDispatcher finds the target the same way and forgets one " +
   "destroyed mid-drag.\n" +
   "      Windows: window_win32.h now registers an OLE IDropTarget " +
   "(detail::OleDropTarget) -- text arrives only as an IDataObject, and once " +
   "a target is registered Windows stops sending WM_DROPFILES, so it reads " +
   "CF_UNICODETEXT, CF_TEXT and CF_HDROP files, giving files live hover " +
   "feedback too. It registers only on a thread the host has already made a " +
   "single-threaded apartment -- every host that does drag and drop has -- " +
   "where OleInitialize adds OLE without changing the apartment and is " +
   "balanced at close; a thread with no apartment, or in the MTA, is left " +
   "untouched and keeps WM_DROPFILES as before (making it an STA from inside " +
   "a plugin re-routes UI Automation's calls through a message loop the host " +
   "may not be pumping -- native_window_test's reader caught exactly that). " +
   "ole32 is loaded at run time, not linked. performExternalDragDropOfText " +
   "drags text OUT through DoDragDrop. Tested: an internal text drag " +
   "delivers enter (with position), moves, exit and drop, a refusing target " +
   "is skipped, an item drag is not mistaken for text; a UTF-8 string " +
   "round-trips through OleTextDataObject, CF_HDROP files parse as UTF-8 " +
   "paths, the IDropTarget gives copy/none feedback and drops text and " +
   "files; opening a window leaves the thread's apartment alone, and on an " +
   "OLE thread its registration is confirmed by OLE refusing a second. RUN " +
   "on Windows.\n" +
   "      macOS: the view registers public.utf8-plain-text beside the file types " +
   "and hands a drag with no files to a TextDragDispatcher -- compiled, " +
   "never run; dragging text OUT is not implemented there (it needs an " +
   "NSDraggingSource and the mouse-down NSEvent). X11: the XDND peer does " +
   "not yet hand text to the dispatcher",
   "class TextDragAndDropTarget"],
  ["gui", "Relative positioning: MarkerList, RelativeCoordinate, RelativePoint, " +
          "RelativeRectangle, RelativePointPath, RelativeParallelogram, " +
          "RelativeCoordinatePositionerBase",
   "gfx/relative_position.h: RelativeCoordinate (an expression), " +
   "RelativePoint (\"x, y\"), RelativeRectangle (\"left, top, right, bottom\", " +
   "and applyToComponent), RelativeParallelogram (three corners, and the " +
   "transform onto them), RelativePointPath (a Path of RelativePoints), " +
   "MarkerList (named coordinates, themselves expressions) and " +
   "RelativeCoordinatePositionerBase, which keeps a component where its " +
   "expressions say by listening (ComponentListener) to its parent, every " +
   "sibling named by Component::componentID, and its marker lists, " +
   "re-applying when any changes -- a name that does not exist yet starts " +
   "working when it is added. Names: " +
   "parent.left/right/top/bottom/x/y/width/height/centreX/centreY, " +
   "<id>.<edge>, this.<edge>, markers; + - * / parentheses and min, max, " +
   "abs; evaluated in a RelativeScope.\n" +
   "      Cycles are detected, not looped on: a marker defined through itself " +
   "fails with the chain named (\"m1 -> m2 -> m3 -> m1\"), and a positioner " +
   "re-entered while applying stops and flags every positioner in the loop " +
   "(hasCycle) unless its answer has not changed -- so x from one component " +
   "and y from the other converges and is not a cycle. The evaluator is " +
   "detail::RelativeExpression, written here because the SDK has no general " +
   "Expression class: a bounded recursive-descent parser (length and depth " +
   "capped, numbers parsed without the C locale, non-finite ones refused) " +
   "behind a parse/evaluate/toString/symbols seam a core Expression could " +
   "later replace. Tested: precedence, functions, nine malformed expressions " +
   "refused with reasons, division by zero, toString round trip, edges " +
   "against parent and siblings following a parent resize, markers and a " +
   "marker change, both cycle kinds, the converging pair, late-added " +
   "siblings, a destroyed positioned component, points, parallelogram and " +
   "path; fuzzed (fuzz_gui_infra, 18,000 expression cases per run under " +
   "ASan). Platform-independent; RUN on Windows",
   "class RelativeCoordinatePositionerBase"],
  ["gui", "File chooser components: FileChooserDialogBox, FileListComponent, " +
          "FileTreeComponent, FilenameComponent, FilePreviewComponent, " +
          "ImagePreviewComponent, FileSearchPathListComponent",
   "gfx/file_chooser.h, beside file_browser.h's FileBrowser and file_dialog.h's " +
   "native chooser -- still the right call for \"load an impulse response\": the " +
   "user's places, network volumes, the macOS sandbox. FileListComponent shows " +
   "one folder as a list (icon, name, size), walked into by a double click or " +
   "Return and out by Backspace, with type-ahead; FileTreeComponent is " +
   "FileBrowser rooted at a folder; both tell a FilePreviewComponent about the " +
   "selection, and ImagePreviewComponent previews PNG and SVG files. " +
   "FilenameComponent is a path field with a browse button, an enforced suffix, " +
   "the recently used files behind a drop-down, and file drops. " +
   "FileSearchPathListComponent edits an ordered list of folders (add, remove, " +
   "change, move; joined with ';'). FileChooserDialogBox puts the list, a " +
   "preview, a name field and the buttons in an in-editor dialog -- open, save " +
   "(asking before it overwrites) or choose a folder -- keeps Tab inside it, and " +
   "answers through a callback.\n" +
   "      Nothing watches the disk: a folder is read when shown and on refresh(), as " +
   "FileBrowser does. Tested over a folder the test writes (with a PNG it " +
   "encodes itself) in gui_widgets_test and fuzz_gui_widgets; run on Windows " +
   "(MSVC /W4, and the fuzz under AddressSanitizer); the macOS and Linux " +
   "branches compiled, not run (macOS against an au_shim dirent.h, Linux against " +
   "the X11 headers)",
   "class FileChooserDialogBox"],
  ["gui", "ContentSharer",
   "out of scope: the iOS and Android share sheet. There is no mobile target " +
   "here -- AUv3 is listed as blocked for exactly that reason -- and no " +
   "desktop equivalent to map it onto",
   null, "class ContentSharer"],
  ["gui", "AccessibleState / AccessibilityActions",
   "gfx/accessible_info.h: the flags on AccessibleInfo -- focusable, focused, " +
   "enabled, checked, expanded -- and the actions a bridge invokes through the " +
   "control itself. The reference separates state into its own object because its " +
   "handler is attached BESIDE a component and has to describe something it " +
   "does not own. AccessibleInfo is produced BY the component, in one virtual " +
   "call, so the state is simply fields on the thing being described and " +
   "cannot fall out of step with it",
   "struct AccessibleInfo"],
  ["gui", "AccessibilityValueInterface / AccessibilityNumericValueInterface / " +
          "AccessibilityRangedNumericValueInterface / AccessibilityTextValueInterface",
   "gfx/accessible_info.h: value, hasRange, minValue, maxValue and " +
   "currentValue on the one struct. The reference splits these into four optional " +
   "interfaces a handler may implement; here a control fills the fields that " +
   "apply and leaves the rest, and hasRange is what tells a bridge whether the " +
   "numeric half means anything.\n" +
   "      The Windows bridge in gfx/uia_win32.h turns those fields into UIA's " +
   "Value and RangeValue patterns, which is the same split arriving at the " +
   "same place -- it is just made at the bridge rather than at every control",
   "bool hasRange"],
  ["gui", "AccessibilityTableInterface / AccessibilityCellInterface",
   "gfx/accessible_info.h: AccessibleRole::Table, with the row and column " +
   "counts and a cell's own position carried on AccessibleInfo. A table read " +
   "as a flat list of cells is a table a screen-reader user cannot navigate -- " +
   "they need to be told which row and column they are in -- which is why the " +
   "role is a closed set with Table in it rather than a string",
   "AccessibleRole::Table"],
  ["gui", "AccessibilityTextInterface",
   "gfx/text_editor.h and the AccessibleInfo it produces: the text, the caret " +
   "position and the selection. the reference's interface also offers ranged reads and " +
   "writes so a reader can walk a document by word or by line, which matters " +
   "for a text editor holding a document. The one here holds a parameter value " +
   "somebody is typing, where the whole content is a few characters and " +
   "announcing all of it is the correct behaviour rather than a compromise",
   "class TextEditor"],
  ["core", "Application",
   "application.h: Application, the reference's application object for programs " +
   "built on the SDK -- initialise(commandLine), shutdown, systemRequestedQuit, " +
   "anotherInstanceStarted, getApplicationName/Version, moreThanOneInstanceAllowed, " +
   "unhandledException, setApplicationReturnValue, quit -- and " +
   "SONORE_START_APPLICATION, which writes main (and WinMain on Windows, where " +
   "the command line is re-read as UTF-16). Application::run makes the calling " +
   "thread the message thread and runs MessageManager's dispatch loop. The " +
   "plugin formats still have no application object (the host owns the " +
   "process), and the plugin standalone keeps its own main in standalone.h.\n" +
   "      Single instance: a named mutex in the session's namespace on Windows, " +
   "flock on a temporary file elsewhere -- both released by the kernel with the " +
   "process, so a crash cannot leave the lock held. The first instance listens " +
   "BEFORE it takes the lock; a second hands its command line over " +
   "(broadcastMessage tagged with the name on Windows and macOS, an " +
   "abstract-namespace Unix socket on Linux) and exits without initialising. " +
   "The OS's quit requests reach systemRequestedQuit: WM_QUERYENDSESSION on a " +
   "hidden top-level window, console Ctrl+C and close, AppKit's " +
   "applicationShouldTerminate:, SIGINT/SIGTERM through a self-pipe the loop " +
   "polls. An exception from a queued callback " +
   "(MessageManager::setUnhandledExceptionHandler), a window procedure, " +
   "initialise or shutdown reaches unhandledException and the loop goes on.\n" +
   "      RUN on Windows, in child processes of tests/app_windows_test: the life " +
   "cycle and return value, the lock and the forwarded command line (quoted, " +
   "UTF-8), an exception thrown by a callback, the end-of-session query, and " +
   "the lock gone with the process. macOS and Linux are compiled only " +
   "(tests/mac_syntax_app_windows.cpp; Linux against the real Xlib headers)",
   "class Application {"],
  ["gui", "ApplicationCommandInfo",
   "commands.h: CommandInfo, with the id, the name and the Shortcut that " +
   "invokes it -- named as the reference names it so the parity walk can find it. The " +
   "row for CommandManager described the manager and never mentioned the " +
   "struct it is a table of",
   "struct CommandInfo"],
  ["extras", "WebBrowserComponent",
   "webview.h and gui.h: the whole web editor. WebView2 on Windows, WebKitGTK " +
   "on Linux, WKWebView on macOS, embedded in the window the host handed over " +
   "-- with the bridge injected BEFORE any page script runs, which is the part " +
   "that is easy to get wrong and impossible to notice, because a bridge that " +
   "arrives late leaves the page's own load handler with nothing to talk to.\n" +
   "      Not the default any more -- the native editor is -- and the cost " +
   "that made it so is now MEASURED rather than folklore, by " +
   "tests/webview_bench (hand-run like simd_bench, because memory numbers on " +
   "shared CI are noise with a pass mark). What it found, and what changed " +
   "because of it, on Windows:\n" +
   "      One user-data folder for ALL Sonore plugins and one environment " +
   "singleton per binary, because WebView2 shares the browser and GPU " +
   "processes only across webviews on the same folder + runtime -- and the " +
   "old per-plugin folders GUARANTEED two Sonore plugins paid for two full " +
   "browser trees. Measured: the second plugin's first editor went from " +
   "+152 MB to +29 MB; six editors across two plugins from 431 MB to 302 MB.\n" +
   "      Hidden editors SUSPEND (put_IsVisible false, the LOW memory target, " +
   "TrySuspend -- all verified succeeding), which stops CPU but returned a " +
   "measured -2 MB: freezing does not give commit back. So after a grace " +
   "period (30 s; SONORE_WEBVIEW_PARK_MS tunes it, 0 disables) a hidden " +
   "editor is PARKED -- its webview destroyed outright and rebuilt on show " +
   "from C++ state, which is where every value the page shows already lives. " +
   "Measured: six hidden editors, 302 MB -> 3 MB. The eval() guard while " +
   "hidden is load-bearing: ExecuteScript on a suspended renderer WAKES it, " +
   "so one frame of an invisible meter would undo the saving forever.\n" +
   "      The park cycle is gate-guarded, not just benched: the clap_gui_bridge " +
   "ctest hides past the grace, proves the webview is really gone, shows, and " +
   "requires the REBUILT page to drive the probe parameter again -- a rebuild " +
   "that comes up blank passes every step except that one.\n" +
   "      The GTK and WKWebView equivalents of environment sharing are " +
   "deliberately NOT written: neither can be run in this loop, and modern " +
   "WebKit pools web-content processes on its own anyway",
   "sharedEnvironment"],
  ["extras", "WebSliderRelay / WebComboBoxRelay / WebToggleButtonRelay / " +
             "WebControlParameterIndexReceiver",
   "gui.h: the bridge's sonore.params, sonore.get, sonore.set and the parameter " +
   "index carried on every message. Typed relays arrived later so a framework " +
   "Slider can be mirrored by a web control; the page here is authored per " +
   "plugin rather than mirroring a native widget, so it binds whatever it " +
   "likes to one small API",
   "sonore.set"],
  ["extras", "Native view embedding: HWNDComponent, NSViewComponent, UIViewComponent, " +
             "XEmbedComponent, ActiveXControlComponent, AndroidViewComponent",
   "gfx/native_embed.h: HWNDComponent, ActiveXControlComponent, NSViewComponent " +
   "and XEmbedComponent -- a platform control inside a component tree, the " +
   "reverse of the window_*.h peers. Each keeps its native window at the " +
   "component's place inside the peer's window of the TopLevelWindow it is in " +
   "(or a parent given with setParentWindow, for a plugin editor), checked on " +
   "resize, on paint (which every move causes) and on the window's tick (which " +
   "catches an ancestor hidden, since a hidden subtree is not painted).\n" +
   "      HWNDComponent makes a foreign HWND a child and gives it back, hidden " +
   "and with its own style, on release. ActiveXControlComponent hosts a control " +
   "by CLSID through IOleObject in-place activation in a site window, with its " +
   "own IOleClientSite, IOleInPlaceSite and IOleInPlaceFrame; queryInterface " +
   "reaches the control, setMouseEventsAllowed disables the site. " +
   "NSViewComponent adds an NSView to the peer's flipped view " +
   "(NSViewComponentWithParent, the reverse, is another row's). XEmbedComponent " +
   "speaks XEmbed as the embedder: a socket window, the client reparented in " +
   "and told XEMBED_EMBEDDED_NOTIFY, activation and focus, kept to the socket's " +
   "size -- or a socket id handed to another process to embed into. " +
   "UIViewComponent and AndroidViewComponent are not built: this SDK has no iOS " +
   "or Android target.\n" +
   "      RUN on Windows: a foreign HWND parented, moved, sized, hidden with an " +
   "ancestor and released; the WebBrowser control created, in-place active, " +
   "reached through IWebBrowser2, moved, mouse-disabled and deleted, and a " +
   "CLSID naming nothing failing cleanly. NSViewComponent and XEmbedComponent " +
   "are compiled only",
   "class HWNDComponent"],
  ["extras", "CodeEditorComponent / CodeDocument / CPlusPlusCodeTokeniser / LuaTokeniser / " +
             "XmlTokeniser",
   "gfx/code_editor.h, for the plugin that runs a user's script or lets a " +
   "preset's XML be edited by hand. CodeDocument keeps text as lines of code " +
   "points: CodeDocument::Position, maintained through every insert and " +
   "delete as a caret is; offsets with a line break counted once whatever it " +
   "is saved as, from line starts recounted only below an edit; each line's " +
   "own ending kept (CR LF, LF, a lone CR) so a mixed file saves byte for " +
   "byte, new breaks in the document's newline, normaliseLineEndings undoable; " +
   "undo as recorded edits in transactions (newTransaction), typing merged, " +
   "bounded by count and characters, with a save point; find forwards, back, " +
   "round the end, across lines, ignoring case; word breaks; listeners; " +
   "loadFromFile and saveToFile in UTF-8 or UTF-16 with their byte order " +
   "marks; a maximum size that refuses an insert, replace or load whole.\n" +
   "      CodeTokeniser reads a line at a time with a state carried between " +
   "lines -- block comments, raw strings, strings and directives continued " +
   "by a backslash, Lua long brackets and \\z, XML comments, CDATA, " +
   "declarations and attribute values -- in CPlusPlusCodeTokeniser, " +
   "LuaTokeniser and XmlTokeniser, each with the reference's token types and " +
   "default ColourScheme, and a state-only scan for the lines above the " +
   "screen. CodeEditorComponent: a monospaced grid (tabs to stops, wide " +
   "characters in two columns, marks in none), a line-number gutter, caret " +
   "and selection by mouse (word, line, gutter) and keys (words, Home to the " +
   "indent, Page Up and Down, up and down keeping the column), the SDK's " +
   "ScrollBars, the clipboard in the document's newline, undo and redo, Tab " +
   "and Shift+Tab indenting lines, auto-indent with brackets, read-only, find " +
   "and replace with Ctrl+F and Ctrl+H hooks for the plugin's own bar, font " +
   "size, caretBounds for an input method, an accessible text field. It paints " +
   "only the lines on screen and keeps each line's tokeniser state, reusing " +
   "the ones below an edit once a re-scan agrees with them.\n" +
   "      Tested by code_editor_test (thousands of random edits, undo and redo " +
   "against a model; maintained positions; line endings and encodings; every " +
   "tokeniser token by token; KeyPress-driven editing; gutter and token " +
   "colours in a Bitmap; 100,000 lines opened, scrolled to the end and edited " +
   "there, each action timed under 50 ms) and fuzz_code_editor, 20,000+ " +
   "cases under ASan. RUN on Windows only; the macOS and Linux branches -- " +
   "the monospaced font's paths and the in-process clipboard -- compiled, not " +
   "run. Not built: bidirectional text and Arabic joining on the grid, a " +
   "code-folding or completion layer, the reference's CodeDocument::Iterator",
   "class CodeEditorComponent"],
  ["extras", "ColourSelector",
   "gfx/colour_selector.h: ColourSelector, laid out as the reference lays it out " +
   "-- the colour across the top with an editable hex code (RRGGBB, or AARRGGBB " +
   "with the alpha channel), a saturation/brightness square beside a hue strip, " +
   "sliders for red, green, blue and alpha, and a row of swatches (click to use " +
   "one, right-click to store the current colour in it). Flags turn each part " +
   "off; onChange reports every change, and a colour set with notify false is " +
   "not echoed back.\n" +
   "      HUE IS KEPT, not recomputed from the colour: brightness dragged to black and " +
   "back up comes out at the hue it started from, where deriving HSV from the " +
   "colour snaps the strip to red. The square is two gradients composited -- " +
   "white to the hue across, transparent to black down -- which is HSV exactly, " +
   "so the test reads pixels back and compares them with the conversion the " +
   "selector itself uses. Arrows work the square and the strip, the hex field " +
   "takes typing, and a reader hears a group, a hue slider and a text field. " +
   "Drawn through LookAndFeel::drawColourSwatch, drawColourSpaceMarker and " +
   "drawHueStripThumb. Tested in gui_widgets_test (known HSV vectors, clicks and " +
   "drags at computed points, hex entry, swatches) and fuzz_gui_widgets; run on " +
   "Windows (MSVC /W4, and the fuzz under AddressSanitizer); the macOS and Linux " +
   "branches compiled, not run",
   "class ColourSelector"],
  ["extras", "SystemTrayIconComponent / SplashScreen / PreferencesPanel / " +
             "RecentlyOpenedFilesList / AppleRemoteDevice / PushNotifications / " +
             "AnimatedAppComponent",
   "application furniture, for programs that own their process.\n" +
   "      gfx/system_tray.h: SystemTrayIconComponent -- on Windows " +
   "Shell_NotifyIconW (version 4, the icon made from a Bitmap, tooltip, the " +
   "balloon, clicks delivered as the Component's mouseDown and contextMenu and " +
   "as callbacks, the icon re-added on TaskbarCreated); on macOS an " +
   "NSStatusItem; on Linux UNAVAILABLE, because the tray there is " +
   "StatusNotifierItem over D-Bus, a bus connection and object this SDK does " +
   "not carry (isAvailable says so). PushNotifications, the LOCAL half: Windows " +
   "through the tray balloon (a toast on 10 and 11), macOS " +
   "UNUserNotificationCenter, Linux notify-send; REMOTE push is not here -- it " +
   "needs APNs or FCM servers and credentials -- and getDeviceToken is empty.\n" +
   "      gfx/splash_screen.h: SplashScreen -- borderless, centred, topmost, off " +
   "the taskbar; deleteAfterDelay(minimum, removeOnMouseClick) fades it out " +
   "through the window's alpha once it has been up long enough and it deletes " +
   "itself from the queue, timed by advance(elapsedMs) from its window's clock.\n" +
   "      gfx/preferences_panel.h: PreferencesPanel -- square page buttons along " +
   "the top (an SVG icon above a label, on the LookAndFeel's icon-button plate) " +
   "and the chosen page below. Each page's component is made on demand by " +
   "createComponentForPage (or a createPage function) and destroyed when " +
   "another is chosen, as the reference does; the arrow keys walk the pages. RUN " +
   "on Windows: rendered to a Bitmap, with switching by click and by keyboard " +
   "checked in pixels.\n" +
   "      recent_files.h: RecentlyOpenedFilesList -- most recent first, 8 by " +
   "default, re-opening moves a file to the top, paths compared the way the " +
   "filesystem compares them, removeNonExistentFiles, toString/restoreFromString, " +
   "and menu items numbered base + index into gfx::PopupMenu. Registered with " +
   "the OS through SHAddToRecentDocs (RUN: the shortcut is checked by its " +
   "target, then removed by forgetRecentFileNatively) and NSDocumentController " +
   "(compiled only). Not registered on Linux, as in the reference.\n" +
   "      Not built: AppleRemoteDevice, because Apple removed the IR remote API; " +
   "and AnimatedAppComponent, deliberately -- it is a component with a timer " +
   "built into it, the shape this SDK avoids everywhere: every timed thing here, " +
   "SplashScreen included, takes the elapsed time as an ARGUMENT, which is what " +
   "makes a fade mid-way an exact assertion instead of a sleep.\n" +
   "      RUN on Windows (tests/app_windows_test): the tray icon added, modified, " +
   "given a balloon and removed with every Shell_NotifyIcon result counted, " +
   "clicks and the balloon's click as the shell sends them, Explorer's restart, " +
   "a notification and its click reaching the listener; the splash's style, " +
   "placement, timing at its edges, alpha read back from the window, the click " +
   "and the self-deletion. NSStatusItem and UNUserNotificationCenter are " +
   "compiled only",
   "class SystemTrayIconComponent : public Component"],
  ["extras", "BubbleMessageComponent",
   "gfx/alert.h: CallOutBox with a caption, and TooltipManager for the fading " +
   "kind. the reference's version is a BubbleComponent that shows a message and removes " +
   "itself after a delay -- the delay here belongs to TooltipManager, which " +
   "takes the clock as an argument so every one of its timing rules is a test " +
   "rather than a wait",
   "class TooltipManager"],
  ["extras", "KeyMappingEditorComponent",
   "gfx/key_mapping_editor.h over commands.h's CommandManager. The commands are " +
   "listed under their categories with their keys drawn as keycaps; \"+\" (or " +
   "Return on a row) waits for the next key -- Tab and Return included, Escape " +
   "alone cancels -- and assigns it; a keycap offers Change and Remove; Delete " +
   "removes a row's last key; Reset to defaults asks first. A key another " +
   "command already has is a QUESTION naming that command, in an AlertOverlay: " +
   "the manager would silently take it, which is right for code and wrong for a " +
   "person. Hidden and read-only commands through two callbacks; " +
   "onMappingsChanged is where an owner saves saveMappings(). KeyPress gained F1 " +
   "to F12 for it, mapped in all three peers and named by Shortcut::describe.\n" +
   "      Tested in gui_widgets_test -- reassign after the question, refuse, replace " +
   "through the keycap menu, Tab captured as a key, reset, the JSON round trip " +
   "-- and under fuzz_gui_widgets, which checks after every change that no key " +
   "belongs to two commands; run on Windows (MSVC /W4, and the fuzz under " +
   "AddressSanitizer); the macOS and Linux branches compiled, not run",
   "class KeyMappingEditorComponent"],
  ["extras", "FileBasedDocument",
   "file_document.h: FileBasedDocument, for the standalone and applications -- a plugin's " +
   "document is still the host's session, its dirty flag reported through " +
   "clap_host_state::mark_dirty. The reference's paths: load, load a chosen file, save, " +
   "save as, save as interactively and the close-a-window question, each in a plain and an " +
   "Async form, around the four things a subclass supplies (title, load, save, last " +
   "document).\n" +
   "      The questions are virtual hooks that answer through a callback: native by default " +
   "(MessageBoxW, NSAlert, a GTK dialog, and FileDialog for the choosers), an AlertOverlay " +
   "in the window when overridden. A failed save leaves the document on the file it had, " +
   "still changed; a question answered after a plain form stopped waiting changes nothing; " +
   "a document deleted before an answer is never called back. setLastDocumentOpened feeds a " +
   "RecentlyOpenedFilesList.\n" +
   "      RUN on Windows with the hooks scripted, and with a real AlertOverlay answered from " +
   "the keyboard; the native boxes are written for all three platforms and compiled on " +
   "macOS and Linux",
   "class FileBasedDocument"],
  ["core", "Timer / TimedCallback",
   "multi_timer.h: MultiTimer, and the 33 ms tick every peer runs. Both take " +
   "the elapsed time as an ARGUMENT rather than reading a clock, which is the " +
   "rule everything timed here follows -- it is what makes \"a late timer " +
   "fires ONCE, not once per missed interval\" an exact assertion instead of a " +
   "thing you watch for.\n" +
   "      A plugin gets its tick from the window it already has, so there is " +
   "no timer thread and nothing to stop when an editor closes",
   "class MultiTimer"],
  ["core", "ActionBroadcaster / ActionListener",
   "broadcaster.h: ChangeBroadcaster and ChangeListener, which are the same " +
   "pattern. The reference has both because an Action carries a STRING and is delivered " +
   "asynchronously, while a Change is a bare notification coalesced to one per " +
   "message loop. A string-addressed broadcast inside one plugin is a " +
   "dispatch nobody can check at compile time, and coalescing is the behaviour " +
   "worth having -- so there is one, and it is the coalescing one",
   "class ChangeBroadcaster"],
  ["core", "MessageManager / MessageManagerLock / CallbackMessage / MessageListener",
   "message_manager.h: MessageManager, MessageManagerLock, CallbackMessage, Message and " +
   "MessageListener, with ThreadExitSignal as the lock's abort hook. callAsync; a blocking " +
   "callFunctionOnMessageThread that runs at once on the message thread, or on a thread " +
   "holding the lock, instead of waiting for itself; isThisTheMessageThread and " +
   "setCurrentThreadAsMessageThread; runDispatchLoop, stopDispatchLoop and " +
   "runDispatchLoopUntil for an application; and broadcastMessage to other processes.\n" +
   "      One queue of our own, with the OS only as the alarm clock: a message-only window " +
   "on Windows, a CFRunLoopSource in the common modes on macOS, an eventfd on Linux that " +
   "runDispatchLoop polls and a host's fd support can be handed. One OS message per call " +
   "is the obvious design and fails three ways -- Windows drops posted messages past " +
   "10,000 per thread, a nested modal loop reorders them, and a lock request has to know " +
   "whether it was ever delivered.\n" +
   "      The lock parks the message thread inside a message, between two others, until it " +
   "is released. Given a ThreadExitSignal or a ThreadPool job's stop flag, the wait gives " +
   "up, so a worker stopped and joined from the message thread cannot deadlock against it. " +
   "The old row's rule still stands for the AUDIO thread: none of this may be called from " +
   "it, and UiEventQueue still carries the parameter traffic on the editor's tick.\n" +
   "      RUN on Windows by app_infra_test, plain and under ASan: 20,000 calls from four " +
   "threads, each thread's order kept; 10,000 lock acquisitions from four threads while the " +
   "message thread pumps, with a counter touched only under the lock coming out exact (and " +
   "the test failing when the message thread is not parked); 600 signal-against-grant " +
   "races; a stop signal ending a wait the joining message thread could never grant; a listener deleted with messages in flight never called; a broadcast to a " +
   "child process and its reply. macOS and Linux are compiled only, by MSVC and by clang, " +
   "through tests/au_shim and POSIX stubs",
   "class MessageManagerLock"],
  ["core", "LockingAsyncUpdater",
   "message_manager.h: LockingAsyncUpdater. Triggered from any thread and coalesced into " +
   "one call on the message thread; the callback runs under the updater's lock and " +
   "cancelPendingUpdate takes the same one, so once cancel returns a pending callback will " +
   "not start and none is still running -- the guarantee an updater built on a bare flag " +
   "cannot give. The lock is recursive, so the callback may re-trigger or cancel itself, and " +
   "a copy of the callback runs, so it may delete its own updater.\n" +
   "      RUN on Windows, under ASan too: 20,000 trigger/cancel rounds against the pumping " +
   "message thread, about 17,000 callbacks and 1,600 cancels arriving mid-callback, none " +
   "started after a cancel returned and none running when it returned. With the callback " +
   "moved outside the lock the same test fails, 1,700 times",
   "class LockingAsyncUpdater"],
  ["core", "Inter-process: ChildProcessCoordinator, ChildProcessWorker, " +
           "InterprocessConnection, NetworkServiceDiscovery, MountedVolumeListChangeDetector, " +
           "DeletedAtShutdown, ApplicationBase",
   "ipc.h and process.h. ChildProcessCoordinator launches a worker -- any " +
   "executable, usually the program itself in a worker mode, found by a " +
   "\"<id>:<pipe>\" argument -- and ChildProcessWorker connects back; both ping " +
   "every quarter of the timeout, so a worker that crashes is noticed at once " +
   "(its pipe ends) and one that hangs once it has been silent for the " +
   "timeout. InterprocessConnection carries whole messages over a TCP socket " +
   "or a NamedPipe in the reference's framing (a magic number and a " +
   "little-endian length, so a peer built with the reference can talk to it " +
   "over a socket), its callbacks on its own thread; " +
   "InterprocessConnectionServer accepts socket clients into them. " +
   "NetworkServiceDiscovery's Advertiser broadcasts a small text datagram and " +
   "its AvailableServiceList listens and forgets what goes quiet. process.h " +
   "also has ChildProcess (arguments as a list, never a shell; stdout and " +
   "stderr drained by threads so a chatty child never blocks; killed with its " +
   "object), InterProcessLock, DynamicLibrary, currentExecutablePath and " +
   "currentModulePath, and DeletedAtShutdown (deleteAll, newest first). " +
   "MountedVolumeListChangeDetector is in file_types.h, polling the volume " +
   "list on its own thread. ApplicationBase is the Application row's answer: a " +
   "plugin has no application object, and the standalone's lifecycle is its " +
   "main().\n" +
   "      Out of scope until now, on the grounds that a plugin runs inside the host. " +
   "An out-of-process scanner, or a helper doing work a host would crash on, " +
   "is honest work all the same -- and doing it wrong (a child inheriting the " +
   "host's handles, a command line through a shell, a process left behind when " +
   "the plugin is removed from the track) is worse than not doing it. So a " +
   "child inherits exactly its three standard streams " +
   "(PROC_THREAD_ATTRIBUTE_HANDLE_LIST on Windows; close-on-exec and " +
   "POSIX_SPAWN_CLOEXEC_DEFAULT elsewhere) and dies with its object. RUN on " +
   "Windows, also under ASan (sdk/tests/system_test relaunches itself: twelve " +
   "awkward arguments round-tripped through CommandLineToArgvW, a worker that " +
   "crashes and one that goes silent, pipes and sockets echoing messages up to " +
   "a megabyte, a lock held by another process); the POSIX branches " +
   "(posix_spawn, Unix-domain sockets, flock) are COMPILED for macOS and Linux " +
   "through tests/au_shim by MSVC and clang, and have not run",
   "class ChildProcessCoordinator"],
  ["core", "ValueTree / ValueTreeSynchroniser / ValueTreePropertyWithDefault / CachedValue",
   "state_bag.h: StateBag, a flat key/value store that saves and restores with " +
   "the parameters. the reference's ValueTree is a shared, reference-counted, " +
   "hierarchical, listener-bearing document with its own undo integration and " +
   "a synchroniser that can replicate changes over a wire.\n" +
   "      Flat and not hierarchical on purpose: the state a plugin has beyond " +
   "its parameters is the sample it loaded and a handful of switches. A tree " +
   "would let a DSP store a document, and a document is a thing to migrate " +
   "every time the plugin changes. CachedValue and " +
   "ValueTreePropertyWithDefault both exist to make a tree bearable to read " +
   "from, which a flat bag does not need",
   "class StateBag"],
  ["core", "UndoableAction",
   "undo.h: UndoHistory stores whole STATES rather than actions. The reference stores " +
   "an action pair -- do and undo -- which is smaller and lets a big document " +
   "be edited without copying it.\n" +
   "      A plugin's state is a parameter array and a small bag, so a whole " +
   "copy is cheap and has a property an action pair does not: it cannot be " +
   "WRONG. An undo action that does not exactly reverse its do is a bug that " +
   "corrupts state slowly and only under sequences nobody tried, and it is the " +
   "commonest bug in undo systems",
   "class UndoHistory"],
  ["core", "ApplicationProperties",
   "application_properties.h: ApplicationProperties over PropertiesFile, with the " +
   "reference's Options -- application and folder name, suffix, the macOS library " +
   "sub-folder, common to all users, case-insensitive keys, doNotSave, " +
   "millisecondsBeforeSaving, storage format. A plugin still wants user_settings.h's " +
   "UserSettings; this is for an application, which has a machine as well as a user.\n" +
   "      The user's file in %APPDATA%, ~/Library/Application Support (or Preferences) or " +
   "$XDG_CONFIG_HOME; the shared one in %ProgramData%, /Library or /etc/xdg. The user's " +
   "reads through to the shared one, and getCommonSettings(true) hands back the user's when " +
   "the shared one cannot be written. XML in the reference's own <PROPERTIES> of <VALUE " +
   "name val> shape, so its files load, or the StateBag binary behind a magic -- either is " +
   "read whatever the options say. The delayed save is debounced and driven by " +
   "advance(elapsedMs), like every timed thing here. The shared file is read and written " +
   "under an OS file lock (LockFileEx or flock, on a .lock beside it), which dies with its " +
   "process; the write is user_settings.h's atomic one, now shared.\n" +
   "      RUN on Windows: the paths, the debounced save, round trips of awkward text, the " +
   "read-only fallback, both formats, a hand-written reference-style file, a held lock " +
   "making a save give up; the readers fuzzed by fuzz_app_infra, under ASan. The macOS and " +
   "Linux paths are compiled only",
   "class ApplicationProperties"],
  ["basics", "MPENote / MPEValue / MPEChannelAssigner",
   "audio.h: NoteExpressionBuffer carries the per-note pitch, timbre and " +
   "pressure, and MpeDecoder turns MPE's channel-per-note MIDI into it. The reference " +
   "models a NOTE as an object with those values on it; here the note is the " +
   "DSP's own voice and the expression arrives beside the MIDI as events " +
   "against a note id, which is the shape every format's own expression event " +
   "already has -- CLAP note expressions, VST3 note expression, AUv3.\n" +
   "      MPEValue exists in the reference because MPE's 14-bit values need a type that " +
   "converts between the MIDI representation and a float without anybody " +
   "rounding twice. That conversion happens once, in MpeDecoder, and what " +
   "leaves it is a float",
   "class NoteExpressionBuffer"],
  ["basics", "MPESynthesiser",
   "dsp.h: VoiceManager, which allocates voices and takes expression per " +
   "voice. The reference has a separate MPE flavour because its Synthesiser predates " +
   "MPE and could not be changed without breaking every plugin using it. There " +
   "is no such history here -- the voice allocator has taken per-note " +
   "expression since it was written, so a synth that ignores it is an ordinary " +
   "polyphonic synth and one that reads it is MPE",
   "class VoiceManager"],
  ["basics", "AudioSource family: BufferingAudioSource, ChannelRemappingAudioSource, " +
             "IIRFilterAudioSource, MemoryAudioSource, ReverbAudioSource, " +
             "ResamplingAudioSource, MixerAudioSource, ToneGeneratorAudioSource",
   "audio_source.h: the reference's PULL model -- a source asked for its next " +
   "block, pulling from whatever it is chained to -- as the target for code " +
   "ported from it, built on the SDK's own parts. AudioSource and " +
   "PositionableAudioSource with AudioSourceChannelInfo over an AudioBlock; " +
   "MemoryAudioSource (WavData or planar channels, copied or borrowed, " +
   "looping), AudioFormatReaderSource over AudioFileReader, " +
   "ToneGeneratorAudioSource, ChannelRemappingAudioSource with the " +
   "reference's input/output map semantics, IIRFilterAudioSource taking a " +
   "Biquad's coefficients, ReverbAudioSource over Reverb, " +
   "ResamplingAudioSource (WindowedSincKernel read from its table, an " +
   "eighth-order SvfSection anti-alias lowpass crossfaded in above a ratio of " +
   "1, ratio changes gliding per sample), MixerAudioSource, " +
   "BufferingAudioSource reading ahead on a ReadAheadThread (the cooperative " +
   "TimeSliceThread shape, since ThreadPool runs jobs to completion and " +
   "read-ahead never completes), AudioTransportSource (start, stop, seek and " +
   "gain all ramped, read-ahead and rate correction in its chain, " +
   "ChangeBroadcaster on finish), and AudioSourcePlayer, which hands the " +
   "backends' RenderFn shape (render, renderWithInput for ASIO) and the " +
   "reference's planar callback to a source.\n" +
   "      Where the reference takes a lock inside the callback -- mixer, " +
   "transport, remapper, filter, reverb, player -- nothing here does: small " +
   "settings cross by atomics or a seqlock the audio thread reads without " +
   "waiting, and anything with a lifetime (the mixer's list, the transport's " +
   "chain, the player's source, the read-ahead ring's slots) is swapped by " +
   "pointer and freed only after the audio thread has provably left any block " +
   "that saw it; the changing thread waits, the audio thread never does. " +
   "Every [audio thread] body is read by audio-thread-rules.mjs for " +
   "allocation, locks, waits and I/O. AudioFormatReaderSource reads a file and " +
   "says so; behind a BufferingAudioSource it runs on the read-ahead thread.\n" +
   "      Tested (sdk/tests/audio_sources_test.cpp, 86 checks): the IIR and " +
   "reverb sources bit-exact against Biquad and Reverb run by hand; " +
   "resampling at ratio 1 bit-exact, length and frequency at 44.1->48 k, " +
   "within -89 dB of resample.h's Resampler, 30 kHz at ratio 2 down 39 dB, " +
   "a 1->1.5 glide with no step; buffering identical to an unbuffered read " +
   "behind a reader that sleeps, the slowest callback microseconds while the " +
   "reader slept 100-200 ms; a mixer stressed with 200 000 adds and removes " +
   "against 370 000 blocks with no torn sum; replaced operator new counting " +
   "zero allocations inside every callback. RUN on Windows x64 (MSVC /W4, " +
   "and under ASan); macOS and Linux only compiled (/Zs with _WIN32 " +
   "undefined) -- the header has no platform code of its own",
   "class BufferingAudioSource"],
  ["basics", "IIRCoefficients / GenericInterpolator",
   "dsp.h: Biquad carries its own coefficients and the designers that fill " +
   "them; interpolation.h has Interpolator with the kernels selectable. The reference " +
   "separates coefficients from the filter so one set can be shared across " +
   "many instances, which matters for a large multi-channel graph; a plugin's " +
   "filter count is small and a Biquad holding its own coefficients is one " +
   "object to reason about instead of two whose lifetimes must agree",
   "class Biquad"],
  ["basics", "AudioWorkgroup / WorkgroupToken",
   "audio_workgroup.h: WorkgroupToken is one thread's membership -- " +
   "os_workgroup_join fills it, leave() or the destructor ends it, on the joining " +
   "thread only; a leave from another thread is refused and reported. The helper " +
   "threads this row once said nothing here spawned exist now: a ThreadPool built " +
   "with RealtimeThreadHints (thread_pool.h) makes each worker a Mach " +
   "time-constraint thread -- period, computation and constraint from the block " +
   "length and the rate, clamped to the kernel's 50 us to 50 ms -- and joins it to " +
   "the workgroup it is given: setAudioWorkgroup() from the main thread, " +
   "offerAudioWorkgroup() wait-free from the audio thread, where an AU's " +
   "workgroupChanged() runs. The interval calls (os_workgroup_interval_*) belong " +
   "to the group's owner, CoreAudio's IO thread, and a joiner does not make them.\n" +
   "      COMPILED for macOS, NEVER RUN (mac_host_selftest.cpp is the run). Off " +
   "macOS the token is inactive and describe() says audio workgroups are an Apple " +
   "API, and a realtime pool runs its jobs and workgroupState() says nothing was " +
   "joined and why -- RUN on Windows by mac_host_test.cpp, with the Mach tick " +
   "arithmetic checked against known timebases (1/1 and 125/3)",
   "class WorkgroupToken"],
  ["graphics", "ScopedBlendContext",
   "gfx/graphics.h: Graphics::ScopedState, which saves and restores the clip, " +
   "the transform and the colour together. The reference added a separate scoped BLEND " +
   "object in 8 for its GPU backend, where the blend mode is pipeline state " +
   "with a real cost to change. Everything here composites premultiplied RGBA " +
   "in software with one blend rule, so there is no second piece of state to " +
   "scope",
   "class ScopedState"],
  ["extras", "AudioThumbnailCache",
   "thumbnail_cache.h: AudioThumbnailCache -- computed AudioThumbnails kept by a " +
   "64-bit key: hashFile (path, length and modification time, so a file the " +
   "user edits gets a NEW key and nothing has to be invalidated) or hashData " +
   "(FNV-1a, for audio that never had a file). Bounded by BYTES rather than the " +
   "reference's count -- a one-shot's thumbnail and an hour-long stem's differ " +
   "a hundredfold -- with the least recently used evicted first and a lookup " +
   "counting as a use. storeThumb/loadThumb/removeThumb/clear, and " +
   "saveTo/loadFrom for the whole cache, most recent first, so a browser opens " +
   "with the waveforms it showed last time; AudioThumbnail gained saveTo/" +
   "loadFrom for one. Both loaders treat the bytes as untrusted: any damage " +
   "refuses the lot and leaves what was there. Mutex-guarded, for a loader " +
   "thread storing while the UI looks up.\n" +
   "      Portable code, RUN here on Windows: audio_io_test (budget, LRU order, " +
   "round trips, refusals, FNV-1a known answers, keys that change with the " +
   "file, a two-thread soak) and fuzz_audio_io (150 000 mutants under ASan, " +
   "every accepted load a save/load fixed point)",
   "class AudioThumbnailCache"],
  ["extras", "MPEKeyboardComponent",
   "gfx/mpe_keyboard.h: MPEKeyboardComponent, MidiKeyboard's keys with every " +
   "sounding note drawn over them as a circle -- across with its pitch (key, " +
   "own bend and its zone's master bend, placed between key centres so a " +
   "slide moves evenly over black and white keys), up with its timbre, " +
   "larger with its pressure. Fed the DSP's own MIDI through MpeDecoder, so " +
   "zones, RPN 0 bend ranges and master-channel bends are read by the code " +
   "the DSP uses, or a host's NoteExpressionBuffer; each channel's last " +
   "bend, pressure and timbre are kept, because MPE sends them BEFORE the " +
   "note-on.\n" +
   "      The old reason for its absence was that a mouse cannot slide and press. " +
   "It has two axes, though: a press plays on the free member channel used " +
   "longest ago, a sideways drag bends from where the button went down, and " +
   "height sets pressure (or timbre, setVerticalGesture), sent as real MPE " +
   "-- bend, CC 74 and pressure before the note-on, in the order the MPE " +
   "specification gives. Tested by rendering a scripted MPE sequence to a " +
   "Bitmap and finding each note where its pitch, timbre and pressure put " +
   "it, and by driving the mouse through MouseRouter and checking every " +
   "message it sent. Run on Windows; parsed as macOS and Linux",
   "class MPEKeyboardComponent"],
  ["extras", "AudioProcessorPlayer / SoundPlayer / AudioAppComponent",
   "standalone.h does this job: it opens the device, prepares the DSP, pumps " +
   "blocks, and feeds MIDI in. the reference's classes are the glue between an " +
   "AudioProcessor and an AudioDeviceManager for an application that hosts " +
   "one; the standalone is that application, and it is one file rather than a " +
   "set of components because there is exactly one plugin in it and no graph " +
   "to build",
   "SONORE_BUILD_STANDALONE"],
  ["extras", "AudioCDReader / AudioCDBurner / BluetoothMidiDevicePairingDialogue",
   "AudioCDReader and AudioCDBurner are BUILT, in audio_cd.h, for " +
   "Windows. The reader opens a drive and reads its table of contents " +
   "(IOCTL_CDROM_READ_TOC_EX, parsed from the raw bytes by CDToc::parse, which " +
   "refuses a TOC that does not describe a disc) and its audio " +
   "(IOCTL_CDROM_RAW_READ in CDDA mode, 588 stereo frames a sector, retried and " +
   "then split per sector), with track offsets in samples, data tracks flagged " +
   "and the CDDB id. The burner lists IMAPI2's recorders, reports the disc, its " +
   "write speeds and free sectors, queues tracks as 16-bit stereo padded to " +
   "whole sectors and the Red Book's four seconds, and burns them through " +
   "IDiscFormat2TrackAtOnce on a thread of its own in the multithreaded " +
   "apartment -- IMAPI2's progress events reported on the CALLER's thread, " +
   "which can cancel. macOS and Linux report no drives.\n" +
   "      What was exercised: this machine has NO optical drive. audio_io_test " +
   "runs the TOC parser on a known six-track disc, an Enhanced CD and eight " +
   "broken TOCs, the sector and sample arithmetic, the CDDB id (0x3404F606, " +
   "worked by hand), the burner's track preparation and every no-drive path; " +
   "fuzz_audio_io mutates TOCs under ASan. The DeviceIoControl and IMAPI2 calls " +
   "compile at /W4 and have never read or burned a real disc.\n" +
   "      BluetoothMidiDevicePairingDialogue: bluetooth_midi.h opens the " +
   "system's own pairing dialogue without blocking. macOS: " +
   "CABTLEMIDIWindowController (CoreAudioKit, 10.11+), modeless, onClosed when " +
   "its window closes; COMPILED against au_shim, NOT RUN. Windows: Settings > " +
   "Bluetooth (ms-settings:bluetooth through ShellExecuteW), available when " +
   "ms-settings: is registered; RUN up to the launch " +
   "(SONORE_BLUETOOTH_MIDI_NO_LAUNCH). BLE device enumeration through " +
   "Windows.Devices.Enumeration is deliberately not built: MidiInput uses " +
   "WinMM, which does not list BLE MIDI ports. Linux: isAvailable() is false " +
   "and unavailableReason() names BlueZ/D-Bus and the bluetoothctl and ALSA " +
   "sequencer route.\n" +
   "      Box2DRenderer, once listed here, is built and has its own row under " +
   "box2d: gfx/box2d_renderer.h draws the plugin's OWN Box2D and vendors " +
   "nothing",
   "class BluetoothMidiDevicePairingDialogue"],
  ["core", "The C++ standard library does it: Array, OwnedArray, ReferenceCountedArray, " +
           "SortedSet, SparseSet, HashMap, Span, Optional, " +
           "NamedValueSet, PropertySet, String, StringArray, StringPairArray, StringRef, " +
           "StringPool, Identifier, CharacterFunctions, NewLine, MemoryBlock, HeapBlock, " +
           "Atomic, ByteOrder, ReferenceCountedObject, SharedResourcePointer, WeakReference, " +
           "OptionalScopedPointer, SingletonHolder, ScopedValueSetter, ScopeGuard, " +
           "ListenerList, Thread, ThreadLocalValue, CriticalSection, ReadWriteLock, SpinLock, " +
           "WaitableEvent, GenericScopedLock, Result",
   "std::vector, std::string, std::unordered_map, std::optional, std::variant, " +
   "std::shared_ptr, std::weak_ptr, std::atomic, std::mutex, std::shared_mutex, " +
   "std::condition_variable, std::thread, std::function.\n" +
   "      One row for forty classes because they have one reason. The reference was " +
   "written before C++11 and had to work on compilers that had none of this, " +
   "so it grew its own of everything -- and could not drop them afterwards " +
   "without breaking every project built on them. This SDK started at C++17 " +
   "and is header-only, so the standard library is available in every " +
   "translation unit that includes anything here.\n" +
   "      Which is not merely equivalent, it is better in the way that " +
   "matters: every one of those types is one this SDK does not have to keep " +
   "correct, and one a plugin author already knows. Optional and Span are the " +
   "clearest cases -- the reference 8 still ships its own because its minimum standard " +
   "is older than std::optional.\n" +
   "      Where a std type genuinely is not enough, there is a row for the " +
   "thing that replaced it: AbstractFifo, Range, NormalisableRange, and the " +
   "file and JSON handling all have their own -- and var and DynamicObject, " +
   "which are a scripting language's values and live with the interpreter, " +
   "in the javascript rows",
   "std::vector"],
  ["core", "AbstractFifo / SingleThreadedAbstractFifo",
   "audio.h: UiEventQueue and the ring buffers, which are single-producer " +
   "single-consumer and lock-free. the reference's is the same idea generalised, and " +
   "the generalisation is the part that does not carry over -- a FIFO that " +
   "does not know how many readers it has cannot be checked by the RT-safety " +
   "gate, and \"is this reachable from the audio thread without a lock\" is a " +
   "question this project answers mechanically rather than by inspection",
   "class UiEventQueue"],
  ["core", "Range / Tolerance / MathConstants",
   "audio.h and dsp.h: a parameter's range is minValue and maxValue on " +
   "ParamInfo with the skew beside them, and the constants are constexpr where " +
   "they are used. the reference's Range is a general interval type used across its " +
   "whole API; the intervals here are all parameter ranges, and they are " +
   "already carried by the struct that describes a parameter -- a second range " +
   "type would be a second place for a minimum to live",
   "float minValue"],
  ["core", "File handling: FileFilter, WildcardFileFilter, FileSearchPath, TemporaryFile, " +
           "MemoryMappedFile, FileInputStream, FileOutputStream, BufferedInputStream, " +
           "SubregionStream, InputSource",
   "streams.h and file_types.h. InputStream and OutputStream with the " +
   "reference's typed readers and writers (byte order named, values built from " +
   "bytes), MemoryInputStream, MemoryOutputStream, BufferedInputStream, " +
   "SubregionStream and InputSource; FileInputStream, FileOutputStream " +
   "(opening an existing file at its end, as the reference's does) and " +
   "FileInputSource, by UTF-8 path through path_io.h; TemporaryFile, which is " +
   "written beside the target and renamed over it, so a save leaves the old " +
   "file or the new one and never half of each; MemoryMappedFile, any range of " +
   "a file, read-only or read-write (the general form of system_font.h's font " +
   "mapping); WildcardFileFilter -- '*' and '?' by code point, case ignored on " +
   "every platform, linear in its input, and built from file_dialog.h's " +
   "FileFilter -- and FileSearchPath. files.h keeps the directory listing they " +
   "stand on.\n" +
   "      This was out of scope on the grounds that a plugin reads one kind of file. " +
   "It reads from more than one kind of SOURCE -- a file, a slice of an " +
   "archive, a download -- and code that takes an InputStream& reads them all; " +
   "net.h's WebInputStream is one. RUN on Windows, also under ASan " +
   "(sdk/tests/system_test: a path with accents and CJK, a range mapped at an " +
   "unaligned offset, a temporary swapped in and one abandoned, a recursive " +
   "search over a real tree); the POSIX branches (fseeko, mmap, flock, " +
   "ftruncate) are COMPILED for macOS and Linux through tests/au_shim by MSVC " +
   "and clang",
   "class MemoryMappedFile"],
  ["core", "XmlElement / XmlDocument",
   "xml.h: XmlElement and XmlDocument in the reference's shape -- typed attribute " +
   "getters (int, double, bool; whole-value, saturating, '.' as the point under a " +
   "comma locale), getChildByName / getChildByAttribute, createNewChildElement, " +
   "text elements and getAllSubText, iteration, deep copies, isEquivalentTo, and " +
   "toString / writeToFile with escaping, pretty printing and attribute wrapping. " +
   "It was absent while plugin state was the only thing to read; what changed is " +
   "what a plugin has to READ -- a VST3 preset's metadata, an .aupreset, a " +
   "DAWproject file, and users' presets saved by plugins ported from the reference.\n" +
   "      UTF-8 XML 1.0: elements, attributes, text, CDATA, comments, processing " +
   "instructions, the five entities and character references, Latin-1 transcoded, " +
   "UTF-16 refused by name. A DOCTYPE is read past and never acted on -- nothing is " +
   "fetched (no XXE) and no declared entity is expanded (no billion laughs); limits " +
   "on size, depth and node count refuse hostile input, and nothing that walks a " +
   "tree recurses, so 100,000 levels parse, copy, print and free without touching " +
   "the stack. Errors give line and column.\n" +
   "      Tested in tests/core_data_test.cpp (known documents both ways byte for " +
   "byte, 4,500 random trees round-tripped, 38 malformed documents refused by name, " +
   "XXE and billion laughs) and fuzzed in tests/fuzz_core_data.cpp with a " +
   "write-and-reread property, under ASan. RUN on Windows (MSVC, and MSVC ASan); " +
   "Linux and macOS only compiled (MSVC and clang, /U_WIN32), the code being " +
   "platform-free",
   "class XmlElement"],
  ["core", "GZIPCompressorOutputStream / GZIPDecompressorInputStream",
   "inflate.h: the DEFLATE decompressor, which is what zip.h reads entries " +
   "with. Decompression only, and on purpose -- a plugin READS compressed " +
   "things (a preset pack, an impulse response bundle) and has no reason to " +
   "write one. Half the code, and the half with no attack surface from a file " +
   "somebody downloaded",
   "class Inflate"],
  ["core", "StatisticsAccumulator",
   "statistics.h: StatisticsAccumulator<FloatType> -- count, mean, population and " +
   "sample variance, standard deviations, RMS, minimum and maximum, and += to merge " +
   "two (per-block statistics summed into per-second ones). Welford's update, not a " +
   "sum of squares: for a DC offset of 1e9 with a ripple of one -- what a meter " +
   "measures -- the sum-of-squares variance comes out NEGATIVE (-14208 in the test, " +
   "for a true 0.332) and Welford's matches a two-pass computation. float is accumulated in double. Allocation-free " +
   "and noexcept, so usable on the audio thread at one division per value.\n" +
   "      Tested in tests/core_data_test.cpp against a two-pass computation over " +
   "100,000 values, merging, empty, NaN and long double. RUN on Windows (MSVC, and " +
   "MSVC ASan); Linux and macOS only compiled",
   "class StatisticsAccumulator"],
  ["core", "BigInteger",
   "bigint.h: BigInteger, signed integers of any size -- sign and magnitude over " +
   "32-bit limbs, built so RSAKey and Primes have something to stand on and named " +
   "as the reference names it. From any machine integer (INT64_MIN included), from " +
   "text in bases 2, 8, 10 and 16, and from little-endian bytes as the reference's " +
   "loadFromMemoryBlock reads them; back out through toString in the same bases and " +
   "toMemoryBlock. + - * / % with truncating division, divideBy, " +
   "findGreatestCommonDivisor, exponentModulo (negative exponents through the " +
   "inverse), inverseModulo; setBit, clearBit, getHighestBit, findNextSetBit, " +
   "getBitRange, & | ^ << >> on the magnitude; every comparison.\n" +
   "      Schoolbook multiplication and Knuth's Algorithm D, written with unsigned " +
   "borrows rather than the textbook's signed shifts; exponentModulo normalises " +
   "the modulus once and reduces into reused buffers. No Karatsuba: its crossover " +
   "is in the tens of limbs, so at RSA sizes it would earn little and cost a second " +
   "multiplier to keep right. " +
   "Where it parts from the reference it says so: parseString REFUSES a stray " +
   "character instead of skipping it, so a key file with a typo fails to load " +
   "rather than loading as a different key.\n" +
   "      Not the licence check's integer. license.h keeps its own minimal one for " +
   "the single signature it verifies, so what a licence accepts cannot move with " +
   "this class; the test cross-checks the two on random inputs and changes " +
   "neither.\n" +
   "      Tested (tests/core_crypto_test.cpp) against sums, products, quotients, " +
   "gcds, powers and inverses computed by Python's integers up to 6,144 bits; " +
   "against the identities a == q*b + r, (a*b)/b == a, distributivity and the " +
   "shifts on 2,500 random signed pairs; against int64 on 20,000 small pairs; " +
   "against repeated multiplication for 3,000 small powers; against license.h's " +
   "BigInt on random 64- to 1024-bit exponentiations; and by round trips through " +
   "every base and through bytes. tests/fuzz_core_crypto.cpp feeds mutated text " +
   "to the parser and checks it against an independent canonicaliser: 25,000 " +
   "cases under ASan, clean. RUN on Windows x64 (MSVC, /W4, ASan); the code is " +
   "portable C++ with no platform branch, compiled but not run elsewhere",
   "class BigInteger"],
  ["core", "Expression",
   "expression.h: Expression -- + - * / and unary minus, parentheses, numbers, " +
   "symbols (dotted ones included) resolved through a Scope at evaluation, and " +
   "sin cos tan asin acos atan sqrt abs exp log log10 floor ceil round pow atan2 " +
   "fmod min max; a Scope may add functions. Expression::VariableScope gives symbols " +
   "values, which may themselves be expressions. Also the reference's introspection " +
   "(getType, getInput, getSymbolOrFunction), operators to build one in code, " +
   "withRenamedSymbol, findReferencedSymbols, simplified() and a toString that " +
   "parses back to the same tree.\n" +
   "      A plugin's own arithmetic is still code; this is for what a USER types -- " +
   "a value box taking \"440 * 2\", a modulation amount as a formula, a tuning as " +
   "ratios. Hostile text is bounded: length, depth and term limits when parsing, a " +
   "step budget and symbol-cycle detection when evaluating, errors as messages with " +
   "a column, never exceptions, and no recursion outside the depth-limited parser -- " +
   "a sum 300,000 terms deep evaluates and frees without touching the stack.\n" +
   "      Tested in tests/core_data_test.cpp against the same formulas compiled as " +
   "C++ (3,000 evaluations), errors, limits and printing; fuzzed in " +
   "tests/fuzz_core_data.cpp with a print-and-reparse property, under ASan. RUN on " +
   "Windows (MSVC, and MSVC ASan); Linux and macOS only compiled",
   "class Expression"],
  ["core", "HighResolutionTimer / PerformanceCounter / ScopedTimeMeasurement",
   "dsp.h: LoadMeasurer reports what fraction of the block budget a DSP " +
   "actually used, pushed to the editor, which is the measurement a plugin " +
   "needs about itself. HighResolutionTimer runs a callback on its own thread " +
   "at sub-millisecond accuracy -- nothing here wants one: the audio thread is " +
   "already the accurate clock, and a second high-priority thread inside a " +
   "plugin competes with it",
   "class LoadMeasurer"],
  ["core", "Process / ChildProcess / DynamicLibrary / InterProcessLock / RuntimePermissions / " +
           "ConsoleApplication / ArgumentList / WindowsRegistry",
   "process.h: ChildProcess (launched without a shell, the three standard " +
   "streams and nothing else inherited, killed with its object), " +
   "InterProcessLock and DynamicLibrary, built for the applications and tools " +
   "made on this SDK (see the Inter-process row). RuntimePermissions " +
   "(Android's and iOS's), ConsoleApplication, ArgumentList and WindowsRegistry " +
   "remain out of scope: a plugin asks for no OS permission, and the " +
   "standalone parses its own arguments in standalone.h.\n" +
   "      DynamicLibrary was the one already here, and it is load-bearing: every " +
   "optional backend is reached by dlopen/LoadLibrary at RUNTIME, which is what " +
   "lets one build run on a machine without WebKitGTK, without X11 or without " +
   "an ASIO driver and say exactly which library was missing rather than " +
   "failing to load at all",
   "class ChildProcess"],
  ["core", "Networking: URL, WebInputStream, StreamingSocket, DatagramSocket, IPAddress, " +
           "MACAddress, NamedPipe",
   "net.h and ipc.h. IPAddress (IPv4 and IPv6, RFC 5952 text, this machine's " +
   "addresses and their broadcast addresses), MACAddress, StreamingSocket " +
   "(TCP: connect with a timeout across every address a name resolves to, " +
   "listen, accept) and DatagramSocket (UDP, broadcast, multicast); URL (RFC " +
   "3986 components, percent-encoding, parameters, child paths, and relative " +
   "resolution that passes all 42 of the RFC's examples); and WebInputStream: " +
   "HTTP and HTTPS with headers, a POST body, a timeout kept by a deadline of " +
   "its own, progress, cancellation from any thread, and redirects followed by " +
   "one set of rules on every platform -- with URL::downloadToFile, which " +
   "replaces its target only with a whole 2xx body. NamedPipe is in ipc.h: a " +
   "named pipe on Windows, and on macOS and Linux a Unix-domain socket rather " +
   "than the reference's pair of FIFOs, which can neither tell a reader that " +
   "the writer died nor be written without risking SIGPIPE in the host.\n" +
   "      The position this row stood for is kept as defaults rather than as a " +
   "missing header: nothing in the SDK opens a socket by itself, and a " +
   "listener binds the loopback unless told otherwise, so a helper channel a " +
   "plugin opens is not reachable from the network the laptop is on. Windows " +
   "is reached through WinHTTP, Winsock and iphlpapi LOADED at run time, their " +
   "few structures declared here and checked against the Windows SDK's at " +
   "compile time, so the header compiles after any <windows.h>; macOS through " +
   "NSURLSession by the Objective-C runtime; Linux through libcurl opened with " +
   "dlopen, so no link dependency. RUN on Windows (sdk/tests/system_test " +
   "against an HTTP server it starts: GET, POST, headers, a redirect chain, " +
   "the 303 and 307 rules, a 404, a timeout, cancellation before the response " +
   "and in the middle of a body, a chunked body, a megabyte with progress, " +
   "IPv6, and one real HTTPS request when online; TCP and UDP on the loopback; " +
   "the parsers fuzzed under ASan, 45,000 cases); the macOS and Linux backends " +
   "are COMPILED through tests/au_shim by MSVC and clang, and have not run",
   "class WebInputStream"],
  ["core", "LocalisedStrings",
   "localisation.h: LocalisedStrings reads the reference's translation file format " +
   "unchanged (language:, countries:, \"original\" = \"translation\", anything else a " +
   "comment) with escapes, CR/LF/CRLF and a BOM; malformed mappings are skipped and " +
   "their line numbers kept for the translator. Lookup falls back through a chain " +
   "of tables (fr-CA on fr) and then to the original; ignoreCase folds ASCII. " +
   "setCurrentMappings installs a process-wide table, translate() and " +
   "SONORE_TRANS(\"...\") read it, SONORE_NEEDS_TRANS marks text translated later.\n" +
   "      The descriptor still carries parameter names; this is for the rest of an " +
   "editor's text, and for switching language at runtime. The current table is an " +
   "immutable snapshot behind a shared_ptr, so reads are thread-safe while the " +
   "language changes: tested with four threads translating through 2,000 changes, " +
   "plus the file format, fallbacks and copies, in tests/core_data_test.cpp. RUN on " +
   "Windows (MSVC, and MSVC ASan); Linux and macOS only compiled",
   "class LocalisedStrings"],
  ["core", "LeakedObjectDetector / UnitTest / UnitTestRunner",
   "leak_detector.h: LeakedObjectDetector and SONORE_LEAK_DETECTOR(Class) -- an " +
   "atomic count of a class's live instances, reported by name when the module " +
   "unloads (stderr, the debugger's output on Windows, or a handler), and a double " +
   "delete reported when it happens. On in debug builds and nothing at all in " +
   "release; SONORE_CHECK_MEMORY_LEAKS chooses. ASan finds leaks better, but a " +
   "plugin's leaks show up in a HOST, which is not an ASan build -- this works in " +
   "an ordinary debug build inside any DAW.\n" +
   "      unit_test.h: UnitTest (name, category, initialise / runTest / shutdown, " +
   "beginTest, expect, expectEquals and the comparisons, expectWithinAbsoluteError, " +
   "expectThrows, logMessage, a reproducible getRandom) registering itself in a " +
   "global list, and UnitTestRunner running all of them or one category, logging " +
   "the seed and keeping each section's passes, failures and messages. The SDK's own " +
   "2600-odd checks stay standalone programs -- a framework inside the thing being " +
   "tested would be a dependency shared with the code under test -- so this is the " +
   "public API for plugin authors' tests.\n" +
   "      Tested in tests/core_data_test.cpp: live counts through copies and moves, a " +
   "child process that leaks and is reported at exit, and sample tests that pass, " +
   "fail and throw. RUN on Windows (MSVC, and MSVC ASan); Linux and macOS only " +
   "compiled",
   "class LeakedObjectDetector"],
  ["core", "TimeSliceThread",
   "thread_pool.h: ThreadPool, which runs queued work on a small fixed set of " +
   "threads, and audio_source.h's ReadAheadThread, the cooperative shape the " +
   "reference calls TimeSliceThread -- clients polled in turn, each saying how " +
   "long until it wants the next slice -- which read-ahead needs, because " +
   "read-ahead never completes",
   "class ReadAheadThread"],
  ["core", "AndroidDocument / AndroidDocumentInfo / AndroidDocumentIterator",
   "out of scope: Android's scoped storage API, which the reference wraps because it " +
   "targets Android. There is no Android target here -- the same reason " +
   "PushNotifications and ContentSharer are out",
   null, "class AndroidDocument"],
  ["core", "TextDiff",
   "text_diff.h: TextDiff -- the changes (at a byte offset, remove so many bytes, " +
   "insert this text) that turn one string into another, applied in order by " +
   "appliedTo. Myers' O(ND) algorithm in linear space, by UTF-8 character so no " +
   "change splits one (invalid UTF-8 compared byte by byte, so any string round-" +
   "trips). The reference uses it for ValueTree changes over a wire; here it is for " +
   "undo in a text field and showing what an edit changed.\n" +
   "      Both strings may come from outside, so the search runs on a work budget and " +
   "a size guard: past either, the unresolved part becomes one replacement -- still " +
   "exact, and isMinimal() says it is no longer the fewest characters. Tested in " +
   "tests/core_data_test.cpp: 3,000 random pairs checked MINIMAL against a " +
   "dynamic-programming LCS, invalid UTF-8, a 200,000-character text, the budget and " +
   "the guard; fuzzed in tests/fuzz_core_data.cpp, under ASan. RUN on Windows (MSVC, " +
   "and MSVC ASan); Linux and macOS only compiled",
   "class TextDiff"],
  ["graphics", "DropShadowEffect / GlowEffect",
   "gfx/effects2d.h: DropShadow, and a glow is the same operation with the " +
   "offset at zero and a bright colour -- which is exactly what the reference's " +
   "GlowEffect is, and saying so is more useful than a second class that " +
   "differs by two arguments.\n" +
   "      The ImageEffectFilter machinery around them is what does not carry " +
   "over: in the reference an effect is attached to a Component and applied when it " +
   "paints. Here a shadow is drawn before the thing that casts it, into the " +
   "same bitmap, so there is nothing to attach and nothing to re-run",
   "struct DropShadow"],
  ["core", "OnlineUnlockForm / TracktionMarketplaceStatus",
   "out of scope: the UI for typing an unlock key, and a client for " +
   "Tracktion's own licence service. Sonorie's entitlement is checked by the " +
   "service that issued it, so there is no key for a user to type into a " +
   "plugin and no in-plugin form to type it into -- see the BigInteger row for " +
   "why the verification is not done in-process either",
   null, "class OnlineUnlockForm|TracktionMarketplace"],
  ["graphics", "DrawableComposite / DrawablePath / DrawableRectangle / DrawableImage / " +
               "DrawableShape",
   "gfx/svg.h: Drawable holds a flat list of SvgShape -- a path, a fill, a " +
   "stroke -- and the clips, masks and layers between them, because the parse " +
   "FLATTENS groups, their styles and their transforms into absolute paths " +
   "with resolved paints at load. The reference keeps the tree so a drawable can be " +
   "manipulated afterwards: a composite reparented, a path's points animated, " +
   "a rectangle's corner radius bound to something.\n" +
   "      Flattening is the right trade for the use this has. An editor " +
   "redraws thirty times a second and a logo must not be re-walked on any of " +
   "them; a plugin that wants a shape to move draws it itself with the same " +
   "Path API the parser produced. The cost is that an SVG cannot be edited " +
   "after loading, which nothing here does",
   "struct SvgShape"],
  ["graphics", "DrawableText",
   "gfx/svg.h and gfx/font_match.h. <text>, and the <tspan>s and <a>s in it, " +
   "are laid out as SVG lays out horizontal text -- x, y, dx and dy singly or " +
   "per character, text-anchor per chunk, font-family, -size, -weight and " +
   "-style, letter- and word-spacing, xml:space -- and turned to glyph " +
   "outlines at parse time, so fill and stroke with every paint, opacity, " +
   "transforms, clips, masks and a clipPath made of text treat a letter as a " +
   "path. Each run is kept as a DrawableText -- its words, its Font, colour, " +
   "justification and a Parallelogram box, with toPath and draw -- which is " +
   "also usable on its own, as the reference's is.\n" +
   "      Which face: FontMatcher, into a FontMatch. A CSS family list in " +
   "order; faces the plugin REGISTERED before the machine's, which are indexed " +
   "once from the name, OS/2 and fvar tables of every file in the system font " +
   "folders (full and PostScript names too, so Illustrator's Arial-BoldMT " +
   "finds that face); generic families per platform; the style, then CSS's " +
   "weight rule; a variable face's wght axis set to the weight when it reaches " +
   "it. No bold or italic is synthesised, as resvg synthesises none. The old " +
   "objection is now a stated fact rather than a reason: a family list matched " +
   "against a machine's fonts gives that machine's answer. A plugin whose art " +
   "must draw the same everywhere registers the face it was designed with and " +
   "parses with FontMatcher(EmbeddedOnly): every family resolves to a " +
   "registered face and no character falls back to a system face -- or it " +
   "converts the text to outlines before export.\n" +
   "      images_test draws eight SVGs with four fonts built by " +
   "tests/data/svgtext/make_svgtext.py (static regular, bold and italic, and a " +
   "variable weight axis) against resvg 0.48 given the same fonts: ink bounds " +
   "within a pixel, total coverage within 5% of resvg's, mean alpha difference " +
   "under 1.1 levels; plus the matcher's CSS order, whitespace, anchors, " +
   "18pt as 24px, the character limit. 815 of 834 SVGs found on this machine " +
   "draw byte-identically to before the change and the 19 with text now draw " +
   "it, matching resvg with the same system fonts at a white-ink IoU of 0.98. " +
   "fuzz_images ran 17,691 mutations of the eight SVGs under ASan, none crashing " +
   "or hanging. Run on Windows; macOS and " +
   "Linux compiled only, font folders and directory walk written for both",
   "class DrawableText"],
  ["graphics", "StrokeOptions",
   "gfx/stroke.h: StrokeStyle, with LineJoin and LineCap beside it, and " +
   "PathStrokeType is claimed by the same file. The reference 8 split the options out " +
   "of PathStrokeType so a stroke can be described without a width; here the " +
   "width lives with the style because every place that strokes needs both",
   "struct StrokeStyle"],
  ["graphics", "Justification",
   "gfx/font.h: Justify, with Left, Centred and Right -- the three a plugin's " +
   "interface uses. the reference's Justification is a bitfield combining horizontal " +
   "and vertical placement with nine named combinations, which its Label and " +
   "TextLayout both take.\n" +
   "      Vertical placement is handled by the layout engine here rather than " +
   "by the text call: a Label is given a rectangle and centres in it, and " +
   "FlexBox's alignItems decides where that rectangle sits. Two mechanisms for " +
   "vertical centring is how they end up disagreeing",
   "enum class Justify"],
  ["graphics", "FontFeatureSetting / FontVariableSetting / FontFeatureTag",
   "gfx/font.h: Font::withFeature(tag, value) for OpenType features and " +
   "Font::withVariation(axis, value) for a variable font's axes. A feature " +
   "the caller gives goes into the shaper's plan as HarfBuzz merges a user's " +
   "-- one already there keeps its stage and takes the value, 0 turning it " +
   "off; a new one goes last; a value above 1 picks an alternate -- and a " +
   "font with features shapes all its text. TABULAR FIGURES are the case a " +
   "plugin has: withFeature(\"tnum\") keeps a readout from jittering as its " +
   "digits change while a knob moves. Fractions are automatic, as in " +
   "HarfBuzz: digits around U+2044 take numr, frac and dnom. Compared with " +
   "HarfBuzz on twelve system fonts under random sets of thirty-five " +
   "features: all of 24,000 strings identical",
   "Font withFeature("],
  ["graphics", "ColourLayer / GlyphLayer / ImageLayer",
   "gfx/truetype.h: Typeface::colourLayers reads COLR version 0 and CPAL -- " +
   "each layer an outline and its palette colour, or the text's own -- and " +
   "Typeface::colourImage the PNG pictures of sbix and CBDT, from the strike " +
   "HarfBuzz takes, placed where HarfBuzz places them. Font::draw draws them " +
   "in colour: Windows' Segoe UI Emoji, macOS's Apple Color Emoji, Linux's " +
   "Noto Color Emoji, and a plugin's own colour icon font. An emoji asked for " +
   "as one -- by default, or with VS16 -- comes from emojiTypeface(), the " +
   "system's colour face, as a browser chooses it; the picture comes from " +
   "the strike suited to the size on the screen, through the transform. " +
   "COLR version 1's gradients are not drawn: a font with only those is " +
   "passed over",
   "struct ColourLayer"],
  ["extras", "AudioFormat / AudioFormatManager",
   "audiofile.h: one loadAudioFile that sniffs the header and dispatches. The reference " +
   "has a registry because an application lets the USER add formats and then " +
   "asks which of them can open a file; the set here is fixed at build time -- " +
   "WAV, AIFF, FLAC, MP3, Ogg Vorbis -- so the dispatch is a switch on the " +
   "first four bytes and there is no registry to populate or to get wrong.\n" +
   "      A fixed set is also the honest one for a plugin: what it can open is " +
   "a property of the plugin, not of the machine it is running on, so a sample " +
   "that loads on one user's system loads on every user's. An application that " +
   "wants the machine's codecs as well opts in: enableSystemAudioFormats() " +
   "(system_audio_format.h, see the CoreAudioFormat row) puts them behind the " +
   "fixed set, through setAudioFileFallback",
   "readAudioFile"],
  ["extras", "CoreAudioFormat / WindowsMediaAudioFormat",
   "system_audio_format.h: WindowsMediaAudioFormat decodes through Media " +
   "Foundation -- a source reader over a FILE byte stream (MFCreateFile, so an " +
   "http:// path is never fetched) asked for float PCM: AAC in M4A/MP4/ADTS, " +
   "MP3, WMA, ALAC, AC-3, whatever decoders the machine has. CoreAudioFormat " +
   "does the same on macOS through ExtAudioFile. Both fill the WavData every " +
   "reader here fills, under the same kMaxDecodedSamples ceiling.\n" +
   "      Opt-in, because the old argument still holds for the DEFAULT -- a " +
   "format that opens on one machine and not the next is a project that loads " +
   "with an empty sampler: readAudioFile keeps its fixed set until " +
   "enableSystemAudioFormats() puts the system decoders behind it, and even " +
   "then the built-in WAV/AIFF/FLAC/MP3/Ogg decoders go first and the system " +
   "only sees what they refuse (an M4A; ADTS AAC, which shares MP3's sync; MP3 " +
   "inside a RIFF). mfplat is loaded at runtime from System32, so a Windows N " +
   "edition without Media Foundation gets isAvailable() == false rather than a " +
   "plugin that will not load; COM is initialised only on a thread that has " +
   "none, and a caller's apartment is never changed.\n" +
   "      RUN on Windows (audio_io_test): Media Foundation's own encoder makes " +
   "AAC in M4A and ADTS, MP3 and WMA of a 1 kHz sine, and each decodes to the " +
   "right rate, length, tone and both channel levels within 0.5 dB; float WAV " +
   "comes back bit-exact; the one MP3 decoded by Media Foundation and by " +
   "minimp3 agrees to 87 dB; the dispatcher's choices are counted; a caller's " +
   "single-threaded apartment survives a decode; 1200 truncated and " +
   "byte-flipped files under ASan decode or refuse without a crash. " +
   "CoreAudioFormat is COMPILED only (tests/mac_syntax_audio_io.cpp against " +
   "au_shim, now with ExtAudioFile); its round-trip test runs when macOS CI " +
   "does. Linux has no system codec API, and says so",
   "class WindowsMediaAudioFormat"],
  ["extras", "LAMEEncoderAudioFormat",
   "lame_encoder.h: LAMEEncoderAudioFormat, the way the reference does it -- the " +
   "audio to a temporary 16-bit WAV, the user's own lame executable run on it " +
   "(VBR -V, CBR -b, ABR, -q, ID3 tags), and the MP3 it writes read back as " +
   "bytes or written to a UTF-8 path. A separate program keeps LAME's LGPL the " +
   "user's rather than every plugin's, and the path is always the caller's: " +
   "nothing searches PATH.\n" +
   "      Launched without a shell -- CreateProcessW naming the program " +
   "exactly, posix_spawn elsewhere -- with no inherited handles, no console " +
   "window and stdio going nowhere; Windows arguments quoted as the exact " +
   "inverse of the C runtime's splitting rules; a timeout after which the " +
   "child is killed and reaped; temporary files created exclusively under " +
   "random names and removed on every path out.\n" +
   "      No LAME is installed here, so the launching path was RUN on Windows " +
   "with a fake encoder: audio_io_test launches ITSELF as lame (an environment " +
   "variable picks copy, fail, hang, empty or echo). Sixteen arguments -- " +
   "spaces, quotes, backslash runs, empty, UTF-8, shell metacharacters -- " +
   "arrive in the child byte for byte; the copied MP3 is the 16-bit WAV of the " +
   "input; exit codes are reported, a hang is killed at 0.5 s, and no " +
   "temporary file is left behind on any path. The posix_spawn branch is " +
   "COMPILED only (macOS against au_shim, Linux against stubs). It blocks for " +
   "as long as the encoder runs: never from the audio thread",
   "class LAMEEncoderAudioFormat"],
  ["extras", "AudioSubsectionReader / BufferingAudioReader / MemoryMappedAudioFormatReader",
   "audiofile.h and the sampler: a region of a file, read-ahead, and mapping. " +
   "BufferingAudioReader and MemoryMappedAudioFormatReader are already claimed " +
   "by rows above; the subsection is what a sampler's start and end points " +
   "are, which live on the zone rather than in a wrapper object because a zone " +
   "already has to carry its own root note, its key range and its loop points",
   "class SampleStreamer"],
  ["extras", "SamplerSound / SamplerVoice",
   "the sampler example and dsp.h's VoiceManager. the reference's pair is a usable " +
   "sampler in two classes -- one sound, one voice -- which is the right size " +
   "for a demo and too small for a real instrument: no round-robin, no " +
   "velocity layers, one loop mode.\n" +
   "      The example here is a real one and shows what a generated plugin " +
   "does: zones with key and velocity ranges, disk streaming, and the " +
   "interpolation from interpolation.h. That is a plugin rather than a " +
   "framework class, and it is in examples/ where somebody can read all of it",
   "class VoiceManager"],
  ["extras", "ARAAudioSourceReader / ARAPlaybackRegionReader",
   "ara_wrapper.h: the ARA plug-in extension, which is the half a PLUGIN " +
   "implements -- it is told about audio sources and playback regions by the " +
   "host and may read them. the reference's two readers wrap that access as its own " +
   "AudioFormatReader so its pull-model classes can consume it; the readers " +
   "here hand out the host's own accessors, because there is no pull-model " +
   "hierarchy for them to fit into",
   "ARA"],
  ["extras", "AudioSourcePlayer",
   "audio_source.h: AudioSourcePlayer hands a pull-model AudioSource to the " +
   "device backends -- their RenderFn, renderWithInput for ASIO, and the " +
   "reference's planar callback. The plugin standalone does not need it: its " +
   "callback IS the DSP call",
   "class AudioSourcePlayer"],
  ["dsp", "FastMathApproximations",
   "fast_math.h: fastTanh, fastExp, fastSin, fastCos and fastLog, each with " +
   "its measured worst-case error stated in the header rather than described " +
   "as \"fast\". That matters more here than the speed does -- a saturator " +
   "built on a tanh approximation whose error nobody measured is a saturator " +
   "with a distortion nobody chose",
   "fastTanh"],
  ["dsp", "LookupTableTransform",
   "dsp.h: LookupTable, which takes the function it tabulates. The reference separates " +
   "the transform because its LookupTable can also be filled point by point; " +
   "every table here is built from a function, so the two collapse into one " +
   "and there is no half-filled state to reason about",
   "class LookupTable"],
  ["dsp", "ProcessSpec / ProcessContextReplacing / ProcessContextNonReplacing",
   "audio.h: ProcessSpec is here under the reference's own name -- sample rate, block " +
   "size, channel count -- and prepare() takes it. The CONTEXTS are different " +
   "on purpose: the reference passes a context object carrying the input and output " +
   "blocks so a processor can be told to work in place or out of place.\n" +
   "      Everything here processes in place, because that is what every " +
   "plugin format hands over: one buffer, filled. A non-replacing form would " +
   "be a second code path through every processor to support a call nobody " +
   "makes",
   "struct ProcessSpec"],
  ["dsp", "ProcessorState / ProcessorWrapper",
   "shaping.h: ProcessorChain holds its processors directly, and prepare and " +
   "reset walk them. The reference needs a wrapper because its chain is variadic over " +
   "types that may not share a base, and ProcessorState is the later addition " +
   "that binds a processor's parameters to an APVTS -- which is a binding this " +
   "SDK does through ParamInfo and the attachments instead",
   "class ProcessorChain"],
  ["dsp", "Phase / Polynomial / SpecialFunctions / Matrix",
   "phase.h: Phase -- a phase accumulator in turns held as a 64-bit fixed " +
   "point number, so the add is exact and the wrap is integer overflow: 440 " +
   "Hz at 48 kHz stays within 5e-12 of a cycle of the exact rational phase " +
   "over 10^8 samples, where the same accumulator in float drifts half a " +
   "cycle. advance() returns the phase before the step, as the reference's " +
   "does; turns() in [0, 1) and radians() in [0, 2pi) cannot round up onto " +
   "the far end; wrapped() marks the sample a sync acts on. The oscillators " +
   "keep their float phase -- a note is not long enough to drift -- and this " +
   "is for what must stay in step for minutes.\n" +
   "      special.h: SpecialFunctions -- besselI0 (whose series was cut off " +
   "at 64 terms and read 0.4% low at x = 100; now good to overflow), " +
   "ellipticIntegralK by the AGM, and the Jacobi cde, sne and asne in " +
   "quarter-period units by the descending Landen transformation: the " +
   "reference's set for elliptic filter design, as free functions in " +
   "sonore::special with the struct forwarding to them. Checked against the " +
   "published I0 and K(m) tables, their integral representations by " +
   "quadrature, sn as the inverse of the incomplete integral, the imaginary " +
   "transformation, both periods, and asne round trips on the real line, " +
   "the stopband and off-axis. Polynomial and Matrix have their own rows " +
   "above. RUN on Windows (MSVC /W4, ASan); standard C++ only",
   "class Phase {"],
  ["osc", "OSCMessage / OSCArgument / OSCAddress",
   "osc.h: Message, Argument and the address string on it. An Argument carries " +
   "the four types every OSC implementation agrees on -- int32, float32, " +
   "string, blob -- because those are the ones a receiver can be relied on to " +
   "understand, and an OSC message nobody can parse is worse than one nobody " +
   "sent.\n" +
   "      The address is a plain string rather than a parsed object: it is " +
   "compared, and it is compared in one place",
   "struct Argument"],
  ["osc", "OSCBundle / OSCTimeTag",
   "osc.h: decodePacket unpacks a BUNDLE into its messages, so a plugin " +
   "receiving one sees the messages inside it. The time tag is read and not " +
   "acted on, which is the honest half -- honouring it means holding messages " +
   "until an NTP timestamp arrives, and a plugin's OSC arrives on a UI tick " +
   "whose resolution is 33 ms.\n" +
   "      Sending bundles is absent for the same reason: a bundle exists to " +
   "make several messages take effect at the same instant, and this cannot " +
   "promise that instant",
   "decodePacket"],
  ["osc", "OSCAddressPattern",
   "osc.h: OSCAddressPattern and matchesPattern, OSC 1.0's rules -- ? and *, " +
   "[abc] with ranges and a leading ! to negate, {foo,bar} -- part by part " +
   "and never across a slash. Matched by carrying the set of reachable " +
   "positions rather than by backtracking, so a pattern from the network " +
   "cannot make it spin: *a*a*...*b against 400 characters takes " +
   "microseconds. A malformed pattern matches nothing.\n" +
   "      This row used to say exact matching only, because a plugin receives OSC " +
   "somebody aimed at it. Both now: Dispatcher hands a message to every " +
   "listener its pattern reaches (Receiver::pollAndDispatch), and a message " +
   "with no wildcard in it -- nearly all of them -- is still one map lookup. " +
   "Listeners register at plain addresses (isValidAddress), because in OSC " +
   "it is the sender that writes the pattern. Tested against a 44-case table " +
   "drawn from the OSC 1.0 rules, over a UDP loopback, and fuzzed against a " +
   "separately written naive matcher. Run on Windows; parsed as macOS and " +
   "Linux. OSC 1.1's double-slash wildcard is not here",
   "class OSCAddressPattern"],
  ["osc", "OSCColour",
   "osc.h: OSCColour, the 'r' argument -- red in the high byte and alpha in " +
   "the low, big-endian like every OSC word -- with fromInt32RGBA and " +
   "toInt32, Argument::makeColour, and isColour and getColour to read one " +
   "back as a colour rather than an int taken apart by hand. That is where " +
   "it goes wrong: ARGB habits put alpha on top, and a lighting desk sent a " +
   "colour read that way shows a different one. Tested bit-exactly on the " +
   "wire, between a string and a float in one message, and an int is not " +
   "read as a colour. Run on Windows; parsed as macOS and Linux",
   "struct OSCColour"],
  ["gui", "AnimatorUpdater / VBlankAnimatorUpdater / ValueAnimatorBuilder / " +
          "StaticAnimationLimits",
   "gfx/animator.h: ValueAnimator::tick takes the time and returns whether " +
   "anything moved. The reference separates the UPDATER -- the thing that decides when " +
   "to advance every animation -- because a framework animation may be driven by a " +
   "timer or by the display's vblank, and the animators must not care which.\n" +
   "      There is one driver here and it is the window's own 33 ms tick, " +
   "which the editor already has for following automation. A second driver " +
   "would be a second clock, and the whole design of every timed thing in this " +
   "SDK is that there is one and it is passed in as an argument. A display's " +
   "own refresh is there for whoever wants to drive it from that instead: " +
   "gfx/vblank.h's VBlankAttachment calls back once a refresh (see its row), " +
   "and tick() takes whatever time it is handed.\n" +
   "      The builders are the reference 8's fluent construction API over the same " +
   "animations; animate() takes its arguments directly",
   "class ValueAnimator"],
  ["gui", "SpringEasingOptions",
   "gfx/animator.h: SpringEasingOptions, a spring as an easing curve -- the " +
   "exact step response of a damped harmonic oscillator starting at rest -- " +
   "with the reference's withFrequency, withAttenuation and " +
   "withExtraAttenuationRange (defaults 3, 3, 0.25; the last stretch blended " +
   "onto the target so a duration-based animation lands when its time is " +
   "up), and the physical forms fromPhysical(mass, stiffness, damping, " +
   "seconds) and fromResponse(response, bounce, seconds), with settlingTime " +
   "for the duration that falls out of the physics; under-, critically and " +
   "over-damped all exact. ValueAnimator::animateWithSpring and an " +
   "Animator::animateBounds overload take one.\n" +
   "      Tested against the formulae: the default curve peaks at t = 1/6 at 1 + " +
   "e^-0.5; m 1, k 100, c 4 is zeta 0.2 and the sampled curve's peak matches " +
   "exp(-pi zeta / sqrt(1 - zeta^2)) = 0.5266 to 1e-6 at the predicted time; " +
   "settlingTime is ln(1000)/2; critical damping is monotonic; a " +
   "ValueAnimator overshoots within the formula and lands exactly on its " +
   "target. Platform-independent; RUN on Windows",
   "class SpringEasingOptions"],
  ["core", "SHA256",
   "hash.h: Sha256, with sha256Hex and sha256File beside it. Named here as " +
   "the reference names it so the walk can find it -- the row that existed described " +
   "the hashing and never used the word",
   "class Sha256"],
  ["core", "MD5 / Whirlpool",
   "hash.h: MD5 and Whirlpool beside Sha256, the same shape as it -- streaming " +
   "update, finish to bytes or hex() to text, and finishing works on a copy so " +
   "the state can keep growing -- with md5Hex / md5File and whirlpoolHex / " +
   "whirlpoolFile. The file forms stream 64 KB at a time through pathio, so a " +
   "4 GB sample costs what a 4 KB preset does and a UTF-8 path opens on Windows; " +
   "a read error mid-file gives an empty string, not the digest of the part read " +
   "(sha256File now goes the same way). Whirlpool is the final ISO/IEC 10118-3 " +
   "version, its S-box and round tables BUILT at first use from the " +
   "specification's mini-boxes and circulant matrix rather than pasted.\n" +
   "      SHA-256 is still the one this SDK picks. MD5 is broken as a security " +
   "hash and is here for the manifests and catalogues that name it; Whirlpool is " +
   "here so a digest the reference made can be checked.\n" +
   "      Tested (tests/core_crypto_test.cpp): the whole RFC 1321 suite and a " +
   "million 'a's; the ISO/NESSIE vectors including a million 'a's; digests at " +
   "every padding boundary from 31 to 129 bytes against Python's hashlib (MD5) " +
   "and OpenSSL (Whirlpool); a 100,003-byte message whole, in random pieces, and " +
   "as a file under an accented and CJK name. RUN on Windows x64 (MSVC, /W4, " +
   "ASan); no platform code beyond pathio",
   "class Whirlpool"],
  ["core", "BlowFish / RSAKey / Primes",
   "crypto.h: BlowFish, RSAKey and Primes as the reference names them, built on " +
   "bigint.h, plus secureRandomBytes from the operating system's CSPRNG -- " +
   "BCryptGenRandom looked up in bcrypt.dll at first use (no link line to " +
   "change), arc4random_buf on macOS and the BSDs, getrandom on Linux with " +
   "/dev/urandom under kernels older than 3.17. Key generation FAILS when the " +
   "OS refuses rather than falling back to anything guessable.\n" +
   "      BlowFish: keys of 1 to 72 bytes, the two-word encrypt/decrypt, 8-byte " +
   "blocks, and the reference's buffer form (ECB, PKCS#7) -- in big-endian words, " +
   "where the reference reads native ones, so a buffer interoperates with every " +
   "other Blowfish including `openssl enc -bf-ecb`. A refused decryption leaves " +
   "the buffer as it was. RSAKey: createKeyPair(public, private, bits, seeds), " +
   "applyToValue with the reference's digit-by-digit handling of values wider " +
   "than the modulus (digits reversed, a zero low digit lost -- inherited and " +
   "documented), toString / fromString in its \"exponent,modulus\" hex. The " +
   "public exponent is 65537 where the reference starts at 3. Seeds make a pair " +
   "a deterministic function of them, as the reference's do. Primes: " +
   "isProbablyPrime -- trial division below 2000, then Miller-Rabin with the " +
   "twelve bases that are a proof below 2^64 and fresh OS-chosen bases above -- " +
   "and createProbablePrime by a sieved incremental search.\n" +
   "      Compatibility, not modern cryptography: textbook RSA with no padding, " +
   "unauthenticated ECB, nothing constant-time. The header says so first; secrecy " +
   "that matters belongs to the platform's audited implementation.\n" +
   "      Tested (tests/core_crypto_test.cpp): Schneier's 33 ECB vectors and the " +
   "24 variable-key ones; an OpenSSL-encrypted buffer; all 1,042 pi words " +
   "recomputed from Machin's formula; RSA pairs at 512 and 1024 bits round-trip " +
   "both ways, and a signature made with one verifies through license.h's " +
   "verifier; a sieve below 50,000, 2,000 numbers across 2^32, Mersenne primes " +
   "to 2^607-1, 28 Carmichael numbers and strong pseudoprimes built to pass " +
   "fixed bases all rejected. tests/fuzz_core_crypto.cpp: random keys and " +
   "garbage through BlowFish, mutated key strings through RSAKey, 25,000 cases " +
   "with the BigInteger targets, ASan clean. RUN on Windows x64 (MSVC, /W4, " +
   "ASan); the macOS and Linux branches of secureRandomBytes are COMPILED only " +
   "-- macOS through tests/au_shim, Linux against stub headers -- never run",
   "class BlowFish"],
  ["client", "StandaloneFilterWindow / StandalonePluginHolder",
   "standalone.h: the whole standalone application, from one #define. the reference's " +
   "two classes are the same idea -- wrap the plugin in a window and an audio " +
   "device so it runs without a host -- and the shapes differ for one reason " +
   "worth stating: the reference's holder exists to hold an AudioProcessor it did not " +
   "write, so it must work through the same public interface a host would.\n" +
   "      This is the same source file as the plugin, so the standalone calls " +
   "the DSP directly and adds what a host would otherwise provide: device " +
   "selection, a file source for playing material through an effect, MIDI " +
   "input, and the offline modes that make it testable in CI on all three " +
   "platforms without a device or a display",
   "SONORE_BUILD_STANDALONE"],
  // ── opengl ──────────────────────────────────────────────────────────
  //
  // This was ONE row, out of scope, with every class named so the decision
  // could be reopened against the real list. It was reopened: the software
  // rasteriser stays the default for knobs and meters, and GL is now an opt-in
  // a component asks for when it is drawing something a CPU cannot afford --
  // a 3D waterfall, a 60 fps scope, a shader visualiser.
  ["opengl", "OpenGLContext / OpenGLRenderer / OpenGLPixelFormat / OpenGLVersion",
   "gfx/opengl.h + gfx/opengl_api.h: OpenGLContext attached to a component inside a native " +
   "editor window (attachTo(component, NativeWindow::handle())). It makes a child surface " +
   "over the component -- WGL on Windows, NSOpenGLContext on macOS, GLX on X11 -- that " +
   "follows its position, size, visibility and ancestor clip, and is transparent to the " +
   "mouse so the component tree still gets every event. The three OpenGLRenderer callbacks " +
   "arrive on one thread, in order: a render thread of its own by default (paced by " +
   "setSwapInterval, setContinuousRepainting or triggerRepaint), or the message thread. " +
   "OpenGLPixelFormat (colour/depth/stencil bits, multisamplingLevel), OpenGLVersion " +
   "(compatibility, or 3.2 / 4.1 / 4.3 core), a shared context, executeOnGLThread, and the " +
   "component's own software painting composited over the GL picture.\n" +
   "      No GLEW, no glad, nothing linked: opengl32 / OpenGL.framework / libGL are opened at " +
   "runtime and each context owns an OpenGLFunctions table, so a host's own loader cannot " +
   "collide with it. Teardown is the part plugins get wrong: closing runs on the GL thread " +
   "while the surface still exists, including when the HOST destroys the editor window under " +
   "a running render thread (the surface hears its own WM_DESTROY), and nothing touches GL " +
   "after. A context that cannot be created -- no driver, or the GDI 1.1 renderer over " +
   "remote desktop -- makes attachTo return false with the reason, and the component keeps " +
   "its software painting.\n" +
   "      RUN on Windows (sdk/tests/opengl_test.cpp, AMD Radeon, GL 4.6 compatibility and 3.2 " +
   "core, 4x MSAA): pixels read back from shaders, textures and the composited component, " +
   "and 200 attach/detach cycles with GDI, USER and kernel handles flat. macOS and X11 are " +
   "COMPILED ONLY -- against tests/au_shim (tests/mac_syntax_opengl.cpp) and real Xlib " +
   "headers, by MSVC and clang -- and have not run on any machine. X11 has no surface clock: " +
   "the editor's own tick must call OpenGLContext::tick()",
   "class OpenGLContext"],
  ["opengl", "OpenGLShaderProgram / OpenGLTexture / OpenGLFrameBuffer / OpenGLHelpers",
   "gfx/opengl.h. OpenGLShaderProgram compiles and links with the driver's own log on " +
   "failure, and sets uniforms by name or through a cached Uniform (setMatrix4 takes a " +
   "Matrix3D, column-major as GL wants). OpenGLTexture uploads a Bitmap's premultiplied " +
   "RGBA as it is, so it composites exactly as the software painter does. OpenGLFrameBuffer " +
   "is render-to-texture with optional depth and stencil, and readPixels / writePixels move " +
   "pixels to and from a Bitmap the right way UP -- the row flip happens once, there. " +
   "OpenGLHelpers: clear, a top-left enableScissorTest, isExtensionSupported (glGetStringi " +
   "on a core profile, which has no GL_EXTENSIONS string), resetErrorState, and the GLSL " +
   "1.10 -> 1.50 translation a core profile needs.\n" +
   "      Every helper that draws puts back the GL state it touched, so a renderer's second " +
   "frame sees what its first one left. Tested on Windows with real pixels in both a " +
   "compatibility and a core context",
   "class OpenGLShaderProgram"],
  ["opengl", "OpenGLImageType / OpenGLGraphicsContextCustomShader / OpenGLAppComponent",
   "gfx/opengl.h. OpenGLImageType makes GPU images (an OpenGLFrameBuffer) from a Bitmap and " +
   "back, and paint() lets the SDK's ordinary Graphics draw on one -- in software, with a " +
   "readback and an upload, because there is one rasteriser here and it is the software " +
   "one; the reference's GL-backed Graphics has no equivalent to be. " +
   "OpenGLGraphicsContextCustomShader fills a rectangle with a fragment shader written " +
   "against pixelPos, screenBounds and pixelAlpha, translated for a core profile " +
   "automatically. OpenGLAppComponent is a Component that owns its context and turns the " +
   "callbacks into initialise / render / shutdown.\n" +
   "      Tested on Windows: a custom shader's ramp read back pixel by pixel, a GPU image " +
   "painted with Graphics, an OpenGLAppComponent initialised and shut down exactly once",
   "class OpenGLGraphicsContextCustomShader"],
  ["opengl", "Matrix3D / Quaternion / Vector3D / Draggable3DOrientation",
   "gfx/maths3d.h, no GL needed. Vector3D (dot, cross, and the reference's * and ^), " +
   "Matrix3D column-major with frustum, perspective, orthographic, lookAt, axis-angle and " +
   "Euler rotations, transform, transpose, determinant and inverse; Quaternion with the " +
   "Hamilton product, rotate, rotationMatrix, inverse and shortest-path slerp; and " +
   "Draggable3DOrientation, an arcball that keeps the point under the pointer under the " +
   "pointer and renormalises as it accumulates.\n" +
   "      Tested against known results on every platform: rotations composed, Euler order, " +
   "inverses multiplied back to the identity, projections taking known points to known " +
   "corners, slerp halfway and the short way round",
   "class Draggable3DOrientation"],
  // ── The rows below close a walk of the reference's own class index ────────────────
  //
  // scripts/reference-index.mjs holds the class NAMES from six gui_basics
  // groups and verify-reference.mjs fails if any of them is not decided about here.
  // Every one of these rows exists because that walk found a name this map had
  // never mentioned -- twenty-odd of them, while the map was reporting one real
  // gap. It could not have reported these: a class nobody has written down
  // cannot be counted as missing.
  //
  // Several are "out of scope" and that is a complete answer. What is not an
  // answer is silence.
  ["gui", "ResizableBorderComponent / ResizableEdgeComponent",
   "gfx/resizer.h: ResizableBorder, eight zones, each edge separately " +
   "enableable.\n" +
   "      The corner grip and the constrainer it uses existed in tooltip.h " +
   "before this and were wired into NOTHING -- so no plugin built with this " +
   "SDK had ever shown a user anything to drag. The classes existed and the " +
   "capability did not, which is the failure mode this map is least able to " +
   "see and the reason the row says where it is USED and not only where it " +
   "lives.\n" +
   "      A plugin editor turns the left and top edges off, and the reason is " +
   "not taste. An editor does not own its window: it asks, through " +
   "clap_host_gui::request_resize or IPlugFrame::resizeView. Dragging the " +
   "right edge asks for a wider window whose top-left stays put, which a host " +
   "can honour; dragging the LEFT edge asks for one whose top-left moves, and " +
   "a plugin cannot move its own frame. Wired up naively the left edge grows " +
   "the window rightwards, which is the opposite of the hand.\n" +
   "      The origin shift is computed from the size that was ALLOWED, not " +
   "from the pointer -- otherwise a left drag at the minimum width slides the " +
   "window sideways while refusing to grow. Tested by pinning one axis and " +
   "leaving the other free, so one drag proves both answers at once",
   "class ResizableBorder"],
  ["gui", "Component::hitTest (a component's own shape)",
   "gfx/component.h: the virtual hitTestPoint, consulted at the leaf of the " +
   "hit test. Added because ResizableBorder has to sit OVER an editor and take " +
   "the mouse only at its edges -- an editor whose sliders stopped working " +
   "near the border would be a worse trade than no resizing at all. Children " +
   "are unaffected on purpose: it answers for this component's own surface, " +
   "so an override cannot silently disable a subtree",
   "virtual bool hitTestPoint"],
  ["gui", "FlexItem / GridItem",
   "gfx/layout.h, named as the reference names them. Listed separately from FlexBox and " +
   "Grid because the reference audit looks for the names it knows, and a row that " +
   "mentions only the container leaves the item undecided",
   "struct FlexItem"],
  ["gui", "TabbedButtonBar / TabBarButton",
   "gfx/containers.h, inside TabbedComponent rather than beside it. The reference " +
   "separates the strip from the panel so a strip can be used alone, which is " +
   "an application need rather than a plugin one; here the strip is the tabbed " +
   "component's own drawing and hit testing. Recorded so the two names are " +
   "decided rather than merely unmentioned",
   "class TabbedComponent"],
  ["gui", "StretchableObjectResizer",
   "gfx/splitter.h, as the arithmetic inside StretchableLayoutManager. The reference " +
   "exposes it separately for laying out things that are not components; " +
   "nothing here needs that, and a second public class computing the same " +
   "distribution is a second one to keep correct",
   "class StretchableLayoutManager"],
  ["gui", "NativeScaleFactorNotifier",
   "the peers themselves: WM_DPICHANGED in window_win32.h, the host's own " +
   "answer through IPlugViewContentScaleSupport and CLAP's gui_set_scale. A " +
   "separate notifier object would be a second path to the same event, and the " +
   "peer is where the event actually arrives",
   "WM_DPICHANGED"],
  ["gui", "TextButton / ToggleButton / ArrowButton / ImageButton",
   "gfx/widgets.h: one Button with a style, plus ImageComponent in widgets2.h " +
   "for the picture cases. The reference splits these into a class each because its " +
   "Button base is abstract; ours is concrete and carries the mode, which is " +
   "one class to keep working rather than four that must stay consistent with " +
   "each other",
   "class Button"],
  ["gui", "PropertyComponent family: BooleanPropertyComponent, ChoicePropertyComponent, " +
   "MultiChoicePropertyComponent, SliderPropertyComponent, TextPropertyComponent, " +
   "ButtonPropertyComponent",
   "gfx/plugin_editor.h, as what a GenericEditor ROW is. A parameter with " +
   "value names is a ChoicePropertyComponent, a continuous one is a " +
   "SliderPropertyComponent with a TextPropertyComponent beside it -- that is " +
   "the ValueBox, which became an editor on double-click precisely because a " +
   "readout you cannot type into is the thing those the reference classes exist to " +
   "avoid.\n" +
   "      Not built as a general property system, and that is the scope " +
   "decision: this SDK edits plugin PARAMETERS, which are a closed set with a " +
   "declared type, a range and a host that must be told about every change. A " +
   "property panel over arbitrary values would have none of that and would be " +
   "a second, weaker way to edit the same things",
   "class ValueBox"],
  ["gui", "Toolbar / ToolbarItemComponent / ToolbarItemPalette / ToolbarButton",
   "gfx/toolbar.h: Toolbar, ToolbarItemComponent, ToolbarButton (an SVG icon, a " +
   "label or both, by ToolbarItemStyle: iconsOnly, iconsWithText, textOnly), " +
   "ToolbarItemFactory with the separator, spacer and flexible-spacer ids, and " +
   "ToolbarItemPalette. Horizontal or vertical; flexible spacers share the room " +
   "left over; items that do not fit are hidden, not squashed, behind a chevron " +
   "that opens them as a menu (a PopupMenuOverlay, so the keyboard works it " +
   "too). toString() and restoreFromString() write the reference's \"TB:\" form, " +
   "skipping an id the factory no longer makes rather than failing the load.\n" +
   "      Customising is drag and drop inside the toolkit: showCustomisationDialog " +
   "puts the palette in an in-editor dialog that leaves a hole over the toolbar, " +
   "and items are dragged from the palette onto it, along it and back off it. " +
   "While editing, a transparent handle over each item takes the press, so Save " +
   "is moved rather than pressed and a slider item is rearranged like a button. " +
   "The toolbar OWNS its items, unlike every other container here, because the " +
   "user creates and destroys them. Tested (layout arithmetic, overflow by mouse " +
   "and keyboard, the drag round trip, the string round trip) in " +
   "gui_widgets_test and fuzz_gui_widgets; run on Windows (MSVC /W4, and the " +
   "fuzz under AddressSanitizer); the macOS and Linux branches compiled, not run",
   "class ToolbarItemPalette"],
  ["gui", "MenuBarComponent / MenuBarModel / BurgerMenuComponent",
   "gfx/menu_bar.h: MenuBarModel (getMenuBarNames; getMenuForIndex, which builds " +
   "a PopupMenu when the menu opens so its ticks are current; menuItemSelected; " +
   "menuBarActivated; menuItemsChanged), MenuBarComponent, and " +
   "BurgerMenuComponent, which lists the same model as headings and items for an " +
   "editor too narrow for a bar. The menus open in gfx/popup_overlay.h's " +
   "PopupMenuOverlay -- a PopupMenu drawn inside the editor over an Overlay -- " +
   "which is what lets the bar hear Left and Right with a menu open and switch " +
   "menus as the mouse moves along it; press, drag and release over an item " +
   "chooses it, as on every desktop.\n" +
   "      The keyboard is the desktop's: F10 enters the bar, the arrows walk it, Down, " +
   "Return or a letter opens a menu, Left and Right move between open menus, and " +
   "Escape steps back to the bar and then out, handing the keyboard back to " +
   "whoever had it. Alt with a menu's '&' letter opens it from anywhere through " +
   "handleShortcutKey, offered from MouseRouter::onUnhandledKey. A bare tap of " +
   "Alt is not a key event on any platform the peers drive, so that route is not " +
   "here. A model destroyed first is forgotten rather than called. Tested in " +
   "gui_widgets_test (clicks, sliding, press-drag-release, the F10 walk, Alt+F, " +
   "type-ahead, the burger's keyboard) and fuzz_gui_widgets; run on Windows " +
   "(MSVC /W4, and the fuzz under AddressSanitizer); the macOS and Linux " +
   "branches compiled, not run",
   "class MenuBarComponent"],
  ["gui", "SidePanel",
   "gfx/side_panel.h: a drawer from the left or the right, over the editor " +
   "rather than pushing it aside, with a title bar, a close button and the " +
   "content (not owned). It covers its parent and dims it in proportion to how " +
   "far in it is. A press on the dimmed part, Escape, the close button, or the " +
   "title bar dragged a third of its width back towards the edge closes it; a " +
   "shorter drag springs back. Opening moves the keyboard into the content, and " +
   "closing hands it back to the component that had it, if that is still in the " +
   "tree.\n" +
   "      The slide is gfx/animator.h's Animator and, like every timed thing here, " +
   "moves only on tick(now) -- with setInputClock(now) for a slide a click " +
   "starts -- so \"halfway through, ease-out has it three quarters in\" is an " +
   "exact assertion. Tested in gui_widgets_test (positions and dimmed pixels " +
   "mid-slide, every way of closing, both sides) and fuzz_gui_widgets; run on " +
   "Windows (MSVC /W4, and the fuzz under AddressSanitizer); the macOS and Linux " +
   "branches compiled, not run",
   "class SidePanel"],
  ["gui", "MultiDocumentPanel / MultiDocumentPanelWindow",
   "gfx/multi_document.h: MultiDocumentPanel holds documents, owned or not, as " +
   "tabs -- one filling the area under a strip with a close cross on each tab -- " +
   "or as MultiDocumentPanelWindow frames floating inside the panel: moved by " +
   "the title bar and never out of reach, resized by a corner grip, maximised by " +
   "a double click, raised by a click. An inactive window takes its first click " +
   "to come forward and nothing else, so a click never presses a button in a " +
   "document the user has not yet seen. Ctrl+Tab walks the documents and takes " +
   "the keyboard along; Ctrl+F4 closes one.\n" +
   "      Closing asks tryToCloseDocument, which answers through a callback -- at once " +
   "or after an alert -- and close-all stops at the first no; the document used " +
   "before a closed one comes forward. A maximum count and fullscreen-when-one, " +
   "as the reference has them. Windows and owned documents are freed at the next " +
   "paint, never inside the click that closed them. Tested in gui_widgets_test " +
   "(tabs, activation order, deferred answers, dragging, resizing and raising " +
   "windows) and fuzz_gui_widgets, which adds and closes documents with answers " +
   "arriving late; run on Windows (MSVC /W4, and the fuzz under " +
   "AddressSanitizer); the macOS and Linux branches compiled, not run",
   "class MultiDocumentPanel "],
  ["gui", "DocumentWindow / ResizableWindow / TopLevelWindow",
   "gfx/document_window.h: TopLevelWindow, ResizableWindow and DocumentWindow -- " +
   "an application's own desktop windows, built the way a host builds a " +
   "plugin's: a frame of ours (an HWND; an NSWindow with a delegate; an X window " +
   "managed through ICCCM and EWMH) with the SDK's existing peer opened inside " +
   "it, so the TopLevelWindow is the root component the peer paints and every " +
   "fix to a peer reaches both. A plugin editor is still the host's CHILD window " +
   "and the plugin standalone keeps its frame in standalone.h.\n" +
   "      The native title bar by default -- the platform's close, minimise and " +
   "maximise boxes, with the limits answered in WM_GETMINMAXINFO, WM_SIZING " +
   "(the aspect ratio too) and WM_WINDOWPOSCHANGING, contentMin/MaxSize, " +
   "WM_NORMAL_HINTS -- or a DRAWN title bar: minimise, maximise and close " +
   "buttons, drag to move, double-click to zoom, and a resize border measured " +
   "in desktop coordinates (the window moves under a left-edge drag). Content " +
   "owned or not, setResizable with a corner grip, setResizeLimits, " +
   "setFixedAspectRatio, setFullScreen (zoomed, as the reference means it), " +
   "setKioskMode (the whole monitor), setMinimised, setAlwaysOnTop, " +
   "setWindowAlpha, setIcon, the getTopLevelWindow list and " +
   "findTopLevelWindowFor, and closeButtonPressed -- reached by the close box, " +
   "Alt+F4 and the drawn button, which reports through the queue so a window " +
   "may delete itself. setMenuBar(MenuBarModel*) is the hook for " +
   "gfx/menu_bar.h: createMenuBarComponent(model) makes the bar, the content is " +
   "laid out under it.\n" +
   "      RUN on Windows (tests/app_windows_test, hidden windows, every action a " +
   "real message): sizes and positions, every limit by message, the corner and " +
   "edge drags, the drawn buttons and the bar's drag, zoom, minimise, kiosk, " +
   "topmost and alpha, keys reaching the tree through the frame, the menu-bar " +
   "hook with a stand-in component, windows deleting themselves from their own " +
   "close. The NSWindow and X11 frames are compiled only",
   "class DocumentWindow : public ResizableWindow"],
  ["gui", "NativeMessageBox / MessageBoxOptions / ScopedMessageBox",
   "gfx/message_box.h: NativeMessageBox (show, showAsync, showScopedAsync and the " +
   "reference's showMessageBox / showOkCancelBox / showYesNoBox / " +
   "showYesNoCancelBox forms), MessageBoxOptions, MessageBoxIconType and " +
   "ScopedMessageBox -- for APPLICATIONS. A plugin still asks with " +
   "gfx/alert.h's AlertOverlay: an OS modal from inside a plugin blocks the " +
   "host's message loop.\n" +
   "      Windows: TaskDialogIndirect with the buttons' own labels, through an " +
   "activation context made from shell32's Common Controls 6 manifest when the " +
   "program has none, else MessageBoxW; macOS NSAlert; Linux zenity, else " +
   "kdialog; and a DRAWN box (a DocumentWindow holding an AlertOverlay) wherever " +
   "none of those can show it. The async forms run the box on a thread of its " +
   "own (Windows, Linux) and answer on the message thread; destroying a " +
   "ScopedMessageBox takes the box down and its callback is never called.\n" +
   "      RUN on Windows: TaskDialog, MessageBoxW and the drawn box, each async " +
   "with a posted click and closed from code; the sync form answered from " +
   "another thread; the owner window. A box a person answers runs only with " +
   "SONORE_INTERACTIVE_TESTS=1. NSAlert and zenity/kdialog are compiled only",
   "class NativeMessageBox"],
  ["gui", "ThreadWithProgressWindow",
   "gfx/progress_window.h: ThreadWithProgressWindow -- run() on a std::thread while " +
   "a DocumentWindow shows a status line, a ProgressBar and Cancel. " +
   "setProgress and setStatusMessage only store; the window copies them out on " +
   "its own clock. Cancel raises the job's ThreadExitSignal, which a " +
   "MessageManagerLock taken by the job also gives up on. runThread waits " +
   "pumping the loop, launchThread returns at once, and " +
   "threadComplete(userPressedCancel) comes from the queue so it may delete the " +
   "object; a job that ignores Cancel past the time-out is given up on, not " +
   "killed (a std::thread cannot safely be). For applications: a plugin still " +
   "composes ThreadPool, Overlay and ProgressBar inside its editor.\n" +
   "      RUN on Windows: runThread to the end, the bar checked in the window's " +
   "own pixels, Cancel clicked, the time-out, threadComplete deleting the " +
   "object. Compiled for macOS and X11",
   "class ThreadWithProgressWindow"],
  ["gui", "ComponentBuilder",
   "gfx/component_builder.h: ComponentBuilder builds a component tree from a " +
   "JsonValue tree (json.h, the SDK's data tree) through registered " +
   "TypeHandlers -- \"type\" picks the handler, \"id\" becomes " +
   "Component::componentID and matches nodes across edits, \"bounds\" is [x, " +
   "y, w, h] or a live RelativeRectangle string, \"children\" nest -- and owns " +
   "what it builds. JsonValue is a value with no listeners, so a change is a " +
   "new tree given to setState(), and the builder RECONCILES: a node with " +
   "the same id and type keeps its component and is told only if its own " +
   "state changed, a new node is created, a removed one destroyed, a changed " +
   "type rebuilt, the z-order made to match; an unknown type is reported " +
   "through onError and skipped. Tested: the built tree, a relative bounds " +
   "string applied live, then an edit that changes one label, keeps one " +
   "untouched, removes one, adds one and reorders -- exactly two updates, " +
   "the components kept, the removed one destroyed -- then a type change " +
   "rebuilt and an unknown type reported. Platform-independent; RUN on " +
   "Windows",
   "class ComponentBuilder"],
  ["gui", "ComponentMovementWatcher",
   "gfx/movement_watcher.h: ComponentMovementWatcher listens to a component " +
   "AND every ancestor (re-attaching whenever the chain changes) and reports " +
   "componentMovedOrResized when its place in the window or its size changes " +
   "-- a parent sliding counts, a parent resized around it does not -- " +
   "componentVisibilityChanged when it may have been shown or hidden, and " +
   "componentPeerChanged when the window it is in (its MouseRouter, which " +
   "now announces itself to the tree on arrival and departure) appears or " +
   "goes. Built on ComponentListener (component_listeners.h), the " +
   "notification half the tree did not have: moved/resized, visibility, " +
   "parent hierarchy, children and being-deleted, with either side free to " +
   "be destroyed first. Tested: parent moved, parent resized, own resize, " +
   "root moved, ancestor hidden and shown, router created and destroyed, " +
   "re-parented, old chain released, the watched component destroyed. " +
   "Platform-independent; RUN on Windows",
   "class ComponentMovementWatcher"],
  ["gui", "VBlankAttachment",
   "gfx/vblank.h: VBlankAttachment calls back once per display refresh, on " +
   "the UI thread, while its component is in a window, with the refresh's " +
   "timestamp; one process-wide source serves every attachment, started with " +
   "the first and stopped with the last, and an attachment may destroy " +
   "itself from its own callback. Windows: a worker thread waits on " +
   "IDXGIOutput::WaitForVBlank (the primary output), else DwmFlush, else a " +
   "sleep at the monitor's rate, and posts ONE message at a time to a " +
   "message-only window -- a UI thread behind sees one late frame, not a " +
   "queue; dxgi and dwmapi loaded at run time. MEASURED on this machine (60 " +
   "Hz display, DXGI): 59.98 callbacks a second through a blocking message " +
   "loop; the test requires the display's rate within 40 % and runs.\n" +
   "      macOS: CVDisplayLink hopping to the main queue with dispatch_async_f, " +
   "both looked up at run time -- compiled against the shim, never run. X11: " +
   "no display link -- a timer at XRandR's current rate (60 Hz if " +
   "unreadable), checked on every pump of the X11 peer, so the rate is the " +
   "lower of the monitor's and the host's pumping; compiled against the real " +
   "Xlib headers, not run",
   "class VBlankAttachment"],
  ["gui", "AnimatedPosition (momentum scrolling)",
   "gfx/animated_position.h: AnimatedPosition<Behaviour> follows a drag " +
   "(beginDrag, drag, endDrag, nudge), measures the release velocity, and " +
   "then lets the behaviour move it under update(now) within setLimits; the " +
   "reference's two behaviours in AnimatedPositionBehaviours -- " +
   "ContinuousWithMomentum (friction 0.08 per 60th of a second, minimum " +
   "velocity 0.05, integrated EXACTLY over elapsed time so a flick goes the " +
   "same distance at 30, 60 or 144 Hz, which the reference's per-tick " +
   "multiply does not) and SnapToPageBoundaries (nearest page, or the next " +
   "one for a flick over a page a second). ViewportDragScroller drives a " +
   "Viewport by dragging its content, the reference's setScrollOnDragMode " +
   "(never, nonHover -- fingers and pens only -- or all): a MouseListener " +
   "over the viewed component, a few pixels' threshold so a tap stays a tap, " +
   "the bars and any setIgnoresDragsFrom component left alone, and the coast " +
   "ticked from a VBlankAttachment.\n" +
   "      Tested on a fake clock: 1000 units/s measured, the first frame and the " +
   "whole coast (v0 / lambda) to the unit, 30 Hz and 144 Hz coasting within " +
   "0.02 of each other, a coast stopped by its limit, snapping back, forward " +
   "and to the nearest page, and a Viewport dragged 100 px by a finger then " +
   "coasting on and stopping -- while a mouse drag does not scroll in " +
   "nonHover mode. Platform-independent; RUN on Windows",
   "class AnimatedPosition"],
  ["gui", "ConcertinaPanel",
   "gfx/concertina.h. Stacked sections that open and close, which is what a " +
   "plain list of forty parameters needs to stop being a wall the user reads " +
   "all of to find one control.\n" +
   "      The transition is through ValueAnimator rather than a snap, and that " +
   "is not decoration: a section snapping open moves everything below it in one " +
   "frame and the user has to re-find where they were looking. Because the " +
   "clock is an argument, a height mid-transition is an exact assertion instead " +
   "of something you watch.\n" +
   "      A closed section's content is HIDDEN, not merely given no height -- a " +
   "zero-height component still takes hit tests and still appears in the " +
   "accessible tree, so a reader would announce the contents of a section the " +
   "user has closed. Past the open limit the LEAST RECENTLY opened closes " +
   "rather than the click being refused, because a click that appears to do " +
   "nothing is worse than one the user can undo by clicking again",
   "class ConcertinaPanel"],
  ["gui", "PropertyPanel (grouped parameter editor)",
   "gfx/plugin_editor.h: GenericEditor lays its rows out in ConcertinaPanel " +
   "sections when the parameter table declares groups.\n" +
   "      This closed a gap that was ours alone. ParamInfo::group has existed " +
   "since groups were added and every wrapper reports it -- CLAP module path, " +
   "VST3 unit, AU clump, LV2 port group -- so a HOST's own generic panel has " +
   "been showing these parameters grouped the whole time while ours showed a " +
   "flat list. The data was there and only the editor ignored it.\n" +
   "      Ungrouped parameters stay flat at the top and each group becomes a " +
   "section under them, which is what the hosts do with a mixed table. A table " +
   "with no groups builds no panel at all, so the flat list is not a special " +
   "case of anything and did not change.\n" +
   "      Sections start OPEN. A closed one is invisible to a screen reader as " +
   "well as to the eye -- the panel PAINTS its headers rather than making them " +
   "components -- so collapsing by default would have taken parameters that " +
   "were reachable and hidden them behind a control a reader cannot see. Each " +
   "section body is announced as a Group carrying the group's name, which is " +
   "what puts the grouping in the tree rather than only on the screen",
   "class RowHolder"],
  ["gui", "ComponentAnimator",
   "gfx/animator.h. The clock is an ARGUMENT, so every rule -- easing shape, " +
   "landing exactly on the target, redirecting mid-flight from where it " +
   "actually is -- is an exact test rather than a sleep. tick() reports whether " +
   "anything moved, which is what stops a repaint thirty times a second when " +
   "nothing is happening", "class Animator"],

  // ── beyond the indexed modules ────────────────────────────────────────────────────────
  ["extras", "PropertiesFile", "UserSettings", "class UserSettings"],
  ["extras", "FileChooser", "FileDialog", "struct FileDialog"],
  ["extras", "(the reference has no CLAP)", "clap_wrapper.h", "CLAP_EXT_PARAMS"],
  // This said "(the reference has no MIDI-CI)". The reference has an entire midi_ci module
  // -- a Device, a Parser, profile and property hosts, and some sixty wire
  // message structs. The claim was simply false, and it sat in the map under a
  // heading that reads "beyond the indexed modules" saying we had something the reference did not.
  //
  // Caught by walking the module's class list, which is the whole argument for
  // doing that: the two older checks compare this map against our own headers,
  // and a row that is wrong ABOUT the reference is invisible to both.
  ["extras", "MIDI-CI: Device, Parser, MUID, ChannelAddress, FunctionBlock, Encodings, " +
             "ProfileHost, ProfileDelegate, ProfileAtAddress, PropertyHost, " +
             "PropertyDelegate, PropertyExchangeResult, Subscription, SubscriptionManager, " +
             "ResponderDelegate, DeviceFeatures, DeviceOptions, DeviceListener, Pagination",
   "midi_ci.h: the midici namespace, and it is deliberately the RESPONDING half " +
   "of the protocol rather than all of it.\n" +
   "      MIDI-CI is a negotiation between two devices: one enquires, the other " +
   "answers. The reference implements both sides because a framework application may be " +
   "either -- a host discovering plugins, or a plugin being discovered. A " +
   "plugin is only ever the second, so what is here is what answers: discovery, " +
   "profile enquiry, property exchange. A plugin that could INITIATE discovery " +
   "would be a plugin enumerating the user\u2019s other MIDI devices, which is " +
   "not its business.\n" +
   "      The subscription and pagination machinery is the part that grows " +
   "large in the reference, and it is large because a host maintains subscriptions for " +
   "many devices at once. One plugin answering for itself does not",
   "namespace midici"],

  // ── javascript ──────────────────────────────────────────────────────
  ["javascript", "JavascriptEngine",
   "javascript.h: JavascriptEngine, a tree-walking interpreter of a JavaScript " +
   "subset, written for this SDK -- no third-party engine, no JIT. var/let/const " +
   "with block scope and the temporal dead zone, closures, recursion, arrow " +
   "functions, default and rest parameters, spread, template literals, " +
   "optional chaining and ??, for / for-in / for-of / while / do / switch, " +
   "try/catch/finally, new and prototypes, the whole operator table with " +
   "ECMAScript's coercions; Object, Array, String, Number, Math, JSON, the " +
   "Error family, parseInt/parseFloat, eval at global scope and console.log " +
   "through a hook; plus the reference's own Math.sqr/range/randInt, " +
   "Array.contains/remove and trace(). Not here, deliberately: regular " +
   "expressions, classes, getters/setters, destructuring, generators, Map/Set, " +
   "Date, labels, sparse arrays.\n" +
   "      registerNativeObject / registerNativeFunction, evaluate(code, &result), " +
   "execute(code), callFunction(name, args), callFunctionObject, stop() from " +
   "any thread. Every failure is a Result with the message and line -- " +
   "nothing is thrown to the caller -- and every way a script could take the " +
   "host down ends in one: maximumExecutionTime (\"Execution timed-out\"), " +
   "stop() (\"Interrupted\"), maximumRecursionDepth plus a measured native-" +
   "stack budget under every recursive path, clipped to what the thread " +
   "really has, maximumMemoryBytes (everything allocated is metered and " +
   "refunded; garbage cycles are collected by trial deletion), and string " +
   "and array length checked before allocating. A script cannot catch any of " +
   "them.\n" +
   "      Tested by sdk/tests/javascript_test.cpp: 140 scripts whose expected " +
   "values were each checked against node, the reference's additions, 17 " +
   "errors at their lines, native objects both ways, the limits (six endless " +
   "loops, recursion a million deep, 200,000 nested brackets, an array nested " +
   "300,000 deep built and freed, memory hogs, 400,000 garbage cycles in 16 MB, " +
   "stop() from a thread) -- and outside the repository, 40,000 generated " +
   "programs run in node and here with identical output, the last 10,000 " +
   "against this exact header. " +
   "sdk/tests/fuzz_javascript.cpp: 25,229 mutated scripts under ASan, no " +
   "report, no hang. RUN on Windows (MSVC, optimised and ASan); the macOS and " +
   "Linux stack-bounds branches are COMPILED only (MSVC with tests/au_shim, " +
   "and clang's front end)",
   "class JavascriptEngine"],
  ["javascript", "var / DynamicObject / NativeFunctionArgs",
   "javascript.h: JsVar is the reference's var -- undefined, null, boolean, " +
   "number, string, array, object, function -- with one numeric kind, because " +
   "ECMAScript has one (isInt() asks about the value; the reference's separate " +
   "int, int64 and double make 1 and 1.0 a question). Strings are UTF-16 to " +
   "the script (\"é\".length is 1, an emoji's 2) and UTF-8 to C++. " +
   "DynamicObject holds properties in ECMAScript's order (integer keys " +
   "ascending, then insertion order), with setMethod for native methods, " +
   "invokeMethod, getPropertyNames and a deep clone() that keeps shared " +
   "structure shared and cycles cycles. JsVar bridges to json.h's JsonValue " +
   "both ways, and parseJson/toJsonString keep property order, which " +
   "JsonValue's sorted keys cannot. Values are reference counted, so a JsVar " +
   "a plugin keeps outlives the engine that made it, and freeing a structure " +
   "nested a million deep is iterative rather than a stack overflow in a " +
   "destructor",
   "class DynamicObject"],
  ["javascript", "JSObject / JSCursor",
   "javascript.h: the reference's newer engine hands C++ a JSObject or a " +
   "JSCursor to walk what a script built, because its values live in a " +
   "separate engine's heap. Here a script's values ARE JsVar and " +
   "DynamicObject: evaluate() returns one, getRootObject() is the global " +
   "scope, and a callback a script passes to C++ is a JsVar that " +
   "callFunctionObject calls later -- there is no second representation to " +
   "navigate",
   "getRootObject"],

  // ── osc ─────────────────────────────────────────────────────────────
  ["osc", "OSCSender / OSCReceiver", "osc.h: Sender, Receiver, and an Argument that " +
   "carries the four types every OSC implementation agrees on", "class Receiver"],

  // ── video ───────────────────────────────────────────────────────────
  ["video", "VideoComponent / CameraDevice",
   "gfx/video.h: VideoComponent plays a file inside an editor -- load, play, pause, stop, " +
   "position, duration, speed, volume -- with each frame copied into a Bitmap and drawn " +
   "through Graphics (letterboxed, clipped and scrolled like any image) rather than a native " +
   "child view, which an editor rasterised into one bitmap could not composite. update(), " +
   "called from the editor's tick, takes the newest frame and fires the callbacks, so all of " +
   "them run on the message thread. VideoReader underneath decodes frame by frame, with exact " +
   "(not keyframe) seeks and the soundtrack as float at a requested rate. CameraDevice lists " +
   "cameras, opens one, hands frames to listeners on the capture thread, and takes a still.\n" +
   "      The soundtrack is MUTED until setAudioEnabled(true): a plugin's sound belongs to " +
   "the host's mix, and a second output plays beside the session, not in it. Enabled, it plays " +
   "through the SDK's own WASAPI output on Windows (the device stays open; play, pause and seek " +
   "move a gate and refill a ring, 30 ms measured; silent at speeds other than 1x) and through " +
   "AVPlayer on macOS.\n" +
   "      Windows (Media Foundation) RUN by modules_test: a 160x120 H.264 + AAC file written " +
   "by Media Foundation's own encoder decodes back to 30 frames of the colours written, the " +
   "right way up, cropped from the codec's 128 rows, with exact seeks and a 440 Hz soundtrack " +
   "(also resampled to 44.1 kHz mono); VideoComponent plays it at 4x on its own clock; a real " +
   "webcam is listed, opened and delivers frames and a still. fuzz_modules decodes 300 mutated " +
   "files under ASan. macOS (AVPlayer + AVPlayerItemVideoOutput, AVAssetReader, " +
   "AVCaptureSession -- through the Objective-C runtime, every C function by dlsym, and a " +
   "refusal rather than a TCC kill when the host declares no camera use) COMPILED against " +
   "tests/au_shim by MSVC and clang, never run. Linux: every entry point reports \"not " +
   "available on this platform\" (compiled)",
   "class VideoComponent"],

  // ── analytics ───────────────────────────────────────────────────────
  ["analytics", "Analytics / AnalyticsDestination / ThreadedAnalyticsDestination / ButtonTracker",
   "analytics.h: Analytics (a shared instance or a private one; user id and properties; " +
   "logEvent with parameters and a type; suspend for an opt-out), AnalyticsEvent, the " +
   "AnalyticsDestination interface, and ThreadedAnalyticsDestination, which batches on a " +
   "thread of its own, retries a failed or throwing send with doubling backoff to a ceiling, " +
   "rate-limits with a token bucket, bounds its queue (oldest dropped, counted), and saves " +
   "unsent events to a JSON file at shutdown and restores them at start -- including the .tmp " +
   "a crash leaves. gfx/button_tracker.h: ButtonTracker logs an event per click of a Button or " +
   "IconButton, with the reference's ButtonState On/Off for a toggle.\n" +
   "      NOTHING IS SENT BY DEFAULT, and the header opens by saying so: Analytics starts with " +
   "no destination, and the SDK ships none that touches a network. The send is a virtual the " +
   "plugin writes, so what leaves a user's machine is code in the plugin where a reviewer can " +
   "read it. This module was out of scope for exactly that reason; the shape is the " +
   "reference's, the policy the opposite of on-by-default.\n" +
   "      RUN on Windows by modules_test (batching, backoff, rate limit, queue bound and the " +
   "file, all on a fake clock; a real thread through Analytics; ButtonTracker, including a " +
   "handler that deletes its own button, under ASan) and by fuzz_modules (27,000 mutated event " +
   "files under ASan, every accepted file re-written and re-read identically). Plain C++, " +
   "compiled for macOS and Linux",
   "class ThreadedAnalyticsDestination"],

  // ── box2d ───────────────────────────────────────────────────────────
  ["box2d", "Box2DRenderer",
   "gfx/box2d_renderer.h: draws a Box2D world through Graphics -- polygons, circles with a " +
   "spoke, edges, chains, capsules, rounded polygons, and joints -- mapping a rectangle of the " +
   "world onto one of the component (y up or down; lines in pixels whatever the zoom), " +
   "coloured from a replaceable Palette by body type and state: static, kinematic, awake, " +
   "asleep, disabled, sensor.\n" +
   "      Against the PLUGIN'S OWN Box2D, found with __has_include: 3.1 and later through " +
   "b2World_Draw (its named colours mapped back to the palette), 2.4 and 2.3 by walking bodies, " +
   "fixtures and joints without touching the world's own debug draw. Nothing is vendored -- " +
   "the reference ships the engine itself, which is how a project ends up with two copies of " +
   "every b2 symbol -- and with no Box2D on the include path the header declares nothing.\n" +
   "      RUN on Windows by box2d_renderer_test, built only when SONORE_BOX2D_DIR names a Box2D " +
   "tree, against Box2D 3.1.1 AND 2.4.2: a scene rendered to a Bitmap has each body's colour " +
   "where the body is, and a box that fell and went to sleep is drawn in the sleeping colour " +
   "where it landed. No platform code",
   "class Box2DRenderer"],
];

// One read of every header, so a hundred rows cost one pass.
let blob = "";
// RECURSIVELY. The headers were flat until the native UI work put a gfx/
// subdirectory beside them, and the first run after that reported four
// capabilities as GONE -- not because they had gone, but because this loop
// only ever looked one level deep. An instrument that cannot see a new
// directory reports its contents as missing.
function readTree(dir) {
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const full = join(dir, entry.name);
    if (entry.isDirectory()) readTree(full);
    // And the TypeScript declarations installed beside them: sonore.d.ts is
    // as much the SDK's surface as a header is.
    else if (entry.name.endsWith(".h") || entry.name.endsWith(".d.ts"))
      blob += readFileSync(full, "utf8") + "\n";
  }
}
readTree(inc);

// COMMENTS REMOVED before anything is looked up.
//
// These headers explain themselves at length, and several of them name a
// capability in order to say the SDK does NOT have it. hash.h contains the
// sentence "the reference ships MD5, SHA-256, RSA and Blowfish" as the reason only
// SHA-256 is here -- and the first version of the absence check read that
// sentence and reported Blowfish as present.
//
// It cuts the other way too, and more dangerously: a presence row whose symbol
// appeared only inside a comment would have this file certifying a capability
// that does not exist. Nothing had ever tested that, because a symbol usually
// appears in the code as well -- usually is not a guarantee.
//
// So the lookup sees code. A comment is documentation, not evidence.
blob = blob.replace(/\/\*[\s\S]*?\*\//g, " ").replace(/\/\/[^\n]*/g, " ");
if (!existsSync(inc) || !blob) {
  log("SDK headers not found at " + inc);
  process.exit(1);
}

let have = 0, missing = 0, deliberate = 0, gaps = 0;
const gone = [], gapList = [];
const byModule = new Map();
for (const [module, reference, ours, symbol, absentProof] of MAP) {
  if (!byModule.has(module)) byModule.set(module, []);
  // Absence, checked. A row claiming something is missing while it sits in
  // the headers is the same class of error as a row naming a symbol that has
  // gone -- and until this existed, only one of the two was catchable.
  // An absence with NOTHING to prove it is an absence nobody re-verifies. Five
  // of the thirteen were in that state -- including the AAX and VST2 rows,
  // whose absence is a legal position rather than an engineering one, held
  // only in prose and in whoever happened to remember it.
  //
  // Required rather than encouraged, because "we should add one" is what the
  // previous five were.
  if ((symbol === null || symbol === false) && !absentProof) {
    ++missing;
    gone.push(`${reference} is claimed absent with no proof-of-absence regex -- an absence ` +
              `nothing re-checks is one that quietly stops being true`);
    byModule.get(module).push(`  ??  ${reference.padEnd(34)} claimed absent, UNVERIFIABLE`);
    continue;
  }
  if ((symbol === null || symbol === false) && absentProof) {
    if (new RegExp(absentProof).test(blob)) {
      ++missing;
      gone.push(`${reference} is claimed ABSENT, but /${absentProof}/ matches the headers`);
      byModule.get(module).push(`  ??  ${reference.padEnd(34)} claimed absent, but it is THERE`);
      continue;
    }
  }
  if (symbol === false) {
    ++gaps;
    gapList.push(`${reference}  --  ${ours}`);
    byModule.get(module).push(`  ..  ${reference.padEnd(34)} NOT BUILT YET: ${ours}`);
    continue;
  }
  if (symbol === null) {
    ++deliberate;
    byModule.get(module).push(`  --  ${reference.padEnd(34)} deliberately not built`);
    byModule.get(module).push(`      ${" ".repeat(34)} ${ours ?? ""}`);
    continue;
  }
  if (blob.includes(symbol)) {
    ++have;
    byModule.get(module).push(`  ok  ${reference.padEnd(34)} ${ours}`);
  } else {
    ++missing;
    gone.push(`${reference}  (looked for "${symbol}")`);
    byModule.get(module).push(`  ??  ${reference.padEnd(34)} ${ours}  SYMBOL GONE`);
  }
}

for (const [module, rows] of byModule) {
  log(`\n── ${module} ${"─".repeat(Math.max(0, 66 - module.length))}`);
  for (const r of rows) log(r);
}

// ── Module coverage ─────────────────────────────────────────────────────────
log("\n── the reference modules " + "─".repeat(58));
let uncovered = 0;
const buckets = new Set(MAP.map((r) => r[0]));
for (const [name, bucket, why] of MODULES) {
  if (bucket === null) {
    log(`  --  ${name.padEnd(32)} out of scope: ${why}`);
    continue;
  }
  if (!buckets.has(bucket)) {
    log(`  ??  ${name.padEnd(32)} claims bucket "${bucket}", which has no rows`);
    ++uncovered;
    continue;
  }
  const n = MAP.filter((r) => r[0] === bucket).length;
  log(`  ok  ${name.padEnd(32)} ${String(n).padStart(2)} row(s) under "${bucket}"` +
      (why ? ` -- ${why}` : ""));
}
for (const b of buckets)
  if (!MODULES.some((m) => m[1] === b)) {
    log(`  ??  rows under "${b}" belong to no the reference module`);
    ++uncovered;
  }

log(`\n${MODULES.length} the reference modules: ${MODULES.filter((m) => m[1]).length} covered, ` +
    `${MODULES.filter((m) => !m[1]).length} out of scope`);
log(`${have} capabilities present, ${deliberate} deliberately absent, ` +
    `${gaps} real gap(s), ${missing} unaccounted for`);
if (gaps) {
  log("\nReal gaps -- the reference has these, they belong here, they are not written yet:");
  for (const g of gapList) log("  " + g);
}
if (uncovered) {
  log("\nA the reference module points at a bucket with no rows, or rows point at no");
  log("module. The row list cannot tell you what it never listed -- that is");
  log("exactly the gap this table exists to close.");
  process.exit(1);
}
if (missing) {
  log("\nThese rows name a symbol that is no longer in the headers. Either the");
  log("capability went away or it was renamed, and the point of this file is");
  log("that nobody finds out which from a document that quietly went stale.");
  for (const g of gone) log("  " + g);
  process.exit(1);
}
log("\nEvery capability this map claims is still in the headers.");
