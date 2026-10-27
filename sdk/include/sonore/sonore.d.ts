// SPDX-License-Identifier: Apache-2.0
//
// The types of `window.sonore`: the bridge a plugin's web page talks to the
// plugin through, in a DAW, in the standalone application and in the studio
// preview alike.
//
// The bridge is a script gui.h generates and injects before the page runs.
// This file describes what that script defines, member for member --
// `npm run verify:bridge-types` checks the two against each other, so a
// member added to one and not the other fails the build rather than a page.
//
// Use it from TypeScript with a reference, or copy it beside your sources:
//
//   /// <reference path="path/to/sonore/sonore.d.ts" />
//   window.addEventListener("sonore:ready", () => {
//     const cutoff = sonore.get("cutoff");
//     sonore.on((values, index, value) => redraw(values));
//   });
//
// Members named with two underscores are the host's way in; a page never
// calls them, and they are not described here.

/** One of the plugin's parameters, as the page sees it. */
interface SonoreParam {
  /** Stable, unique: what a page should key on. */
  readonly id: string;
  /** What a person reads on the faceplate. Lookups by label ignore case,
   *  spaces and punctuation -- but "1.25k" and "12.5k" stay apart. */
  readonly label: string;
  /** "dB", "Hz", "ms", "%" ... or "". */
  readonly unit: string;
  readonly min: number;
  readonly max: number;
  readonly default: number;
  /** How many values a stepped control has; 0 for a continuous one. */
  readonly step: number;
  /** The exponent of the curve between the plain value and 0..1; 1 is
   *  linear. toNormalised and fromNormalised apply it to a continuous
   *  control; a stepped one is spread evenly, as the host spreads it. */
  readonly skew: number;
  /** A value the plugin REPORTS -- a meter, a gain reduction. set, begin and
   *  end ignore it. */
  readonly readOnly: boolean;
  /** The current plain value: kept up to date by the bridge, host
   *  automation included. Write it through set(), not here. */
  readonly value: number;
  /** For a stepped control, the name of each step, or null where a step has
   *  none: "Lowpass", "Bandpass" ... */
  readonly names: ReadonlyArray<string | null>;
  /** A stepped control's positions, in order, as a page draws them: each
   *  step's name, or its value as text where it has none, so a loop over
   *  them never meets a null. Null for a continuous control. The same field
   *  the studio's bridge gives a page. */
  readonly options: ReadonlyArray<string> | null;
  /** 'log' for a skewed control (a log one puts its centre at the
   *  geometric mean), 'linear' otherwise. */
  readonly curve: "log" | "linear";
  /** The parameter's display group, or null at the top level. */
  readonly group: string | null;
}

/** A parameter named by the parameter itself, its index, its id or its
 *  label. A name that matches nothing is ignored by every call. */
type SonoreParamRef = SonoreParam | number | string;

/** Plain values by parameter id. A fresh object on every call: keep it. */
type SonoreValues = Record<string, number>;

/** The audio devices the STANDALONE application can play through. In a
 *  plugin these lists stay empty, always: the host owns the audio device. */
interface SonoreAudioDevices {
  /** Output devices of the system's own audio API, by index. */
  readonly system: ReadonlyArray<string>;
  /** ASIO drivers, by name (Windows). */
  readonly asio: ReadonlyArray<string>;
  /** MIDI inputs, by index. */
  readonly midi: ReadonlyArray<string>;
  /** Capture devices, by index. */
  readonly inputs: ReadonlyArray<string>;
  /** The capture device in use, or -1. */
  readonly currentInput: number;
  /** The built-in test tone is playing in place of the plugin's input. */
  readonly testSignal: boolean;
  /** The MIDI input in use, or -1. */
  readonly currentMidi: number;
  /** What is making sound: "system", "asio", or "" when no output is open --
   *  before anything opens, or after a switch to a device that refused.
   *  sampleRate and bufferFrames are 0 then. */
  readonly current: { readonly backend: string; readonly index: number; readonly name: string };
  readonly sampleRate: number;
  readonly bufferFrames: number;
}

/** window.sonore. */
interface Sonore {
  /** The plugin's name and vendor, as its descriptor gives them. */
  readonly name: string;
  readonly vendor: string;
  /** Every parameter, in the plugin's order: index i is the parameter the
   *  host knows as i. */
  readonly params: ReadonlyArray<SonoreParam>;

  /** A parameter's plain value, or undefined for a name that matches none. */
  get(param: SonoreParamRef): number | undefined;
  /** Set a plain value, held to the parameter's range. Recorded by the host
   *  as automation when it falls between begin() and end(). */
  set(param: SonoreParamRef, value: number): void;
  /** A gesture: begin when a drag starts, end when it ends. A drag between
   *  the two is one undo step and one automation pass in the host. */
  begin(param: SonoreParamRef): void;
  end(param: SonoreParamRef): void;
  /** Every change, from the page or the host: the whole map, then the index
   *  and value of the parameter that moved (-1 and 0 when none did). */
  on(callback: (values: SonoreValues, index: number, value: number) => void): void;

  /** The output level up to thirty times a second while it moves: linear
   *  0..1 and in dB. A silent or steady signal sends nothing new, so this is
   *  not a clock. */
  onLevel(callback: (level: number, db: number) => void): void;
  /** The same, as a VU reading 0..1. */
  onVU(callback: (vu: number) => void): void;
  /** How far the plugin is turning the signal down, in positive decibels
   *  (6 means 6 dB quieter), for a compressor's meter. 0 from a plugin
   *  whose DSP does not declare gainReduction(), which is also what a
   *  compressor reads when it is not compressing. */
  onGR(callback: (db: number) => void): void;
  /** The plugin's spectrum, up to thirty times a second once a page asks:
   *  256 dBFS values on a fixed log grid from 20 Hz to 20 kHz (point i sits
   *  at 20 * 1000^(i / 255) Hz), a full-scale sine reading 0 dBFS. `input` is
   *  the same for what enters the plugin when `{ input: true }` was asked,
   *  else null. Nothing is measured until the first call. */
  onSpectrum(
    callback: (out: ReadonlyArray<number>, input: ReadonlyArray<number> | null, sampleRate: number) => void,
    options?: { input?: boolean },
  ): void;
  /** The latest of each, for a page that draws on its own schedule. */
  readonly level: number;
  readonly db: number;
  readonly vu: number;
  readonly gr: number;
  /** The last spectrum the plugin sent, or null before the first. */
  readonly spectrumFrame: {
    readonly out: ReadonlyArray<number>;
    readonly in: ReadonlyArray<number> | null;
    readonly sr: number;
  } | null;
  /** The plugin's sample rate, once a spectrum has said it; 0 before. */
  readonly sampleRate: number;

  /** On-screen keyboards for an instrument. Velocity 1..127, 100 when not
   *  given. */
  noteOn(note: number, velocity?: number): void;
  noteOff(note: number): void;
  /** The notes sounding, as four 32-bit words: notes 0..31 in the first. */
  readonly notes: ReadonlyArray<number>;
  isNoteDown(note: number): boolean;
  /** Called when the set of sounding notes changes. */
  onNotes(callback: (notes: ReadonlyArray<number>) => void): void;

  /** Whether the page takes the keyboard. Off by default, because a page
   *  that takes it eats the space bar -- the DAW's transport. The bridge
   *  takes it by itself while a text field (an input that takes text, a
   *  textarea, a select, a content-editable element) has focus; call this
   *  for anything else that reads keys, and turn it off after. */
  captureKeys(on: boolean): void;

  /** A native file dialog. The answer arrives through onFile; an empty path
   *  means the user cancelled, which is the common case. */
  chooseFile(purpose?: string, mode?: "open" | "save" | "folder"): void;
  onFile(callback: (purpose: string, path: string) => void): void;

  /** The HOST's menu for a control -- MIDI learn, automation -- at a point
   *  in the plugin window. */
  contextMenu(param: SonoreParamRef, x: number, y: number): void;

  /** The standalone's audio and MIDI devices. See SonoreAudioDevices. */
  readonly audioDevices: SonoreAudioDevices;
  onAudioDevices(callback: (devices: SonoreAudioDevices) => void): void;
  /** A system output or capture device by index, or an ASIO driver by name. */
  selectAudioDevice(backend: "system" | "input", index: number): void;
  selectAudioDevice(backend: "asio", driverName: string): void;
  selectMidiDevice(index: number): void;
  setTestSignal(on: boolean): void;

  /** Factory presets, by name; loading one applies its values in the plugin. */
  readonly presets: ReadonlyArray<string>;
  loadPreset(index: number): void;

  /** A value as the host's own readout shows it: "-6.0 dB", "1.25 kHz",
   *  "Bandpass". The parameter's current value when none is given. */
  format(param: SonoreParamRef, value?: number): string;
  /** The host's automation curve: plain value to 0..1, and back. */
  toNormalised(param: SonoreParamRef, value?: number): number;
  fromNormalised(param: SonoreParamRef, normalised: number): number;

  /** How much of the audio deadline the plugin uses, 0..1 and beyond; the
   *  worst lately; and how many deadlines it missed. */
  readonly cpu: number;
  readonly cpuPeak: number;
  readonly xruns: number;
  onCpu(callback: (load: number, peak: number, xruns: number) => void): void;

  /** The plugin's own state beyond its parameters -- the sample it loaded,
   *  the preset it is on -- as strings. Replaced whole when it changes. */
  readonly state: Readonly<Record<string, string>>;
  /** Called with the state whenever it changes, and at once if it already
   *  arrived before the callback was registered. */
  onState(callback: (state: Readonly<Record<string, string>>) => void): void;

  /** The studio's skin API, in the shape a page written there expects. This
   *  build serves no skins: an empty list, a null lookup, an empty style. */
  readonly accent: string;
  readonly assets: Readonly<Record<string, string>>;
  readonly skins: ReadonlyArray<unknown>;
  skin(name: string): null;
  skinFrame(...args: unknown[]): Record<string, string>;
}

interface Window {
  readonly sonore: Sonore;
}

interface WindowEventMap {
  /** The bridge is up and the plugin has been asked for everything it has
   *  pushed so far. Fired once. */
  "sonore:ready": Event;
}

declare const sonore: Sonore;
