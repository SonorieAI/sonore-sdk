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
//   };
//
// Everything here is allocation-free and real-time safe: AudioBlock is a view
// over host-owned channel pointers, MidiBuffer is a fixed-capacity inline
// array. The same header compiles under MSVC/clang/gcc AND emscripten, so the
// browser preview and the shipped native plugin run the identical DSP source.
#pragma once
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
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

/** What prepare() learns about the host: rate, worst-case block, channels. */
struct ProcessSpec {
  double sampleRate = 48000.0;
  uint32_t maximumBlockSize = 128;
  uint32_t numChannels = 2;
  /** Is this an OFFLINE render rather than live playback?
   *
   *  When a host bounces, nothing has to keep up with a clock. A DSP that
   *  normally trades quality for CPU: fewer oversampling stages, a cheaper
   *  interpolator, a shorter FFT: can spend what it likes here, and the
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
  AudioBlock(SampleType* const* channels, size_t numChannels, size_t numSamples,
             size_t startOffset = 0)
      : chans_(channels), numChannels_(numChannels), numSamples_(numSamples),
        offset_(startOffset) {}

  /** An EMPTY block: no channels, no samples. What an aux bus the host left
   *  unconnected looks like, so a DSP's ordinary loops simply do nothing
   *  rather than needing a null check. */
  AudioBlock() = default;

  size_t getNumChannels() const { return numChannels_; }
  size_t getNumSamples() const { return numSamples_; }
  SampleType* getChannelPointer(size_t channel) const { return chans_[channel] + offset_; }

  SampleType getSample(size_t channel, size_t i) const { return chans_[channel][i + offset_]; }
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

/** One 3-byte MIDI channel message (note on/off, CC, pitch bend, aftertouch). */

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
  return isChannelMessage(status) || isSystemCommon(status) || isSystemRealtime(status);
}

class MidiMessage {
public:
  MidiMessage() = default;
  MidiMessage(int status, int data1, int data2)
      : status_(status & 0xff), d1_(data1 & 0x7f), d2_(data2 & 0x7f) {}

  static MidiMessage noteOn(int channel, int note, int velocity) {
    return MidiMessage(0x90 | (channel & 0x0f), note, velocity);
  }
  static MidiMessage noteOff(int channel, int note) {
    return MidiMessage(0x80 | (channel & 0x0f), note, 0);
  }
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
  static MidiMessage polyPressure(int channel, int note, int value) {
    return MidiMessage(0xa0 | (channel & 0x0f), note, value);
  }

  bool isNoteOn() const { return (status_ & 0xf0) == 0x90 && d2_ > 0; }
  bool isNoteOff() const {
    return (status_ & 0xf0) == 0x80 || ((status_ & 0xf0) == 0x90 && d2_ == 0);
  }
  bool isController() const { return (status_ & 0xf0) == 0xb0; }
  bool isPitchWheel() const { return (status_ & 0xf0) == 0xe0; }
  bool isChannelPressure() const { return (status_ & 0xf0) == 0xd0; }
  bool isPolyPressure() const { return (status_ & 0xf0) == 0xa0; }

  int getChannel() const { return (status_ & 0x0f) + 1; } // 1-based, MIDI convention
  int getNoteNumber() const { return d1_; }
  int getVelocity() const { return d2_; }
  float getFloatVelocity() const { return (float) d2_ / 127.0f; }
  int getControllerNumber() const { return d1_; }
  int getControllerValue() const { return d2_; }
  int getPitchWheelValue() const { return (d2_ << 7) | d1_; } // 0..16383, 8192 = center
  /** Channel pressure carries its value in the FIRST data byte, unlike poly
   *  pressure which puts the note there and the pressure second. */
  int getChannelPressureValue() const { return d1_; }
  int getPolyPressureValue() const { return d2_; }
  int getRawStatus() const { return status_; }
  int getRawData1() const { return d1_; }
  int getRawData2() const { return d2_; }

  static double getMidiNoteInHertz(int note, double a4Hz = 440.0) {
    return a4Hz * std::pow(2.0, (note - 69) / 12.0);
  }

private:
  int status_ = 0, d1_ = 0, d2_ = 0;
};

/** Fixed-capacity, sample-ordered event list for one processing block.
 *  Iterate with a range-for; each entry carries the message + frame offset. */
class MidiBuffer {
public:
  static constexpr int kMaxEvents = 64;

  struct Entry {
    MidiMessage message;
    int samplePosition = 0;
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
  static constexpr int kSysexCapacity = 2048;
  static constexpr int kMaxSysex = 8;

  struct SysexEntry {
    uint16_t offset = 0;
    uint16_t length = 0;
    int samplePosition = 0;
  };

  void clear() {
    count_ = 0;
    sysexCount_ = 0;
    sysexUsed_ = 0;
  }
  int getNumEvents() const { return count_; }
  /** Empty means NOTHING, SysEx included. A caller testing this before
   *  skipping a block would otherwise skip a MIDI-CI request. */
  bool isEmpty() const { return count_ == 0 && sysexCount_ == 0; }

  /** Insert keeping sample order (stable for equal offsets). Silently drops
   *  events past capacity: a full block's 64 events is beyond any keyboard. */
  void addEvent(const MidiMessage& m, int samplePosition) {
    if (count_ >= kMaxEvents) return;
    int i = count_;
    while (i > 0 && events_[i - 1].samplePosition > samplePosition) {
      events_[i] = events_[i - 1];
      --i;
    }
    events_[i] = {m, samplePosition};
    ++count_;
  }

  const Entry* begin() const { return events_; }
  const Entry* end() const { return events_ + count_; }

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

  int getNumSysex() const { return sysexCount_; }
  const SysexEntry* sysexBegin() const { return sysex_; }
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
  kExprVolume = 0,     // gain, 0..4 (linear, 1 = unity)
  kExprPan = 1,        // 0 left .. 1 right
  kExprTuning = 2,     // SEMITONES away from the key, -120..120
  kExprVibrato = 3,    // 0..1
  kExprExpression = 4, // 0..1
  kExprBrightness = 5, // 0..1
  kExprPressure = 6,   // 0..1
};

/** One block's expression events, sample-ordered like MidiBuffer.
 *
 *  A DSP correlates these with its voices by KEY plus CHANNEL: MPE guarantees
 *  one note per member channel, and CLAP hosts fill both fields even when
 *  they also carry a note id. Matching on the pair is what works everywhere. */
class NoteExpressionBuffer {
public:
  static constexpr int kMaxEvents = 128;

  struct Entry {
    int32_t noteId = -1; // host's note id, or -1 when the format has none
    int16_t key = -1;    // MIDI note number, -1 = every note
    int16_t channel = -1;
    uint8_t expression = kExprTuning;
    float value = 0.0f;
    int samplePosition = 0;
  };

  void clear() { count_ = 0; }
  int getNumEvents() const { return count_; }
  bool isEmpty() const { return count_ == 0; }

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

  const Entry* begin() const { return events_; }
  const Entry* end() const { return events_ + count_; }

private:
  Entry events_[kMaxEvents];
  int count_ = 0;
};

/** Turns MPE's channel-per-note MIDI into expression events.
 *
 *  MPE spells per-note control as ordinary MIDI on a member channel: pitch
 *  bend is that note's tuning, channel pressure is its pressure, CC74 its
 *  brightness. The decoder tracks which note owns which channel, because the
 *  bend arrives with no note number attached -- that mapping IS the protocol.
 *  Pitch bend range defaults to MPE's own ±48 semitones, not a keyboard's ±2. */
/** One Registered or Non-Registered Parameter, once it has been assembled. */
struct RpnMessage {
  int channel = 1;         // 1-based, MIDI convention
  int parameterNumber = 0; // 14-bit
  int value = 0;           // 7-bit, or 14-bit when is14Bit
  bool isNrpn = false;
  bool is14Bit = false;
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
  struct State {
    int paramMsb = -1, paramLsb = -1, valueMsb = -1, valueLsb = -1;
    bool isNrpn = false;
  };
  State channels_[16];
};

/** The registered parameters this SDK acts on. */
enum : int {
  kRpnPitchBendSensitivity = 0,
  kRpnMpeConfiguration = 6, // the MPE Configuration Message
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
  std::string name;
  std::string vendor;
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
  bool hasName = false;
  std::string name;

  /** 8 bits per component. Alpha is normally opaque; a host that sends a
   *  transparent track colour means it, and a plugin that ignores alpha will
   *  paint a colour the user never chose. */
  bool hasColour = false;
  unsigned char red = 0, green = 0, blue = 0, alpha = 255;

  /** WHERE in the session, which is the part that changes a default rather
   *  than a pixel. A host that does not say leaves all three false, and
   *  "probably an ordinary track" is the right thing to assume then. */
  bool isReturnTrack = false;
  bool isBus = false;
  bool isMaster = false;

  /** How many channels the track carries, or -1 when the host did not say.
   *  Zero is a real answer -- a MIDI track has no audio channels -- so it
   *  cannot double as "unknown". */
  int audioChannelCount = -1;

  /** The colour as #RRGGBB, which is what a web UI wants and what every
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

struct MpeZone {
  bool active = false;
  bool isLowerZone = true;
  int masterChannel = 0;     // 0-based
  int numMemberChannels = 0;

  /** Member channels run UP from the master in the lower zone and DOWN from
   *  it in the upper one, which is the part everyone implements backwards
   *  once. */
  bool hasMember(int channel) const {
    if (!active || numMemberChannels <= 0) return false;
    if (isLowerZone) return channel > masterChannel && channel <= masterChannel + numMemberChannels;
    return channel < masterChannel && channel >= masterChannel - numMemberChannels;
  }
  bool isMaster(int channel) const { return active && channel == masterChannel; }
};

class MpeDecoder {
public:
  /** Override the range manually. Whatever a controller announces through
   *  RPN 0 takes precedence from the moment it arrives -- the instrument
   *  knows how it is configured and this does not. */
  void setPitchBendRange(float semitones) { bendRange_ = semitones; }
  float pitchBendRange() const { return bendRange_; }

  /** The zone, if a controller has announced one. Inactive until then, and
   *  while it is inactive every channel is treated independently -- which is
   *  exactly what a plain MIDI keyboard needs and what this did before zones
   *  existed. */
  const MpeZone& lowerZone() const { return lower_; }
  const MpeZone& upperZone() const { return upper_; }

  /** The master channel's own bend range. MPE gives it a different default
   *  from a member channel's: two semitones against forty-eight, because a
   *  global bend is a pitch wheel and a per-note bend is a finger sliding
   *  across a whole keyboard. Sharing one number would make every global bend
   *  twenty-four times too wide. */
  float masterBendRange() const { return masterBendRange_; }

  void reset() {
    for (int i = 0; i < 16; ++i) noteOnChannel_[i] = -1;
    rpn_.reset();
    lower_ = MpeZone();
    upper_ = MpeZone();
  }

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
              if (lower_.isMaster(ch) || upper_.isMaster(ch)) masterBendRange_ = semis;
              else bendRange_ = semis;
            }
          } else if (rpn.parameterNumber == kRpnMpeConfiguration) {
            applyConfiguration(rpn.channel - 1, rpn.is14Bit ? (rpn.value >> 7) : rpn.value);
          }
        }
      }
      const MidiMessage& m = ev.message;
      const int ch = (m.getRawStatus() & 0x0f);
      if (m.isNoteOn()) {
        noteOnChannel_[ch] = m.getNoteNumber();
        continue;
      }
      if (m.isNoteOff()) {
        if (noteOnChannel_[ch] == m.getNoteNumber()) noteOnChannel_[ch] = -1;
        continue;
      }
      // A message on the MASTER channel belongs to every note in the zone, not
      // to one of them. This is what a controller uses for a global bend or a
      // whole-hand press, and it arrives on a channel with no note of its own
      // -- so the old code, which looked up noteOnChannel_ and gave up when it
      // found nothing, discarded it.
      if (lower_.isMaster(ch) || upper_.isMaster(ch)) {
        const MpeZone& zone = lower_.isMaster(ch) ? lower_ : upper_;
        for (int member = 0; member < 16; ++member) {
          if (!zone.hasMember(member) || noteOnChannel_[member] < 0) continue;
          NoteExpressionBuffer::Entry g;
          g.key = (int16_t) noteOnChannel_[member];
          g.channel = (int16_t) member;
          g.samplePosition = ev.samplePosition;
          if (!fillExpression(m, g, masterBendRange_)) break;
          out.addEvent(g);
        }
        continue;
      }

      const int key = noteOnChannel_[ch];
      if (key < 0) continue; // nothing sounding on this channel to express

      NoteExpressionBuffer::Entry e;
      e.key = (int16_t) key;
      e.channel = (int16_t) ch;
      e.samplePosition = ev.samplePosition;
      if (m.isPitchWheel()) {
        e.expression = kExprTuning;
        e.value = ((float) m.getPitchWheelValue() - 8192.0f) / 8192.0f * bendRange_;
      } else if ((m.getRawStatus() & 0xf0) == 0xd0) { // channel pressure
        e.expression = kExprPressure;
        e.value = (float) m.getRawData1() / 127.0f;
      } else if (m.isController() && m.getControllerNumber() == 74) {
        e.expression = kExprBrightness;
        e.value = (float) m.getControllerValue() / 127.0f;
      } else {
        continue;
      }
      out.addEvent(e);
    }
  }

private:
  int noteOnChannel_[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
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
  }

  MpeZone lower_, upper_;
  float masterBendRange_ = 2.0f;
  RpnDetector rpn_;
  /** MPE's default, and only the default. A controller that announces its own
   *  through RPN 0 replaces this the moment it does. */
  float bendRange_ = 48.0f;
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
  kChannelFL = 0,   // front left
  kChannelFR = 1,   // front right
  kChannelFC = 2,   // front centre
  kChannelLFE = 3,  // low frequency effects
  kChannelBL = 4,   // back left
  kChannelBR = 5,   // back right
  kChannelFLC = 6,  // front left of centre
  kChannelFRC = 7,  // front right of centre
  kChannelBC = 8,   // back centre
  kChannelSL = 9,   // side left
  kChannelSR = 10,  // side right
  kChannelTC = 11,  // top centre
  kChannelTFL = 12, // top front left
  kChannelTFC = 13, // top front centre
  kChannelTFR = 14, // top front right
  kChannelTBL = 15, // top back left
  kChannelTBC = 16, // top back centre
  kChannelTBR = 17, // top back right
  kChannelTSL = 18, // top side left
  kChannelTSR = 19, // top side right
  kChannelUnknown = 255,
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
  uint32_t n = 0;
  for (uint8_t role = 0; role < 64 && n < capacity; ++role)
    if (mask & ((uint64_t) 1 << role)) out[n++] = role;
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
  size_t numAux;
  /** The key input, silence-filled when the host routed nothing. Zero width
   *  when the DSP never declared a sidechain. */
  AudioBlock<SampleType>& sidechain;
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
   *  the sidechain, can say so: CLAP calls this audio-ports-activation. The
   *  buffers are still handed over and still zero-filled, so this changes
   *  nothing about correctness; what it buys is the chance to SKIP the work.
   *  A three-way splitter whose Mid and High bands go nowhere can run one
   *  crossover instead of two.
   *
   *  Defaults mean "assume everything is in use", so a DSP that ignores these
   *  behaves exactly as it did before they existed. */
  const bool* auxActive = nullptr;
  bool sidechainActive = true;
  /** Offline render, same meaning as ProcessSpec::offline. Repeated here so a
   *  DSP can react per block rather than only at prepare(). */
  bool offline = false;

  /** Is aux bus `index` being consumed? True when the host never said
   *  otherwise, which is the common case and the safe one. */
  bool isAuxActive(size_t index) const { return !auxActive || index >= numAux || auxActive[index]; }

  ChannelRole roleOf(size_t channel) const {
    if (!channelRoles || channel >= main.getNumChannels()) return kChannelUnknown;
    return (ChannelRole) channelRoles[channel];
  }

  AudioBlock<SampleType>& auxOut(size_t i) const { return aux[i]; }
  size_t getNumSamples() const { return main.getNumSamples(); }
};

using ProcessContext = ProcessContextT<float>;

} // namespace sonore
