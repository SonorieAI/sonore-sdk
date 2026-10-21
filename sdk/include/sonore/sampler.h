// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: sample playback.
//
// A drum sampler, a multisampled instrument, a one-shot player: all of them
// are the same three problems, and each has a way of going quietly wrong.
//
//   PITCH. Playing a note is a rate change, but so is the difference between
//   the file's sample rate and the session's. A 44.1 kHz sample played at
//   48 kHz without correction is sharp by a third of a semitone: enough to
//   sound out of tune against anything else, and subtle enough to be blamed
//   on the tuning of the other instrument.
//
//   INTERPOLATION. A rate change means reading between samples, and linear
//   interpolation is a lowpass whose corner moves with the fraction. On a
//   sustained sample that is a shimmer that follows the pitch.
//
//   LOOPS. Reading across a loop point has to see the samples on BOTH sides,
//   or the interpolator runs off the end of the loop into whatever follows and
//   clicks once per cycle.
//
// The sample data is owned elsewhere (an ImpulseResponse, a WavData, anything)
// and referenced here, because a voice must not own or copy audio while the
// audio thread is running.

#pragma once

#include <cmath>
#include <cstddef>

#include "audio.h"
#include "dsp.h"
#include "sample_stream.h"

namespace sonore {

/** Where a sample lives and how it should be played. Shared by every voice
 *  playing it, so the per-voice state stays tiny. */
struct SampleData {
  /** The frames, channels interleaved: numFrames x numChannels floats. Not
   *  owned: it must outlive every voice playing it. */
  const float* interleaved = nullptr;
  /** Streamed from disk instead of resident, for material too big to hold.
   *
   *  When this is set `interleaved` is not used. One streamer per PLAYING
   *  VOICE, not per sample: a streamer has one position, and two voices
   *  playing the same file at different points would fight over it. */
  SampleStreamer* streamer = nullptr;
  /** Length in frames, one sample per channel each. */
  size_t numFrames = 0;
  /** Channels per frame, at least 1. A voice asked for a channel past the
   *  last reads the last, so a mono sample fills a stereo output. */
  uint16_t numChannels = 1;
  /** The recording's own rate in Hz; a voice corrects for the session's. */
  double sampleRate = 48000.0;

  /** The note the recording actually sounds. Playing it at this note means
   *  playing it at its own rate. */
  int rootNote = 60;
  /** Fine tuning of the recording itself, in cents. */
  float tuningCents = 0.0f;

  /** Loop bounds in frames. A zero-length loop means one-shot. */
  size_t loopStart = 0;
  /** One past the loop's last frame: the loop is loopStart .. loopEnd - 1. */
  size_t loopEnd = 0;
  /** Whether the loop plays. It also needs loopStart < loopEnd <= numFrames;
   *  otherwise the sample is a one-shot. A voice loops until noteOff(),
   *  then plays the file's tail. */
  bool looping = false;

  /** Something to play: data or a streamer, at least one frame and one
   *  channel. A voice given an invalid sample stays silent and inactive. */
  bool isValid() const {
    return (interleaved != nullptr || streamer != nullptr) && numFrames > 0 && numChannels > 0;
  }
  /** Whether the frames come from `streamer` rather than `interleaved`. */
  bool isStreamed() const { return streamer != nullptr; }
};

/**
 * One voice playing one sample.
 *
 * Deliberately does NOT include an envelope: the toolkit already has ADSR, and
 * a voice that hard-codes one cannot be given a different shape. What it does
 * own is position, rate and looping: the parts that are easy to get subtly
 * wrong.
 */
class SampleVoice {
public:
  /** How a voice reads between a sample's frames.
   *
   *  Sinc, the default, is a Kaiser-windowed sinc low-passed at 0.9 of the
   *  slower of the file's and the session's Nyquist: flat to 16 kHz from a
   *  44.1 kHz file, and what lies above the session's Nyquist when a note is
   *  transposed up is removed instead of folded down -- up to two octaves up
   *  (+24 semitones, or a file at four times the session's rate). Past that
   *  the kernel stops widening and what is above folds, as the cubic read's
   *  does: a 4.5 kHz partial transposed +30 comes back at full level. 36 taps
   *  a frame per channel at or below the file's own pitch, up to 144 two
   *  octaves up: 1.6 to 1.8 times the cubic read's cost.
   *
   *  Cubic is the Catmull-Rom read this used to be, a quarter of the cost and
   *  no band limit: a 44.1 kHz file in a 48 kHz session imaged a 10 kHz tone
   *  at 13.9 kHz only 26 dB down, drooped 15 kHz by 1.65 dB, and a file
   *  played faster than the session folded everything past its Nyquist back
   *  in. For a voice count where that cost matters more than the sound. */
  enum class Interpolation {
    Sinc, ///< band-limited Kaiser-windowed sinc, the default
    Cubic ///< Catmull-Rom: a quarter of the cost, no band limit
  };
  /** Which read renderChannel() uses, from its next call. Cheap; fine while
   *  the voice plays. */
  void setInterpolation(Interpolation mode) {
    interpolation_ = mode;
    heldChannel_ = kNothingHeld;
  }
  /** The read in use; Sinc unless set. */
  Interpolation interpolation() const { return interpolation_; }

  /** The session's rate in Hz (0 or less means 48 kHz), which the file's
   *  rate is corrected against. Call it when preparing, not from the audio
   *  thread: the first call in a process builds the sinc tables, about
   *  70 KB of static storage shared by every voice. */
  void setSampleRate(double hostSampleRate) {
    (void) phaseTable(); // built here, with the kernel, not in the first audio block
    hostRate_ = hostSampleRate > 0.0 ? hostSampleRate : 48000.0;
    updateRate();
  }

  /** [main thread] Attach a sample. ALLOCATES when the sample is streamed,
   *  which is why this is not something to call from a note-on handler on the
   *  audio thread -- give a voice its sample when the instrument loads. */
  void setSample(const SampleData* sample) {
    sample_ = sample;
    updateRate();
    cacheStart_ = 0;
    cacheFilled_ = 0;
    if (sample && sample->isStreamed()) {
      const size_t channels = sample->numChannels ? sample->numChannels : 1;
      cache_.assign(kCacheFrames * channels, 0.0f);
      // The first few frames of the loop, kept separately and permanently.
      //
      // At the loop boundary the interpolator wants points from BOTH sides,
      // and the far side is thousands of frames away from the rolling cache.
      // Without this the cache would be dragged back and forth once per
      // sample at exactly the loop point -- a refill per sample, which is the
      // one place a streaming sampler must not stall.
      loopPrefix_.assign(kLoopPrefixFrames * channels, 0.0f);
      loopPrefixFilled_ = 0;
      if (sample->looping && sample->loopEnd > sample->loopStart && sample->streamer) {
        loopPrefixStart_ = sample->loopStart;
        loopPrefixFilled_ =
            sample->streamer->read(loopPrefixStart_, loopPrefix_.data(), kLoopPrefixFrames);
      }
    } else {
      cache_.clear();
      loopPrefix_.clear();
      loopPrefixFilled_ = 0;
    }
  }

  /** Start at `note`, from the beginning. */
  void noteOn(int note) {
    note_ = note;
    position_ = 0.0;
    passes_ = 0;
    // Cleared HERE, not only in stop(). A voice is reused for a new note while
    // its previous note is still releasing -- the voice allocator steals the
    // same-key voice by design -- and a leftover releasing_ makes looping()
    // return false, so the new note plays as a one-shot and dies at loopEnd
    // instead of sustaining. Every re-pressed key on a looped instrument.
    releasing_ = false;
    active_ = sample_ && sample_->isValid();
    updateRate();
  }

  /** Stop looping and let the tail play out: what a sustain loop means when
   *  the key is released. A hard stop would cut the release of the recording
   *  itself, which is the sound the sampler was chosen for. */
  void noteOff() {
    releasing_ = true;
    heldChannel_ = kNothingHeld; // past the loop's end the file's tail, not the loop
  }

  /** Silence at once, with no tail: the voice goes inactive and is free for
   *  the next noteOn(). */
  void stop() {
    active_ = false;
    releasing_ = false;
    heldChannel_ = kNothingHeld;
  }

  /** Playing: from noteOn() of a valid sample until stop(), or until a read
   *  that is not looping runs past the last frame. */
  bool isActive() const { return active_; }
  /** The note of the last noteOn(); 60 before any. */
  int note() const { return note_; }
  /** The read position in the file's frames, fractional, from 0. While the
   *  voice loops it stays below loopEnd. */
  double position() const { return position_; }

  /** Extra pitch offset in semitones, for per-note expression or vibrato. */
  void setPitchOffset(float semitones) {
    pitchOffset_ = semitones;
    updateRate();
  }

  /** One frame for one channel, advancing only on channel 0 so a caller can
   *  read every channel at the same position.
   *
   *  A channel past the file's last reads the last, so a mono file played
   *  into a stereo output is asked for the same read twice a frame. The
   *  first is kept until anything about the voice changes -- advance()
   *  included -- and the second is that value: the sinc read was a third of
   *  a voice's frame again for nothing. Not for a streamed file, whose cache
   *  can fill between the two calls. */
  inline float renderChannel(size_t channel) const {
    if (!active_ || !sample_ || !sample_->isValid()) return 0.0f;
    const size_t channels = sample_->numChannels;
    const size_t c = channel < channels ? channel : channels - 1;
    if (c == heldChannel_) return held_;

    const double pos = position_;
    float y;
    if (interpolation_ == Interpolation::Cubic) {
      const long i1 = (long) pos;
      const float frac = (float) (pos - (double) i1);
      // The common case read straight from memory: resident, inside the file
      // and clear of the loop's seam, where sampleAt() would only have
      // returned these four values after its loop and range checks, four
      // times per channel per frame.
      if (!sample_->streamer && i1 >= 1 && i1 + 2 < (long) sample_->numFrames && tapsClearOfLoop(i1 - 1, 4)) {
        const float* p = sample_->interleaved + (size_t) (i1 - 1) * channels + c;
        y = DelayLine<2>::catmullRomTap(p[0], p[channels], p[2 * channels], p[3 * channels], frac);
      } else {
        y = DelayLine<2>::catmullRomTap(sampleAt(i1 - 1, c), sampleAt(i1, c), sampleAt(i1 + 1, c),
                                        sampleAt(i1 + 2, c), frac);
      }
    } else if (rate_ == 1.0 && pos == std::floor(pos)) {
      // At the file's own rate, on a frame, the read is the frame: a sample
      // played at its own pitch and rate comes out bit for bit. The low-pass
      // below would otherwise take the top tenth of its band.
      y = sampleAt((long) pos, c);
    } else {
      // The weights depend on the position and the rate only, so every
      // channel of one frame shares them.
      if (pos != weightsFor_ || rate_ != weightsRate_) prepareWeights(pos);
      const long first = weightsFirst_;
      const int n = weightsCount_;
      // Straight from memory when every tap is inside the sample and clear of
      // the loop's seams, which is nearly always; through sampleAt() -- which
      // wraps the loop and pads the ends -- otherwise.
      if (!sample_->streamer && first >= 0 && first + n <= (long) sample_->numFrames &&
          tapsClearOfLoop(first, n)) {
        const float* p = sample_->interleaved + (size_t) first * channels + c;
        if (channels == 1) {
          y = dot(weights_, p, n);
        } else {
          for (int k = 0; k < n; ++k) taps_[k] = p[(size_t) k * channels];
          y = dot(weights_, taps_, n);
        }
      } else {
        // The same dot product as above over the same values in the same
        // order, so a streamed voice and a resident one agree to the bit.
        for (int k = 0; k < n; ++k) taps_[k] = sampleAt(first + (long) k, c);
        y = dot(weights_, taps_, n);
      }
    }
    if (!sample_->streamer) {
      heldChannel_ = c;
      held_ = y;
    }
    return y;
  }

  /** Move on by one output frame. Call once per frame, after reading every
   *  channel. */
  inline void advance() {
    heldChannel_ = kNothingHeld;
    if (!active_ || !sample_) return;
    position_ += rate_;

    if (looping()) {
      const double end = (double) sample_->loopEnd;
      const double start = (double) sample_->loopStart;
      const double length = end - start;
      // A loop of one frame is a loop too: it holds that frame. It was taken
      // for none, and the voice played on past it into silence, active and
      // silent until its note-off.
      if (length >= 1.0) {
        // Many lengths past the end -- a short loop played far up -- is one
        // division, not that many subtractions. Subtracting a length at a
        // time took 8 ms a frame for a 100-frame loop 400 semitones up, and
        // for ever once the position was too large for a length to change it.
        if (position_ - end >= (double) kMaxReach * length) {
          position_ -= std::floor((position_ - end) / length) * length;
          passes_ = kMaxReach + 1;
        }
        while (position_ >= end) {
          position_ -= length;
          if (passes_ <= kMaxReach) ++passes_; // past the reach, more says nothing
        }
      }
      return;
    }
    if (position_ >= (double) sample_->numFrames) active_ = false;
  }

private:
  // ── The band-limited read ──
  static constexpr int kHalfWidth = 16;         // zero crossings each side
  static constexpr double kCutoff = 0.9;        // of the file's Nyquist
  static constexpr double kMaxStretch = 4.0;    // two octaves up; past it, folds
  static constexpr int kTableResolution = 512;  // per zero crossing
  /** The furthest a tap reaches from the position, in frames. */
  static constexpr long kMaxReach = (long) (kHalfWidth * kMaxStretch / kCutoff) + 1;
  static constexpr int kMaxTaps = 2 * (int) kMaxReach + 1;

  /** sinc(u) * Kaiser(u / kHalfWidth), u in zero crossings, 0 .. kHalfWidth.
   *  Built once, on first use; setSample() and setSampleRate() make that use,
   *  off the audio thread. */
  struct KernelTable {
    /** The kernel at i / kTableResolution zero crossings; 0 from
     *  kHalfWidth on. */
    float g[kHalfWidth * kTableResolution + 2];
    /** Fills the table: runs once, the first time kernelTable() is called. */
    KernelTable() {
      const double pi = 3.14159265358979323846;
      const int n = kHalfWidth * kTableResolution + 2;
      for (int i = 0; i < n; ++i) {
        const double u = (double) i / (double) kTableResolution;
        const double sinc = u < 1e-12 ? 1.0 : std::sin(pi * u) / (pi * u);
        const double r = u / (double) kHalfWidth;
        g[i] = r >= 1.0 ? 0.0f : (float) (sinc * special::kaiser(r, 8.6));
      }
    }
  };
  static const KernelTable& kernelTable() {
    static const KernelTable table;
    return table;
  }
  static float kernelAt(const KernelTable& table, double crossings) {
    const double u = crossings * (double) kTableResolution;
    const int i = (int) u;
    if (i >= kHalfWidth * kTableResolution) return 0.0f;
    const float t = (float) (u - (double) i);
    return table.g[i] + t * (table.g[i + 1] - table.g[i]);
  }

  /** At or below the file's own rate the cutoff is fixed, so the weights
   *  depend on the fraction alone: worked out once for 256 fractions, each
   *  normalised, and blended between neighbours. Computed per frame they
   *  cost seven times the cubic read; from the table, four lanes at a time,
   *  64 voices take 1.6 times the cubic read's time in mono and 1.8 in
   *  stereo. */
  static constexpr int kPhases = 256;
  static constexpr long kFixedBefore = (long) (kHalfWidth / kCutoff); // taps before floor(pos)
  static constexpr int kFixedTaps = 2 * (int) kFixedBefore + 2;
  /** The sinc weights for a read at or below the file's own rate: kFixedTaps
   *  of them for each of kPhases + 1 fractions, every row normalised to sum
   *  to one (kPhases says why there is a table). Built once, on first use,
   *  which setSampleRate() makes off the audio thread. */
  struct PhaseTable {
    /** w[ph] are the weights for a fraction of ph / kPhases. */
    float w[kPhases + 1][kFixedTaps];
    /** Fills the table: runs once, the first time phaseTable() is called. */
    PhaseTable() {
      const KernelTable& g = kernelTable();
      for (int ph = 0; ph <= kPhases; ++ph) {
        const double frac = (double) ph / (double) kPhases;
        double sum = 0.0;
        for (int k = 0; k < kFixedTaps; ++k) {
          const double x = (double) ((long) k - kFixedBefore) - frac;
          w[ph][k] = kernelAt(g, std::fabs(x) * kCutoff);
          sum += (double) w[ph][k];
        }
        for (int k = 0; k < kFixedTaps; ++k) w[ph][k] = (float) ((double) w[ph][k] / sum);
      }
    }
  };
  static const PhaseTable& phaseTable() {
    static const PhaseTable table;
    return table;
  }

  /** The taps and weights for a read at `pos`: a sinc low-passed at kCutoff
   *  of the file's Nyquist, or lower by the rate when the file is played
   *  faster than the session, so nothing past the session's Nyquist folds.
   *  Normalised to unity at DC, so a constant reads as itself. */
  void prepareWeights(double pos) const {
    weightsFor_ = pos;
    weightsRate_ = rate_;
    if (rate_ <= 1.0) {
      const double whole = std::floor(pos);
      const float scaled = (float) (pos - whole) * (float) kPhases;
      int ph = (int) scaled;
      if (ph >= kPhases) ph = kPhases - 1;
      const float blend = scaled - (float) ph;
      const float* a = phaseTable().w[ph];
      const float* b = phaseTable().w[ph + 1];
      static_assert(kFixedTaps % 4 == 0, "the blend runs four taps at a time");
      const simd::Vec4f t = simd::Vec4f::splat(blend);
      for (int k = 0; k < kFixedTaps; k += 4) {
        const simd::Vec4f wa = simd::Vec4f::load(a + k);
        (wa + (simd::Vec4f::load(b + k) - wa) * t).store(weights_ + k);
      }
      weightsFirst_ = (long) whole - kFixedBefore;
      weightsCount_ = kFixedTaps;
      return;
    }
    const KernelTable& table = kernelTable();
    const double stretch = rate_ < kMaxStretch ? rate_ : kMaxStretch;
    const double cutoff = kCutoff / stretch;
    const double reach = (double) kHalfWidth / cutoff;
    long first = (long) std::ceil(pos - reach);
    long last = (long) std::floor(pos + reach);
    if (last - first + 1 > kMaxTaps) last = first + kMaxTaps - 1;
    const int n = (int) (last - first + 1);
    double sum = 0.0;
    for (int k = 0; k < n; ++k) {
      const float w = kernelAt(table, std::fabs((double) (first + k) - pos) * cutoff);
      weights_[k] = w;
      sum += (double) w;
    }
    const float norm = sum > 1e-9 ? (float) (1.0 / sum) : 0.0f;
    for (int k = 0; k < n; ++k) weights_[k] *= norm;
    weightsFirst_ = first;
    weightsCount_ = n;
  }

  /** The one dot product every path uses: four lanes, then what is left, in
   *  a fixed order. Two summation orders for the same taps -- vector for one
   *  path, scalar for another -- differed in the last bits, and a voice
   *  streamed from disk stopped matching the same voice from memory. */
  static float dot(const float* w, const float* v, int n) {
    simd::Vec4f acc = simd::Vec4f::zero();
    int k = 0;
    for (; k + 4 <= n; k += 4) acc = acc + simd::Vec4f::load(w + k) * simd::Vec4f::load(v + k);
    float sum = acc.sum();
    for (; k < n; ++k) sum += w[k] * v[k];
    return sum;
  }

  /** No tap crosses a seam of the loop that sampleAt() would wrap. */
  bool tapsClearOfLoop(long first, int n) const {
    if (looping() && first + n > (long) sample_->loopEnd) return false;
    return !loopPlayed() || first >= (long) sample_->loopStart;
  }

  /** Whether the voice has been round its loop -- still looping, or released
   *  from it -- so that what lies behind loopStart is not only the attack. */
  bool loopPlayed() const {
    return passes_ > 0 && sample_ && sample_->looping && sample_->loopEnd > sample_->loopStart &&
           sample_->loopEnd <= sample_->numFrames;
  }

  bool looping() const {
    return sample_ && sample_->looping && !releasing_ && sample_->loopEnd > sample_->loopStart &&
           sample_->loopEnd <= sample_->numFrames;
  }

  /** Read one sample, wrapping INSIDE the loop rather than off its end.
   *
   *  This is the whole reason the interpolator is given four points through a
   *  function instead of raw pointers: at the loop boundary two of them come
   *  from the other side of the loop, and reading straight past the end
   *  splices in whatever follows: a click once per cycle. */
  inline float sampleAt(long frame, size_t channel) const {
    const long total = (long) sample_->numFrames;
    if (looping() || loopPlayed()) {
      const long start = (long) sample_->loopStart;
      const long end = (long) sample_->loopEnd;
      const long length = end - start;
      if (length >= 1) {
        // Ahead, past the loop's end, is the loop again -- while it loops.
        // Released, ahead is the file's own tail.
        if (looping())
          while (frame >= end) frame -= length;
        // Only once the loop has actually been round at least once.
        //
        // This used to wrap unconditionally, which meant every frame before
        // loopStart was pulled forward into the loop -- so a looping sample
        // NEVER PLAYED ITS ATTACK. A file beginning 0.0000, 0.0028, 0.0056
        // came out as -0.6113, -0.6099, -0.6086: material from the middle of
        // the sustain, from the first sample of the note. On a sampled
        // instrument that is the most audible thing there is, and it was
        // invisible until a streamed voice and a resident one were asked to
        // agree.
        //
        // What precedes loopStart is the ATTACK the first time through and
        // the loop's own tail every time after, and only the voice knows
        // which pass it is on. Counted, not flagged: going back from this
        // pass, the first passes_ * length frames are loop and the attack is
        // behind them. One flag for "has wrapped" pulled every tap before
        // loopStart into the loop from the first wrap on, and a loop shorter
        // than the sinc's reach (18 frames, 72 at four times the rate) read
        // loop where it had played attack: 0.085 out of 0.6 on an 8-frame
        // loop at 3.68 times the rate. The same held after noteOff, where
        // the taps behind loopStart went back to reading the attack.
        if (loopPlayed() && frame < start) {
          const long back = start - frame;
          const long looped = passes_ * length;
          frame += back <= looped ? ((back + length - 1) / length) * length : looped;
        }
      }
    }
    if (frame < 0 || frame >= total) return 0.0f;
    if (sample_->streamer) return streamedSample(frame, channel);
    return sample_->interleaved[(size_t) frame * sample_->numChannels + channel];
  }

  /** One sample from a file, through a cache.
   *
   *  The streamer already holds its window in memory, so this could in
   *  principle point straight at it -- except that the window is two halves
   *  which are not contiguous in the order the file is read, so a range
   *  crossing the seam is two pointers. Copying a block into a flat cache
   *  costs one memcpy per thousand samples and gives back plain indexing,
   *  which is what the loop-wrapping arithmetic above already assumes.
   *
   *  Never blocks. If the streamer cannot serve the frames -- the worker is
   *  behind, or nobody has called service() -- the cache stays short and this
   *  returns silence, which the streamer has already counted as an underrun. */
  inline float streamedSample(long frame, size_t channel) const {
    const size_t channels = sample_->numChannels ? sample_->numChannels : 1;

    // The loop's opening frames, held permanently, because a wrapped read
    // lands there and the rolling cache is somewhere else entirely.
    if (loopPrefixFilled_ > 0 && (uint64_t) frame >= loopPrefixStart_ &&
        (uint64_t) frame < loopPrefixStart_ + loopPrefixFilled_) {
      const size_t at = (size_t) ((uint64_t) frame - loopPrefixStart_);
      return loopPrefix_[at * channels + channel];
    }

    if (cacheFilled_ == 0 || (uint64_t) frame < cacheStart_ ||
        (uint64_t) frame >= cacheStart_ + cacheFilled_) {
      // Refill from twice the kernel's reach BEFORE the point being asked
      // for: the read reaches backwards, and a cache that starts at the
      // requested frame -- the newest tap, when the read runs off the end --
      // refills again for the oldest tap of the very same frame.
      const uint64_t back = (uint64_t) (2 * kMaxReach);
      const uint64_t from = (uint64_t) frame > back ? (uint64_t) frame - back : 0;
      cacheStart_ = from;
      cacheFilled_ = sample_->streamer->read(from, cache_.data(), kCacheFrames);
      if (cacheFilled_ == 0) return 0.0f;
    }
    const size_t at = (size_t) ((uint64_t) frame - cacheStart_);
    if (at >= cacheFilled_) return 0.0f;
    return cache_[at * channels + channel];
  }

  /** A thousand frames is 21 ms at 48 kHz: long enough that the refill is
   *  amortised to nothing, short enough that a voice costs kilobytes. */
  static constexpr size_t kCacheFrames = 1024;
  /** Only the interpolator's reach is needed here: the most frames past the
   *  loop's end a read wraps onto, with room to spare. */
  static constexpr size_t kLoopPrefixFrames = (size_t) kMaxReach + 8;

  void updateRate() {
    heldChannel_ = kNothingHeld; // every caller changes what a read returns
    if (!sample_ || !sample_->isValid()) {
      rate_ = 0.0;
      return;
    }
    // Two independent rate changes, and forgetting either is a tuning bug:
    // the musical interval, and the file-versus-session rate.
    const double semitones =
        (double) (note_ - sample_->rootNote) + (double) sample_->tuningCents / 100.0 +
        (double) pitchOffset_;
    const double pitch = std::pow(2.0, semitones / 12.0);
    rate_ = pitch * (sample_->sampleRate / hostRate_);
    // A rate that is not a number stops the voice where it is, and one past
    // kMaxRate is clamped to it. A NaN pitch offset -- per-note expression
    // worked out from a NaN -- made the position NaN, the band-limited read
    // indexed its kernel table at (int) NaN and the host crashed; an infinite
    // one made the position infinite and the loop wrap never ended.
    if (!(rate_ >= 0.0)) rate_ = 0.0;
    else if (rate_ > kMaxRate) rate_ = kMaxRate;
  }
  /** The fastest a voice reads: sixteen octaves up. Far past anything with a
   *  pitch -- the band-limited read stops widening at two -- and finite, so
   *  the position always is. */
  static constexpr double kMaxRate = 65536.0;

  const SampleData* sample_ = nullptr;
  Interpolation interpolation_ = Interpolation::Sinc;
  /** The band-limited read's weights for the frame in hand, shared by its
   *  channels. Mutable for the same reason as the cache below. */
  mutable float weights_[kMaxTaps]{};
  mutable float taps_[kMaxTaps]{}; // one channel's taps, gathered
  mutable long weightsFirst_ = 0;
  mutable int weightsCount_ = 0;
  mutable double weightsFor_ = -1.0, weightsRate_ = -1.0;
  /** renderChannel()'s last read and its channel, until anything changes. */
  static constexpr size_t kNothingHeld = ~size_t(0);
  mutable size_t heldChannel_ = kNothingHeld;
  mutable float held_ = 0.0f;
  /** Mutable because reading a sample is logically const -- the cache is how
   *  the answer is fetched, not part of what the voice IS. */
  mutable std::vector<float> cache_;
  /** How many times playback has been round the loop (up to the kernel's
   *  reach), which decides how much of what lies before loopStart is the
   *  loop's tail and how much the attack. */
  long passes_ = 0;

  mutable uint64_t cacheStart_ = 0;
  mutable size_t cacheFilled_ = 0;
  std::vector<float> loopPrefix_;
  uint64_t loopPrefixStart_ = 0;
  size_t loopPrefixFilled_ = 0;

  double position_ = 0.0, rate_ = 1.0, hostRate_ = 48000.0;
  int note_ = 60;
  float pitchOffset_ = 0.0f;
  bool active_ = false, releasing_ = false;
};

} // namespace sonore
