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
  const float* interleaved = nullptr;
  /** Streamed from disk instead of resident, for material too big to hold.
   *
   *  When this is set `interleaved` is not used. One streamer per PLAYING
   *  VOICE, not per sample: a streamer has one position, and two voices
   *  playing the same file at different points would fight over it. */
  SampleStreamer* streamer = nullptr;
  size_t numFrames = 0;
  uint16_t numChannels = 1;
  double sampleRate = 48000.0;

  /** The note the recording actually sounds. Playing it at this note means
   *  playing it at its own rate. */
  int rootNote = 60;
  /** Fine tuning of the recording itself, in cents. */
  float tuningCents = 0.0f;

  /** Loop bounds in frames. A zero-length loop means one-shot. */
  size_t loopStart = 0;
  size_t loopEnd = 0;
  bool looping = false;

  bool isValid() const {
    return (interleaved != nullptr || streamer != nullptr) && numFrames > 0 && numChannels > 0;
  }
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
  void setSampleRate(double hostSampleRate) {
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
    wrapped_ = false;
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
  void noteOff() { releasing_ = true; }

  void stop() {
    active_ = false;
    releasing_ = false;
  }

  bool isActive() const { return active_; }
  int note() const { return note_; }
  double position() const { return position_; }

  /** Extra pitch offset in semitones, for per-note expression or vibrato. */
  void setPitchOffset(float semitones) {
    pitchOffset_ = semitones;
    updateRate();
  }

  /** One frame for one channel, advancing only on channel 0 so a caller can
   *  read every channel at the same position. */
  inline float renderChannel(size_t channel) const {
    if (!active_ || !sample_ || !sample_->isValid()) return 0.0f;
    const size_t channels = sample_->numChannels;
    const size_t c = channel < channels ? channel : channels - 1;

    const double pos = position_;
    const long i1 = (long) pos;
    const float frac = (float) (pos - (double) i1);

    return DelayLine<2>::catmullRomTap(sampleAt(i1 - 1, c), sampleAt(i1, c),
                                       sampleAt(i1 + 1, c), sampleAt(i1 + 2, c), frac);
  }

  /** Move on by one output frame. Call once per frame, after reading every
   *  channel. */
  inline void advance() {
    if (!active_ || !sample_) return;
    position_ += rate_;

    if (looping()) {
      const double end = (double) sample_->loopEnd;
      const double start = (double) sample_->loopStart;
      const double length = end - start;
      if (length > 1.0)
        while (position_ >= end) {
          position_ -= length;
          wrapped_ = true;
        }
      return;
    }
    if (position_ >= (double) sample_->numFrames) active_ = false;
  }

private:
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
    if (looping()) {
      const long start = (long) sample_->loopStart;
      const long end = (long) sample_->loopEnd;
      const long length = end - start;
      if (length > 1) {
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
        // which pass it is on.
        if (wrapped_)
          while (frame < start) frame += length;
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
      // Refill from one frame BEFORE the point being asked for, because the
      // interpolator reaches backwards and a cache that starts exactly at the
      // requested frame refills again on the very next call.
      const uint64_t from = (uint64_t) frame > 0 ? (uint64_t) frame - 1 : 0;
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
  /** Only the interpolator's reach is needed here -- three points at most
   *  wrap across a loop boundary. Eight is that with room to spare. */
  static constexpr size_t kLoopPrefixFrames = 8;

  void updateRate() {
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
  }

  const SampleData* sample_ = nullptr;
  /** Mutable because reading a sample is logically const -- the cache is how
   *  the answer is fetched, not part of what the voice IS. */
  mutable std::vector<float> cache_;
  /** Whether playback has been round the loop yet, which decides whether
   *  what lies before loopStart is the attack or the loop's tail. */
  bool wrapped_ = false;

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
