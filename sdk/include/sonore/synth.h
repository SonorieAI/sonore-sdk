// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the synthesiser side, past one oscillator and one envelope.
//
// dsp.h has a polyBLEP Oscillator, an ADSR, a ladder and a VoiceManager, and
// every "make me a synth" reached for those four and stopped, because that is
// where the toolkit stopped. These are the parts the next request needs:
//
//   Wavetable + WavetableOscillator   any single cycle, band-limited by
//                                     mip-mapping, shared between voices
//   UnisonOscillator                  the supersaw: N detuned saws, spread
//   HardSyncOscillator                oscillator sync with the reset BLEPped
//   FmOperator                        a phase-modulation operator, with
//                                     feedback, to stack into algorithms
//   Dahdsr                            delay-attack-hold-decay-sustain-release
//   Portamento                        a glide between notes, in pitch
//   KarplusStrong                     a plucked string
//   ModalResonator                    a struck body: a bank of ringing modes
//   ModRouting + ModMatrix            a modulation matrix: a patch's routes,
//                                     each voice's sources and offsets
//
// Everything is per voice and allocation-free; the Wavetable is the one big
// object and is designed to be held ONCE and read by every voice.
//
// Included by dsp.h. Uses Oscillator, Fft, DelayLine, OnePole, WhiteNoise.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "synth.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include <cstddef>
#include "audio.h"
#include "random.h"

namespace sonore {

// ── Wavetable ────────────────────────────────────────────────────────────────

/**
 * One cycle, stored at every octave of band-limiting.
 *
 * A table read at a high note aliases exactly as a naive saw does: the cycle
 * holds harmonics up to Size/2, and the ones above Nyquist for the note being
 * played fold back down. The fix is the graphics one -- a mip-map. Mip m
 * keeps the harmonics up to (Size/2) / 2^m, built by transforming the cycle,
 * zeroing what is above the limit, and transforming back. The oscillator
 * picks the first mip whose highest harmonic stays below Nyquist for its
 * frequency; the harmonics between that limit and Nyquist are lost, which is
 * the cost of octave steps and why an oscillator that crosses a mip boundary
 * mid-note can sound its top harmonic come and go. Half-octave steps would
 * halve that and double the memory; octaves are the usual compromise.
 *
 * SIZE: Mips x Size floats plus the FFT's tables -- 88 KB at the defaults.
 * Hold one per plugin in static storage (the wasm ABI already does), never
 * one per voice and never on the stack.
 */
template <size_t Size = 2048, int Mips = 10>
class Wavetable {
  static_assert(isPowerOfTwo(Size) && Size >= 64, "Size must be a power of two");
  static_assert(Mips >= 1, "at least one level");

public:
  /** Samples in one cycle: the template's Size, a power of two. */
  static constexpr size_t size() { return Size; }
  /** Band-limited levels kept: mip m holds the harmonics up to
   *  (Size/2) / 2^m. */
  static constexpr int mips() { return Mips; }

  /** Build from one cycle of Size samples. Runs at prepare() time. */
  void build(const float* cycle) {
    for (size_t i = 0; i < Size; ++i) { re_[i] = cycle[i]; im_[i] = 0.0f; }
    fft_.transform(re_, im_, false);
    // Mip 0 keeps everything the table can hold; each level halves the limit.
    for (int m = 0; m < Mips; ++m) {
      const size_t limit = (Size / 2) >> m; // highest harmonic kept
      for (size_t i = 0; i < Size; ++i) { scratchRe_[i] = re_[i]; scratchIm_[i] = im_[i]; }
      for (size_t k = limit + 1; k < Size - limit; ++k) { scratchRe_[k] = 0.0f; scratchIm_[k] = 0.0f; }
      // No DC either: an offset in the cycle becomes a click on every
      // note-on and a thump on every filter.
      scratchRe_[0] = 0.0f;
      scratchIm_[0] = 0.0f;
      fft_.transform(scratchRe_, scratchIm_, true);
      for (size_t i = 0; i < Size; ++i) table_[m][i] = scratchRe_[i];
    }
    built_ = true;
  }

  /** Build additively: `amplitude(h)` for harmonic h = 1.. is the level of
   *  that partial. A saw is 1/h, a square 1/h for odd h and 0 for even. */
  template <typename Fn>
  void buildAdditive(Fn&& amplitude) {
    for (size_t i = 0; i < Size; ++i) { re_[i] = 0.0f; im_[i] = 0.0f; }
    // A sine partial at bin k is -i/2 at k and +i/2 at Size-k (for a real
    // inverse transform that scales by 1/N, times N to land at amplitude 1).
    for (size_t h = 1; h < Size / 2; ++h) {
      const float a = amplitude((int) h);
      if (a == 0.0f) continue;
      im_[h] = -0.5f * a * (float) Size;
      im_[Size - h] = 0.5f * a * (float) Size;
    }
    fft_.transform(re_, im_, true);
    build(re_);
  }

  /** Whether build() or buildAdditive() has run. Until one has, every read
   *  is 0. */
  bool isBuilt() const { return built_; }

  /** Which mip an oscillator at `phaseIncrement` cycles/sample should read:
   *  the first whose top harmonic stays under Nyquist. */
  static int mipFor(float phaseIncrement) {
    if (phaseIncrement <= 0.0f) return 0;
    // Highest harmonic kept by mip m is (Size/2)/2^m; alias-free needs
    // h * increment < 1/2, i.e. 2^m >= Size * increment.
    const float need = (float) Size * phaseIncrement;
    int m = 0;
    float have = 1.0f;
    while (have < need && m < Mips - 1) { have *= 2.0f; ++m; }
    return m;
  }

  /** Linear-interpolated read of one mip at a phase in turns.
   *
   *  The index is WRAPPED, not trusted. A phase a hair below zero -- which
   *  phase modulation produces in ordinary use -- leaves phase - floor(phase)
   *  at 1 - 1e-9, which rounds to exactly 1.0f, so p came out as Size and t[p]
   *  read one past the table: the next mip's first sample, or past the last
   *  mip altogether. The same rounding DelayLine documents having fixed. */
  inline float read(int mip, float phase) const {
    const float p = (phase - std::floor(phase)) * (float) Size;
    const size_t whole = (size_t) p;
    const float frac = p - (float) whole;
    const size_t i0 = whole & (Size - 1);
    const size_t i1 = (i0 + 1) & (Size - 1);
    const float* t = table_[mip];
    return t[i0] + frac * (t[i1] - t[i0]);
  }

private:
  Fft<Size> fft_;
  float re_[Size]{}, im_[Size]{}, scratchRe_[Size]{}, scratchIm_[Size]{};
  float table_[Mips][Size]{};
  bool built_ = false;
};

/** Reads a shared Wavetable at a pitch. One per voice; the table is shared. */
template <typename Table = Wavetable<>>
class WavetableOscillator {
public:
  /** The table to read, not owned: it must outlive every next() that reads
   *  it. Null (the default) makes next() return 0. Changing it keeps the
   *  phase. */
  void setTable(const Table* table) { table_ = table; }
  /** Sample rate in Hz; 1 or less means 48 kHz. Does not recompute the
   *  increment: call setFreq() after it. */
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }
  /** Pitch in Hz, held to 0 .. half the sample rate (NaN reads as 0), and
   *  the mip that keeps it alias-free. Cheap enough to call every sample. */
  inline void setFreq(float hz) {
    inc_ = clampf(hz, 0.0f, sr_ * 0.5f) / sr_;
    mip_ = Table::mipFor(inc_);
  }
  /** Restart at `phase`, in turns (0..1). */
  void reset(float phase = 0.0f) { phase_ = phase; }
  /** Where in the cycle the next sample is read, in turns, 0..1. */
  float phase() const { return phase_; }

  /** The next sample: the chosen mip read at the phase, linearly
   *  interpolated, then the phase advanced. No allocation. */
  inline float next() {
    if (!table_) return 0.0f;
    const float y = table_->read(mip_, phase_);
    phase_ += inc_;
    if (phase_ >= 1.0f) phase_ -= 1.0f;
    return y;
  }

private:
  const Table* table_ = nullptr;
  float sr_ = 48000.0f, inc_ = 0.0f, phase_ = 0.0f;
  int mip_ = 0;
};

// ── Unison ───────────────────────────────────────────────────────────────────

/**
 * N polyBLEP saws (or squares) spread in pitch and in the stereo field: the
 * supersaw. Detune is the total spread in cents, voices placed evenly across
 * it; `spread` pans the outer voices out to the sides. Level is normalised by
 * 1/sqrt(N), which keeps the RMS steady as voices are added -- the voices
 * are detuned, so they add as noise, not as a coherent sum.
 */
template <int MaxVoices = 8>
class UnisonOscillator {
  static_assert(MaxVoices >= 1 && MaxVoices <= 32, "1..32 voices");

public:
  /** What each voice plays. */
  enum class Shape {
    Saw,   ///< polyBLEP sawtooth, the default: the supersaw
    Square ///< polyBLEP square at 50% duty
  };

  /** Spread from the start, as reset() spreads them: only reset() did, and a
   *  fresh supersaw's voices began in phase, 6 dB hot for its first 20 ms. */
  UnisonOscillator() { reset(); }

  /** Sample rate in Hz (1 or less means 48 kHz); retunes every voice. Call
   *  it before the first render(): until it, setVoices(), setDetune() or
   *  setSpread() has run, the pan gains are zero and the output is silent. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    for (int i = 0; i < MaxVoices; ++i) osc_[i].setSampleRate(sr_);
    update();
  }
  /** Saw or square for every voice, from the next sample. */
  void setShape(Shape s) { shape_ = s; }
  /** How many voices sound, clamped to 1..MaxVoices (1 by default). Places
   *  them across the detune and the spread again, so it computes a pow, a
   *  sine and a cosine per voice: a setting, not a per-sample call. */
  void setVoices(int n) { voices_ = n < 1 ? 1 : (n > MaxVoices ? MaxVoices : n); update(); }
  /** Total detune width in cents, outermost voice to outermost voice. */
  void setDetune(float cents) { detune_ = clampf(cents, 0.0f, 200.0f); update(); }
  /** 0 = all centred, 1 = outer voices hard left and right. */
  void setSpread(float s) { spread_ = clampf(s, 0.0f, 1.0f); update(); }
  /** Cheap enough to call every sample (glide, pitch bend): one multiply per
   *  voice. The detune ratios and pan gains depend only on the voice count,
   *  the detune and the spread, and are kept from when those last changed --
   *  recomputed here, they were 21 transcendentals a sample at seven voices,
   *  13x the cost of rendering them. */
  void setFreq(float hz) {
    freq_ = hz;
    for (int i = 0; i < voices_; ++i) osc_[i].setFreq(freq_ * ratio_[i]);
  }
  /** Voices start at distinct phases so they do not stack into one saw for
   *  the first cycle and then drift apart audibly. */
  void reset() {
    for (int i = 0; i < MaxVoices; ++i) osc_[i].reset((float) i / (float) MaxVoices);
  }

  /** One stereo sample: the voices panned and summed, scaled by
   *  1/sqrt(voices). No allocation. */
  inline void render(float& left, float& right) {
    float l = 0.0f, r = 0.0f;
    for (int i = 0; i < voices_; ++i) {
      const float v = shape_ == Shape::Saw ? osc_[i].saw() : osc_[i].square();
      l += v * gainL_[i];
      r += v * gainR_[i];
    }
    left = l * norm_;
    right = r * norm_;
  }

private:
  void update() {
    norm_ = 1.0f / std::sqrt((float) voices_);
    for (int i = 0; i < voices_; ++i) {
      // -1..+1 across the voices; a single voice sits at 0.
      const float pos = voices_ > 1 ? 2.0f * (float) i / (float) (voices_ - 1) - 1.0f : 0.0f;
      const float cents = pos * detune_ * 0.5f;
      ratio_[i] = std::pow(2.0f, cents / 1200.0f);
      osc_[i].setFreq(freq_ * ratio_[i]);
      // Constant-power pan.
      const float pan = pos * spread_;                   // -1..+1
      const float angle = (pan + 1.0f) * 0.25f * kPi;    // 0..pi/2
      gainL_[i] = std::cos(angle);
      gainR_[i] = std::sin(angle);
    }
  }

  Oscillator osc_[MaxVoices];
  float ratio_[MaxVoices]{};
  float gainL_[MaxVoices]{}, gainR_[MaxVoices]{};
  Shape shape_ = Shape::Saw;
  float sr_ = 48000.0f, freq_ = 440.0f, detune_ = 20.0f, spread_ = 0.7f, norm_ = 1.0f;
  int voices_ = 1;
};

// ── Hard sync ────────────────────────────────────────────────────────────────

/**
 * A slave saw reset every time a master cycle completes. The output has the
 * master's pitch and the slave's harmonic shape, and sweeping the slave's
 * ratio is the sound of "sync".
 *
 * The reset is a discontinuity like the saw's own wrap, at a moment that is
 * not a sample boundary, and left alone it aliases as badly as a naive saw.
 * It is BLEPped the same way: the master's phase says when the reset will
 * fall inside the next sample (so the sample before can be corrected without
 * a delay) and how far into the current sample it fell (so the sample after
 * can be), and the residual is scaled by the height of the drop, which is
 * however far up the slave had climbed. The unit test compares against the
 * same oscillator with band limiting turned off.
 */
class HardSyncOscillator {
public:
  /** Sample rate in Hz (1 or less means 48 kHz); re-applies the master
   *  frequency. Call it, or setMasterFreq(), before the first next(): until
   *  then nothing advances and the output is a constant -1. */
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; setMasterFreq(masterHz_); }
  /** The pitch heard, in Hz (220 by default), held to 0 .. half the sample
   *  rate; the slave runs at this times the ratio, held there too. */
  void setMasterFreq(float hz) {
    masterHz_ = clampf(hz, 0.0f, sr_ * 0.5f);
    masterInc_ = masterHz_ / sr_;
    slaveInc_ = clampf(masterHz_ * ratio_, 0.0f, sr_ * 0.5f) / sr_;
  }
  /** Slave frequency as a multiple of the master's. 1..8 is the classic range. */
  void setRatio(float ratio) { ratio_ = ratio < 0.0f ? 0.0f : ratio; setMasterFreq(masterHz_); }
  /** Off = the naive reset, for comparison. On is the default. */
  void setBandLimited(bool on) { blep_ = on; }
  /** A fresh oscillator: the pending sync and wrap corrections too. Left
   *  set, the first samples after a reset were corrected for a reset that
   *  never happened -- up to 0.10 away from a fresh one. */
  void reset() {
    master_ = 0.0f;
    slave_ = 0.0f;
    syncHeight_ = 0.0f;
    afterSync_ = 0.0f;
    afterWrap_ = 0.0f;
    justSynced_ = false;
    justWrapped_ = false;
  }

  /** The next sample of the synced saw, -1..1 before the band-limiting
   *  corrections. No allocation. */
  inline float next() {
    // Will the master wrap inside the NEXT sample? Then the reset is
    // imminent and this sample gets the "before" half of the residual.
    const bool syncImminent = master_ + masterInc_ >= 1.0f;
    const float fSync = syncImminent ? (1.0f - master_) / masterInc_ : 1.0f; // fraction of the step
    const bool wrapImminent = slave_ + slaveInc_ >= 1.0f;
    const float fWrap = wrapImminent ? (1.0f - slave_) / slaveInc_ : 1.0f;

    float y = 2.0f * slave_ - 1.0f;

    if (blep_) {
      if (syncImminent && fSync <= fWrap) {
        // The slave will be at slave_ + fSync*inc when it is cut down to 0:
        // a drop of that height (in a 0..1 phase, i.e. 2x in the -1..1 wave).
        const float height = 2.0f * (slave_ + fSync * slaveInc_);
        const float x = (master_ - 1.0f) / masterInc_; // -1..0: how far before
        y -= 0.5f * height * (x * x + x + x + 1.0f);
      } else if (wrapImminent) {
        const float x = (slave_ - 1.0f) / slaveInc_;
        y -= x * x + x + x + 1.0f; // the saw's own drop is a full 2
      }
      if (justSynced_) {
        const float x = afterSync_; // 0..1: how far after
        y -= 0.5f * syncHeight_ * (x + x - x * x - 1.0f);
      } else if (justWrapped_) {
        const float x = afterWrap_; // 0..1: how far after
        y -= x + x - x * x - 1.0f;
      }
    }

    // Advance, and record what happened for the next sample's "after" half.
    justSynced_ = false;
    justWrapped_ = false;
    master_ += masterInc_;
    if (master_ >= 1.0f) {
      master_ -= 1.0f;
      const float after = master_ / masterInc_;        // fraction since the reset
      const float atSync = slave_ + (1.0f - after) * slaveInc_;
      syncHeight_ = 2.0f * (atSync >= 1.0f ? atSync - 1.0f : atSync);
      slave_ = after * slaveInc_;
      afterSync_ = after;
      justSynced_ = true;
    } else {
      slave_ += slaveInc_;
      if (slave_ >= 1.0f) {
        slave_ -= 1.0f;
        // How far past the wrap, measured in the step that wrapped -- kept
        // now, as afterSync_ is. Worked out on the next sample from the
        // increment in force THEN, a pitch that fell in between (a legato
        // note two octaves down) put it at up to 4 where the residual
        // polynomial holds for 0..1, and the sample came out at 5.7; a
        // ratio or a pitch set to 0 divided by zero and it came out NaN.
        afterWrap_ = slave_ / slaveInc_;
        justWrapped_ = true;
      }
    }
    return y;
  }

private:
  float sr_ = 48000.0f, masterHz_ = 220.0f, ratio_ = 1.5f;
  float masterInc_ = 0.0f, slaveInc_ = 0.0f, master_ = 0.0f, slave_ = 0.0f;
  float syncHeight_ = 0.0f, afterSync_ = 0.0f, afterWrap_ = 0.0f;
  bool blep_ = true, justSynced_ = false, justWrapped_ = false;
};

// ── FM ───────────────────────────────────────────────────────────────────────

/**
 * One phase-modulation operator: a sine whose phase is pushed around by
 * whatever is passed in, in RADIANS, plus its own last output scaled by
 * `feedback`. Stack them as the algorithms do -- modulator.render(0) into
 * carrier.render(index * m) -- and the spectrum follows Bessel: at a
 * modulation index of 2.405 the carrier itself vanishes (J0's first zero),
 * which the unit test asserts, because it is the one FM fact that is easy to
 * check and impossible to fake.
 */
class FmOperator {
public:
  /** Sample rate in Hz (1 or less means 48 kHz); re-applies the frequency.
   *  Call it, or setFreq(), before the first render(): until then the phase
   *  does not move. */
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; setFreq(hz_); }
  /** Frequency in Hz (440 by default), held to 0 .. half the sample rate.
   *  The request is kept and fitted again when the rate changes. */
  void setFreq(float hz) { hz_ = hz; inc_ = clampf(hz, 0.0f, sr_ * 0.5f) / sr_; }
  /** Self-modulation index in radians, 0..1.5. At 1 the loop is Tomisawa's
   *  feedback sawtooth -- the largest index for which sin(theta + beta y) is
   *  still single-valued -- and past it the loop turns chaotic: measured on a
   *  440 Hz operator, the harmonic share of the output is 98% at 1.0, 90% at
   *  1.5, 75% at 2 and 63% at pi, the rest being noise. 1.5 is kept because
   *  the DX7's top feedback level is exactly that gritty and people use it;
   *  the first version allowed pi and called it "a saw". */
  void setFeedback(float radians) { feedback_ = clampf(radians, 0.0f, 1.5f); }
  /** Linear output gain (1 by default), not clamped. A modulator's level is
   *  part of the index it hands the next operator. */
  void setLevel(float level) { level_ = level; }
  /** Restart at `phase`, in turns (0..1), with the feedback history empty. */
  void reset(float phase = 0.0f) { phase_ = phase; last1_ = last2_ = 0.0f; }

  /** The next sample: sin(2 pi phase + phaseModRadians + feedback) times the
   *  level, then the phase advanced. Pass another operator's output, scaled
   *  by the modulation index, as `phaseModRadians`. No allocation. */
  inline float render(float phaseModRadians = 0.0f) {
    // The feedback is the AVERAGE of the last two outputs, which is how the
    // DX7 does it and the reason it has to: one sample of feedback at a high
    // index is a map whose slope exceeds one, and it alternates sign every
    // sample -- a buzz at Nyquist riding the note. Averaging two puts a zero
    // at Nyquist in the loop and the alternation cannot sustain.
    const float fb = feedback_ * 0.5f * (last1_ + last2_);
    const float turns = phase_ + (phaseModRadians + fb) * (1.0f / kTwoPi);
    const float y = fastmath::sinTurns(turns - std::floor(turns));
    last2_ = last1_;
    last1_ = y;
    phase_ += inc_;
    if (phase_ >= 1.0f) phase_ -= 1.0f;
    return y * level_;
  }

private:
  float sr_ = 48000.0f, hz_ = 440.0f, inc_ = 0.0f, phase_ = 0.0f;
  float feedback_ = 0.0f, level_ = 1.0f, last1_ = 0.0f, last2_ = 0.0f;
};

// ── DAHDSR ───────────────────────────────────────────────────────────────────

/**
 * The six-stage envelope: a delay before anything happens, a hold at the
 * peak before the decay, and the four an ADSR has. Same conventions as ADSR:
 * a linear attack, exponential decay and release timed to -60 dB so the
 * numbers on the controls are the numbers a meter reads.
 */
class Dahdsr {
public:
  /** The six settings: delay, attack, hold, decay and release in seconds
   *  (each at least one sample long), sustain a level 0..1. The attack is a
   *  straight ramp to 1 from wherever the level is; decay and release are
   *  the times to fall 60 dB. */
  struct Parameters {
    ///@{
    /** In declaration order: delay, attack, hold, decay and release in
     *  seconds; sustain a level, 0..1 and not clamped. */
    float delay = 0.0f, attack = 0.01f, hold = 0.0f, decay = 0.1f, sustain = 0.8f, release = 0.2f;
    ///@}
  };
  /** Idle, with the default Parameters at 48 kHz. */
  Dahdsr() { updateCoefficients(); }
  /** Sample rate in Hz (1 or less means 48 kHz). The times are converted
   *  at the new rate from the next sample, mid-note too. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    updateCoefficients();
  }
  /** Takes a copy; a sustain that is not finite becomes 0. Applies from the
   *  next sample, to a sounding note too. Computes two exponentials: call
   *  it when a setting changes, not every sample. */
  void setParameters(const Parameters& p) {
    p_ = p;
    // A sustain that is not finite is none, as in ADSR: a NaN level never
    // settles or falls idle, and the voice held NaN until reset().
    if (!std::isfinite(p_.sustain)) p_.sustain = 0.0f;
    updateCoefficients();
  }
  /** Start again from the delay stage. The level is kept, so a retrigger
   *  ramps up from where the envelope was instead of stepping to 0. */
  void noteOn() { stage_ = Delay; counter_ = 0; }
  /** Release from the current level, whatever the stage; idle stays idle. */
  void noteOff() { if (stage_ != Idle) stage_ = Release; }
  /** False once the release has fallen below 1e-4 (-80 dB), or after
   *  reset(): the voice can be freed. */
  bool isActive() const { return stage_ != Idle; }
  /** Idle at 0 at once, with no release. */
  void reset() { stage_ = Idle; level_ = 0.0f; counter_ = 0; }

  /** Advance one sample and return the level: 0..1 for a sustain in 0..1.
   *  No allocation. */
  inline float getNextSample() {
    switch (stage_) {
      case Delay:
        // The level is HELD, not zeroed: from idle it is zero already, and a
        // retrigger -- the same key again, a stolen voice -- starts from where
        // the envelope is, as ADSR does. Zeroing it here was a step from
        // sustain to silence in one sample.
        if (++counter_ >= samples(p_.delay)) { stage_ = Attack; counter_ = 0; }
        break;
      case Attack:
        level_ += 1.0f / (float) samples(p_.attack);
        if (level_ >= 1.0f) { level_ = 1.0f; stage_ = Hold; counter_ = 0; }
        break;
      case Hold:
        level_ = 1.0f;
        if (++counter_ >= samples(p_.hold)) { stage_ = Decay; counter_ = 0; }
        break;
      case Decay:
        level_ = p_.sustain + decayK_ * (level_ - p_.sustain);
        if (std::fabs(level_ - p_.sustain) < 1e-4f) { level_ = p_.sustain; stage_ = Sustain; }
        break;
      case Sustain:
        level_ = p_.sustain;
        break;
      case Release:
        level_ *= releaseK_;
        if (level_ < 1e-4f) { level_ = 0.0f; stage_ = Idle; }
        break;
      case Idle:
      default:
        level_ = 0.0f;
        break;
    }
    return level_;
  }

private:
  enum Stage { Idle, Delay, Attack, Hold, Decay, Sustain, Release };
  static constexpr float kDecay60 = 6.907755f; // ln(1000): the -60 dB convention

  int samples(float seconds) const {
    // Held in float before the conversion: a NaN time fails every
    // comparison, and a time of hours overflows an int, and either makes
    // the cast undefined behaviour -- which x86 happens to turn into one
    // sample and another target need not. A billion samples is over five
    // hours even at 48 kHz.
    const float n = seconds * sr_;
    if (!(n >= 1.0f)) return 1;
    if (n >= 1.0e9f) return 1000000000;
    return (int) n;
  }
  /** When the settings or the rate change, not per sample per voice. */
  void updateCoefficients() {
    decayK_ = std::exp(-kDecay60 / (float) samples(p_.decay));
    releaseK_ = std::exp(-kDecay60 / (float) samples(p_.release));
  }

  Stage stage_ = Idle;
  float sr_ = 48000.0f, level_ = 0.0f;
  int counter_ = 0;
  Parameters p_{};
  float decayK_ = 0.0f, releaseK_ = 0.0f;
};

// ── Portamento ───────────────────────────────────────────────────────────────

/**
 * A glide between notes, in pitch: what a mono or legato line does between
 * one key and the next.
 *
 * Linear in SEMITONES, which is exponential in hertz, as the ear hears it: a
 * glide over an octave spends as long on the lower half as on the upper one,
 * where a glide linear in hertz rushes through the bass and crawls at the
 * top. next() returns the note it has reached -- fractional, so 60.5 is a
 * quarter tone above middle C -- for the voice to turn into a frequency with
 * whatever bend it adds.
 *
 * Two ways to time it, as hardware synths offer:
 *   ConstantTime  every glide takes setTime() seconds, a semitone or two
 *                 octaves: the same feel whatever the interval.
 *   ConstantRate  setTime() seconds per OCTAVE: a wide leap takes longer,
 *                 which is what a slide on a string does.
 *
 * In a voice: jumpTo() at noteOn() when the voice was silent (there is no
 * pitch to come from), glideTo() at legatoTo(), and glideTo() at noteOn() too
 * for "glide always". No allocation; call next() once per sample.
 */
class Portamento {
public:
  enum class Timing { ConstantTime, ConstantRate };

  void setSampleRate(float sampleRate) { sr_ = sampleRate > 0.0f ? sampleRate : 48000.0f; }
  /** The glide's time: its whole length, or its length per octave (see
   *  Timing). 0 is no glide: every move lands at once. */
  void setTime(float seconds) { time_ = seconds > 0.0f ? seconds : 0.0f; }
  void setTiming(Timing timing) { timing_ = timing; }

  /** Be at `note` now, with no glide. */
  void jumpTo(float note) {
    from_ = to_ = current_ = note;
    remaining_ = 0;
  }
  /** Glide from wherever the pitch is now -- mid-glide included -- to `note`. */
  void glideTo(float note) {
    from_ = current_;
    to_ = note;
    float seconds = time_;
    if (timing_ == Timing::ConstantRate) seconds *= std::fabs(to_ - from_) / 12.0f;
    const int samples = (int) (seconds * sr_ + 0.5f);
    if (samples <= 0 || from_ == to_) {
      jumpTo(note);
      return;
    }
    total_ = samples;
    remaining_ = samples;
  }

  /** The note this sample: one step of the glide, or the target once there. */
  inline float next() {
    if (remaining_ > 0) {
      --remaining_;
      const float t = 1.0f - (float) remaining_ / (float) total_;
      current_ = from_ + (to_ - from_) * t;
    }
    return current_;
  }
  /** Where the pitch is, without stepping. */
  float current() const { return current_; }
  /** Where it is going. */
  float target() const { return to_; }
  bool isGliding() const { return remaining_ > 0; }

private:
  float sr_ = 48000.0f;
  float time_ = 0.0f;
  Timing timing_ = Timing::ConstantTime;
  float from_ = 60.0f, to_ = 60.0f, current_ = 60.0f;
  int remaining_ = 0;
  int total_ = 1;
};

// ── Karplus-Strong ───────────────────────────────────────────────────────────

/**
 * A plucked string: a burst of noise circulating in a delay the length of
 * one period, losing a little level and a little brightness on every pass.
 * Decay is the -60 dB time at the fundamental; damping closes a one-pole in
 * the loop so the upper partials die first, as they do on a real string.
 *
 * The loop's length is the delay PLUS the one-pole's own delay, and the pitch
 * error from forgetting that is a quarter-tone at the top of a guitar. The
 * filter's phase delay is evaluated AT THE STRING'S OWN FUNDAMENTAL from its
 * transfer function (OnePole::phaseDelaySamples), not approximated by its DC
 * value: a dull string sits a good fraction of the way up its damping
 * filter, where the two differ by whole samples. It is subtracted from the
 * line, and the fractional remainder is read with the cubic tap, which is
 * what makes the tuning continuous rather than stepping at every sample
 * boundary. The same filter also LOSES level at the fundamental, and the
 * loop gain is raised by that loss (Jaffe & Smith's compensation) so the
 * declared decay is the measured one at every damping -- capped just under
 * unity, because a loop gain above one at DC is an oscillator.
 *
 * MaxDelay is the longest period, so it sets the lowest note: the default
 * 16384 holds 20 Hz at every rate up to 327 kHz. It was 4096, which is 47 Hz
 * at 192 kHz -- every bass note below that sounded AT 47 Hz. A note below
 * what the line can hold is now clamped to it in noteOn, where it is visible,
 * rather than in the delay, where it was not.
 */
template <int MaxDelay = 16384>
class KarplusStrong {
public:
  /** Sample rate in Hz (1 or less means 48 kHz); re-designs the damping
   *  filter and retunes the string. lowestHz() moves with it. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    damp_.setSampleRate(sr_);
    setDamping(damping_);
  }
  /** Seconds to -60 dB at the fundamental. */
  void setDecay(float seconds) { decay_ = seconds > 0.01f ? seconds : 0.01f; retune(); }
  /** 0 = bright (loop open to 20 kHz), 1 = dull (closed at 1 kHz). */
  void setDamping(float d) {
    damping_ = clampf(d, 0.0f, 1.0f);
    const float cutoff = 20000.0f * std::pow(1000.0f / 20000.0f, damping_);
    damp_.setCutoff(clampf(cutoff, 20.0f, sr_ * 0.45f));
    retune();
  }

  /** The lowest note the line holds at the current rate, never under 20 Hz. */
  float lowestHz() const {
    const float fits = sr_ / (float) (MaxDelay - 8);
    return fits > 20.0f ? fits : 20.0f;
  }

  /** Pluck at `hz`, held to lowestHz() .. 0.45 x the sample rate, with
   *  `velocity` as the linear level of the burst (0..1 is the usual range).
   *  Empties the string first, then writes one period of noise into it:
   *  that much work in this call, and no allocation. */
  void noteOn(float hz, float velocity) {
    hz_ = clampf(hz, lowestHz(), sr_ * 0.45f);
    retune();
    // A period of noise, scaled by velocity, is the pluck.
    //
    // Scaled by the rate too. Each harmonic of a period of white noise
    // carries 1/sqrt(period) of it, and the period in samples grows with the
    // rate: the same note was 3 dB quieter at every doubling, 11 dB from
    // 48 kHz to 384. sqrt(rate / 48 kHz) puts every harmonic a 48 kHz pluck
    // has at the level it has there. Above 48 kHz the noise is first held to
    // what a 48 kHz pluck contains -- a fourth-order lowpass at 24 kHz --
    // or the scale would raise the first period's peak with ultrasound.
    line_.reset();
    damp_.reset();
    const float scale = std::sqrt(sr_ / 48000.0f);
    const bool limit = sr_ > 48000.0f;
    SvfSection band[2];
    for (SvfSection& b : band) {
      b.setSampleRate(sr_);
      b.lowpass(24000.0, 0.70710678);
    }
    const int fill = (int) (sr_ / hz_) + 1;
    for (int i = 0; i < fill && i < MaxDelay - 4; ++i) {
      float v = noise_.next();
      if (limit) v = band[1].process(band[0].process(v));
      line_.write(v * scale * velocity);
    }
    env_ = velocity;
    // A new note is not releasing, whatever the last one on this voice did.
    // Never cleared, every note after a voice's first note-off decayed with
    // the release damping: 78 dB/s where the decay setting said 37.
    releasing_ = false;
  }
  /** Damp the string: the loop loses an extra 0.5% per pass, so it dies
   *  faster than the decay says, until isActive() turns false. */
  void noteOff() { releasing_ = true; }
  /** Whether a slow follower on the output is still above 1e-4 (-80 dB):
   *  false once the string has rung out, or after reset(). */
  bool isActive() const { return env_ > 1e-4f; }
  /** Silence at once, and the noise generator restarted, so the next noteOn
   *  of a note plucks exactly as the first did. */
  void reset() {
    line_.reset();
    damp_.reset();
    noise_.reset(); // the same pluck again, for the same note
    env_ = 0.0f;
    releasing_ = false;
  }

  /** The next sample of the string. No allocation. */
  inline float render() {
    const float y = line_.readCubic(delay_);
    // A release just shortens the decay: the string is damped by a hand.
    const float g = releasing_ ? gain_ * 0.995f : gain_;
    line_.write(flushDenormal(damp_.lp(y) * g));
    // A slow follower on the output decides when the voice is spent.
    const float a = std::fabs(y);
    env_ = a > env_ ? a : flushDenormal(env_ * 0.9995f);
    return y;
  }

private:
  void retune() {
    const float hz = hz_ > 1.0f ? hz_ : 1.0f;
    const float period = sr_ / hz;
    // The loop filter's delay and loss at THIS fundamental, from its own
    // transfer function.
    delay_ = clampf(period - damp_.phaseDelaySamples(hz), 2.0f, (float) (MaxDelay - 4));
    // Jot's rule again: -60 dB after `decay` seconds, per pass of `period` --
    // divided by what the filter takes off the fundamental on each pass.
    const float loss = dbToGain(damp_.magnitudeDb(hz));
    gain_ = std::pow(10.0f, -3.0f * period / (decay_ * sr_)) / (loss > 1e-3f ? loss : 1e-3f);
    if (gain_ > 0.99999f) gain_ = 0.99999f;
  }

  DelayLine<MaxDelay> line_;
  OnePole damp_;
  WhiteNoise noise_;
  float sr_ = 48000.0f, hz_ = 220.0f, decay_ = 2.0f, damping_ = 0.3f;
  float delay_ = 218.0f, gain_ = 0.999f, env_ = 0.0f;
  bool releasing_ = false;
};

// ── Modal ────────────────────────────────────────────────────────────────────

/**
 * A struck object as a sum of ringing modes -- a bar, a bell, a drum shell.
 * Each mode is a two-pole resonator with its own frequency, decay (-60 dB
 * time) and amplitude, excited by whatever is fed in: an impulse for a
 * strike, a noise burst for a brush, a saw for something stranger.
 *
 * The recursion y = A sin(w) x + 2r cos(w) y1 - r^2 y2 has the impulse
 * response A r^n sin(w (n+1)): amplitude exactly A, decaying by r per
 * sample, which is what lets the decay be set in seconds rather than tuned.
 */
template <int MaxModes = 8>
class ModalResonator {
public:
  /** Sample rate in Hz (1 or less means 48 kHz); redesigns every mode from
   *  the frequencies and decays it was given. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    for (int i = 0; i < MaxModes; ++i) design(i);
  }
  /** How many modes sound, from index 0: clamped to 0..MaxModes. It is 0
   *  until set, and then the resonator is silent. Modes past it keep their
   *  settings. */
  void setNumModes(int n) { modes_ = n < 0 ? 0 : (n > MaxModes ? MaxModes : n); }
  /** Mode `index` (0-based; out of range is ignored): its frequency in Hz,
   *  held to 1 Hz .. 0.49 x the sample rate; its -60 dB time in seconds, at
   *  least 1 ms; and the peak of its impulse response, linear. Keeps the
   *  mode's ringing. It only sounds if `index` is below setNumModes(). */
  void setMode(int index, float hz, float decaySeconds, float amplitude) {
    if (index < 0 || index >= MaxModes) return;
    hz_[index] = hz > 1.0f ? hz : 1.0f; // the request; design() fits it to the rate
    decay_[index] = decaySeconds > 0.001f ? decaySeconds : 0.001f;
    amp_[index] = amplitude;
    design(index);
  }
  /** Stops every mode ringing; the settings are kept. */
  void reset() {
    for (int i = 0; i < MaxModes; ++i) y1_[i] = y2_[i] = 0.0f;
  }

  /** One sample of excitation in, the sum of the sounding modes out. An
   *  impulse of 1 rings each mode at its amplitude. No allocation. */
  inline float process(float x) {
    float out = 0.0f;
    for (int i = 0; i < modes_; ++i) {
      const float y = b0_[i] * x + a1_[i] * y1_[i] - a2_[i] * y2_[i];
      y2_[i] = y1_[i];
      y1_[i] = flushDenormal(y);
      out += y;
    }
    return out;
  }

private:
  void design(int i) {
    // Clamped HERE, at the rate in force, not once when the mode was set: a
    // 30 kHz mode set at 96 kHz and then moved to 44.1 rang at 14.1 kHz, its
    // angle past pi and folded down. The request is kept, so going back up
    // brings it back.
    const float hz = clampf(hz_[i], 1.0f, sr_ * 0.49f);
    const float w = kTwoPi * hz / sr_;
    const float r = std::pow(10.0f, -3.0f / (decay_[i] * sr_));
    b0_[i] = amp_[i] * std::sin(w);
    a1_[i] = 2.0f * r * std::cos(w);
    a2_[i] = r * r;
  }

  float hz_[MaxModes]{}, decay_[MaxModes]{}, amp_[MaxModes]{};
  float b0_[MaxModes]{}, a1_[MaxModes]{}, a2_[MaxModes]{};
  float y1_[MaxModes]{}, y2_[MaxModes]{};
  float sr_ = 48000.0f;
  int modes_ = 0;
};

// ── Modulation matrix ────────────────────────────────────────────────────────

/** One route of a modulation matrix: a source, a destination, how far.
 *  ModRouting::set() fills these. */
struct ModRoute {
  int source = -1;      ///< the source's index, -1 off
  int destination = -1; ///< the destination's index, -1 off
  /** How far a source at 1 moves the destination, in the destination's own
   *  units: semitones for a pitch, octaves for a cutoff, whatever the voice
   *  reads the offset as. Negative inverts. */
  float amount = 0.0f;
  /** A second source that scales the route, -1 for none: the mod wheel
   *  opening an LFO's vibrato, velocity deepening an envelope's sweep. */
  int via = -1;
};

/**
 * The routes of a modulation matrix, as a patch sets them: which source
 * moves which destination, and how far. One per plugin, shared by every
 * voice's ModMatrix.
 *
 * Sources and destinations are numbered by the plugin -- an enum of its LFOs
 * and envelopes, an enum of what they can move -- so the matrix knows
 * nothing about what they mean and a plugin is not limited to a list
 * somebody else chose. A route is usually three parameters (source,
 * destination, amount: two choices and a knob), set here once per block from
 * the values the DSP is handed. Allocation-free, for the audio thread.
 */
template <int MaxRoutes = 8>
class ModRouting {
public:
  static_assert(MaxRoutes >= 1, "a routing needs at least one slot");
  /** How many routes a patch can have. */
  static constexpr int kMaxRoutes = MaxRoutes;

  /** Route slot `slot`: `source` to `destination` by `amount`, scaled by
   *  `via` where it is not -1. A negative source or destination, or an amount
   *  of 0, turns the slot off. A slot out of range is ignored. */
  void set(int slot, int source, int destination, float amount, int via = -1) {
    if (slot < 0 || slot >= MaxRoutes) return;
    ModRoute& r = routes_[slot];
    if (r.source == source && r.destination == destination && r.amount == amount && r.via == via) return;
    r.source = source;
    r.destination = destination;
    r.amount = amount;
    r.via = via;
    pack();
  }
  /** Slot `slot` off. */
  void clear(int slot) { set(slot, -1, -1, 0.0f); }
  /** Every slot off. */
  void clearAll() {
    for (ModRoute& r : routes_) r = ModRoute{};
    count_ = 0;
  }
  /** Slot `slot` as it was set. */
  const ModRoute& route(int slot) const { return routes_[slot]; }
  /** The routes that are on, packed, for evaluation: `activeCount()` of
   *  them, in slot order. */
  const ModRoute* active() const { return active_; }
  /** How many routes are on. */
  int activeCount() const { return count_; }

private:
  void pack() {
    count_ = 0;
    for (const ModRoute& r : routes_)
      if (r.source >= 0 && r.destination >= 0 && r.amount != 0.0f) active_[count_++] = r;
  }
  ModRoute routes_[MaxRoutes];
  ModRoute active_[MaxRoutes];
  int count_ = 0;
};

/**
 * One voice's side of a modulation matrix: its sources' values in, the
 * offset each destination gets out.
 *
 * Each voice has one, because a voice's sources are its own -- its envelope,
 * its LFO started at its note-on, its velocity and key, its pressure -- while
 * the routes are the patch's (ModRouting). Set the sources that moved, call
 * process() with the routing, and read each destination's offset: the sum,
 * over every route into it, of source x amount (x via). A destination no
 * route reaches reads 0.
 *
 * Cheap enough per sample -- a multiply-add per route -- though what a
 * voice does with the offsets usually is not (a filter's coefficients), and
 * that part can run at a control rate: every 16 or 32 samples, as the
 * example synth does.
 */
template <int NumSources, int NumDestinations>
class ModMatrix {
public:
  static_assert(NumSources >= 1 && NumDestinations >= 1, "a matrix needs a source and a destination");
  /** How many sources the plugin numbered. */
  static constexpr int kNumSources = NumSources;
  /** How many destinations the plugin numbered. */
  static constexpr int kNumDestinations = NumDestinations;

  /** Source `source`'s value now: bipolar (-1..1) for an LFO, 0..1 for an
   *  envelope or a velocity, as the source is. Out of range is ignored. */
  void setSource(int source, float value) {
    if (source >= 0 && source < NumSources) sources_[source] = value;
  }
  /** Source `source`'s value, as last set; 0 out of range. */
  float source(int source) const { return source >= 0 && source < NumSources ? sources_[source] : 0.0f; }

  /** Every destination's offset, from the sources as they are now. */
  template <int MaxRoutes>
  void process(const ModRouting<MaxRoutes>& routing) {
    for (float& o : offsets_) o = 0.0f;
    const ModRoute* r = routing.active();
    for (int i = 0, n = routing.activeCount(); i < n; ++i) {
      const ModRoute& route = r[i];
      if (route.source >= NumSources || route.destination >= NumDestinations) continue;
      float v = sources_[route.source] * route.amount;
      if (route.via >= 0 && route.via < NumSources) v *= sources_[route.via];
      offsets_[route.destination] += v;
    }
  }
  /** Destination `destination`'s offset as process() left it; 0 out of
   *  range. */
  float offset(int destination) const {
    return destination >= 0 && destination < NumDestinations ? offsets_[destination] : 0.0f;
  }

  /** Every source and offset to 0: a voice's note-on, before it sets what
   *  its note brings (velocity, key). */
  void reset() {
    for (float& s : sources_) s = 0.0f;
    for (float& o : offsets_) o = 0.0f;
  }

private:
  float sources_[NumSources] = {};
  float offsets_[NumDestinations] = {};
};

} // namespace sonore
