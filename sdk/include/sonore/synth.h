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
//   KarplusStrong                     a plucked string
//   ModalResonator                    a struck body: a bank of ringing modes
//
// Everything is per voice and allocation-free; the Wavetable is the one big
// object and is designed to be held ONCE and read by every voice.
//
// Included by dsp.h. Uses Oscillator, Fft, DelayLine, OnePole, WhiteNoise.
#pragma once
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
  static constexpr size_t size() { return Size; }
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

  /** Linear-interpolated read of one mip at a phase in turns. */
  inline float read(int mip, float phase) const {
    const float p = (phase - std::floor(phase)) * (float) Size;
    const size_t i0 = (size_t) p;
    const size_t i1 = (i0 + 1) & (Size - 1);
    const float frac = p - (float) i0;
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
  void setTable(const Table* table) { table_ = table; }
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }
  inline void setFreq(float hz) {
    inc_ = clampf(hz, 0.0f, sr_ * 0.5f) / sr_;
    mip_ = Table::mipFor(inc_);
  }
  void reset(float phase = 0.0f) { phase_ = phase; }
  float phase() const { return phase_; }

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
  enum class Shape { Saw, Square };

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    for (int i = 0; i < MaxVoices; ++i) osc_[i].setSampleRate(sr_);
    update();
  }
  void setShape(Shape s) { shape_ = s; }
  void setVoices(int n) { voices_ = n < 1 ? 1 : (n > MaxVoices ? MaxVoices : n); update(); }
  /** Total detune width in cents, outermost voice to outermost voice. */
  void setDetune(float cents) { detune_ = clampf(cents, 0.0f, 200.0f); update(); }
  /** 0 = all centred, 1 = outer voices hard left and right. */
  void setSpread(float s) { spread_ = clampf(s, 0.0f, 1.0f); update(); }
  void setFreq(float hz) { freq_ = hz; update(); }
  /** Voices start at distinct phases so they do not stack into one saw for
   *  the first cycle and then drift apart audibly. */
  void reset() {
    for (int i = 0; i < MaxVoices; ++i) osc_[i].reset((float) i / (float) MaxVoices);
  }

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
      osc_[i].setFreq(freq_ * std::pow(2.0f, cents / 1200.0f));
      // Constant-power pan.
      const float pan = pos * spread_;                   // -1..+1
      const float angle = (pan + 1.0f) * 0.25f * kPi;    // 0..pi/2
      gainL_[i] = std::cos(angle);
      gainR_[i] = std::sin(angle);
    }
  }

  Oscillator osc_[MaxVoices];
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
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; setMasterFreq(masterHz_); }
  void setMasterFreq(float hz) {
    masterHz_ = clampf(hz, 0.0f, sr_ * 0.5f);
    masterInc_ = masterHz_ / sr_;
    slaveInc_ = clampf(masterHz_ * ratio_, 0.0f, sr_ * 0.5f) / sr_;
  }
  /** Slave frequency as a multiple of the master's. 1..8 is the classic range. */
  void setRatio(float ratio) { ratio_ = ratio < 0.0f ? 0.0f : ratio; setMasterFreq(masterHz_); }
  /** Off = the naive reset, for comparison. On is the default. */
  void setBandLimited(bool on) { blep_ = on; }
  void reset() { master_ = 0.0f; slave_ = 0.0f; }

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
        const float x = slave_ / slaveInc_;
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
      if (slave_ >= 1.0f) { slave_ -= 1.0f; justWrapped_ = true; }
    }
    return y;
  }

private:
  float sr_ = 48000.0f, masterHz_ = 220.0f, ratio_ = 1.5f;
  float masterInc_ = 0.0f, slaveInc_ = 0.0f, master_ = 0.0f, slave_ = 0.0f;
  float syncHeight_ = 0.0f, afterSync_ = 0.0f;
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
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; setFreq(hz_); }
  void setFreq(float hz) { hz_ = hz; inc_ = clampf(hz, 0.0f, sr_ * 0.5f) / sr_; }
  /** Self-modulation index in radians, 0..1.5. At 1 the loop is Tomisawa's
   *  feedback sawtooth -- the largest index for which sin(theta + beta y) is
   *  still single-valued -- and past it the loop turns chaotic: measured on a
   *  440 Hz operator, the harmonic share of the output is 98% at 1.0, 90% at
   *  1.5, 75% at 2 and 63% at pi, the rest being noise. 1.5 is kept because
   *  the DX7's top feedback level is exactly that gritty and people use it;
   *  the first version allowed pi and called it "a saw". */
  void setFeedback(float radians) { feedback_ = clampf(radians, 0.0f, 1.5f); }
  void setLevel(float level) { level_ = level; }
  void reset(float phase = 0.0f) { phase_ = phase; last1_ = last2_ = 0.0f; }

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
  struct Parameters {
    float delay = 0.0f, attack = 0.01f, hold = 0.0f, decay = 0.1f, sustain = 0.8f, release = 0.2f;
  };
  void setSampleRate(float sr) { sr_ = sr > 1.0f ? sr : 48000.0f; }
  void setParameters(const Parameters& p) { p_ = p; }
  void noteOn() { stage_ = Delay; counter_ = 0; }
  void noteOff() { if (stage_ != Idle) stage_ = Release; }
  bool isActive() const { return stage_ != Idle; }
  void reset() { stage_ = Idle; level_ = 0.0f; counter_ = 0; }

  inline float getNextSample() {
    switch (stage_) {
      case Delay:
        level_ = 0.0f;
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
        level_ = p_.sustain + decayK() * (level_ - p_.sustain);
        if (std::fabs(level_ - p_.sustain) < 1e-4f) { level_ = p_.sustain; stage_ = Sustain; }
        break;
      case Sustain:
        level_ = p_.sustain;
        break;
      case Release:
        level_ *= releaseK();
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
    const int n = (int) (seconds * sr_);
    return n < 1 ? 1 : n;
  }
  float decayK() const { return std::exp(-kDecay60 / (float) samples(p_.decay)); }
  float releaseK() const { return std::exp(-kDecay60 / (float) samples(p_.release)); }

  Stage stage_ = Idle;
  float sr_ = 48000.0f, level_ = 0.0f;
  int counter_ = 0;
  Parameters p_{};
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
 */
template <int MaxDelay = 4096>
class KarplusStrong {
public:
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

  void noteOn(float hz, float velocity) {
    hz_ = clampf(hz, 20.0f, sr_ * 0.45f);
    retune();
    // A period of noise, scaled by velocity, is the pluck.
    line_.reset();
    damp_.reset();
    const int fill = (int) (sr_ / hz_) + 1;
    for (int i = 0; i < fill && i < MaxDelay - 4; ++i) line_.write(noise_.next() * velocity);
    env_ = velocity;
  }
  void noteOff() { releasing_ = true; }
  bool isActive() const { return env_ > 1e-4f; }
  void reset() { line_.reset(); damp_.reset(); env_ = 0.0f; releasing_ = false; }

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
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    for (int i = 0; i < MaxModes; ++i) design(i);
  }
  void setNumModes(int n) { modes_ = n < 0 ? 0 : (n > MaxModes ? MaxModes : n); }
  void setMode(int index, float hz, float decaySeconds, float amplitude) {
    if (index < 0 || index >= MaxModes) return;
    hz_[index] = clampf(hz, 1.0f, sr_ * 0.49f);
    decay_[index] = decaySeconds > 0.001f ? decaySeconds : 0.001f;
    amp_[index] = amplitude;
    design(index);
  }
  void reset() {
    for (int i = 0; i < MaxModes; ++i) y1_[i] = y2_[i] = 0.0f;
  }

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
    const float w = kTwoPi * hz_[i] / sr_;
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

} // namespace sonore
