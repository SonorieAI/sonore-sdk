// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: core audio types. Our own, dependency-free equivalents of the
// framework types generated DSP code is written against. The authoring contract
// stays what it has always been:
//
//   struct SonoreDsp {
//     void prepare(const sonore::ProcessSpec& spec);
//     void process(sonore::AudioBlock<float>& io, const float* p);            // effect
//     void process(sonore::AudioBlock<float>& io, const float* p,
//                  sonore::MidiBuffer& midi);                                 // instrument
//     void reset();                                       // optional, see below
//   };
//
// prepare() is where buffers are sized, but it is not only called from the
// main thread. A host's "start over" -- CLAP's reset(), an Audio Unit's Reset
// -- comes on the AUDIO thread with the rate and block size unchanged. Give
// the DSP a reset() that forgets what is sounding and keeps what prepare()
// sized, and those resets call it instead. Without one they call prepare()
// again, which must then neither allocate nor free when nothing it sizes from
// has changed. An LV2 host's switch into or out of freewheeling also comes on
// the audio thread, and calls prepare() whether or not there is a reset(),
// because spec.offline is what changed and LV2 offers no other moment to say
// so; the same rule holds there. A DSP taking ProcessContext also has
// ctx.offline, block by block.
//
// Everything here is allocation-free and real-time safe: AudioBlock is a view
// over host-owned channel pointers, MidiBuffer is a fixed-capacity inline
// array. The same header compiles under MSVC/clang/gcc AND emscripten, so the
// browser preview and the shipped native plugin run the identical DSP source.
#pragma once
#include "version.h"
#include <cstddef> // std::size_t: MSVC leaks it via other headers, clang does not
#include <cstdint>
#include <cmath>
#include <cctype>
#include <string>

namespace sonore {

using std::size_t;

// Universal constants live here rather than in dsp.h so every header that needs
// them (transport, effects, the toolkit) can include just this one without a
// dependency cycle.
/** Pi, as a float: half a turn in radians. */
constexpr float kPi = 3.14159265358979323846f;
/** Two pi, as a float: one whole turn in radians, the factor that turns a
 *  frequency in cycles into a phase increment in radians. */
constexpr float kTwoPi = 6.28318530717958647692f;

/** What prepare() learns about the host: rate, worst-case block, channels. */
struct ProcessSpec {
  /** Samples per second, in Hz, of every block until the next prepare(). */
  double sampleRate = 48000.0;
  /** The most frames one process() call will be handed. The plugin wrappers
   *  keep to it -- a longer host block is run as pieces no longer than this,
   *  or in AU refused as the format says -- so a scratch buffer sized from it
   *  in prepare() is big enough. */
  uint32_t maximumBlockSize = 128;
  /** Channels of the main bus, as negotiated with the host: the width of the
   *  AudioBlock that process() receives. */
  uint32_t numChannels = 2;
  /** Is this an OFFLINE render rather than live playback?
   *
   *  When a host bounces, nothing has to keep up with a clock. A DSP that
   *  normally trades quality for CPU (fewer oversampling stages, a cheaper
   *  interpolator, a shorter FFT) can spend what it likes here, and the
   *  result the user keeps is the better one. That is the whole point:
   *  monitoring is a preview, the bounce is the product.
   *
   *  A DSP that ignores this is correct and unchanged; false is the safe
   *  answer and the one every host gets by default.
   *
   *  The mode can change WITHOUT the session restarting, so the wrappers call
   *  prepare() again when it does, which is also what makes this reachable
   *  from the simple process() signatures, which never see a
   *  ProcessContext. */
  bool offline = false;
};

/** A non-owning view over planar channel data. The host owns the buffers; the
 *  DSP reads and writes through the channel pointers in place. */
template <typename SampleType>
class AudioBlock {
public:
  /** A view of `numChannels` channels of `numSamples` samples each, starting
   *  `startOffset` samples into every one of the `channels` buffers. Nothing
   *  is copied: the array of channel pointers, as well as the samples it
   *  points at, must outlive the block, and each buffer must hold at least
   *  `startOffset + numSamples` samples. */
  AudioBlock(SampleType* const* channels, size_t numChannels, size_t numSamples,
             size_t startOffset = 0)
      : chans_(channels), numChannels_(numChannels), numSamples_(numSamples),
        offset_(startOffset) {}

  /** An EMPTY block: no channels, no samples. What an aux bus the host left
   *  unconnected looks like, so a DSP's ordinary loops simply do nothing
   *  rather than needing a null check. */
  AudioBlock() = default;

  /** How many channels the view covers; 0 for an empty block. */
  size_t getNumChannels() const { return numChannels_; }
  /** How many samples, per channel, the view covers; 0 for an empty block. */
  size_t getNumSamples() const { return numSamples_; }
  /** The first sample of this view in `channel` (0-based), for a loop that
   *  works on the raw array; getNumSamples() samples follow it. Not
   *  bounds-checked: `channel` must be below getNumChannels(). */
  SampleType* getChannelPointer(size_t channel) const { return chans_[channel] + offset_; }

  /** Sample `i` of `channel`, both 0-based and counted from the start of this
   *  view. Not bounds-checked. */
  SampleType getSample(size_t channel, size_t i) const { return chans_[channel][i + offset_]; }
  /** Write sample `i` of `channel`, both 0-based and counted from the start of
   *  this view. Not bounds-checked. Const because the view does not change;
   *  the samples it points at are not const. */
  void setSample(size_t channel, size_t i, SampleType v) const {
    chans_[channel][i + offset_] = v;
  }

  /** Multiply every sample by a constant gain. */
  void multiplyBy(SampleType gain) const {
    for (size_t c = 0; c < numChannels_; ++c)
      for (size_t i = 0; i < numSamples_; ++i) chans_[c][i + offset_] *= gain;
  }
  /** Zero the block. */
  void clear() const {
    for (size_t c = 0; c < numChannels_; ++c)
      for (size_t i = 0; i < numSamples_; ++i) chans_[c][i + offset_] = SampleType(0);
  }

  /** Fill with a constant. */
  void fill(SampleType value) const {
    for (size_t c = 0; c < numChannels_; ++c)
      for (size_t i = 0; i < numSamples_; ++i) chans_[c][i + offset_] = value;
  }

  /** A gain that SLIDES across the block. Applying a changed gain as a step
   *  is what makes an automated fader click; a ramp is the one-line fix, and
   *  every DSP that lacks it writes this loop itself. */
  void applyGainRamp(SampleType startGain, SampleType endGain) const {
    if (numSamples_ == 0) return;
    const SampleType step =
        (endGain - startGain) / (SampleType) (numSamples_ > 1 ? numSamples_ - 1 : 1);
    for (size_t c = 0; c < numChannels_; ++c) {
      SampleType g = startGain;
      for (size_t i = 0; i < numSamples_; ++i) {
        chans_[c][i + offset_] *= g;
        g += step;
      }
    }
  }

  /** Copy another block over this one, channel for channel. Anything the
   *  source does not cover is left alone rather than zeroed: a caller mixing
   *  a mono source into a stereo block means to keep the other channel. */
  void copyFrom(const AudioBlock& source) const {
    const size_t channels = numChannels_ < source.numChannels_ ? numChannels_ : source.numChannels_;
    const size_t samples = numSamples_ < source.numSamples_ ? numSamples_ : source.numSamples_;
    for (size_t c = 0; c < channels; ++c)
      for (size_t i = 0; i < samples; ++i)
        chans_[c][i + offset_] = source.chans_[c][i + source.offset_];
  }

  /** Mix another block in, optionally scaled. */
  void addFrom(const AudioBlock& source, SampleType gain = SampleType(1)) const {
    const size_t channels = numChannels_ < source.numChannels_ ? numChannels_ : source.numChannels_;
    const size_t samples = numSamples_ < source.numSamples_ ? numSamples_ : source.numSamples_;
    for (size_t c = 0; c < channels; ++c)
      for (size_t i = 0; i < samples; ++i)
        chans_[c][i + offset_] += source.chans_[c][i + source.offset_] * gain;
  }

  /** Loudest absolute sample anywhere in the block. */
  SampleType getMagnitude() const {
    SampleType peak = SampleType(0);
    for (size_t c = 0; c < numChannels_; ++c)
      for (size_t i = 0; i < numSamples_; ++i) {
        const SampleType v = chans_[c][i + offset_];
        const SampleType a = v < SampleType(0) ? -v : v;
        if (a > peak) peak = a;
      }
    return peak;
  }

  /** RMS of one channel. Loudness, not peak: the number a meter should show. */
  double getRmsLevel(size_t channel) const {
    if (channel >= numChannels_ || numSamples_ == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < numSamples_; ++i) {
      const double v = (double) chans_[channel][i + offset_];
      sum += v * v;
    }
    return std::sqrt(sum / (double) numSamples_);
  }

  /** A view of a slice, for processing a block in pieces without copying. */
  AudioBlock getSubBlock(size_t startSample, size_t length) const {
    if (startSample >= numSamples_) return AudioBlock();
    const size_t available = numSamples_ - startSample;
    return AudioBlock(chans_, numChannels_, length < available ? length : available,
                      startSample + offset_);
  }

private:
  SampleType* const* chans_ = nullptr;
  size_t numChannels_ = 0;
  size_t numSamples_ = 0;
  /** Where this view starts inside the caller's buffers, so getSubBlock()
   *  shares the same channel pointers instead of copying anything. */
  size_t offset_ = 0;
};

// ── What a status byte is ───────────────────────────────────────────────────
//
// Written ONCE. Every wrapper used to carry its own copy of the same test --
// `status >= 0x80 && status < 0xf0` in the CLAP wrapper, the LV2 wrapper, the
// AU wrapper and both of the standalone's MIDI parsers. Five copies, all
// saying "channel messages only", and none of them saying why.
//
// What they were quietly excluding: MIDI clock, start, stop and continue.
// An arpeggiator or a tempo-synced delay driven from an external sequencer
// could not see any of it, in any format.

/** Anything below 0x80: a payload byte, not a message. Named because the
 *  parsers ask this constantly and a bare `< 0x80` is the kind of comparison
 *  that gets copied with its meaning left behind. */
inline bool isDataByte(int status) { return status < 0x80; }

/** 0x80..0xEF -- note on, note off, control change, pitch bend and the rest.
 *  The low nibble is the channel. */
inline bool isChannelMessage(int status) { return status >= 0x80 && status < 0xF0; }

/** 0xF8..0xFF -- clock, start, continue, stop, active sensing, reset.
 *
 *  One byte each, and they may appear BETWEEN the bytes of another message:
 *  a clock byte arriving in the middle of a note-on is legal MIDI, which is
 *  why the running-status parsers handle them separately rather than as part
 *  of the message they interrupt. */
inline bool isSystemRealtime(int status) { return status >= 0xF8; }

/** 0xF1..0xF7 -- song position, song select, MTC quarter frame, tune request,
 *  and the SysEx terminator. */
inline bool isSystemCommon(int status) { return status >= 0xF1 && status < 0xF8; }

/** 0xF0 -- the start of a System Exclusive message.
 *
 *  Carried on its OWN channel, not through MidiBuffer's fixed three-byte
 *  events: a SysEx message is arbitrarily long, and pretending otherwise is
 *  how you get a truncated one that looks valid. See the sysex gap named in
 *  scripts/feature-map.mjs. */
inline bool isSysexStart(int status) { return status == 0xF0; }

inline int shortMessageLength(int status);
/**
 * Whether a three-byte MIDI event should be handed to a DSP.
 *
 * The ONE policy, so a message a plugin receives over CLAP is a message it
 * receives over LV2 and over a MIDI cable.
 *
 * SysEx is excluded because it does not fit -- not because it is unwanted.
 * Everything else on the wire goes through, including the realtime bytes that
 * carry tempo. A DSP that does not care about clock simply never looks at it;
 * a DSP that does could not previously find out.
 */
inline bool deliverableToDsp(int status) {
  // A message with a length: a channel message, a defined system common
  // one, or a realtime byte. isSystemCommon() alone let through the
  // undefined 0xF4 and 0xF5 and a stray 0xF7 terminator, which reached a
  // DSP as [F4 xx yy] events made of whatever bytes followed.
  return shortMessageLength(status) > 0;
}

/**
 * How many bytes a short message occupies on the wire, status included: 3, 2
 * or 1. Zero for anything that is not a short message -- SysEx (0xF0/0xF7),
 * a bare data byte, the undefined 0xF4/0xF5.
 *
 * Every output that writes bytes rather than a packed word needs exactly
 * this, and getting it wrong does not fail loudly: a clock byte sent as
 * F8 00 00 is read by a receiver using running status as the clock and then
 * one more event made of the two zeros.
 */
inline int shortMessageLength(int status) {
  if (status >= 0xF8) return 1; // realtime: clock, start, continue, stop, sensing, reset
  switch (status) {
    case 0xF1: return 2;        // MTC quarter frame
    case 0xF2: return 3;        // song position pointer
    case 0xF3: return 2;        // song select
    case 0xF6: return 1;        // tune request
    default: break;
  }
  if (status >= 0xF0 || status < 0x80) return 0;
  const int kind = status & 0xF0;
  return (kind == 0xC0 || kind == 0xD0) ? 2 : 3;
}

/**
 * One short MIDI message -- a status byte and up to two data bytes -- as a DSP
 * reads it out of a MidiBuffer.
 *
 * Mirrors the reference's MidiMessage for the channel messages a plugin acts
 * on, and only those: SysEx travels beside these in MidiBuffer's arena, never
 * inside one. A plain value of three bytes, free to make and copy on the
 * audio thread.
 *
 * The channel numbering is not symmetric, and the README says so as well: the
 * factories (noteOn, controlChange and the rest) take a channel from 0 to 15,
 * while getChannel() answers 1 to 16, as MIDI is written --
 * `noteOn(m.getChannel() - 1, ...)` is the round trip. Data bytes are masked
 * to 7 bits on the way in, so an out-of-range note number wraps instead of
 * spilling into the status byte. A note-on with velocity 0 is a note-off, to
 * isNoteOff() as on the wire.
 */
class MidiMessage {
public:
  /** Three zero bytes. Status 0 is not a message, and every type test below
   *  (isNoteOn() and the rest) answers false for it. */
  MidiMessage() = default;
  /** From raw bytes: the status byte (channel in its low nibble) and two data
   *  bytes. The status is kept to 8 bits and each data byte to 7; nothing
   *  checks that the status is one a DSP would be handed (see
   *  deliverableToDsp()). Pass 0 for a data byte the message does not use. */
  MidiMessage(int status, int data1, int data2)
      : status_((uint8_t) (status & 0xff)), d1_((uint8_t) (data1 & 0x7f)),
        d2_((uint8_t) (data2 & 0x7f)) {}

  /** A note-on. `channel` is 0 to 15, `note` and `velocity` 0 to 127. A
   *  velocity of 0 makes it a note-off, as isNoteOff() and the wire both
   *  read it. */
  static MidiMessage noteOn(int channel, int note, int velocity) {
    return MidiMessage(0x90 | (channel & 0x0f), note, velocity);
  }
  /** A note-off (0x80) with release velocity 0. `channel` is 0 to 15, `note`
   *  0 to 127. */
  static MidiMessage noteOff(int channel, int note) {
    return MidiMessage(0x80 | (channel & 0x0f), note, 0);
  }
  /** A control change. `channel` is 0 to 15; `controller` and `value` are 0
   *  to 127. Controllers 120 to 127 are the channel mode messages (all sound
   *  off, all notes off and the rest). */
  static MidiMessage controlChange(int channel, int controller, int value) {
    return MidiMessage(0xb0 | (channel & 0x0f), controller, value);
  }
  /** `value14` is 0..16383 with 8192 at centre, which is how the wire format
   *  carries it -- split across two 7-bit bytes, LSB first. */
  static MidiMessage pitchBend(int channel, int value14) {
    const int v = value14 < 0 ? 0 : (value14 > 16383 ? 16383 : value14);
    return MidiMessage(0xe0 | (channel & 0x0f), v & 0x7f, (v >> 7) & 0x7f);
  }
  /** Channel pressure, one byte. Not to be confused with polyPressure, which
   *  is per-NOTE and is what a poly-aftertouch keyboard sends. */
  static MidiMessage channelPressure(int channel, int value) {
    return MidiMessage(0xd0 | (channel & 0x0f), value, 0);
  }
  /** Polyphonic (per-note) pressure. `channel` is 0 to 15; `note` and
   *  `value` are 0 to 127. */
  static MidiMessage polyPressure(int channel, int note, int value) {
    return MidiMessage(0xa0 | (channel & 0x0f), note, value);
  }

  /** A note-on with a velocity above 0. A note-on at velocity 0 is a
   *  note-off, and answers false here. */
  bool isNoteOn() const { return (status_ & 0xf0) == 0x90 && d2_ > 0; }
  /** A note-off, or a note-on with velocity 0, which means the same. */
  bool isNoteOff() const {
    return (status_ & 0xf0) == 0x80 || ((status_ & 0xf0) == 0x90 && d2_ == 0);
  }
  /** A control change (0xB0), the channel mode messages 120 to 127
   *  included. */
  bool isController() const { return (status_ & 0xf0) == 0xb0; }
  /** A pitch bend (0xE0); read it with getPitchWheelValue(). */
  bool isPitchWheel() const { return (status_ & 0xf0) == 0xe0; }
  /** Channel pressure (0xD0); read it with getChannelPressureValue(). */
  bool isChannelPressure() const { return (status_ & 0xf0) == 0xd0; }
  /** A program change (0xC0); read it with getProgramChangeNumber(). */
  bool isProgramChange() const { return (status_ & 0xf0) == 0xc0; }
  /** The program of a program change, 0 to 127 as it is on the wire.
   *  Meaningless for any other message. */
  int getProgramChangeNumber() const { return d1_; }
  /** Polyphonic pressure (0xA0): the note in getNoteNumber(), the pressure
   *  in getPolyPressureValue(). */
  bool isPolyPressure() const { return (status_ & 0xf0) == 0xa0; }

  /** The channel, 1 to 16 as MIDI is written -- one more than the 0 to 15
   *  the factories take. Meaningless for a system message. */
  int getChannel() const { return (status_ & 0x0f) + 1; } // 1-based, MIDI convention
  /** The note number, 0 to 127, of a note-on, note-off or poly pressure. */
  int getNoteNumber() const { return d1_; }
  /** The velocity of a note-on or note-off, 0 to 127. */
  int getVelocity() const { return d2_; }
  /** The velocity as 0 to 1: getVelocity() divided by 127. */
  float getFloatVelocity() const { return (float) d2_ / 127.0f; }
  /** The controller number of a control change, 0 to 127. */
  int getControllerNumber() const { return d1_; }
  /** The value of a control change, 0 to 127. */
  int getControllerValue() const { return d2_; }
  /** The bend of a pitch bend, 0 to 16383 with 8192 at centre: both data
   *  bytes, LSB first on the wire, joined back into 14 bits. */
  int getPitchWheelValue() const { return (d2_ << 7) | d1_; } // 0..16383, 8192 = center
  /** Channel pressure carries its value in the FIRST data byte, unlike poly
   *  pressure which puts the note there and the pressure second. */
  int getChannelPressureValue() const { return d1_; }
  /** The pressure of a poly pressure message, 0 to 127 (the second data
   *  byte; the note is the first). */
  int getPolyPressureValue() const { return d2_; }
  /** The whole status byte, 0 to 255: the message type in the high nibble
   *  and, for a channel message, the 0-based channel in the low one. */
  int getRawStatus() const { return status_; }
  /** The first data byte, 0 to 127, whatever the message is. */
  int getRawData1() const { return d1_; }
  /** The second data byte, 0 to 127. The factories leave it 0 in a message
   *  that has only one. */
  int getRawData2() const { return d2_; }

  /** The frequency, in Hz, of MIDI note `note` in twelve-tone equal
   *  temperament with note 69 (A4) at `a4Hz`. Any int is accepted -- notes
   *  outside 0 to 127 simply continue the scale -- and nothing allocates. */
  static double getMidiNoteInHertz(int note, double a4Hz = 440.0) {
    return a4Hz * std::pow(2.0, (note - 69) / 12.0);
  }

private:
  // Bytes, which is all the constructor ever lets in: it masks the status to
  // eight bits and the data to seven, and every accessor widens back to the
  // int it returned. As three ints each message was 12 bytes, a MidiBuffer
  // entry 16, and the two buffers every plugin instance holds 37 KB of an
  // effect's 74; as bytes an entry is 8 and the pair 20 KB.
  uint8_t status_ = 0, d1_ = 0, d2_ = 0;
};

/** Fixed-capacity, sample-ordered event list for one processing block.
 *  Iterate with a range-for; each entry carries the message + frame offset. */
class MidiBuffer {
public:
  /** 1024, and it was 64. 64 is beyond any keyboard, and a keyboard is not
   *  what fills a block: an MPE controller sends pitch, pressure and timbre
   *  per finger at hundreds of messages a second each, and a host rendering
   *  offline hands over thousands of frames at once. Past 64 the newest
   *  events were dropped in silence -- note-offs among them, so notes hung.
   *  8 KB per buffer, and every buffer in the SDK is a member or a static. */
  static constexpr int kMaxEvents = 1024;

  /** One event of the block: the message, and the frame it falls on counted
   *  from the start of the block. Shaped like the reference's per-event
   *  metadata (a samplePosition and a getMessage()), so a loop written
   *  against that reads the same here. */
  struct Entry {
    /** The message itself. */
    MidiMessage message;
    /** The frame it falls on, 0-based from the start of the block. The
     *  buffer keeps entries in ascending order of it. */
    int samplePosition = 0;
    /** The message, under the name the reference's metadata gives it. */
    const MidiMessage& getMessage() const { return message; }
  };

  // ── SysEx ──────────────────────────────────────────────────────────────
  //
  // A second channel alongside the three-byte events, and it has to be a
  // second channel: a System Exclusive message is arbitrarily long, and the
  // whole value of the fixed Entry above is that a block's MIDI costs no
  // allocation and no indirection.
  //
  // So the bytes live in an arena inside this object and each message is an
  // offset and a length into it. Still no allocation, still safe on the audio
  // thread, and a DSP that never asks pays nothing but the memory.
  //
  // The capacity comes from what SysEx is actually FOR here. MIDI-CI -- the
  // reason this exists at all, see midi_ci.h -- declares its own maximum
  // message size and defaults it to 512 bytes. Four of those in one block is
  // already unusual traffic, and a universal SysEx past 2 KB is a firmware
  // dump rather than something a plugin answers in real time.
  /** Bytes in the SysEx arena, shared by every message in the block. A
   *  message that does not fit in what is left is refused by addSysex(). */
  static constexpr int kSysexCapacity = 2048;
  /** The most SysEx messages one block holds, however short they are. */
  static constexpr int kMaxSysex = 8;

  /** Where one SysEx message sits in the arena -- an offset and a length into
   *  it -- and the frame it falls on. The entry is not the message: its bytes,
   *  0xF0 and 0xF7 included, come from sysexData(entry), and stay valid until
   *  the buffer is cleared. */
  struct SysexEntry {
    /** Where the message's first byte (its 0xF0) sits in the arena. */
    uint16_t offset = 0;
    /** The message's length in bytes, 0xF0 and 0xF7 included. */
    uint16_t length = 0;
    /** The frame it falls on, 0-based from the start of the block. SysEx
     *  entries keep the order they were added in, not sorted by this. */
    int samplePosition = 0;
  };

  /** Empty the buffer: the events, the SysEx and the dropped count. Every
   *  SysexEntry and sysexData() pointer taken before is stale after it. No
   *  allocation. A DSP need not call it on the buffers a wrapper hands it:
   *  the wrapper clears those between blocks. */
  void clear() {
    count_ = 0;
    dropped_ = 0;
    sysexCount_ = 0;
    sysexUsed_ = 0;
  }
  /** How many three-byte events the block holds, 0 to kMaxEvents. SysEx is
   *  counted apart, by getNumSysex(). */
  int getNumEvents() const { return count_; }
  /** Empty means NOTHING, SysEx included. A caller testing this before
   *  skipping a block would otherwise skip a MIDI-CI request. */
  bool isEmpty() const { return count_ == 0 && sysexCount_ == 0; }
  /** No room for another event. A producer that can wait -- a live input
   *  queue -- stops here and keeps the rest for the next block. */
  bool isFull() const { return count_ >= kMaxEvents; }
  /** Events this block lost to a full buffer since the last clear(). */
  int getNumDropped() const { return dropped_; }

  /**
   * Insert keeping sample order (stable for equal offsets).
   *
   * Past capacity the event is dropped and counted -- with one exception: a
   * NOTE-OFF evicts the latest continuous event (a controller, a bend, a
   * pressure value) to make room. Losing one step of a pitch bend is
   * inaudible; losing a note-off leaves a note sounding until the transport
   * stops, which is the failure a full buffer actually produced.
   */
  void addEvent(const MidiMessage& m, int samplePosition) {
    if (count_ >= kMaxEvents) {
      ++dropped_;
      if (!m.isNoteOff()) return;
      int victim = count_ - 1;
      while (victim >= 0 && (events_[victim].message.isNoteOn() ||
                             events_[victim].message.isNoteOff()))
        --victim;
      if (victim < 0) return; // nothing but notes: the newest one is what gives
      for (int k = victim; k < count_ - 1; ++k) events_[k] = events_[k + 1];
      --count_;
    }
    int i = count_;
    while (i > 0 && events_[i - 1].samplePosition > samplePosition) {
      events_[i] = events_[i - 1];
      --i;
    }
    events_[i] = {m, samplePosition};
    ++count_;
  }

  /** The first event, for a range-for over the block in frame order. The
   *  entries live inside the buffer: adding an event while iterating moves
   *  them under the loop. */
  const Entry* begin() const { return events_; }
  /** One past the last event. */
  const Entry* end() const { return events_ + count_; }

  /** Every event at or past `numSamples` moved onto the block's last frame,
   *  and any before 0 onto its first. The order holds: the buffer is sorted
   *  and the clamp keeps it so. For a format that hears of events before it
   *  knows how long their block is -- AU's MIDI arrives ahead of the render
   *  call -- and so cannot clamp each one as it comes. */
  void clampPositions(int numSamples) {
    const int last = numSamples > 0 ? numSamples - 1 : 0;
    for (int i = 0; i < count_; ++i) {
      int& at = events_[i].samplePosition;
      at = at < 0 ? 0 : (at > last ? last : at);
    }
    for (int i = 0; i < sysexCount_; ++i) {
      int& at = sysex_[i].samplePosition;
      at = at < 0 ? 0 : (at > last ? last : at);
    }
  }

  /**
   * Add one COMPLETE SysEx message, 0xF0 through 0xF7 inclusive.
   *
   * Complete, because reassembling a fragmented stream is the wire path's job
   * and not a DSP's -- see SysexAssembler. A DSP handed half a message would
   * have to keep state across blocks to make sense of it, which is exactly
   * the burden this SDK exists to remove.
   *
   * Returns false when it does not fit, and DROPS rather than truncating. A
   * truncated SysEx is not a smaller message; it is a different one, and it
   * parses as valid right up until it does not.
   */
  bool addSysex(const uint8_t* data, size_t bytes, int samplePosition) {
    if (!data || bytes < 2) return false;
    if (sysexCount_ >= kMaxSysex) return false;
    if (bytes > (size_t) (kSysexCapacity - sysexUsed_)) return false;
    SysexEntry entry;
    entry.offset = (uint16_t) sysexUsed_;
    entry.length = (uint16_t) bytes;
    entry.samplePosition = samplePosition;
    for (size_t i = 0; i < bytes; ++i) sysexBytes_[sysexUsed_ + (int) i] = data[i];
    sysexUsed_ += (int) bytes;
    sysex_[sysexCount_++] = entry;
    return true;
  }

  /** How many SysEx messages the block holds, 0 to kMaxSysex. */
  int getNumSysex() const { return sysexCount_; }
  /** The first SysEx entry, in the order they were added; pass each to
   *  sysexData() for its bytes. */
  const SysexEntry* sysexBegin() const { return sysex_; }
  /** One past the last SysEx entry. */
  const SysexEntry* sysexEnd() const { return sysex_ + sysexCount_; }

  /** The bytes of one message, including its 0xF0 and its 0xF7. */
  const uint8_t* sysexData(const SysexEntry& entry) const { return sysexBytes_ + entry.offset; }

  /** How much of the arena is spoken for. Worth having: a wire path that
   *  starts dropping should be able to say whether it ran out of slots or out
   *  of room. */
  int sysexBytesUsed() const { return sysexUsed_; }

private:
  Entry events_[kMaxEvents];
  int count_ = 0;
  int dropped_ = 0;
  SysexEntry sysex_[kMaxSysex];
  uint8_t sysexBytes_[kSysexCapacity];
  int sysexCount_ = 0, sysexUsed_ = 0;
};

/**
 * Copy every SysEx message from one block's MIDI to another's.
 *
 * For MIDI EFFECTS, which sit in the middle of a chain. One that swallows
 * what it does not recognise breaks everything downstream of it -- the
 * sustain pedal stops working, the mod wheel stops working, an external clock
 * never arrives, and a MIDI-CI negotiation dies at the first plugin in the
 * path.
 *
 * The arpeggiator in this SDK did exactly that until it was measured: it read
 * note-on and note-off and dropped the rest on the floor.
 *
 * Consuming is the exception a DSP makes deliberately, not the default. The
 * arp consumes notes because notes are what it replaces; everything else goes
 * through.
 */
inline void forwardSysex(const MidiBuffer& in, MidiBuffer& out) {
  for (auto it = in.sysexBegin(); it != in.sysexEnd(); ++it)
    out.addSysex(in.sysexData(*it), it->length, it->samplePosition);
}


/** Per-NOTE expression. A keyboard sends one pitch bend for the whole
 *  instrument; an MPE controller (Osmose, LinnStrument, Seaboard) bends,
 *  presses and brightens each finger independently. The ids are CLAP's. */
enum NoteExpression : uint8_t {
  kExprVolume = 0,     ///< gain, 0..4 (linear, 1 = unity)
  kExprPan = 1,        ///< 0 left .. 1 right, 0.5 centre
  kExprTuning = 2,     ///< SEMITONES away from the key, -120..120
  kExprVibrato = 3,    ///< vibrato, 0..1
  kExprExpression = 4, ///< expression, 0..1, as CLAP defines it
  kExprBrightness = 5, ///< timbre, 0..1; what MPE's CC 74 becomes
  kExprPressure = 6,   ///< pressure, 0..1; what MPE's channel pressure becomes
};

/** One block's expression events, sample-ordered like MidiBuffer.
 *
 *  A DSP correlates these with its voices by KEY plus CHANNEL: MPE guarantees
 *  one note per member channel, and CLAP hosts fill both fields even when
 *  they also carry a note id. Matching on the pair is what works everywhere. */
class NoteExpressionBuffer {
public:
  /** The most expression events one block holds; addEvent() drops the
   *  newest past it, without a count. */
  static constexpr int kMaxEvents = 128;

  /** One expression change for one note, at a frame within the block: which
   *  note (host id, key and channel -- match on key and channel, as the class
   *  comment says), which expression (a NoteExpression id), and the new value
   *  in that expression's own range, which is not always 0..1. Past
   *  kMaxEvents in a block, addEvent drops the newest without a count. */
  struct Entry {
    int32_t noteId = -1; ///< host's note id, or -1 when the format has none
    int16_t key = -1;    ///< MIDI note number, -1 = every note
    int16_t channel = -1; ///< MIDI channel 0..15 (0-based), -1 = every channel
    uint8_t expression = kExprTuning; ///< which expression: a NoteExpression id
    float value = 0.0f; ///< the new value, in that expression's own range
    int samplePosition = 0; ///< the frame, 0-based from the start of the block
  };

  /** Empty the buffer. No allocation; the wrappers clear the one they hand a
   *  DSP between blocks. */
  void clear() { count_ = 0; }
  /** How many events the block holds, 0 to kMaxEvents. */
  int getNumEvents() const { return count_; }
  /** No events this block. */
  bool isEmpty() const { return count_ == 0; }

  /** Insert `e` keeping frame order; an event at the same frame as others
   *  goes after them. Past kMaxEvents it is dropped without a count. No
   *  allocation. */
  void addEvent(const Entry& e) {
    if (count_ >= kMaxEvents) return;
    int i = count_;
    while (i > 0 && events_[i - 1].samplePosition > e.samplePosition) {
      events_[i] = events_[i - 1];
      --i;
    }
    events_[i] = e;
    ++count_;
  }

  /** The first event, for a range-for over the block in frame order. */
  const Entry* begin() const { return events_; }
  /** One past the last event. */
  const Entry* end() const { return events_ + count_; }

private:
  Entry events_[kMaxEvents];
  int count_ = 0;
};

/** One Registered or Non-Registered Parameter, once it has been assembled. */
struct RpnMessage {
  int channel = 1;         ///< 1-based, MIDI convention
  int parameterNumber = 0; ///< 14-bit, 0..16383: the selecting MSB times 128 plus its LSB
  int value = 0;           ///< 7-bit, or 14-bit when is14Bit
  bool isNrpn = false;     ///< selected with CC 99/98 (non-registered), not 101/100
  bool is14Bit = false;    ///< `value` includes the data entry LSB (CC 38)
};

/**
 * Assembles RPN and NRPN messages out of the control changes that carry them.
 *
 * There is no such thing as an "RPN message" on the wire. A controller spells
 * one across up to four separate control changes -- 101 and 100 select the
 * parameter (99 and 98 for the non-registered kind), then 6 and 38 carry the
 * value -- and a plugin that only looks at individual CCs sees four unrelated
 * knob movements instead of one instruction.
 *
 * This matters here for one reason above all others: RPN 0 is PITCH BEND
 * SENSITIVITY, and it is how an MPE controller announces the bend range it is
 * using. MpeDecoder assumed 48 semitones, which is the MPE default and is
 * simply wrong for any player who has configured their instrument differently
 * -- a controller set to +/-2 would have been bent twenty-four times too far,
 * silently, with every note.
 *
 * The value is reported TWICE where a controller sends both halves: once when
 * the MSB arrives (7-bit, because that may be all there is) and again when the
 * LSB follows (14-bit). A caller that only wants the precise one checks
 * is14Bit; one that wants to respond immediately uses the first.
 */
class RpnDetector {
public:
  /** Forget every channel's selected parameter and partial value, as if
   *  each had been sent RPN NULL. */
  void reset() {
    for (int i = 0; i < 16; ++i) channels_[i] = State();
  }

  /** Feed one message. Returns true and fills `out` when this control change
   *  completed an RPN or NRPN; false for everything else, including the
   *  selection messages that only set up what comes next. */
  bool parse(const MidiMessage& m, RpnMessage& out) {
    if (!m.isController()) return false;
    const int ch = m.getChannel() - 1;
    if (ch < 0 || ch >= 16) return false;
    State& st = channels_[ch];
    const int cc = m.getControllerNumber();
    const int v = m.getControllerValue();

    switch (cc) {
      case 101: // RPN parameter MSB
        st.paramMsb = v;
        st.isNrpn = false;
        // 101/100 both at 127 is RPN NULL: the standard way to say "I am done
        // addressing a parameter". Without honouring it, a later stray data
        // entry lands on whatever was selected minutes ago.
        if (v == 127 && st.paramLsb == 127) st = State();
        return false;
      case 100: // RPN parameter LSB
        st.paramLsb = v;
        st.isNrpn = false;
        if (v == 127 && st.paramMsb == 127) st = State();
        return false;
      case 99: // NRPN parameter MSB
        st.paramMsb = v;
        st.isNrpn = true;
        return false;
      case 98: // NRPN parameter LSB
        st.paramLsb = v;
        st.isNrpn = true;
        return false;
      case 6: // data entry MSB
        if (st.paramMsb < 0 || st.paramLsb < 0) return false;
        st.valueMsb = v;
        st.valueLsb = -1; // a new MSB starts a new value
        out.channel = ch + 1;
        out.parameterNumber = (st.paramMsb << 7) | st.paramLsb;
        out.value = v;
        out.isNrpn = st.isNrpn;
        out.is14Bit = false;
        return true;
      case 38: // data entry LSB
        if (st.paramMsb < 0 || st.paramLsb < 0 || st.valueMsb < 0) return false;
        st.valueLsb = v;
        out.channel = ch + 1;
        out.parameterNumber = (st.paramMsb << 7) | st.paramLsb;
        out.value = (st.valueMsb << 7) | v;
        out.isNrpn = st.isNrpn;
        out.is14Bit = true;
        return true;
      default:
        return false;
    }
  }

private:
  /** What one channel has been told so far: the parameter selected and the
   *  value's MSB, each -1 until it arrives. Cleared by RPN NULL (101 and 100
   *  both at 127) and by reset(). */
  struct State {
    /** The selected parameter's MSB and LSB and the value's MSB and LSB,
     *  7 bits each, or -1 until they arrive. */
    int paramMsb = -1, paramLsb = -1, valueMsb = -1, valueLsb = -1;
    /** The selection came through CC 99/98 rather than 101/100. */
    bool isNrpn = false;
  };
  State channels_[16];
};

/** The registered parameters this SDK acts on. */
enum : int {
  kRpnPitchBendSensitivity = 0, ///< bend range: semitones in the MSB, cents in the LSB
  kRpnMpeConfiguration = 6, ///< the MPE Configuration Message
};

/**
 * The other direction: an RPN or NRPN, spelled out as the control changes
 * that carry it.
 *
 * RpnDetector has been here since MPE bend range turned out to be readable
 * from the wire. Nothing could WRITE one -- which is the same asymmetry this
 * SDK keeps finding, and it has the same consequence: a plugin that wants to
 * tell a controller its bend range, or send an MPE Configuration Message
 * downstream, had no way to say it.
 *
 * Emits into a MidiBuffer at one offset, in the order a receiver expects:
 * parameter select first (MSB then LSB), then the value. A receiver that sees
 * the data entry before the parameter number applies it to whatever was
 * selected last, which is how a bend range ends up on somebody else's
 * parameter.
 */
inline void sendRpn(MidiBuffer& out, int channel, int parameterNumber, int value,
                    bool isNrpn = false, bool is14Bit = false, int samplePosition = 0) {
  const int ch = (channel < 1 ? 1 : (channel > 16 ? 16 : channel)) - 1;
  const int number = parameterNumber < 0 ? 0 : (parameterNumber > 0x3FFF ? 0x3FFF : parameterNumber);
  // 99/98 for non-registered, 101/100 for registered. Getting these the wrong
  // way round selects a parameter in the other namespace entirely.
  const int msbCc = isNrpn ? 99 : 101;
  const int lsbCc = isNrpn ? 98 : 100;
  out.addEvent(MidiMessage::controlChange(ch, msbCc, (number >> 7) & 0x7f), samplePosition);
  out.addEvent(MidiMessage::controlChange(ch, lsbCc, number & 0x7f), samplePosition);

  if (is14Bit) {
    const int v = value < 0 ? 0 : (value > 0x3FFF ? 0x3FFF : value);
    // Data Entry MSB then LSB. A receiver acts on the MSB, so sending the LSB
    // first would have it act once on a half-formed value and again on the
    // whole one.
    out.addEvent(MidiMessage::controlChange(ch, 6, (v >> 7) & 0x7f), samplePosition);
    out.addEvent(MidiMessage::controlChange(ch, 38, v & 0x7f), samplePosition);
  } else {
    const int v = value < 0 ? 0 : (value > 0x7F ? 0x7F : value);
    out.addEvent(MidiMessage::controlChange(ch, 6, v), samplePosition);
  }
}

/** Announce a pitch bend range, which is RPN 0. The message an instrument
 *  sends to say how far its bend wheel travels -- and the one MpeDecoder
 *  reads on the way in. */
inline void sendPitchBendRange(MidiBuffer& out, int channel, int semitones, int cents = 0,
                               int samplePosition = 0) {
  const int s = semitones < 0 ? 0 : (semitones > 127 ? 127 : semitones);
  const int c = cents < 0 ? 0 : (cents > 99 ? 99 : cents);
  // Semitones in the MSB and CENTS in the LSB -- not a 14-bit number. This is
  // the one RPN whose two data bytes are different units, and treating it as
  // a 14-bit value sends a bend range of 0 semitones and 48 cents where 48
  // semitones was meant.
  sendRpn(out, channel, kRpnPitchBendSensitivity, (s << 7) | c, false, true, samplePosition);
}


/**
 * Which host is running this plugin.
 *
 * Nobody wants to need this and everybody ends up needing it. Hosts have bugs
 * and quirks that a plugin cannot fix and has to work around -- one that
 * calls process() before activate, one whose automation lane cannot show a
 * stepped control, one that reports a block size it then exceeds. A plugin
 * that cannot tell them apart has to apply every workaround to everybody, and
 * each workaround is a small wrongness inflicted on the hosts that never
 * needed it.
 *
 * It is also the honest way to say "this needs a newer host" instead of
 * failing in a way the user has to guess at.
 *
 * Empty strings are a normal answer. LV2 has no way for a host to introduce
 * itself, and a plugin that assumes a name will find one missing.
 */
struct HostInfo {
  /** The host's name as it gives it, UTF-8: from CLAP's host description,
   *  VST3's host application, or the name an Audio Unit host sets in
   *  kAudioUnitProperty_AUHostIdentifier; the standalone app gives its own.
   *  Empty when the host did not say, which in LV2 is always. Match it with
   *  is(), not with ==. */
  std::string name;
  /** The host's vendor, as CLAP reports it. VST3 and Audio Units give only a
   *  name and a version, so this stays empty there. */
  std::string vendor;
  /** The host's version string, free-form, as CLAP reports it; from an
   *  Audio Unit host, its version number as "major.minor.bugfix" (see
   *  detail::auHostVersionText()); empty from VST3. Compare it only for a
   *  host already matched by name. */
  std::string version;

  /** Case-insensitive substring match, because host names are not stable
   *  enough to compare exactly -- "REAPER" has been "REAPER (x64)" and
   *  "Cockos REAPER" in living memory, and a plugin checking for equality
   *  would stop recognising it after an update. */
  bool is(const char* needle) const {
    if (!needle || !*needle || name.empty()) return false;
    std::string lowerName = name, lowerNeedle = needle;
    for (char& c : lowerName) c = (char) std::tolower((unsigned char) c);
    for (char& c : lowerNeedle) c = (char) std::tolower((unsigned char) c);
    return lowerName.find(lowerNeedle) != std::string::npos;
  }
};

namespace detail {

/**
 * An Audio Unit host's version as text, from the UInt32 it sets beside its
 * name in kAudioUnitProperty_AUHostIdentifier (AUHostVersionIdentifier's
 * hostVersion). The field was a NumVersion, and Apple redefined it as a UInt32
 * of the same four bytes, so it reads as one: the major version in the top
 * byte, in BCD; the minor and the bug-fix version one digit each in the next;
 * then the release stage and the pre-release revision, not shown. 0x10410000
 * is "10.4.1". A major byte that is not BCD -- a host that wrote the number in
 * binary -- is read as binary: 0x0A400000 is "10.4.0". 0, no version given,
 * is "".
 */
inline std::string auHostVersionText(uint32_t hostVersion) {
  if (hostVersion == 0) return std::string();
  const unsigned majorByte = (hostVersion >> 24) & 0xFFu;
  const unsigned tens = majorByte >> 4, units = majorByte & 0xFu;
  // Not "major" and "minor": the BSD and glibc headers define those as macros.
  const unsigned majorPart = tens <= 9 && units <= 9 ? tens * 10 + units : majorByte;
  const unsigned minorPart = (hostVersion >> 20) & 0xFu, bugfixPart = (hostVersion >> 16) & 0xFu;
  return std::to_string(majorPart) + "." + std::to_string(minorPart) + "." +
         std::to_string(bugfixPart);
}

/** The HostInfo an Audio Unit host's identifier makes, as the AU wrapper
 *  hands it to setHostInfo(): `utf8Name` (null is no name) and the version
 *  as auHostVersionText() reads it. No vendor: the property carries none. */
inline HostInfo auHostInfo(const char* utf8Name, uint32_t hostVersion) {
  HostInfo info;
  if (utf8Name) info.name = utf8Name;
  info.version = auHostVersionText(hostVersion);
  return info;
}

} // namespace detail

/**
 * The track this instance was dropped onto.
 *
 * The most visible use is cosmetic and the reason people ask for it: a plugin
 * that tints its own face to the colour of the track it sits on, so a rack of
 * eight identical compressors is eight distinguishable compressors. It is
 * worth more than that at CREATION time, though, which is when a host
 * normally sends it -- a reverb landing on a return track wants to start at
 * 100% wet, and one landing on a channel does not, and guessing wrong means
 * every user's first action is to undo the default.
 *
 * Every field is optional, separately. `hasName` and `hasColour` exist
 * because "the host did not say" is a different answer from "" and from
 * black, and a plugin that cannot tell them apart paints itself black on
 * hosts that simply never mention colour.
 *
 * Not every format can carry this. CLAP has an extension for it, VST3 has
 * IInfoListener, and LV2 has nothing at all -- so an LV2 build is told
 * nothing, forever, which is the reason its fields stay false rather than a
 * bug in the host.
 */
struct TrackInfo {
  /** The host named the track. While false, `name` means nothing. */
  bool hasName = false;
  /** The track's name, UTF-8, when hasName. */
  std::string name;

  /** The host gave the track a colour. While false the components below
   *  mean nothing, colourHex() is empty and luminance() is 0. */
  bool hasColour = false;
  ///@{
  /** 8 bits per component. Alpha is normally opaque; a host that sends a
   *  transparent track colour means it, and a plugin that ignores alpha will
   *  paint a colour the user never chose. */
  unsigned char red = 0, green = 0, blue = 0, alpha = 255;
  ///@}

  /** WHERE in the session, which is the part that changes a default rather
   *  than a pixel. A host that does not say leaves all three false, and
   *  "probably an ordinary track" is the right thing to assume then. */
  bool isReturnTrack = false;
  /** The plugin sits on a bus (a group or submix) rather than a track. */
  bool isBus = false;
  /** The plugin sits on the master bus. */
  bool isMaster = false;

  /** How many channels the track carries, or -1 when the host did not say.
   *  Zero is a real answer -- a MIDI track has no audio channels -- so it
   *  cannot double as "unknown". */
  int audioChannelCount = -1;

  /** The colour as `#RRGGBB`, which is what a web UI wants and what every
   *  caller would otherwise write out by hand. Alpha is deliberately left
   *  off: CSS hex with alpha is not understood everywhere, and a UI that
   *  wants it can read the components. */
  std::string colourHex() const {
    if (!hasColour) return std::string();
    static const char* digits = "0123456789abcdef";
    std::string out = "#000000";
    const unsigned char parts[3] = {red, green, blue};
    for (int i = 0; i < 3; ++i) {
      out[1 + i * 2] = digits[(parts[i] >> 4) & 0xF];
      out[2 + i * 2] = digits[parts[i] & 0xF];
    }
    return out;
  }

  /** Perceived brightness, 0 to 1, for the one decision a UI actually has to
   *  make with a track colour: whether text on top of it should be black or
   *  white. Weighted the way the eye responds rather than averaged -- green
   *  reads as far brighter than blue at the same value, and an unweighted
   *  average puts white text on yellow. */
  double luminance() const {
    if (!hasColour) return 0.0;
    return (0.2126 * red + 0.7152 * green + 0.0722 * blue) / 255.0;
  }
};

/**
 * An MPE zone: which channel is the MASTER, and how many members follow it.
 *
 * MPE splits a controller's sixteen channels into a zone with one master
 * channel and a run of member channels. A note lives on a member channel and
 * its bend, pressure and timbre are that finger's alone. The master channel
 * carries the same three things for the ZONE -- the player's whole hand
 * pushing, a global bend, the sustain pedal -- and they apply to every note
 * sounding in it.
 *
 * That distinction is not decoration. A controller sending a global bend on
 * its master channel has no note on that channel, so a decoder that treats
 * every channel alike drops the message on the floor.
 *
 * A zone is announced with the MPE Configuration Message: RPN 6 on channel 1
 * for the lower zone or channel 16 for the upper, valued with the number of
 * member channels. Zero turns the zone off, which is also how a controller
 * says "I am an ordinary MIDI instrument now".
 */
struct MpeZone {
  /** A controller has configured this zone. While false MPE is off for it:
   *  every query below answers false and each channel stands alone. */
  bool active = false;
  /** The lower zone (master channel 1, members above it) rather than the
   *  upper (master channel 16, members below it). */
  bool isLowerZone = true;
  int masterChannel = 0;     ///< 0-based: 0 for the lower zone, 15 for the upper
  /** How many member channels follow the master, 1 to 15 while active; a
   *  controller asking for more is clamped to 15. */
  int numMemberChannels = 0;

  /** Member channels run UP from the master in the lower zone and DOWN from
   *  it in the upper one, which is the part everyone implements backwards
   *  once. */
  bool hasMember(int channel) const {
    if (!active || numMemberChannels <= 0) return false;
    if (isLowerZone) return channel > masterChannel && channel <= masterChannel + numMemberChannels;
    return channel < masterChannel && channel >= masterChannel - numMemberChannels;
  }
  /** Whether `channel` (0-based) is this zone's master channel. False for
   *  every channel while the zone is inactive. */
  bool isMaster(int channel) const { return active && channel == masterChannel; }
};

/** Turns MPE's channel-per-note MIDI into expression events.
 *
 *  MPE spells per-note control as ordinary MIDI on a member channel: pitch
 *  bend is that note's tuning, channel pressure is its pressure, CC74 its
 *  brightness. The decoder tracks which notes sound on which channel, because
 *  the bend arrives with no note number attached -- that mapping IS the
 *  protocol. A channel message reaches EVERY note sounding on its channel:
 *  one under MPE, a whole chord from a plain keyboard.
 *  Pitch bend range defaults to MPE's own ±48 semitones, not a keyboard's ±2. */
class MpeDecoder {
public:
  /** Override the range manually. Whatever a controller announces through
   *  RPN 0 takes precedence from the moment it arrives -- the instrument
   *  knows how it is configured and this does not. */
  void setPitchBendRange(float semitones) { bendRange_ = semitones; }
  /** The per-note (member channel) bend range in semitones: a full bend
   *  either way moves a note this far. 48 until set or announced; a zone
   *  being configured puts it back to 48. */
  float pitchBendRange() const { return bendRange_; }

  /** The bend range for MIDI that is NOT MPE: a channel in no zone -- every
   *  channel, until a controller announces one -- which is what an ordinary
   *  keyboard's pitch wheel is. Two semitones unless set, as MIDI has it and
   *  as nearly every keyboard ships; a range announced through RPN 0 on such
   *  a channel takes over. A plugin that offers its own "bend range" control
   *  passes it here (the wrappers do, for a DSP that declares
   *  pitchBendRange()).
   *
   *  It used to be the per-note 48 -- MPE's default, applied to a keyboard that
   *  never said it was MPE -- so a plain pitch wheel threw a note four octaves
   *  in any plugin that declared MPE support. */
  void setLegacyBendRange(float semitones) { legacyBendRange_ = semitones >= 0.0f ? semitones : 0.0f; }
  float legacyBendRange() const { return legacyBendRange_; }

  /** The zone, if a controller has announced one. Inactive until then, and
   *  while it is inactive every channel is treated independently -- which is
   *  exactly what a plain MIDI keyboard needs and what this did before zones
   *  existed. */
  const MpeZone& lowerZone() const { return lower_; }
  /** The upper zone (master channel 16), on the same terms as
   *  lowerZone(). */
  const MpeZone& upperZone() const { return upper_; }

  /** The master channel's own bend range. MPE gives it a different default
   *  from a member channel's: two semitones against forty-eight, because a
   *  global bend is a pitch wheel and a per-note bend is a finger sliding
   *  across a whole keyboard. Sharing one number would make every global bend
   *  twenty-four times too wide. */
  float masterBendRange() const { return masterBendRange_; }

  /** Forget what is sounding and what the controller said: held notes, each
   *  channel's last bend, pressure and CC 74, half-sent RPNs, and both zones,
   *  which go back to inactive. The two bend ranges are kept. No
   *  allocation. */
  void reset() {
    for (int i = 0; i < 16; ++i) {
      for (bool& key : held_[i]) key = false;
      bend_[i] = 8192;
      pressure_[i] = -1;
      timbre_[i] = -1;
    }
    rpn_.reset();
    lower_ = MpeZone();
    upper_ = MpeZone();
  }

  /**
   * Read one block's MIDI and APPEND the expression it carries to `out`,
   * which is not cleared first.
   *
   * Every message is read, none consumed: `in` is left as it was. Pitch
   * bend becomes kExprTuning in semitones (the note's own bend at
   * pitchBendRange() plus, in a zone, the master's at masterBendRange()),
   * channel pressure kExprPressure and CC 74 kExprBrightness, both 0 to 1.
   * Each goes out once per note it reaches -- every note held on its
   * channel, or on a zone's master channel every note in the zone -- at the
   * message's frame, with key and a 0-based channel filled and noteId -1.
   * A note-on also sends whatever its channel was given before it. RPN 0
   * and the MPE Configuration Message update the bend ranges and the zones
   * as they arrive. No allocation; the wrappers call it on the audio thread
   * for a DSP whose descriptor says supportsMpe.
   */
  void process(const MidiBuffer& in, NoteExpressionBuffer& out) {
    for (const auto& ev : in) {
      // Watch for the instrument announcing its own bend range before doing
      // anything else with this message. Observed, never consumed: the control
      // changes still reach the DSP, which may have its own use for them.
      {
        RpnMessage rpn;
        if (rpn_.parse(ev.getMessage(), rpn) && !rpn.isNrpn) {
          if (rpn.parameterNumber == kRpnPitchBendSensitivity) {
            // MSB is semitones, LSB is cents -- so a 14-bit value carries both
            // and a 7-bit one is whole semitones. A range of zero is what a
            // controller sends to say "do not bend", and is honoured rather
            // than treated as a mistake.
            const float semis =
                rpn.is14Bit ? (float) (rpn.value >> 7) + (float) (rpn.value & 0x7f) / 100.0f
                            : (float) rpn.value;
            if (semis >= 0.0f && semis <= 96.0f) {
              // Which range this sets depends on WHERE it arrived. On a master
              // channel it is the zone's global bend; anywhere else it is the
              // per-note one.
              const int ch = rpn.channel - 1;
              if (lower_.isMaster(ch) || upper_.isMaster(ch)) {
                masterBendRange_ = semis;
              } else {
                bendRange_ = semis;
                rangeAnnounced_ = true; // a plain channel said what it wants, too
              }
            }
          } else if (rpn.parameterNumber == kRpnMpeConfiguration) {
            applyConfiguration(rpn.channel - 1, rpn.is14Bit ? (rpn.value >> 7) : rpn.value);
          }
        }
      }
      const MidiMessage& m = ev.message;
      const int ch = (m.getRawStatus() & 0x0f);
      if (m.isPitchWheel()) bend_[ch] = m.getPitchWheelValue();
      else if ((m.getRawStatus() & 0xf0) == 0xd0) pressure_[ch] = m.getRawData1();
      else if (m.isController() && m.getControllerNumber() == 74) timbre_[ch] = m.getControllerValue();
      if (m.isNoteOn()) {
        held_[ch][m.getNoteNumber()] = true;
        startNote(ch, m.getNoteNumber(), ev.samplePosition, out);
        continue;
      }
      if (m.isNoteOff()) {
        held_[ch][m.getNoteNumber()] = false;
        continue;
      }
      if (m.isController() && (m.getControllerNumber() == 120 || m.getControllerNumber() == 123)) {
        // All sound / all notes off -- what a host sends on stop instead of
        // a note-off per key. On a master channel it ends the whole zone.
        const MpeZone* zone = lower_.isMaster(ch) ? &lower_ : (upper_.isMaster(ch) ? &upper_ : nullptr);
        for (int c = 0; c < 16; ++c)
          if (c == ch || (zone && zone->hasMember(c)))
            for (bool& key : held_[c]) key = false;
        continue;
      }
      // A message on the MASTER channel belongs to every note in the zone, not
      // to one of them. This is what a controller uses for a global bend or a
      // whole-hand press, and it arrives on a channel with no note of its own
      // -- so the old code, which looked up the note on this channel and gave
      // up when it found nothing, discarded it.
      if (lower_.isMaster(ch) || upper_.isMaster(ch)) {
        const MpeZone& zone = lower_.isMaster(ch) ? lower_ : upper_;
        NoteExpressionBuffer::Entry g;
        g.samplePosition = ev.samplePosition;
        if (!fillExpression(m, g, masterBendRange_)) continue;
        for (int member = 0; member < 16; ++member) {
          if (!zone.hasMember(member)) continue;
          for (int key = 0; key < 128; ++key) {
            if (!held_[member][key]) continue;
            g.key = (int16_t) key;
            g.channel = (int16_t) member;
            // A zone bend moves each note from where its own bend has it:
            // MPE ADDS the two. Sent as the zone's bend alone, it replaced
            // the finger's -- a note slid up twelve semitones dropped to the
            // master's one the moment the pitch wheel moved, and jumped back
            // at the finger's next message.
            if (g.expression == kExprTuning) g.value = tuningOf(member);
            out.addEvent(g);
          }
        }
        continue;
      }

      NoteExpressionBuffer::Entry e;
      e.channel = (int16_t) ch;
      e.samplePosition = ev.samplePosition;
      if (m.isPitchWheel()) {
        e.expression = kExprTuning;
        e.value = tuningOf(ch); // its own bend, and its zone's on top
      } else if ((m.getRawStatus() & 0xf0) == 0xd0) { // channel pressure
        e.expression = kExprPressure;
        e.value = (float) m.getRawData1() / 127.0f;
      } else if (m.isController() && m.getControllerNumber() == 74) {
        e.expression = kExprBrightness;
        e.value = (float) m.getControllerValue() / 127.0f;
      } else {
        continue;
      }
      // Every note on the channel. One slot per channel held only the LAST
      // note-on: a plain keyboard's chord bent one note of three under the
      // wheel, and once that key was let go the wheel bent nothing at all
      // while the other two still sounded.
      for (int key = 0; key < 128; ++key) {
        if (!held_[ch][key]) continue;
        e.key = (int16_t) key;
        out.addEvent(e);
      }
    }
  }

private:
  /** Which keys sound on each channel, set by note-on and cleared by
   *  note-off. */
  bool held_[16][128] = {};
  /** Each channel's last pitch bend, as the 14-bit value it arrived as, so
   *  a note's tuning can be told whole whichever half of it moved: a member
   *  channel's at the per-note range, plus its zone master's at the master
   *  range. Kept raw, so a range announced later reads it at that range. */
  int bend_[16] = {8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192,
                   8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192};
  /** Each channel's last pressure and CC 74, or -1 while it has had none. */
  int pressure_[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  int timbre_[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

  /** A note starts with what its channel was given BEFORE it.
   *
   *  MPE has a controller send a new note's bend, pressure and CC 74 ahead
   *  of its note-on -- where the finger landed, how hard, how high on the
   *  key -- and this SDK's own MPE keyboard sends them in that order. With
   *  no note on the channel yet, each was dropped, and the note started at
   *  the DSP's defaults: the initial timbre lost until the finger moved,
   *  a note landed off-centre sounding on the key. They are the channel's
   *  state, so they go out at the note-on's frame, after it, where a DSP
   *  that starts its notes first finds the voice. A bend of nothing is the
   *  note's own default and is not sent. */
  void startNote(int channel, int key, int samplePosition, NoteExpressionBuffer& out) const {
    NoteExpressionBuffer::Entry e;
    e.key = (int16_t) key;
    e.channel = (int16_t) channel;
    e.samplePosition = samplePosition;
    const float tuning = tuningOf(channel);
    if (tuning != 0.0f) {
      e.expression = kExprTuning;
      e.value = tuning;
      out.addEvent(e);
    }
    if (pressure_[channel] >= 0) {
      e.expression = kExprPressure;
      e.value = (float) pressure_[channel] / 127.0f;
      out.addEvent(e);
    }
    if (timbre_[channel] >= 0) {
      e.expression = kExprBrightness;
      e.value = (float) timbre_[channel] / 127.0f;
      out.addEvent(e);
    }
  }

  /** The tuning, in semitones, of a note on `channel`: its own bend, and
   *  the zone's from the master channel when it is a member of one. */
  float tuningOf(int channel) const {
    const MpeZone* zone =
        lower_.hasMember(channel) ? &lower_ : (upper_.hasMember(channel) ? &upper_ : nullptr);
    // A member channel bends at the per-note range; any other channel is a
    // plain one, at the legacy range unless it announced its own.
    const float range = (zone || rangeAnnounced_) ? bendRange_ : legacyBendRange_;
    float semitones = ((float) bend_[channel] - 8192.0f) / 8192.0f * range;
    if (zone)
      semitones += ((float) bend_[zone->masterChannel] - 8192.0f) / 8192.0f * masterBendRange_;
    return semitones;
  }
  /** Turn one message into an expression entry, or say it is not one. Shared
   *  by the per-note and the zone-wide paths so the two cannot interpret the
   *  same message differently. */
  static bool fillExpression(const MidiMessage& m, NoteExpressionBuffer::Entry& e, float range) {
    if (m.isPitchWheel()) {
      e.expression = kExprTuning;
      e.value = ((float) m.getPitchWheelValue() - 8192.0f) / 8192.0f * range;
      return true;
    }
    if ((m.getRawStatus() & 0xf0) == 0xd0) { // channel pressure
      e.expression = kExprPressure;
      e.value = (float) m.getRawData1() / 127.0f;
      return true;
    }
    if (m.isController() && m.getControllerNumber() == 74) {
      e.expression = kExprBrightness;
      e.value = (float) m.getControllerValue() / 127.0f;
      return true;
    }
    return false;
  }

  /** The MPE Configuration Message. Channel 1 configures the lower zone and
   *  channel 16 the upper; anything else is not an MCM and is ignored rather
   *  than guessed at. Zero members turns the zone off, which is how a
   *  controller says it is an ordinary MIDI instrument again. */
  void applyConfiguration(int channel, int members) {
    MpeZone* zone = nullptr;
    if (channel == 0) zone = &lower_;
    else if (channel == 15) zone = &upper_;
    else return;

    if (members <= 0) {
      *zone = MpeZone();
      return;
    }
    zone->active = true;
    zone->isLowerZone = channel == 0;
    zone->masterChannel = channel;
    // Fifteen is the most a zone can have -- the master takes one of the
    // sixteen -- and a controller asking for more is clamped rather than
    // allowed to address a channel that is not there.
    zone->numMemberChannels = members > 15 ? 15 : members;
    // A freshly configured zone starts at MPE's own defaults, so a controller
    // that configures a zone and then bends without stating a range gets what
    // the specification says it should.
    bendRange_ = 48.0f;
    masterBendRange_ = 2.0f;
    rangeAnnounced_ = false;
  }

  MpeZone lower_, upper_;
  float masterBendRange_ = 2.0f;
  RpnDetector rpn_;
  /** MPE's default, and only the default. A controller that announces its own
   *  through RPN 0 replaces this the moment it does. */
  float bendRange_ = 48.0f;
  /** A plain channel's range (see setLegacyBendRange), and whether a range
   *  announced through RPN 0 outside any zone has replaced it. */
  float legacyBendRange_ = 2.0f;
  bool rangeAnnounced_ = false;
};

/** What a channel MEANS. Width alone is not enough: a limiter that ducks the
 *  LFE like a full-range channel, or a panner that treats side as back, is
 *  wrong in a way no amount of testing at stereo reveals.
 *
 *  The numbering is CLAP's, and VST3's speaker bit positions happen to match
 *  it exactly for every value here -- both descend from WAVE_FORMAT_EXTENSIBLE
 *  -- which is what lets the wrappers translate by bit index instead of by
 *  lookup table. That coincidence is asserted in the test suite rather than
 *  trusted. */
enum ChannelRole : uint8_t {
  kChannelFL = 0,   ///< front left
  kChannelFR = 1,   ///< front right
  kChannelFC = 2,   ///< front centre
  kChannelLFE = 3,  ///< low frequency effects
  kChannelBL = 4,   ///< back left
  kChannelBR = 5,   ///< back right
  kChannelFLC = 6,  ///< front left of centre
  kChannelFRC = 7,  ///< front right of centre
  kChannelBC = 8,   ///< back centre
  kChannelSL = 9,   ///< side left
  kChannelSR = 10,  ///< side right
  kChannelTC = 11,  ///< top centre
  kChannelTFL = 12, ///< top front left
  kChannelTFC = 13, ///< top front centre
  kChannelTFR = 14, ///< top front right
  kChannelTBL = 15, ///< top back left
  kChannelTBC = 16, ///< top back centre
  kChannelTBR = 17, ///< top back right
  kChannelTSL = 18, ///< top side left
  kChannelTSR = 19, ///< top side right
  kChannelUnknown = 255, ///< no role known: what roleOf() answers when the host gave none
};

/** The layout the industry means by "N channels", as a role bitmask. Hosts may
 *  propose others; this is only what WE offer when nobody said otherwise. */
inline uint64_t defaultChannelMask(uint32_t width) {
  auto bit = [](ChannelRole r) { return (uint64_t) 1 << (uint64_t) r; };
  switch (width) {
    case 1: return bit(kChannelFC);
    case 2: return bit(kChannelFL) | bit(kChannelFR);
    case 3: return bit(kChannelFL) | bit(kChannelFR) | bit(kChannelFC);
    case 4: return bit(kChannelFL) | bit(kChannelFR) | bit(kChannelBL) | bit(kChannelBR);
    case 5:
      return bit(kChannelFL) | bit(kChannelFR) | bit(kChannelFC) | bit(kChannelBL) |
             bit(kChannelBR);
    case 6: // 5.1
      return bit(kChannelFL) | bit(kChannelFR) | bit(kChannelFC) | bit(kChannelLFE) |
             bit(kChannelBL) | bit(kChannelBR);
    case 7: // 6.1
      return bit(kChannelFL) | bit(kChannelFR) | bit(kChannelFC) | bit(kChannelLFE) |
             bit(kChannelBL) | bit(kChannelBR) | bit(kChannelBC);
    case 8: // 7.1
      return bit(kChannelFL) | bit(kChannelFR) | bit(kChannelFC) | bit(kChannelLFE) |
             bit(kChannelBL) | bit(kChannelBR) | bit(kChannelSL) | bit(kChannelSR);
    default: {
      uint64_t m = 0;
      for (uint32_t i = 0; i < width && i < 20; ++i) m |= (uint64_t) 1 << i;
      return m;
    }
  }
}

/** Channel order follows ASCENDING role: channel 0 is the lowest set bit.
 *  Both CLAP and VST3 order their channels this way, so one rule serves both.
 *  Writes at most `capacity` roles and returns how many it wrote. */
inline uint32_t rolesFromMask(uint64_t mask, uint8_t* out, uint32_t capacity) {
  // The set bits only, lowest first: the wrappers rebuild this every block,
  // and walking all 64 bits was 67 ns of it.
  uint32_t n = 0;
  while (mask && n < capacity) {
    uint8_t role = 0;
    for (uint64_t m = mask; !(m & 1u); m >>= 1) ++role;
    out[n++] = role;
    mask &= mask - 1; // clear the lowest set bit
  }
  return n;
}

/** Everything one process() call is given, in one object.
 *
 *  The simple signatures -- process(block, params), (block, params, midi),
 *  (block, sidechain, params) -- stay the common case and are unchanged. A DSP
 *  that needs EXTRA OUTPUT BUSES (a drum sampler routing pads to their own
 *  channels, a band splitter, a synth emitting stems) declares
 *
 *      void process(sonore::ProcessContext& ctx, const float* params);
 *
 *  instead, and gets the main bus, the aux buses, the sidechain and MIDI
 *  together. One signature rather than eight: aux-outs times MIDI times
 *  sidechain would otherwise be a combinatorial explosion of traits.
 *
 *  Aux buses are ALWAYS present in the object -- a host that left one
 *  unconnected gets a zero-width block rather than a null pointer, so a DSP
 *  writing to it needs no host-specific checks. Check getNumSamples() before
 *  writing if the DSP cares.
 */
template <typename SampleType>
struct ProcessContextT {
  /** The main output. For an effect the input has already been copied in, so
   *  this is read-modify-write exactly like the simple signatures. */
  AudioBlock<SampleType>& main;
  /** Extra output buses in declaration order; count matches the descriptor's
   *  auxOutputs. Their contents are UNDEFINED on entry: a DSP that writes
   *  only some frames must clear the rest. */
  AudioBlock<SampleType>* aux;
  /** How many entries `aux` has: the descriptor's aux output count (at most
   *  32), whether or not the host connected them. */
  size_t numAux;
  /** The key input, silence-filled when the host routed nothing. Zero width
   *  when the DSP never declared a sidechain. */
  AudioBlock<SampleType>& sidechain;
  /** The MIDI the host sent for this block: its events in frame order, and
   *  its SysEx -- what the simple instrument signature is handed. */
  MidiBuffer& midi;
  /** MIDI the plugin EMITS this block, empty on entry. Add events with
   *  frame offsets exactly as they arrive on the input side; the wrapper
   *  hands them to the host in the format's own spelling. Ignored unless the
   *  descriptor says producesMidi. */
  MidiBuffer& midiOut;
  /** What each MAIN-bus channel means, one role per channel. A DSP that
   *  treats channels differently (skip the LFE, steer the sides) reads this
   *  instead of guessing from the count. */
  const uint8_t* channelRoles = nullptr;
  /** Per-note expression for this block: native where the format has it,
   *  MPE-decoded from raw MIDI where it does not. Empty unless the descriptor
   *  opts in with supportsMpe. */
  const NoteExpressionBuffer* expression = nullptr;
  /** Which ports the host has told us it is actually using.
   *
   *  A host that routes nothing out of an aux bus, or has nothing patched into
   *  the sidechain, can say so. CLAP calls this audio-ports-activation. The
   *  buffers are still handed over and still zero-filled, so this changes
   *  nothing about correctness; what it buys is the chance to SKIP the work.
   *  A three-way splitter whose Mid and High bands go nowhere can run one
   *  crossover instead of two.
   *
   *  Defaults mean "assume everything is in use", so a DSP that ignores these
   *  behaves exactly as it did before they existed. */
  const bool* auxActive = nullptr;
  /** False when the host has said nothing is patched into the sidechain,
   *  so the DSP may skip its key detection. The sidechain block is handed
   *  over either way, so ignoring this is always correct. True when the
   *  host never said. */
  bool sidechainActive = true;
  /** Offline render, same meaning as ProcessSpec::offline. Repeated here so a
   *  DSP can react per block rather than only at prepare(). */
  bool offline = false;

  /** Is aux bus `index` being consumed? True when the host never said
   *  otherwise, which is the common case and the safe one. */
  bool isAuxActive(size_t index) const { return !auxActive || index >= numAux || auxActive[index]; }

  /** What main-bus channel `channel` (0-based) means; kChannelUnknown when
   *  the host gave no roles or the channel is past the bus's width. */
  ChannelRole roleOf(size_t channel) const {
    if (!channelRoles || channel >= main.getNumChannels()) return kChannelUnknown;
    return (ChannelRole) channelRoles[channel];
  }

  /** Aux output bus `i`, 0-based in declaration order. Not bounds-checked:
   *  `i` must be below numAux. */
  AudioBlock<SampleType>& auxOut(size_t i) const { return aux[i]; }
  /** Frames in this block: the main bus's length, which every bus shares. */
  size_t getNumSamples() const { return main.getNumSamples(); }
};

/** The ProcessContext of 32-bit float processing, the one a DSP declares in
 *  `process(sonore::ProcessContext& ctx, const float* params)`. */
using ProcessContext = ProcessContextT<float>;

} // namespace sonore
