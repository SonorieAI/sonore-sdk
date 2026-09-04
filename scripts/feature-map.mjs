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
  ["analytics", null,
   "telemetry. A generated plugin that phoned home would be a liability, not a feature"],
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
  ["box2d", null, "a 2D physics engine, shipped with the reference for historical reasons"],
  ["core", "core", null],
  ["cryptography", "core", null],
  ["data_structures", "core", null],
  ["dsp", "dsp", "audited class by class against the reference's own list"],
  ["events", "core", null],
  ["graphics", "graphics", null],
  ["gui_basics", "gui", null],
  ["gui_extra", "extras", null],
  // Out of scope, and the REASON needed restating for the same cause as the
  // two above: "the webview carries a JS engine" describes one of the two
  // editors. The native one carries no interpreter at all, and should not --
  // a plugin that could run scripts from a preset file is a plugin that runs
  // whatever arrives in a preset file.
  ["javascript", null,
   "scripting inside a plugin: the native editor deliberately has no interpreter, because a " +
   "preset that could carry code is a preset that can carry anything. A plugin that wants " +
   "scripting has the webview editor and its engine"],
  ["midi_ci", "extras", null],
  // The note here read "WebGL, in the same webview, without a second context to
  // manage", and that was true when the webview WAS the editor. The default is
  // the native toolkit now, and for that path this is a real decision rather
  // than a redirection: everything is rasterised in software, on the CPU, into
  // a bitmap the peer blits.
  //
  // Which is the right trade for a plugin editor, and worth saying why. A
  // software rasteriser produces the same pixels on every machine, which is
  // what makes the rendering checks in sdk_tests possible at all -- a GPU path
  // could only assert "approximately, on this driver". It needs no context,
  // survives a host that recreates the window, and cannot fail to initialise on
  // a remote desktop or in a VM, which is where a surprising number of plugin
  // users are.
  //
  // The cost, stated rather than hidden: repainting a large editor is CPU work.
  // Partial repaint is the answer -- damage is tracked per rectangle and
  // measured at 200 pixels against 12,100 for one knob moving -- and a plugin
  // that genuinely needs a shader still has the webview and WebGL.
  ["opengl", null,
   "software rasterisation, deliberately: identical pixels on every machine, no " +
   "context to lose, nothing to fail to initialise in a VM. WebGL remains " +
   "available through the webview editor for a plugin that needs a shader"],
  ["osc", "osc", null],
  ["product_unlocking", "core", null],
  ["video", null, "not an audio plugin concern; a page can carry a <video> if it must"],
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
   "not built: macOS realtime workgroups, which join a thread to the audio " +
   "workgroup the OS schedules", null, "AudioWorkgroup|os_workgroup"],
  ["basics", "MidiMessage system messages", "one policy: deliverableToDsp()", "inline bool deliverableToDsp"],
  ["basics", "(the reference has no MIDI clock follower)", "MidiClock: tempo from the wire", "class MidiClock"],
  ["basics", "MidiDataConcatenator (SysEx reassembly)", "SysexAssembler", "class SysexAssembler"],
  ["basics", "SysEx in MidiBuffer", "an arena beside the fixed events", "bool addSysex"],
  ["client", "SysEx input (CLAP, VST3, LV2)", "copied, never referenced", "CLAP_EVENT_MIDI_SYSEX"],
  ["client", "SysEx input (the standalone's MIDI ports)",
   "winmm MIM_LONGDATA, ALSA byte stream, CoreMIDI packets -- all through one " +
   "SysexAssembler",
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
   "not built: score metadata, endpoint discovery and 8-bit SysEx. A host " +
   "translating a MIDI 2.0 stream sends channel voice and SysEx; the rest is " +
   "stream configuration it settles before a plugin sees anything", null,
   "flexDataFrom|sysex8Assemble|class UmpStream"],
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
  ["processors", "AudioProcessorEditor", "webview editor", "inline std::string bridgeScript"],
  ["processors", "GenericAudioProcessorEditor", "gfx/plugin_editor.h, built from ParamInfo",
   "class GenericEditor"],
  ["processors", "SliderParameterAttachment", "gfx/plugin_editor.h", "class SliderAttachment"],
  ["processors", "ComboBoxParameterAttachment", "gfx/plugin_editor.h", "class ComboAttachment"],
  ["processors", "editor host context menu", "FileDialog + host menu", "showHostContextMenu"],
  ["processors", "AudioProcessorValueTreeState",
   "no capability gap, and deliberately not one object. APVTS bundles four " +
   "jobs -- the parameter list, non-parameter state, undo, and binding widgets " +
   "to values -- and here they are four things: ParamInfo, StateBag, " +
   "UndoHistory, and SliderAttachment/ComboAttachment for the native editor or " +
   "sonore.params and sonore.set for the web one.\n" +
   "      Four separable things rather than one is the point: a plugin that " +
   "wants parameters without undo, or state without a GUI, takes what it needs. " +
   "The reason on this row previously mentioned only the web interface, which " +
   "stopped being the whole answer when the native editor gained attachments",
   null, "AudioProcessorValueTreeState|class ValueTreeState"],


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
   "not built: an allpass fractional delay is flat in magnitude but disperses " +
   "phase and rings on a fast sweep. The third-order read is the better " +
   "default for the one thing this SDK modulates", null, "Thiran|thiran"],
  ["dsp", "dsp::Matrix",
   "not built: general matrix arithmetic. The reference ships it for ambisonics and " +
   "matrix mixing; nothing here needs one, and an unused numerics class is " +
   "an untested one", null, "class\s+Matrix|struct\s+Matrix"],
  ["dsp", "dsp::Polynomial",
   "not built as a class: the polynomials here are inline where they are used " +
   "-- the BLEP residual, the sine approximation, the dB curve -- and each is " +
   "checked against the function it approximates", null, "class\s+Polynomial|struct\s+Polynomial"],

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
   "pitch_correct.h: PitchCorrector -- YIN to the nearest allowed scale note in cents, phase " +
   "vocoder moves it there, retune-speed glide", "class PitchCorrector"],
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
   "limiting.h: TruePeakLimiter -- the look-ahead limiter with BS.1770-4's 4x inter-sample " +
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
   "graph.h: a processor graph does both, and does routing besides", "class PluginGraph"],
  ["basics", "ToneGeneratorAudioSource", "the standalone's test source", "class TestSource"],
  ["basics", "AudioTransportSource", "SampleStream + the standalone's transport",
   "class SampleStreamer"],
  ["basics", "MemoryMappedAudioFormatReader / BufferingAudioReader",
   "SampleStream: a worker thread reads ahead, the audio thread never touches the file",
   "class SampleStreamer"],
  ["basics", "MidiMessageCollector",
   "MessageQueue collects, but timestamps every event at offset 0 rather than " +
   "at the frame it arrived. A USB keyboard's own jitter is already larger " +
   "than a block, so the precision would be invented", null,
   "collectorTimestamp|midiTimestampFrames"],
  ["devices", "SystemAudioVolume",
   "not built: changing the machine's output volume is not a plugin's business, " +
   "and a standalone that did it would be altering something the user set for " +
   "everything else", null, "SystemAudioVolume|setSystemVolume"],

  // ── audio_plugin_client ─────────────────────────────────────────────
  ["client", "clap.preset-discovery",
   "clap_wrapper.h: PresetProvider. How a CLAP host finds a plugin's factory " +
   "presets WITHOUT loading the plugin -- it reads them from an index the " +
   "plugin declares, so a browser can list ten thousand presets across forty " +
   "plugins without instantiating any of them",
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
  ["client", "AAX wrapper",
   "blocked: Avid's SDK needs a developer agreement, and PACE signing needs " +
   "hardware. Fabricating it against a guessed ABI would be worse than absent",
   // A guard rather than a note. The instruction not to fabricate an AAX
   // wrapper has been carried in prose and in somebody's memory; this makes
   // the build fail if one ever appears, which is the only form of the rule
   // that survives whoever remembers it.
   null, "AAX_|AaxWrapper|AAXEffectParameters|AAX_CEffectParameters"],
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
   "resample.h: Resampler, plus PolyphaseIirHalfBand for the two-to-one steps. " +
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
   "random.h. Pink is Voss-McCartney, flat to a tenth of a decibel per octave; " +
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
   "host.h: MemoryStream, which is what a plugin's state actually travels " +
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
  ["core", "InputStream / OutputStream", "AudioFileReader + writeWav", "class AudioFileReader"],
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
   "not built. SHA-256 covers the one job this SDK has for a hash -- naming a " +
   "state blob -- and a cipher nobody uses is a cipher nobody has tested", null, "blowfish|Blowfish|rsaEncrypt|md5Hex"],
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
  ["graphics", "PathStrokeType (joins, caps, miter limit)",
   "gfx/stroke.h: a quad per segment, unioned -- no self-intersecting outline",
   "class Stroker"],
  ["graphics", "ColourGradient",
   "gfx/gradient.h -- linear and radial, any number of stops, interpolated in " +
   "STRAIGHT alpha so a fade to transparent does not pass through black",
   "class ColourGradient"],
  ["graphics", "Typeface", "gfx/truetype.h: glyf outlines parsed to Paths", "class Typeface"],
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
   "so no .mm file and no mixed-language build. Compiled on macOS by CI since " +
   "2026-09-01",
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
   "opens", "class NativeEditor"],
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
   "licence rides along in every generated plugin", "inline std::shared_ptr<Typeface> systemTypeface"],

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
   "Win32 one cannot",
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
   "bits, 16-bit reduced to 8, tRNS, and multiple IDAT chunks. JPEG " +
   "deliberately not: a plugin's artwork is flat colour and hard edges",
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
   "gfx/svg.h: paths, the six shape elements, groups with cascading fills and " +
   "composed transforms, viewBox. The one piece of real geometry is the arc -- " +
   "SVG states it endpoint-first and a renderer needs centre-first, including " +
   "the spec's rule that radii too small to span the endpoints are scaled UP " +
   "rather than producing a NaN. Text, gradients and filters are named as " +
   "unsupported rather than silently dropped", "class Drawable"],

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
   "      Not yet: UiaRaiseAutomationEvent, so a value moved by AUTOMATION " +
   "rather than by the user is announced without being asked for. Named in the " +
   "header rather than left to be discovered missing",
   "class RootProvider"],
  ["gui", "the accessibility bridge, macOS (NSAccessibility)",
   "NOT BUILT, and a REAL GAP. The description layer and the Windows bridge " +
   "between them make this a mechanical mapping -- the same AccessibleInfo, " +
   "through the Objective-C runtime the Cocoa peer already uses, onto " +
   "NSAccessibility\'s roles. Not built yet: the Windows bridge was proved by " +
   "driving it with a real UI Automation client, and the same standard is " +
   "what this one is held to",
   false, "NSAccessibilityElement|accessibilityLabel|NSAccessibilityRole"],
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
   "gfx/effects2d.h, and the SVG parser uses it rather than its own thirteen. " +
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
   "deliberately absent, and png.h has carried the reason since it was " +
   "written: a plugin's artwork is flat colour and hard edges, which is what " +
   "PNG is for and what JPEG is worst at -- ringing on every hard edge, and no " +
   "alpha channel at all, so a knob cannot have a transparent surround. A JPEG " +
   "decoder is a DCT and a Huffman decoder for a format that would make the " +
   "art worse. GIF adds a 256-colour palette on top of that.\n" +
   "      The reason was in the header and not in this map, so the audit " +
   "counted it as undecided. It was decided; nobody had written it down where " +
   "the count could see it",
   null, "JPEGImageFormat|JpegDecoder|GIFImageFormat|GifDecoder"],
  ["graphics", "ImageType / NativeImageType / SoftwareImageType",
   "out of scope, and it is the same decision the opengl row makes. The reference " +
   "has these because an Image may live on the GPU or in main memory and the " +
   "code drawing it must not care. Everything here is software-rasterised into " +
   "one premultiplied RGBA Bitmap, on purpose -- identical pixels on every " +
   "machine, no context to lose, nothing to fail to initialise in a VM. With " +
   "one backing there is nothing for the abstraction to abstract",
   null, "class ImageType|NativeImageType|SoftwareImageType"],
  ["graphics", "ImageCache",
   "deliberately absent: the reference caches decoded images by file hash because its " +
   "Image can be loaded from a path anywhere, repeatedly, by code that does not " +
   "know it is repeating. Nothing here decodes during paint -- a plugin decodes " +
   "its art once when the editor is built and holds the Bitmap -- so a cache " +
   "would be a lookup on a thing already held.\n" +
   "      It would become real if a plugin ever decoded per frame, which is a " +
   "bug rather than a use case",
   null, "class ImageCache"],
  ["graphics", "ScaledImage",
   "deliberately absent as an object: an image bundled with the scale it was " +
   "drawn for, so @2x art picks itself on a HiDPI screen. The pieces are here " +
   "and are the honest ones -- Backing knows the device scale, drawImage and " +
   "drawImageTransformed take arbitrary transforms -- and a plugin with two " +
   "sizes of art chooses on Backing::scale.\n" +
   "      Not wrapped up, because the wrapper's value is in the reference's ecosystem of " +
   "components that accept one. Nothing here accepts one",
   null, "class ScaledImage"],
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
   "deliberately absent: the reference uses it to describe an image's destination under " +
   "an affine transform, as three corners. drawImageTransformed takes the " +
   "Transform itself, which says the same thing in the form the rasteriser " +
   "already needs and cannot describe a shape the transform could not produce",
   null, "class Parallelogram|struct Parallelogram"],
  ["graphics", "GlyphArrangement / PositionedGlyph / TypefaceMetrics / FontOptions",
   "gfx/font.h, inside Font rather than beside it. A run of text is walked once " +
   "with kerning and advance widths accumulated, and each glyph appended to a " +
   "Path at its pen position -- which is a glyph arrangement, built and " +
   "consumed in one pass. The reference keeps the arrangement because its text can be " +
   "hit-tested, re-justified and edited in place after layout.\n" +
   "      The one place that genuinely needs positions per glyph is the text " +
   "editor's caret, and it asks Font for the width of a prefix, so measuring " +
   "and drawing cannot disagree -- which is the failure an arrangement object " +
   "exists to prevent and is prevented here by there being one code path",
   "class Font"],
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
   "Mac and Cocoa routes it to rightMouseDown: for us",
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
   "deliberately absent: the reference's way of asking WHICH host you are in, so a " +
   "plugin can work around a named DAW's quirks. Deliberate because the " +
   "workarounds are the problem -- a plugin branching on the host is a plugin " +
   "with a code path per DAW, most of which nobody can test, and each one " +
   "outliving the bug it was written for by years.\n" +
   "      Where a host difference is real it belongs in the WRAPPER, behind a " +
   "capability question rather than a name: does this host offer a context " +
   "menu extension, does it answer gui_request_resize, did it give us a frame. " +
   "Every one of those is asked here already, and each is true or false about " +
   "the host in front of us rather than about a name we recognised",
   null, "class PluginHostType|whichHost|isAbletonLive|isLogic"],
  ["core", "Plugin HOSTING: KnownPluginList, PluginDirectoryScanner, PluginListComponent, " +
           "AudioUnitPluginFormat, LADSPAPluginFormat, LV2PluginFormat, VSTPluginFormat",
   "out of scope, and it is the largest single block of the reference that is. These " +
   "are for an application that LOADS plugins -- scanning directories, keeping " +
   "a list, showing the user a chooser, and implementing each format from the " +
   "host's side. This SDK is the other end of that pipe: it makes the thing " +
   "those classes load.\n" +
   "      Worth naming rather than leaving unmentioned, because a count of " +
   "audio_processors that treated these as missing capabilities would " +
   "make the module look half covered when the half that applies is complete",
   null, "class KnownPluginList|class PluginDirectoryScanner|class VSTPluginFormat"],
  ["gui", "NSViewComponentWithParent",
   "out of scope: the reference's workaround for hosting a plugin's NSView inside its " +
   "own view hierarchy on macOS, which is a HOST's problem. Our Cocoa peer is " +
   "the plugin end -- it is handed a parent NSView and puts one view in it",
   null, "NSViewComponentWithParent"],
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
   "PARTIAL, on purpose, and the split follows what can be run here.\n" +
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
   "      X11 and macOS: still absent. No XOpenIM/XCreateIC, no " +
   "NSTextInputClient. Same rule as the macOS accessibility bridge -- neither " +
   "can be run from here, and IME is a feature whose entire behaviour is what " +
   "a user sees while composing, so code for it that has never been typed into " +
   "is code nobody has tested.\n" +
   "      What is proved on Windows is the half that can be: the point handed " +
   "to imm32 is beside the caret of the focused field in device pixels, " +
   "asserted through a real window with a real focused editor -- because the " +
   "failure being guarded against is a point of (0,0) from coordinates nobody " +
   "converted, which looks exactly like not having implemented this. What is " +
   "NOT proved is the visual result, which needs an IME installed and a human " +
   "composing",
   "compositionPoint"],
  ["gui", "IME on X11 and macOS",
   "NOT BUILT, and a REAL GAP -- the remaining two thirds of TextInputTarget " +
   "above. X11 needs XOpenIM and an XIC per window with the spot location set " +
   "from the caret; macOS needs the view to implement NSTextInputClient, which " +
   "is a protocol with a dozen methods and no shortcut.\n" +
   "      Blocked on the same thing as the accessibility bridge and honest for " +
   "the same reason: neither platform runs in this loop",
   false, "XCreateIC|XOpenIM|NSTextInputClient"],
  ["gui", "ModalComponentManager",
   "gfx/alert.h: Overlay and AlertOverlay, which are drawn INSIDE the editor " +
   "and take the mouse while they are up. The reference needs a manager because its " +
   "modal components are real windows with a nested event loop; ours cannot be, " +
   "and must not be -- a nested loop inside a plugin is a loop the host is not " +
   "running, and the transport keeps going while the DAW stops answering",
   "class AlertOverlay"],
  ["gui", "CachedComponentImage / ComponentPaintDiagnostics",
   "deliberately absent. CachedComponentImage keeps a component's rendering in " +
   "a bitmap so a repaint can blit instead of redraw -- worth it in the reference " +
   "because a paint may go through a GPU context with real per-call cost. Here " +
   "a repaint is a software rasteriser writing into one bitmap, and only the " +
   "damaged region is touched: the cache would be a second copy of pixels to " +
   "keep in step with the first.\n" +
   "      ComponentPaintDiagnostics flashes repainted areas for debugging. The " +
   "region test does the same job by assertion instead of by eye -- it checks " +
   "WHICH rectangles were marked dirty, which is the thing the flashing is for " +
   "working out",
   null, "class CachedComponentImage|ComponentPaintDiagnostics"],
  ["gui", "LookAndFeel_V1 / V2 / V3 / V4",
   "out of scope: the reference carries four historical skins so old projects keep " +
   "looking as they did. This SDK has one LookAndFeel and its first release " +
   "has not happened, so there is no old appearance to preserve -- and a " +
   "generated plugin that wanted a different look overrides the one there is",
   null, "LookAndFeel_V1|LookAndFeel_V2|LookAndFeel_V3|LookAndFeel_V4"],
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
   "deliberately absent as separate classes: a Component whose whole job is to " +
   "draw a Drawable. gfx/svg.h's Drawable::draw takes a Graphics and a " +
   "rectangle, so a component that shows one is two lines in its own paint() " +
   "-- and a plugin's artwork is nearly always drawn as part of a background " +
   "rather than as a component of its own. ImageComponent exists for the " +
   "bitmap case because a bitmap also needs placement and scaling rules",
   null, "class DrawableComponent|OwningDrawableComponent"],
  ["gui", "MouseInputSource / PenDetails",
   "deliberately absent: the reference models several simultaneous pointers, and a " +
   "stylus with pressure and tilt, because it targets tablets. A plugin editor " +
   "has one pointer -- every host embeds it in a desktop window -- and a second " +
   "one would need every control to decide what two simultaneous drags on it " +
   "mean. Pressure would be worth having the day this targets a tablet, and " +
   "that is a change of answer rather than a change of effort",
   null, "class MouseInputSource|struct PenDetails"],
  ["gui", "MouseWheelDetails",
   "gfx/component.h: MouseRouter::mouseWheel takes the delta directly. The reference " +
   "wraps it because its struct also carries smooth-scroll and inertia flags " +
   "from the platform; ours is one number, and the peers already turn each " +
   "platform's wheel message into it",
   "mouseWheel"],
  ["gui", "MouseInactivityDetector",
   "deliberately absent: hides the pointer after a period of stillness, which " +
   "is for kiosks and full-screen players. A plugin editor is a panel in " +
   "somebody else's window and must never hide the user's cursor",
   null, "class MouseInactivityDetector"],
  ["gui", "SettableTooltipClient",
   "gfx/widgets.h: setTooltip on Widget, read by TooltipManager. The reference splits " +
   "the interface from the settable implementation so a component can compute " +
   "its tip; a std::string that anything may overwrite covers both, and the " +
   "manager takes the clock as an argument so every tooltip rule is an exact " +
   "test rather than a wait",
   "setTooltip"],
  ["gui", "TextDragAndDropTarget",
   "deliberately absent: dropping selected TEXT onto a component. " +
   "FileDragAndDropTarget is here because dropping a sample onto a plugin is " +
   "the gesture people actually make; dragging text into an audio plugin is " +
   "not a thing that happens, and a target for it would be a code path with no " +
   "caller",
   null, "class TextDragAndDropTarget"],
  ["gui", "Relative positioning: MarkerList, RelativeCoordinate, RelativePoint, " +
          "RelativeRectangle, RelativePointPath, RelativeParallelogram, " +
          "RelativeCoordinatePositionerBase",
   "out of scope, and the reference itself moved on from it: positions expressed as " +
   "expressions against named markers and other components' edges, parsed from " +
   "strings and re-evaluated when anything moves. FlexBox and Grid arrived " +
   "later and are what the reference's own documentation now points at, and they are " +
   "what this SDK has.\n" +
   "      A layout language evaluated at runtime is also the thing this SDK " +
   "deliberately does not do anywhere -- the same reason ComponentBuilder is " +
   "out of scope. A generated plugin's layout is code, and the compiler checks " +
   "it",
   null, "class RelativeCoordinate|class RelativePoint|class MarkerList"],
  ["gui", "File chooser components: FileChooserDialogBox, FileListComponent, " +
          "FileTreeComponent, FilenameComponent, FilePreviewComponent, " +
          "ImagePreviewComponent, FileSearchPathListComponent",
   "gfx/file_browser.h has FileBrowser -- a list, navigation and a selection -- " +
   "and file_dialog.h has the NATIVE chooser, which is what a plugin should " +
   "use to load a sample: it is the dialog the user already knows, it can " +
   "reach network volumes and permissions a drawn list cannot, and on macOS it " +
   "is the only way to get a file out of the sandbox.\n" +
   "      The rest are pieces of an application's file UI. A preview pane and " +
   "a search-path editor belong to a program that manages a library; a plugin " +
   "loads a file and gets on with it",
   null, "class FileChooserDialogBox|class FilenameComponent|class ImagePreviewComponent"],
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
   "out of scope for the plugin formats, which have no application object at " +
   "all -- the host owns the process, the message loop and the lifetime, and a " +
   "plugin that created an application singleton would be a second one inside " +
   "somebody else's. The standalone genuinely needs the job done and does it " +
   "in standalone.h's main(): parse the options, open the devices, run the " +
   "loop, and shut down in the reverse order",
   null, "class Application"],
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
   "out of scope in the direction the reference means, and already solved in the " +
   "direction that matters here. These put a platform CONTROL inside a framework " +
   "component tree. This SDK is on the other side of that: the window_*.h " +
   "peers are handed a parent HWND, NSView or X window by the host and put " +
   "themselves inside IT, and webview.h does the one piece of embedding that " +
   "goes the other way -- a browser control inside our own window.\n" +
   "      A plugin that genuinely needed a third-party native control inside " +
   "its editor would be reaching past this SDK, and that is a reasonable place " +
   "for the boundary",
   null, "class HWNDComponent|class NSViewComponent|class XEmbedComponent"],
  ["extras", "CodeEditorComponent / CodeDocument / CPlusPlusCodeTokeniser / LuaTokeniser / " +
             "XmlTokeniser",
   "out of scope: a syntax-highlighting source editor. The reference carries it because " +
   "a framework's own project editor and demos are applications that edit code. An " +
   "audio plugin's editor edits parameters, and shipping a code editor inside " +
   "one would be shipping a text engine nobody asked for -- with the same " +
   "objection the javascript row makes about a plugin that can run code",
   null, "class CodeEditorComponent|class CodeDocument|Tokeniser"],
  ["extras", "ColourSelector",
   "deliberately absent: the reference's colour picker, a wheel with a brightness " +
   "column and hex entry. Nothing generated by this SDK asks a user to choose " +
   "a colour -- a plugin's appearance is designed by whoever built it, and " +
   "user-theming would need a colour to persist in the session and to survive " +
   "every future change to the interface's palette.\n" +
   "      The pieces exist if a plugin wants one: Colour converts to and from " +
   "HSV, gradient.h has the sweeps a wheel is drawn from, and ValueBox takes " +
   "typed entry",
   null, "class ColourSelector"],
  ["extras", "SystemTrayIconComponent / SplashScreen / PreferencesPanel / " +
             "RecentlyOpenedFilesList / AppleRemoteDevice / PushNotifications / " +
             "AnimatedAppComponent",
   "out of scope: application furniture. A tray icon, a splash screen, a " +
   "preferences window, a recent-files menu, an Apple IR remote and mobile " +
   "push notifications all belong to a program that owns its process. A plugin " +
   "owns a rectangle inside somebody else's.\n" +
   "      AnimatedAppComponent is the odd one and worth naming separately: it " +
   "is a component with a timer built into it, which is exactly the shape this " +
   "SDK avoids everywhere. Every timed thing here takes the elapsed time as an " +
   "ARGUMENT, which is what makes an animation mid-transition an exact " +
   "assertion instead of a sleep",
   null, "class SystemTrayIconComponent|class SplashScreen|class PreferencesPanel|class AnimatedAppComponent"],
  ["extras", "BubbleMessageComponent",
   "gfx/alert.h: CallOutBox with a caption, and TooltipManager for the fading " +
   "kind. the reference's version is a BubbleComponent that shows a message and removes " +
   "itself after a delay -- the delay here belongs to TooltipManager, which " +
   "takes the clock as an argument so every one of its timing rules is a test " +
   "rather than a wait",
   "class TooltipManager"],
  ["extras", "KeyMappingEditorComponent",
   "deliberately absent: the UI for letting a user reassign shortcuts. " +
   "commands.h has the CommandManager and the Shortcut table it maps, so the " +
   "model is here; an editor for it needs the mapping to PERSIST, and a " +
   "plugin's shortcut set is small, fixed, and shared with a host that owns " +
   "most of the keyboard already",
   null, "class KeyMappingEditorComponent"],
  ["extras", "FileBasedDocument",
   "deliberately absent as a class: open, save, save-as and a dirty flag, with " +
   "the prompts that go around them. A plugin has exactly this problem and the " +
   "host owns the answer -- state is saved and restored through the format's " +
   "own state calls, into the session, and the dirty flag is reported through " +
   "clap_host_state::mark_dirty rather than kept here. presets.h covers the " +
   "part a plugin does own, which is named presets it can load",
   null, "class FileBasedDocument"],
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
   "deliberately absent, and this is a rule rather than a preference. The reference " +
   "owns a message loop and hands out a lock on it. A plugin does NOT own the " +
   "loop -- the host does -- and a plugin that took a lock on the host's " +
   "message thread from its audio thread would be a plugin that can deadlock a " +
   "DAW.\n" +
   "      The same job is done without a lock: UiEventQueue is single-producer " +
   "single-consumer and lock-free, the audio thread pushes and the editor " +
   "drains on the tick the host already gives us. That is the whole of it, and " +
   "it is checked by the RT-safety gate rather than by inspection",
   null, "class MessageManager|MessageManagerLock"],
  ["core", "LockingAsyncUpdater",
   "deliberately absent: the reference's AsyncUpdater variant that may be triggered " +
   "while the message thread is inside a callback, at the cost of a lock. " +
   "There is no lock to take here for the reason above -- the queue is " +
   "lock-free and drains on the host's tick, so triggering it from any thread " +
   "is already safe and costs no lock at all",
   null, "class LockingAsyncUpdater"],
  ["core", "Inter-process: ChildProcessCoordinator, ChildProcessWorker, " +
           "InterprocessConnection, NetworkServiceDiscovery, MountedVolumeListChangeDetector, " +
           "DeletedAtShutdown, ApplicationBase",
   "out of scope: process and machine plumbing for an application. The reference has " +
   "the child-process pair because it can run a plugin out-of-process during " +
   "scanning, which is a HOST's problem; the connection and discovery classes " +
   "are for programs that talk to other programs.\n" +
   "      A plugin runs inside the host's process by definition. Spawning a " +
   "child from one is a thing users find in a firewall log and do not forgive",
   null, "class ChildProcessCoordinator|class InterprocessConnection|class NetworkServiceDiscovery|DeletedAtShutdown"],
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
   "user_settings.h: PropertiesFile is there, and this is the layer above it " +
   "-- the reference's pairing of a per-user file with a shared machine-wide one, and " +
   "the logic for reading through from one to the other. A plugin has settings " +
   "that belong to the person using it and none that belong to the machine, so " +
   "the second file would never hold anything",
   null, "class ApplicationProperties"],
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
   "out of scope: the reference's PULL model, where a source is asked for the next " +
   "block and pulls from whatever it is chained to. A plugin is the other " +
   "shape entirely -- the host PUSHES a block in and expects it filled, which " +
   "is what every format's process call is.\n" +
   "      The processing those classes wrap is all here and reachable in the " +
   "push shape: shaping.h has ProcessorChain and ProcessorDuplicator for " +
   "chaining, dsp.h has the filters and the reverb, interpolation.h has " +
   "resampling, and channel remapping is what the bus negotiation does. " +
   "BufferingAudioSource is the one with no equivalent, and deliberately: it " +
   "reads ahead on a background thread, which is a disk-streaming design the " +
   "sampler solves its own way",
   null, "class AudioSource|class BufferingAudioSource|class MixerAudioSource"],
  ["basics", "IIRCoefficients / GenericInterpolator",
   "dsp.h: Biquad carries its own coefficients and the designers that fill " +
   "them; interpolation.h has Interpolator with the kernels selectable. The reference " +
   "separates coefficients from the filter so one set can be shared across " +
   "many instances, which matters for a large multi-channel graph; a plugin's " +
   "filter count is small and a Biquad holding its own coefficients is one " +
   "object to reason about instead of two whose lifetimes must agree",
   "class Biquad"],
  ["basics", "AudioWorkgroup / WorkgroupToken",
   "deliberately absent: Apple's audio workgroups, which let a plugin's own " +
   "worker threads be scheduled with the same real-time priority as the audio " +
   "thread. It is real and it matters for a plugin that farms work out to " +
   "helper threads on macOS.\n" +
   "      Absent for two reasons that hold together. Nothing here spawns a " +
   "helper thread during processing -- ThreadPool exists for loading a sample, " +
   "not for rendering a block, and the RT-safety gate would fail anything that " +
   "did. And it is macOS-only API on the one platform this loop cannot run, " +
   "so it would be written blind against a rule nothing here currently needs. " +
   "The day a DSP genuinely renders across threads, this becomes required " +
   "rather than optional",
   null, "AudioWorkgroup|os_workgroup"],
  ["graphics", "ScopedBlendContext",
   "gfx/graphics.h: Graphics::ScopedState, which saves and restores the clip, " +
   "the transform and the colour together. The reference added a separate scoped BLEND " +
   "object in 8 for its GPU backend, where the blend mode is pipeline state " +
   "with a real cost to change. Everything here composites premultiplied RGBA " +
   "in software with one blend rule, so there is no second piece of state to " +
   "scope",
   "class ScopedState"],
  ["extras", "AudioThumbnailCache",
   "deliberately absent: the reference caches computed waveform peaks by file hash so a " +
   "thumbnail does not have to be rebuilt every time a file is shown. " +
   "audio_display.h computes peaks once, when a sample is loaded, and the " +
   "sampler holds them for as long as it holds the sample -- so there is " +
   "nothing to look up and nothing to invalidate.\n" +
   "      A cache would be worth it for a browser that draws a hundred files " +
   "the user is scrolling past, which is a library manager rather than a " +
   "plugin",
   null, "class AudioThumbnailCache"],
  ["extras", "MPEKeyboardComponent",
   "deliberately absent: the reference's MPE keyboard draws each note's pitch bend and " +
   "pressure as it is played, which needs the keyboard to be told about every " +
   "expression event. gfx/midi_keyboard.h shows which notes are sounding, " +
   "which is the question a user actually has in front of a plugin -- is it " +
   "receiving MIDI at all.\n" +
   "      A real MPE surface is a controller, not a mouse: the gestures it " +
   "displays are slides and pressures that a mouse cannot produce, so the " +
   "drawing would be showing the user something they cannot do with the " +
   "control they are looking at",
   null, "class MPEKeyboardComponent"],
  ["extras", "AudioProcessorPlayer / SoundPlayer / AudioAppComponent",
   "standalone.h does this job: it opens the device, prepares the DSP, pumps " +
   "blocks, and feeds MIDI in. the reference's classes are the glue between an " +
   "AudioProcessor and an AudioDeviceManager for an application that hosts " +
   "one; the standalone is that application, and it is one file rather than a " +
   "set of components because there is exactly one plugin in it and no graph " +
   "to build",
   "SONORE_BUILD_STANDALONE"],
  ["extras", "AudioCDReader / AudioCDBurner / BluetoothMidiDevicePairingDialogue / " +
             "Box2DRenderer",
   "out of scope, and these are the clearest cases in the whole audit. Reading " +
   "and burning audio CDs is a thing the reference carries from an era when a DAW " +
   "shipped a mastering step; Bluetooth MIDI pairing is an iOS system dialogue; " +
   "Box2DRenderer draws a physics engine's debug output, which is in the reference " +
   "because the reference ships a Box2D demo.\n" +
   "      Named rather than skipped because the point of this audit is that " +
   "nothing is passed over silently -- including the parts where the answer is " +
   "obvious",
   null, "class AudioCDReader|class AudioCDBurner|Box2DRenderer|BluetoothMidiDevicePairing"],
  ["core", "The C++ standard library does it: Array, OwnedArray, ReferenceCountedArray, " +
           "SortedSet, SparseSet, HashMap, Span, Optional, var, DynamicObject, " +
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
   "file and JSON handling all have their own",
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
   "files.h and std::filesystem, plus audiofile.h for the one format that " +
   "matters. the reference's stream hierarchy exists so any source -- a file, memory, a " +
   "URL, a subregion of another stream -- can be read by the same code, which " +
   "is what its ecosystem needs.\n" +
   "      A plugin reads sample files and writes preset files. audiofile.h " +
   "reads a WAV by parsing it directly, which is why there is no library and " +
   "no licence to carry with it, and the rest is std::ifstream. " +
   "MemoryMappedFile is the one with a real argument for it -- a large sample " +
   "mapped rather than read -- and the sampler streams from disk on its own " +
   "terms instead, so the mapping would be a second answer to a solved problem",
   null, "class FileInputStream|class MemoryMappedFile|class WildcardFileFilter"],
  ["core", "XmlElement / XmlDocument",
   "deliberately absent: plugin state is a binary blob with a version header, " +
   "and json.h covers the one place a human-readable format is genuinely " +
   "wanted -- the bridge to the web editor, where the other end is JavaScript " +
   "and JSON is its native form.\n" +
   "      XML is here in the reference because VST2 presets and its own ValueTree " +
   "serialise to it. Neither applies: VST2 is deliberately never, and the " +
   "state model is a flat bag. An XML parser is a meaningful amount of code to " +
   "carry and to keep safe against malformed input, for a format nothing here " +
   "reads or writes",
   null, "class XmlElement|class XmlDocument"],
  ["core", "GZIPCompressorOutputStream / GZIPDecompressorInputStream",
   "inflate.h: the DEFLATE decompressor, which is what zip.h reads entries " +
   "with. Decompression only, and on purpose -- a plugin READS compressed " +
   "things (a preset pack, an impulse response bundle) and has no reason to " +
   "write one. Half the code, and the half with no attack surface from a file " +
   "somebody downloaded",
   "class Inflate"],
  ["core", "StatisticsAccumulator",
   "deliberately absent: a running mean, variance, min and max. Genuinely " +
   "small and genuinely useful, and the reason it is not here is that nothing " +
   "asks for it -- LoadMeasurer keeps the one running average this SDK needs " +
   "and keeps it the way an audio thread requires, which is without a division " +
   "per block. The test harness measures with explicit arithmetic where it " +
   "measures at all.\n" +
   "      Listed rather than quietly built, because a utility added on the " +
   "grounds that it is small and might be handy is how a framework grows a " +
   "surface nobody uses and everybody has to keep working",
   null, "class StatisticsAccumulator"],
  ["core", "BigInteger",
   "deliberately absent: arbitrary-precision integers, which the reference needs " +
   "because product_unlocking verifies RSA signatures in-process. " +
   "Licensing here is not done that way -- it is a SaaS, the entitlement is " +
   "checked by the service that issued it, and a plugin verifying its own " +
   "licence offline is a plugin whose verification is on the attacker's " +
   "machine.\n" +
   "      No other part of an audio plugin needs numbers wider than 64 bits",
   null, "class BigInteger"],
  ["core", "Expression",
   "out of scope: a parsed arithmetic expression with named symbols, evaluated " +
   "at runtime. Same answer as ComponentBuilder and the relative-positioning " +
   "family, and the same reason -- a generated plugin's arithmetic is code, " +
   "which the compiler checks, and an expression parsed from a string moves " +
   "every mistake in it from build time to load time",
   null, "class Expression"],
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
   "out of scope with one exception that is already here. A plugin does not " +
   "spawn processes, take machine-wide locks, or ask for OS permissions -- and " +
   "the standalone parses its own arguments in standalone.h rather than " +
   "through a command-line framework.\n" +
   "      DynamicLibrary is the exception and it is load-bearing: every " +
   "optional backend is reached by dlopen/LoadLibrary at RUNTIME, which is what " +
   "lets one build run on a machine without WebKitGTK, without X11 or without " +
   "an ASIO driver and say exactly which library was missing rather than " +
   "failing to load at all",
   "dlopen"],
  ["core", "Networking: URL, WebInputStream, StreamingSocket, DatagramSocket, IPAddress, " +
           "MACAddress, NamedPipe",
   "deliberately absent, and this one is a position rather than a scope call. " +
   "An audio plugin that opens a socket is doing something its user did not " +
   "ask for, in a process the user did not start, on a machine that may be in " +
   "a studio with no business talking to the internet. Several well-known " +
   "plugins have been caught at it and the reaction was not forgiving.\n" +
   "      Where a network genuinely is wanted -- checking an entitlement -- it " +
   "belongs to the application around the plugin, which is where this SDK " +
   "leaves it. The OSC support is the one thing here that touches UDP, and it " +
   "is opt-in, local, and asked for by name",
   null, "class StreamingSocket|class DatagramSocket|class WebInputStream|class NamedPipe"],
  ["core", "LocalisedStrings",
   "deliberately absent: the reference's translation table, looked up by the English " +
   "string. A plugin's interface is a handful of parameter names that come " +
   "from the DESCRIPTOR, and a generated plugin can be generated with them in " +
   "any language -- the translation belongs upstream, where the plugin is " +
   "described, not in a lookup table shipped inside it.\n" +
   "      What this SDK does owe is that the interface can DRAW those " +
   "languages, and that is a font and text-shaping question rather than a " +
   "string-table one. See the IME row for the part of it that is a real gap",
   null, "class LocalisedStrings"],
  ["core", "LeakedObjectDetector / UnitTest / UnitTestRunner",
   "sdk/tests: 2600-odd checks in one harness that prints what it measured, " +
   "and the gate runs the whole suite a second time under ASan -- which " +
   "catches leaks, use-after-free and buffer overruns rather than only the " +
   "leaked-instance count the reference's macro tracks.\n" +
   "      the reference ships a test framework because it is a library used by " +
   "applications that need one. This is verified by its own gate, and a second " +
   "framework inside the thing being tested would be a dependency the tests " +
   "share with the code under test",
   null, "class LeakedObjectDetector"],
  ["core", "TimeSliceThread",
   "thread_pool.h: ThreadPool, which runs queued work on a small fixed set of " +
   "threads. the reference's TimeSliceThread is the cooperative version -- clients are " +
   "polled in turn and each returns how long to wait before it wants the next " +
   "slice -- which suits background jobs that must not be interrupted. The " +
   "background work a plugin has is loading a sample, and a pool that runs it " +
   "to completion is the simpler shape",
   "class ThreadPool"],
  ["core", "AndroidDocument / AndroidDocumentInfo / AndroidDocumentIterator",
   "out of scope: Android's scoped storage API, which the reference wraps because it " +
   "targets Android. There is no Android target here -- the same reason " +
   "PushNotifications and ContentSharer are out",
   null, "class AndroidDocument"],
  ["core", "TextDiff",
   "out of scope: a minimal edit script between two strings, which the reference uses " +
   "to send incremental ValueTree changes over a wire. There is no ValueTree " +
   "here and nothing sends text over a wire",
   null, "class TextDiff"],
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
   "stroke -- because the parse FLATTENS groups and their transforms into " +
   "absolute paths at load. The reference keeps the tree so a drawable can be " +
   "manipulated afterwards: a composite reparented, a path's points animated, " +
   "a rectangle's corner radius bound to something.\n" +
   "      Flattening is the right trade for the use this has. An editor " +
   "redraws thirty times a second and a logo must not be re-walked on any of " +
   "them; a plugin that wants a shape to move draws it itself with the same " +
   "Path API the parser produced. The cost is that an SVG cannot be edited " +
   "after loading, which nothing here does",
   "struct SvgShape"],
  ["graphics", "DrawableText",
   "deliberately absent, and svg.h has said so since it was written: text in " +
   "an SVG is not rendered. It would need font matching against whatever " +
   "typefaces the machine happens to have, and an SVG whose text depends on " +
   "that is one that looks different on every machine -- which is the opposite " +
   "of what a logo is for.\n" +
   "      The answer for anybody exporting art is to convert text to paths " +
   "before export, which every drawing tool does on request and which makes " +
   "the file say exactly what it means. Text that must be TEXT -- a label, a " +
   "readout -- goes through Font and Label, where the typeface is chosen at " +
   "runtime deliberately",
   null, "class DrawableText|renderSvgText"],
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
   "deliberately absent: OpenType feature tags -- ligatures, tabular figures, " +
   "stylistic sets -- and variable-font axes. gfx/truetype.h reads glyf and " +
   "cmap and kerns from kern/GPOS, which is what drawing a label needs.\n" +
   "      One of these has a real argument behind it and is worth naming: " +
   "TABULAR FIGURES stop a numeric readout from jittering as its digits " +
   "change, which is exactly what a plugin's value boxes do while a knob " +
   "moves. It is not free -- it means reading GSUB and applying a feature " +
   "substitution -- and the jitter is currently avoided by giving the readout " +
   "a fixed width instead, which solves the visible problem without a shaping " +
   "engine",
   null, "FontFeatureSetting|FontVariableSetting|hb_feature"],
  ["graphics", "ColourLayer / GlyphLayer / ImageLayer",
   "deliberately absent: the reference 8's layered glyph rendering, which is how a " +
   "COLOUR font is drawn -- emoji, and multi-coloured icon fonts, where one " +
   "glyph is several coloured layers stacked. Glyphs here are cached as PATHS " +
   "and filled with one colour, which is what a plugin's labels and numbers " +
   "are.\n" +
   "      An interface that wanted colour emoji would need the whole layered " +
   "path, and an interface that wants a coloured icon uses an SVG, which this " +
   "already draws",
   null, "class GlyphLayer|class ColourLayer|COLR"],
  ["extras", "AudioFormat / AudioFormatManager",
   "audiofile.h: one loadAudioFile that sniffs the header and dispatches. The reference " +
   "has a registry because an application lets the USER add formats and then " +
   "asks which of them can open a file; the set here is fixed at build time -- " +
   "WAV, AIFF, FLAC, MP3, Ogg Vorbis -- so the dispatch is a switch on the " +
   "first four bytes and there is no registry to populate or to get wrong.\n" +
   "      A fixed set is also the honest one for a plugin: what it can open is " +
   "a property of the plugin, not of the machine it is running on, so a sample " +
   "that loads on one user's system loads on every user's",
   "readAudioFile"],
  ["extras", "CoreAudioFormat / WindowsMediaAudioFormat",
   "deliberately absent: the OS's own decoders, which the reference wraps to open " +
   "whatever the platform happens to support -- m4a on a Mac, wma on Windows. " +
   "Every decoder here is our own or a permissively-licensed one compiled IN, " +
   "which is what makes a sample open identically on all three platforms.\n" +
   "      A format that works on the user's Mac and silently fails on their " +
   "collaborator's PC is worse than one that never worked: the project opens, " +
   "the sampler is empty, and nothing says why",
   null, "CoreAudioFormat|WindowsMediaAudioFormat|MFCreateSourceReader"],
  ["extras", "LAMEEncoderAudioFormat",
   "out of scope: the reference shells out to a LAME BINARY the user must supply to " +
   "write MP3. Nothing here writes audio files except the standalone's WAV " +
   "render, and MP3 encoding is not something a plugin does -- the host " +
   "renders the mix. Spawning an external encoder from inside a plugin is also " +
   "the process-spawning objection from the core walk",
   null, "LAMEEncoder|lame_encode"],
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
   "standalone.h: the device callback that fills a block from the DSP. the reference's " +
   "class is the adapter between an AudioSource and an AudioIODeviceCallback, " +
   "which exists because its device layer and its processing layer are " +
   "separate hierarchies. Here the callback IS the DSP call -- the standalone " +
   "prepares once and pushes blocks -- and an adapter between two things that " +
   "are already the same shape would be a layer with nothing in it",
   "SONORE_BUILD_STANDALONE"],
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
   "deliberately absent as separate types, except where they are already " +
   "inside something. A phase accumulator is two lines inside every oscillator " +
   "here and wrapping it would be a class to construct per voice; Polynomial " +
   "and SpecialFunctions (Bessel functions, for window design) are used by " +
   "the reference's FilterDesign, and the filter designs here compute their " +
   "coefficients directly.\n" +
   "      Matrix is the one worth naming: the reference has it for state-space filters " +
   "and for its own ambisonic code. Nothing here does either, and a general " +
   "matrix type in an audio SDK is an invitation to allocate one on the audio " +
   "thread",
   null, "class Polynomial|class SpecialFunctions|class Matrix"],
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
   "deliberately absent: OSC's wildcard matching, where a sender addresses " +
   "/synth/*/cutoff and every matching receiver responds. That is for a " +
   "controller talking to a rack of devices it did not configure. A plugin " +
   "receives OSC that somebody aimed at it, and the address is compared " +
   "exactly -- which is also the version with no surprises about which " +
   "parameter just moved",
   null, "class OSCAddressPattern|matchesPattern"],
  ["osc", "OSCColour",
   "deliberately absent: OSC's 32-bit RGBA argument type, which the reference supports " +
   "because the spec lists it. It is used by lighting desks. Nothing in an " +
   "audio plugin has a colour that another machine sets, and an argument type " +
   "the four common ones do not include is one most receivers reject anyway",
   null, "OSCColour|class Colour32"],
  ["gui", "AnimatorUpdater / VBlankAnimatorUpdater / ValueAnimatorBuilder / " +
          "StaticAnimationLimits",
   "gfx/animator.h: ValueAnimator::tick takes the time and returns whether " +
   "anything moved. The reference separates the UPDATER -- the thing that decides when " +
   "to advance every animation -- because a framework animation may be driven by a " +
   "timer or by the display's vblank, and the animators must not care which.\n" +
   "      There is one driver here and it is the window's own 33 ms tick, " +
   "which the editor already has for following automation. A second driver " +
   "would be a second clock, and the whole design of every timed thing in this " +
   "SDK is that there is one and it is passed in as an argument. See the " +
   "VBlankAttachment row for why the vblank one specifically is absent.\n" +
   "      The builders are the reference 8's fluent construction API over the same " +
   "animations; animate() takes its arguments directly",
   "class ValueAnimator"],
  ["gui", "SpringEasingOptions",
   "deliberately absent: a spring solved from stiffness, damping and mass, " +
   "which is a physical model rather than a curve. CubicBezierEasing covers " +
   "what a designer actually hands over -- every design tool exports " +
   "cubic-bezier -- and Elastic is here for the overshooting case.\n" +
   "      A real spring's value is that its duration falls OUT of the physics " +
   "rather than being set, which matters when an animation is interrupted and " +
   "must continue from wherever it was with its current velocity. Nothing here " +
   "interrupts an animation mid-flight: a section opens or closes, and " +
   "re-triggering cancels and re-aims",
   null, "SpringEasing|stiffness"],
  ["core", "SHA256",
   "hash.h: Sha256, with sha256Hex and sha256File beside it. Named here as " +
   "the reference names it so the walk can find it -- the row that existed described " +
   "the hashing and never used the word",
   "class Sha256"],
  ["core", "MD5 / Whirlpool",
   "deliberately absent. MD5 is broken for anything that matters and its " +
   "remaining honest use is a checksum, which crc32 in zip.h already covers " +
   "where a zip entry needs one. Whirlpool is a fine hash almost nothing " +
   "speaks, so a digest in it is one no other tool can check.\n" +
   "      Offering three hashes invites a caller to pick the wrong one. There " +
   "is one, it is SHA-256, and it is the one every other system in this " +
   "project already speaks",
   null, "class MD5|class Whirlpool"],
  ["core", "BlowFish / RSAKey / Primes",
   "deliberately absent: a block cipher, a public-key implementation, and " +
   "prime generation for it. The reference ships them because product_unlocking " +
   "verifies licence keys in-process -- see the BigInteger row for why " +
   "licensing here is not done that way.\n" +
   "      Hand-rolled cryptography in an audio SDK is also the wrong place for " +
   "it on its own merits: it is the one kind of code where a subtle mistake is " +
   "invisible until somebody exploits it, and where the correct answer is " +
   "always to use the platform's own audited implementation rather than to " +
   "carry a copy",
   null, "class BlowFish|class RSAKey|class Primes"],
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
  ["gui", "OpenGL: OpenGLContext, OpenGLRenderer, OpenGLShaderProgram, OpenGLTexture, " +
          "OpenGLFrameBuffer, OpenGLPixelFormat, OpenGLImageType, OpenGLHelpers, " +
          "OpenGLVersion, OpenGLAppComponent, OpenGLGraphicsContextCustomShader, " +
          "Matrix3D, Quaternion, Vector3D, Draggable3DOrientation",
   "out of scope, and this is the one out-of-scope module whose classes are " +
   "indexed by name -- because \"should this rasterise on the GPU\" is a " +
   "question somebody could reasonably reopen, and it should be reopened " +
   "against the actual list rather than a memory of it.\n" +
   "      The answer today: software rasterisation gives identical pixels on " +
   "every machine, no context to lose when a laptop switches GPU, and nothing " +
   "to fail to initialise in a VM or over remote desktop -- which is where a " +
   "lot of plugin support tickets come from. A plugin editor is knobs and " +
   "meters, and the region-based repaint already keeps that cheap.\n" +
   "      The 3D maths classes come with the module and would go with it. " +
   "WebGL remains available through the webview editor for a plugin that " +
   "genuinely wants a shader",
   null, "class OpenGLContext|OpenGLShaderProgram|opengl"],
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
   "out of scope: a Toolbar in the reference is a bar the USER rearranges, with a " +
   "palette to drag items from and a saved layout string. That is an " +
   "application affordance. A plugin's interface is designed by whoever built " +
   "the plugin and is the same for everybody who loads it -- a user-arranged " +
   "toolbar inside one would be a second layout to persist in the session and " +
   "to migrate whenever the plugin gains a control",
   null, "class Toolbar|ToolbarItemFactory|ToolbarItemPalette"],
  ["gui", "MenuBarComponent / MenuBarModel / BurgerMenuComponent",
   "out of scope: a plugin editor has no menu bar. It is a child window inside " +
   "a host that owns the frame, the title and the menus -- a bar along the top " +
   "of a plugin would be a second menu system inside somebody else's " +
   "application. PopupMenu exists and is what a plugin actually needs: a " +
   "right-click on a control, and the drop-down behind a ComboBox",
   null, "class MenuBarComponent|class BurgerMenuComponent"],
  ["gui", "SidePanel",
   "out of scope: a drawer that slides in from the edge, which is a phone " +
   "pattern the reference carries because it targets iOS and Android. A plugin editor " +
   "is a fixed rectangle on a desktop; ConcertinaPanel is the same idea in the " +
   "shape this actually has",
   null, "class SidePanel"],
  ["gui", "MultiDocumentPanel / MultiDocumentPanelWindow",
   "out of scope: MDI, for an application holding several open documents. A " +
   "plugin instance is one plugin",
   null, "class MultiDocumentPanel"],
  ["gui", "DocumentWindow / ResizableWindow / TopLevelWindow",
   "out of scope as the reference means them, which is desktop application windows with " +
   "title bars, menus and close boxes. The two windows this SDK really has are " +
   "elsewhere and are not these: a plugin editor is a CHILD window the host " +
   "owns, made by the window_*.h peers, and the standalone's own frame is made " +
   "in standalone.h -- where it answers WM_GETMINMAXINFO from the same " +
   "declared limits every format is told",
   null, "class DocumentWindow|class ResizableWindow|class TopLevelWindow"],
  ["gui", "NativeMessageBox / MessageBoxOptions / ScopedMessageBox",
   "deliberately absent, and this one is a rule rather than a scope call. An OS " +
   "modal dialog from inside a plugin blocks the HOST's message loop: the " +
   "transport keeps running, the audio thread keeps calling process, and the " +
   "user cannot reach the DAW to stop it. gfx/alert.h has AlertOverlay and " +
   "CallOutBox, which are drawn inside the editor and block nothing",
   null, "NativeMessageBox|class MessageBoxOptions"],
  ["gui", "ThreadWithProgressWindow",
   "deliberately absent as one object: it is a thread, a modal window and a " +
   "cancel button welded together, and the modal window half is the part a " +
   "plugin must not have (see NativeMessageBox above). The pieces are here -- " +
   "ThreadPool in thread_pool.h, Overlay in gfx/alert.h, ProgressBar in " +
   "gfx/containers.h -- and a sampler loading a large file composes them into " +
   "something that does not freeze the host",
   null, "class ThreadWithProgressWindow"],
  ["gui", "ComponentBuilder",
   "out of scope: builds a component tree from a ValueTree at runtime, which " +
   "is the reference's layout-from-data path. A generated plugin's interface is " +
   "generated as CODE, which is checked by the compiler; describing it as data " +
   "instead would move every mistake from build time to load time",
   null, "class ComponentBuilder"],
  ["gui", "ComponentMovementWatcher",
   "out of scope: notifies when a component's position or its peer changes, " +
   "which the reference needs because it embeds native subviews that have to be moved " +
   "in lockstep. Nothing here embeds one -- the whole tree is drawn into a " +
   "single bitmap by the software rasteriser, so a moved component is drawn in " +
   "its new place and there is nothing to keep in step with it",
   null, "class ComponentMovementWatcher"],
  ["gui", "VBlankAttachment",
   "deliberately absent: repaint driven by the display's refresh. The peers " +
   "tick at 33 ms and repaint only what is dirty, which is what an editor of " +
   "knobs and meters needs. Tying repaint to vblank would matter for something " +
   "animating continuously at the refresh rate, and would mean a per-platform " +
   "display-link on three platforms -- two of which cannot be tested from " +
   "here. Named rather than left out so the trade is somebody's decision and " +
   "not an oversight",
   null, "class VBlankAttachment|CVDisplayLink"],
  ["gui", "AnimatedPosition (momentum scrolling)",
   "deliberately absent: the reference's inertial scroller, with a momentum behaviour " +
   "and a snap-to-page one. Our Viewport scrolls from wheel events, and on " +
   "both platforms this SDK is tested on a precision trackpad already delivers " +
   "those smoothed and decelerating -- so the flywheel would be a second one " +
   "running on top of the OS's. It becomes a real gap the day this targets " +
   "TOUCH, where there is no OS smoothing and a flick has to be integrated by " +
   "the application. Said plainly because that day is a change of answer, not " +
   "a change of effort",
   null, "class AnimatedPosition"],
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

  // ── osc ─────────────────────────────────────────────────────────────
  ["osc", "OSCSender / OSCReceiver", "osc.h: Sender, Receiver, and an Argument that " +
   "carries the four types every OSC implementation agrees on", "class Receiver"],
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
    else if (entry.name.endsWith(".h")) blob += readFileSync(full, "utf8") + "\n";
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
