// SPDX-License-Identifier: Apache-2.0
// Sonore SDK. The pull model: sources that are ASKED for their next block.
//
// A plugin is pushed: the host hands it a block and expects it filled, and
// every format's process call is that shape. An application playing a file is
// the other shape. Something at the device end asks for the next block, and
// that request travels backwards through a chain -- a transport asks a
// resampler, the resampler asks a read-ahead buffer, the buffer asks a file --
// each link pulling from the one behind it. The reference builds its playback
// on that model, and code ported from it (a file player, a DJ deck, a sampler's
// preview, a test harness) is written against these class names. This header
// is the target for that code: the reference's shapes, built on the SDK's own
// parts rather than beside them.
//
//   AudioFileReader (audiostream.h)  ->  AudioFormatReaderSource
//   WavData (wav.h / audiofile.h)    ->  MemoryAudioSource
//   Biquad (dsp.h)                   ->  IIRFilterAudioSource
//   Reverb (effects.h)               ->  ReverbAudioSource
//   WindowedSincKernel + SvfSection  ->  ResamplingAudioSource
//   ChangeBroadcaster                ->  AudioTransportSource's notifications
//   the backends' RenderFn           ->  AudioSourcePlayer
//
// ── The one rule, and how each class keeps it ───────────────────────────────
//
// getNextAudioBlock() runs on the audio thread, and on the audio thread nothing
// allocates, locks, waits or touches a file. The reference keeps that rule for
// the samples and breaks it for the bookkeeping: its mixer, transport,
// remapper, filter and reverb sources each take a lock inside the callback so
// that another thread can change them, which is a priority inversion waiting
// for a busy message thread. Nothing here does that. What changes while audio
// runs is handed over in one of three ways, chosen by what it is:
//
//   A few numbers (a gain, a ratio, a frequency)   one atomic each.
//   A small table (coefficients, reverb settings,  a seqlock (SeqBox below): the
//   a channel map)                                 audio thread takes a
//                                                  consistent copy or, if a
//                                                  writer is mid-update, keeps
//                                                  the copy it had for one more
//                                                  block. It never waits.
//   Something with a LIFETIME (the mixer's input   published by atomic pointer,
//   list, the transport's chain, the player's      and the OLD one freed only
//   source)                                        after the audio thread is
//                                                  provably not inside a block
//                                                  that saw it (BlockEpoch).
//                                                  The thread making the change
//                                                  waits; the audio thread
//                                                  never does.
//
// The MixerAudioSource case is the one the reference documents as needing a
// lock, so it is worth saying plainly: there is no lock and no try-lock. A
// try-lock that outputs silence on contention would turn every add or remove
// into a dropout of the whole mix, which is the failure the lock was there to
// avoid. The list is swapped whole, and the thread that swapped it waits out
// the block in flight before it frees anything.
//
// Every function that runs on the audio thread says "[audio thread]" at the top
// of its comment, and scripts/audio-thread-rules.mjs reads those bodies for
// allocation, locks, waits and file calls. AudioFormatReaderSource is the
// exception by necessity and is marked [reading thread] instead: reading a file
// is a seek and an fread. Behind a BufferingAudioSource -- which is what
// AudioTransportSource puts it behind when given a read-ahead size -- that read
// happens on the ReadAheadThread, which is the point of having one.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#include "audio.h"
#include "audiostream.h"
#include "broadcaster.h"
#include "denormals.h"
#include "dsp.h"
#include "interpolation.h"
#include "resample.h"
#include "wav.h"

namespace sonore {

// ── Where a source writes ───────────────────────────────────────────────────

/**
 * A region of a buffer for a source to fill: `numSamples` frames starting at
 * `startSample`, across every channel of `buffer`.
 *
 * The reference's struct, with the SDK's AudioBlock where it has its owning
 * buffer: AudioBlock is a view over channel pointers somebody else owns, which
 * is exactly what a device callback, a ring and a scratch area all are. A
 * source must write EVERY channel of the region -- silence where it has
 * nothing -- because the caller's buffer holds whatever the last block left in
 * it, and a source that skipped a channel would replay that.
 */
struct AudioSourceChannelInfo {
  AudioBlock<float>* buffer = nullptr; ///< The buffer the region is in, not owned; null for none.
  int startSample = 0;                 ///< The region's first frame within `buffer`, 0-based.
  int numSamples = 0;                  ///< The region's length, in frames.

  /** No buffer and no region: a source handed it writes nothing. */
  AudioSourceChannelInfo() = default;
  /** `count` frames of `bufferToUse` from frame `start`, which the caller
   *  keeps inside the buffer: nothing here checks. */
  AudioSourceChannelInfo(AudioBlock<float>* bufferToUse, int start, int count) noexcept
      : buffer(bufferToUse), startSample(start), numSamples(count) {}
  /** The whole of `bufferToUse`. */
  explicit AudioSourceChannelInfo(AudioBlock<float>& bufferToUse) noexcept
      : buffer(&bufferToUse), startSample(0), numSamples((int) bufferToUse.getNumSamples()) {}

  /** Every channel of the buffer, which the region spans; 0 with no buffer. */
  int numChannels() const noexcept { return buffer ? (int) buffer->getNumChannels() : 0; }

  /** Channel `c`'s first sample OF THE REGION -- not of the buffer, which is
   *  the mistake every hand-written loop over startSample makes once. */
  float* channel(int c) const noexcept {
    return buffer->getChannelPointer((size_t) c) + startSample;
  }

  /** The region as a block of its own, for the block operations. */
  AudioBlock<float> region() const noexcept {
    if (!buffer || numSamples <= 0) return AudioBlock<float>();
    return buffer->getSubBlock((size_t) startSample, (size_t) numSamples);
  }

  /** Silence the region, in every channel, and nothing outside it. */
  void clearActiveBufferRegion() const noexcept {
    if (buffer && numSamples > 0) region().clear();
  }
};

// ── The two interfaces ──────────────────────────────────────────────────────

/**
 * Something that can be asked for the next block.
 *
 * The lifecycle is the reference's: prepareToPlay() before the first block,
 * from a thread that is not running blocks at the same time; then any number
 * of getNextAudioBlock() calls from the audio thread; then releaseResources().
 * prepareToPlay() is where everything a source needs on the audio thread is
 * allocated, because getNextAudioBlock() will not.
 */
class AudioSource {
public:
  /** The widest a source in this family goes, and the widest file
   *  AudioFileReader opens. Per-channel state (a filter per channel, scratch
   *  for a mix) is sized for this many when it cannot be sized from the
   *  buffer it will be handed -- which it cannot, because the buffer arrives
   *  on the audio thread. */
  static constexpr int kMaxChannels = 32;

  /** Not while a block is being pulled from it. */
  virtual ~AudioSource() = default;

  /** Get ready for blocks of about `samplesPerBlockExpected` frames at
   *  `sampleRate` Hz, allocating here whatever the audio thread will need.
   *  From a thread that is not pulling blocks at the time; called again,
   *  with blocks stopped, when either number changes. */
  virtual void prepareToPlay(int samplesPerBlockExpected, double sampleRate) = 0;
  /** Free what prepareToPlay() allocated, once blocks have stopped. The next
   *  block comes after another prepareToPlay(). */
  virtual void releaseResources() = 0;

  /** [audio thread] Fill every channel of the region. Blocks of ANY length
   *  may arrive, including ones longer than samplesPerBlockExpected: a host
   *  promise about block size is a hint, and every source here chunks rather
   *  than trusting it. */
  virtual void getNextAudioBlock(const AudioSourceChannelInfo& bufferToFill) = 0;
};

/**
 * A source with a position: a file, a buffer, a transport.
 *
 * Positions are in the source's own samples. setNextReadPosition() may be
 * called from a thread other than the one pulling blocks -- every source in
 * this header keeps its position in an atomic for that reason -- but a source
 * written elsewhere need not, which is why AudioTransportSource only calls it
 * from the audio thread while playing.
 */
class PositionableAudioSource : public AudioSource {
public:
  /** Where the next block starts, in the source's own samples from 0. Every
   *  source here takes a negative position as 0 and a position past the end
   *  as silence. */
  virtual void setNextReadPosition(int64_t newPosition) = 0;
  /** Where the next block will start, in the source's own samples. Every
   *  source here answers within the length while looping, and keeps
   *  counting past it while not, so a caller can see the end go by. */
  virtual int64_t getNextReadPosition() const = 0;
  /** The length, in the source's own samples; 0 when there is nothing to
   *  play. */
  virtual int64_t getTotalLength() const = 0;
  /** True when reaching the end starts it again from 0. */
  virtual bool isLooping() const = 0;
  /** Loop, or stop at the end. The default ignores it, for a source that
   *  cannot loop. */
  virtual void setLooping(bool shouldLoop) { (void) shouldLoop; }
};

// ── Machinery ────────────────────────────────────────────────────────────────

namespace sourcedetail {

/** The reference's (pointer, deleteWhenDone) pair, as one member. */
template <typename T>
class MaybeOwned {
public:
  MaybeOwned() = default;
  MaybeOwned(T* pointer, bool takeOwnership) : ptr_(pointer), owned_(takeOwnership ? pointer : nullptr) {}
  T* get() const noexcept { return ptr_; }
  T* operator->() const noexcept { return ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
  T* ptr_ = nullptr;
  std::unique_ptr<T> owned_;
};

/**
 * How a thread that is not the audio thread learns the audio thread has
 * stopped looking at something it unpublished.
 *
 * The audio thread bumps a counter on the way into a block and again on the
 * way out, so the counter is odd exactly while a block is in flight. A thread
 * that has just swapped a pointer away then reads the counter, behind a full
 * fence (see blockInFlight()). Those two orders cannot both be missed -- either
 * the block that is starting sees the new pointer, or the swapping thread sees
 * the odd count and waits for it to change -- so once the wait returns,
 * nothing on the audio thread can still hold the old one. It is Dekker's
 * mutual exclusion with one side made wait-free: the audio thread never loops,
 * never sleeps, and never learns anybody was waiting.
 *
 * The waiting side spins with yields, then sleeps in 100 us steps: the wait is
 * bounded by one block, which is microseconds of copying in the common case.
 */
class BlockEpoch {
public:
  /** [audio thread] */
  void enter() noexcept {
    owner_.store(std::this_thread::get_id(), std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_seq_cst);
  }
  /** [audio thread] */
  void leave() noexcept { count_.fetch_add(1, std::memory_order_seq_cst); }

  /**
   * [not the audio thread] Wait out any block that might have seen what was
   * just unpublished. Returns false, without waiting, when called from INSIDE
   * such a block -- a source asked to remove itself from inside its own
   * callback -- because waiting there would be waiting for oneself. The
   * caller then must not free what it unpublished; see MixerAudioSource.
   */
  bool waitForBlocksInFlight() const {
    const uint64_t seen = blockInFlight();
    if (isInside(seen)) return false;
    waitUntilOver(seen);
    return true;
  }

  /** The same in its three parts, for a caller that must not hold a lock
   *  while it waits (MixerAudioSource). The block in flight now -- read
   *  AFTER the unpublishing store -- as a tag: odd while one is.
   *
   *  Behind a full fence, because not every unpublishing store is seq_cst:
   *  BufferingAudioSource publishes its ring's range through a SeqBox, whose
   *  last store is a release, and a release store followed by a load of
   *  another location is exactly the pair x86's store buffer reorders. Then
   *  the count is read as even while a block enters and reads the OLD range,
   *  and the reader overwrites slots that block is copying. Measured: about
   *  one in two thousand such hand-overs without the fence, none with it. */
  uint64_t blockInFlight() const noexcept {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    return count_.load(std::memory_order_seq_cst);
  }
  /** Whether the block `seen` names has finished, or there was none. */
  bool isOver(uint64_t seen) const noexcept {
    return (seen & 1u) == 0 || count_.load(std::memory_order_acquire) != seen;
  }
  /** Whether the calling thread is inside the block `seen` names. */
  bool isInside(uint64_t seen) const noexcept {
    return (seen & 1u) != 0 && owner_.load(std::memory_order_relaxed) == std::this_thread::get_id();
  }
  /** [not the audio thread] Until isOver(seen). */
  void waitUntilOver(uint64_t seen) const {
    for (int spins = 0; !isOver(seen); ++spins) {
      if (spins < 64) std::this_thread::yield();
      else std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }

private:
  std::atomic<uint64_t> count_{0};
  std::atomic<std::thread::id> owner_{std::thread::id()};
};

/** [audio thread] enter() and leave() as a scope. */
class InBlock {
public:
  explicit InBlock(BlockEpoch& epoch) noexcept : epoch_(epoch) { epoch_.enter(); }
  ~InBlock() { epoch_.leave(); }
  InBlock(const InBlock&) = delete;
  InBlock& operator=(const InBlock&) = delete;

private:
  BlockEpoch& epoch_;
};

/**
 * A small value written by one thread at a time and read wait-free by the
 * audio thread -- filter coefficients, reverb settings, a channel map.
 *
 * A seqlock over TWO copies, with every word an atomic so the read is not a
 * data race in the C++ sense. The writer fills the copy readers are not on
 * -- its own sequence odd while it writes, even after -- and only then
 * points them at it. So a reader always finds the published copy whole: a
 * writer stopped halfway, preempted under load for milliseconds, is
 * writing the other one. With one copy the reader found the sequence odd
 * for as long as the writer was stopped, every try, and the buffering
 * source's block -- which cannot wait -- went out as silence with the
 * audio sitting in its ring. A reader loses now only if the writer laps it
 * twice inside one copy; then it does NOT retry until it wins -- that would
 * be a wait -- it keeps the value it had and tries again next block.
 * Writers serialise among themselves (each owner holds a mutex on the
 * writing side, which the audio thread never touches).
 */
template <typename T>
class SeqBox {
  static_assert(std::is_trivially_copyable<T>::value, "a SeqBox holds plain data");
  static constexpr size_t kWords = (sizeof(T) + 7) / 8;

public:
  explicit SeqBox(const T& initial = T()) { write(initial); }

  /** [one writer at a time] */
  void write(const T& value) noexcept {
    uint64_t words[kWords] = {};
    std::memcpy(words, &value, sizeof(T));
    const uint32_t s = seq_.load(std::memory_order_relaxed);
    Slot& slot = slots_[((s >> 1) + 1u) & 1u]; // the copy readers are not on
    const uint32_t g = slot.seq.load(std::memory_order_relaxed);
    slot.seq.store(g + 1u, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (size_t i = 0; i < kWords; ++i) slot.words[i].store(words[i], std::memory_order_relaxed);
    slot.seq.store(g + 2u, std::memory_order_release);
    seq_.store(s + 2u, std::memory_order_release); // and now they are
  }

  /** [audio thread] True when `out` received a consistent copy. `version`
   *  changes with every write, so a reader can tell a new value from the
   *  one it already applied without comparing the bytes. */
  bool tryRead(T& out, uint32_t* version = nullptr) const noexcept {
    const uint32_t s = seq_.load(std::memory_order_acquire);
    const Slot& slot = slots_[(s >> 1) & 1u];
    const uint32_t before = slot.seq.load(std::memory_order_acquire);
    if (before & 1u) return false; // lapped: the writer is back on this copy
    uint64_t words[kWords];
    for (size_t i = 0; i < kWords; ++i) words[i] = slot.words[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (slot.seq.load(std::memory_order_relaxed) != before) return false;
    std::memcpy(&out, words, sizeof(T));
    if (version) *version = s;
    return true;
  }

private:
  struct Slot {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint64_t> words[kWords];
  };
  /** Twice the writes so far; which copy is published is (seq_ / 2) % 2. */
  std::atomic<uint32_t> seq_{0};
  Slot slots_[2];
};

/** Planar float storage, owned: the scratch a source sizes in prepareToPlay()
 *  and hands out as AudioBlocks. */
class PlanarBuffer {
public:
  void setSize(int channels, int frames) {
    channels_ = channels > 0 ? channels : 0;
    frames_ = frames > 0 ? frames : 0;
    data_.assign((size_t) channels_ * (size_t) frames_, 0.0f);
    pointers_.assign((size_t) channels_, nullptr);
    for (int c = 0; c < channels_; ++c) pointers_[(size_t) c] = data_.data() + (size_t) c * (size_t) frames_;
  }
  void release() {
    std::vector<float>().swap(data_);
    std::vector<float*>().swap(pointers_);
    channels_ = frames_ = 0;
  }
  int numChannels() const noexcept { return channels_; }
  int numFrames() const noexcept { return frames_; }
  float* channel(int c) const noexcept { return pointers_[(size_t) c]; }
  float* const* channels() const noexcept { return pointers_.data(); }

private:
  std::vector<float> data_;
  std::vector<float*> pointers_;
  int channels_ = 0, frames_ = 0;
};

/** [audio thread] The reference's gain ramp: `from` on the first sample,
 *  stepping by (to - from) / n, so the next block starting at `to` continues
 *  the line instead of repeating its last point. */
inline void rampGain(const AudioSourceChannelInfo& info, float from, float to) noexcept {
  const int n = info.numSamples;
  const int channels = info.numChannels();
  if (n <= 0 || channels <= 0) return;
  if (from == to) {
    if (from == 1.0f) return;
    for (int c = 0; c < channels; ++c) {
      float* d = info.channel(c);
      for (int i = 0; i < n; ++i) d[i] *= from;
    }
    return;
  }
  const float step = (to - from) / (float) n;
  for (int c = 0; c < channels; ++c) {
    float* d = info.channel(c);
    for (int i = 0; i < n; ++i) d[i] *= from + step * (float) i;
  }
}

/** Clamp a channel count to what the family supports. */
inline int clampChannels(int channels) noexcept {
  return channels < 1 ? 1 : (channels > AudioSource::kMaxChannels ? AudioSource::kMaxChannels : channels);
}

} // namespace sourcedetail

// ── Sources of audio ─────────────────────────────────────────────────────────

/**
 * Audio already in memory, played from a position, optionally looping.
 *
 * Built from a decoded file -- readAudioFile() hands back WavData for WAV,
 * AIFF, FLAC, MP3 and Ogg alike, so this is how a compressed file is played
 * -- or from planar channels, copied or borrowed.
 *
 * One deliberate difference from the reference: the position keeps counting
 * through the silence after the end, as a file reader's does, instead of
 * stopping at the length. A transport decides the stream has finished by
 * watching the position pass the length, and a position that stopped AT the
 * length would never pass it.
 */
class MemoryAudioSource : public PositionableAudioSource {
public:
  /** A decoded file's interleaved samples, de-interleaved and kept. */
  explicit MemoryAudioSource(const WavData& data, bool shouldLoop = false) {
    const int channels = data.numChannels > 0 ? (int) data.numChannels : 0;
    const size_t frames = data.numFrames();
    own(channels, (int64_t) frames);
    for (size_t i = 0; i < frames; ++i)
      for (int c = 0; c < channels; ++c)
        owned_[(size_t) c * frames + i] = data.samples[i * (size_t) channels + (size_t) c];
    looping_.store(shouldLoop, std::memory_order_relaxed);
  }

  /** Planar channels. `copyMemory` false BORROWS them: they must outlive this
   *  source and must not change while it plays. */
  MemoryAudioSource(const float* const* channels, int numChannels, int numSamples, bool copyMemory,
                    bool shouldLoop = false) {
    const int chans = channels && numChannels > 0 ? numChannels : 0;
    const int64_t frames = numSamples > 0 ? numSamples : 0;
    if (copyMemory) {
      own(chans, frames);
      for (int c = 0; c < chans; ++c)
        if (channels[c] && frames > 0)
          std::memcpy(owned_.data() + (size_t) c * (size_t) frames, channels[c], (size_t) frames * sizeof(float));
    } else {
      numChannels_ = chans;
      numSamples_ = frames;
      channels_.assign(channels, channels + chans);
    }
    looping_.store(shouldLoop, std::memory_order_relaxed);
  }

  /** How many channels it holds; a wider buffer's other channels get
   *  silence. */
  int getNumChannels() const noexcept { return numChannels_; }

  void prepareToPlay(int, double) override {}
  void releaseResources() override {}

  /** [audio thread] A copy and, past the end, silence. */
  void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
    if (!info.buffer || info.numSamples <= 0) return;
    const int n = info.numSamples;
    const int dstChannels = info.numChannels();
    int64_t start = position_.load(std::memory_order_relaxed);
    const bool loop = looping_.load(std::memory_order_relaxed);
    if (numSamples_ <= 0 || numChannels_ == 0) {
      info.clearActiveBufferRegion();
      advance(start, start + n, loop);
      return;
    }
    const int copied = dstChannels < numChannels_ ? dstChannels : numChannels_;
    int64_t pos = start;
    int done = 0;
    while (done < n) {
      int64_t at = pos;
      if (loop) at = pos % numSamples_;
      else if (pos >= numSamples_) break;
      const int64_t left = numSamples_ - at;
      const int run = (int) (left < (int64_t) (n - done) ? left : (int64_t) (n - done));
      for (int c = 0; c < copied; ++c)
        std::memcpy(info.channel(c) + done, channels_[(size_t) c] + at, (size_t) run * sizeof(float));
      for (int c = copied; c < dstChannels; ++c)
        std::memset(info.channel(c) + done, 0, (size_t) run * sizeof(float));
      done += run;
      pos += run;
    }
    if (done < n)
      for (int c = 0; c < dstChannels; ++c)
        std::memset(info.channel(c) + done, 0, (size_t) (n - done) * sizeof(float));
    advance(start, start + n, loop);
  }

  void setNextReadPosition(int64_t newPosition) override {
    position_.store(newPosition > 0 ? newPosition : 0, std::memory_order_relaxed);
  }
  int64_t getNextReadPosition() const override {
    const int64_t pos = position_.load(std::memory_order_relaxed);
    return looping_.load(std::memory_order_relaxed) && numSamples_ > 0 ? pos % numSamples_ : pos;
  }
  int64_t getTotalLength() const override { return numSamples_; }
  bool isLooping() const override { return looping_.load(std::memory_order_relaxed); }
  void setLooping(bool shouldLoop) override { looping_.store(shouldLoop, std::memory_order_relaxed); }

private:
  void own(int channels, int64_t frames) {
    numChannels_ = channels;
    numSamples_ = frames;
    owned_.assign((size_t) channels * (size_t) frames, 0.0f);
    channels_.resize((size_t) channels);
    for (int c = 0; c < channels; ++c) channels_[(size_t) c] = owned_.data() + (size_t) c * (size_t) frames;
  }

  /** [audio thread] Move on -- unless somebody seeked while this block was
   *  being read, in which case their position wins: a compare-and-swap, so a
   *  seek from another thread is never overwritten by the block it raced. */
  void advance(int64_t from, int64_t to, bool loop) noexcept {
    const int64_t next = loop && numSamples_ > 0 ? to % numSamples_ : to;
    position_.compare_exchange_strong(from, next, std::memory_order_relaxed);
  }

  std::vector<float> owned_;
  std::vector<const float*> channels_;
  int numChannels_ = 0;
  int64_t numSamples_ = 0;
  std::atomic<int64_t> position_{0};
  std::atomic<bool> looping_{false};
};

/**
 * A file, read on demand through AudioFileReader.
 *
 * The reader is a seek and an fread, so getNextAudioBlock() here is NOT for
 * the audio thread -- in the reference it is not either, it just does not say
 * so. Put it behind a BufferingAudioSource, or give AudioTransportSource a
 * read-ahead size and it does that itself, and the read happens on the
 * ReadAheadThread.
 *
 * A mono file into a stereo buffer plays on both sides, as the reference's
 * reader does it; a file narrower than a wider buffer leaves the extra
 * channels silent. Compressed formats: AudioFileReader streams WAV and AIFF
 * and refuses the rest by name, so decode those with readAudioFile() and use
 * MemoryAudioSource.
 */
class AudioFormatReaderSource : public PositionableAudioSource {
public:
  /** Play `reader`, an open AudioFileReader. With
   *  `deleteReaderWhenThisIsDeleted` this source owns it; without, it is
   *  borrowed and must outlive the source. A null or closed reader plays
   *  silence. */
  AudioFormatReaderSource(AudioFileReader* reader, bool deleteReaderWhenThisIsDeleted)
      : reader_(reader, deleteReaderWhenThisIsDeleted) {}

  /** The reader it plays, owned as the constructor said; null if none. */
  AudioFileReader* getAudioFormatReader() const noexcept { return reader_.get(); }

  void prepareToPlay(int samplesPerBlockExpected, double) override {
    reserve(samplesPerBlockExpected > 0 ? samplesPerBlockExpected : 512);
  }
  void releaseResources() override { std::vector<float>().swap(scratch_); }

  /** [reading thread] Reads the file. Not for the audio thread: see above. */
  void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
    if (!info.buffer || info.numSamples <= 0) return;
    const int n = info.numSamples;
    int64_t start = nextPlayPos_.load(std::memory_order_relaxed);
    const bool loop = looping_.load(std::memory_order_relaxed);
    const int64_t length = getTotalLength();
    int done = 0;
    int64_t pos = start;
    while (done < n && reader_ && reader_->isOpen() && length > 0) {
      int64_t at = pos;
      if (loop) at = pos % length;
      else if (pos >= length) break;
      const int64_t left = length - at;
      const int run = (int) (left < (int64_t) (n - done) ? left : (int64_t) (n - done));
      readInto(info, done, run, at);
      done += run;
      pos += run;
    }
    if (done < n) silence(info, done, n - done);
    const int64_t next = loop && length > 0 ? (start + n) % length : start + n;
    nextPlayPos_.compare_exchange_strong(start, next, std::memory_order_relaxed);
  }

  void setNextReadPosition(int64_t newPosition) override {
    nextPlayPos_.store(newPosition > 0 ? newPosition : 0, std::memory_order_relaxed);
  }
  int64_t getNextReadPosition() const override {
    const int64_t pos = nextPlayPos_.load(std::memory_order_relaxed);
    const int64_t length = getTotalLength();
    return looping_.load(std::memory_order_relaxed) && length > 0 ? pos % length : pos;
  }
  int64_t getTotalLength() const override {
    return reader_ && reader_->isOpen() ? (int64_t) reader_->numFrames() : 0;
  }
  bool isLooping() const override { return looping_.load(std::memory_order_relaxed); }
  void setLooping(bool shouldLoop) override { looping_.store(shouldLoop, std::memory_order_relaxed); }

private:
  void reserve(int frames) {
    const size_t channels = reader_ ? (size_t) reader_->numChannels() : 0;
    const size_t want = (size_t) frames * (channels > 0 ? channels : 1);
    if (scratch_.size() < want) scratch_.assign(want, 0.0f);
  }

  static void silence(const AudioSourceChannelInfo& info, int offset, int count) {
    for (int c = 0; c < info.numChannels(); ++c)
      std::memset(info.channel(c) + offset, 0, (size_t) count * sizeof(float));
  }

  /** `count` frames from file frame `at` into the region at `offset`, through
   *  interleaved scratch a prepared block at a time. */
  void readInto(const AudioSourceChannelInfo& info, int offset, int count, int64_t at) {
    const int fileChannels = (int) reader_->numChannels();
    const int dstChannels = info.numChannels();
    if (scratch_.empty()) reserve(512);
    const int chunk = (int) (scratch_.size() / (size_t) (fileChannels > 0 ? fileChannels : 1));
    int done = 0;
    while (done < count) {
      const int want = (count - done) < chunk ? (count - done) : chunk;
      const size_t got = reader_->read((uint64_t) (at + done), scratch_.data(), (size_t) want);
      // A short read is a file that ended early or an I/O error; either way
      // the rest of this run is silence, never stale scratch.
      for (int c = 0; c < dstChannels; ++c) {
        float* d = info.channel(c) + offset + done;
        // Mono feeds both sides of a stereo pair; anything else maps channel
        // to channel and leaves the extra destination channels silent.
        const int from = fileChannels == 1 && c < 2 ? 0 : c;
        if (from < fileChannels) {
          for (size_t i = 0; i < got; ++i) d[i] = scratch_[i * (size_t) fileChannels + (size_t) from];
          for (int i = (int) got; i < want; ++i) d[i] = 0.0f;
        } else {
          std::memset(d, 0, (size_t) want * sizeof(float));
        }
      }
      done += want;
      if ((int) got < want) {
        if (done < count) silence(info, offset + done, count - done);
        return;
      }
    }
  }

  sourcedetail::MaybeOwned<AudioFileReader> reader_;
  std::vector<float> scratch_;
  std::atomic<int64_t> nextPlayPos_{0};
  std::atomic<bool> looping_{false};
};

/**
 * A sine wave at a frequency and level: the reference's test tone.
 *
 * Every channel gets the same sample. The phase is kept in CYCLES and wrapped,
 * not in radians that grow without bound, so an hour in it is still in tune;
 * and a level change glides across the next block instead of stepping.
 */
class ToneGeneratorAudioSource : public AudioSource {
public:
  /** [any thread] The peak level, linear: 0.5 to start. A change glides
   *  across the next block. */
  void setAmplitude(float newAmplitude) noexcept { amplitude_.store(newAmplitude, std::memory_order_relaxed); }
  /** [any thread] The level last set, linear. */
  float getAmplitude() const noexcept { return amplitude_.load(std::memory_order_relaxed); }
  /** [any thread] The pitch in Hz: 1000 to start. Takes effect at the next
   *  block. */
  void setFrequency(double newFrequencyHz) noexcept { frequency_.store(newFrequencyHz, std::memory_order_relaxed); }
  /** [any thread] The pitch last set, in Hz. */
  double getFrequency() const noexcept { return frequency_.load(std::memory_order_relaxed); }

  void prepareToPlay(int, double sampleRate) override {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    phase_ = 0.0;
    lastAmplitude_ = amplitude_.load(std::memory_order_relaxed);
  }
  void releaseResources() override {}

  /** [audio thread] */
  void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
    if (!info.buffer || info.numSamples <= 0) return;
    const int n = info.numSamples;
    const int channels = info.numChannels();
    const double increment = frequency_.load(std::memory_order_relaxed) / sampleRate_;
    const float target = amplitude_.load(std::memory_order_relaxed);
    const float step = (target - lastAmplitude_) / (float) n;
    for (int i = 0; i < n; ++i) {
      const float level = lastAmplitude_ + step * (float) (i + 1);
      const float sample = level * (float) std::sin(2.0 * 3.14159265358979323846 * phase_);
      phase_ += increment;
      phase_ -= std::floor(phase_);
      for (int c = 0; c < channels; ++c) info.channel(c)[i] = sample;
    }
    lastAmplitude_ = target;
  }

private:
  std::atomic<float> amplitude_{0.5f};
  std::atomic<double> frequency_{1000.0};
  double sampleRate_ = 44100.0;
  double phase_ = 0.0;
  float lastAmplitude_ = 0.5f;
};

// ── Processing a source ──────────────────────────────────────────────────────

/**
 * Feeds a source from chosen channels of the incoming buffer and sends its
 * channels to chosen channels of the result.
 *
 * The reference's semantics, exactly, because a ported routing table has to
 * mean the same thing: setInputChannelMapping(dest, src) says channel `src`
 * of the INCOMING buffer feeds channel `dest` of the wrapped source;
 * setOutputChannelMapping(src, dest) says channel `src` of the source's
 * output is ADDED into channel `dest` of the result. An unmapped input is
 * silence, an unmapped output goes nowhere, and two outputs mapped to one
 * destination sum. The XML save and restore the reference offers is not here;
 * the getters give a caller everything it needs to store the map its own way.
 *
 * The map is a SeqBox: a change made while audio runs lands at the next block
 * boundary, whole, never half-applied.
 */
class ChannelRemappingAudioSource : public AudioSource {
public:
  /** Wrap `source`, which must not be null: owned with
   *  `deleteSourceWhenDeleted`, else borrowed and outliving this. Nothing is
   *  mapped to start, so the result is silence until a mapping is set. */
  ChannelRemappingAudioSource(AudioSource* source, bool deleteSourceWhenDeleted)
      : source_(source, deleteSourceWhenDeleted) {
    active_ = edit_;
    published_.write(edit_);
  }

  /** How many channels the wrapped source is asked to produce. Two unless
   *  told otherwise, and never more than AudioSource::kMaxChannels. */
  void setNumberOfChannelsToProduce(int requiredNumberOfChannels) {
    std::lock_guard<std::mutex> lock(writer_);
    edit_.required = (int8_t) sourcedetail::clampChannels(requiredNumberOfChannels);
    published_.write(edit_);
  }

  /** Unmap every input and output channel: silence in, nothing out. Lands
   *  at the next block. Not for the audio thread: it takes the writers'
   *  lock, as every setter here does. */
  void clearAllMappings() {
    std::lock_guard<std::mutex> lock(writer_);
    for (int i = 0; i < AudioSource::kMaxChannels; ++i) edit_.inputs[i] = edit_.outputs[i] = -1;
    published_.write(edit_);
  }

  /** Feed the wrapped source's channel `destChannelIndex` from incoming
   *  channel `sourceChannelIndex`, both 0-based. A source channel out of
   *  range (-1, say) unmaps it; a destination out of range is ignored. */
  void setInputChannelMapping(int destChannelIndex, int sourceChannelIndex) {
    if (destChannelIndex < 0 || destChannelIndex >= AudioSource::kMaxChannels) return;
    std::lock_guard<std::mutex> lock(writer_);
    edit_.inputs[destChannelIndex] = (int8_t) clampIndex(sourceChannelIndex);
    published_.write(edit_);
  }

  /** Add the wrapped source's channel `sourceChannelIndex` into result
   *  channel `destChannelIndex`, both 0-based. A destination out of range
   *  (-1, say) sends it nowhere; a source channel out of range is ignored. */
  void setOutputChannelMapping(int sourceChannelIndex, int destChannelIndex) {
    if (sourceChannelIndex < 0 || sourceChannelIndex >= AudioSource::kMaxChannels) return;
    std::lock_guard<std::mutex> lock(writer_);
    edit_.outputs[sourceChannelIndex] = (int8_t) clampIndex(destChannelIndex);
    published_.write(edit_);
  }

  /** Which incoming channel feeds the source's channel `inputChannelIndex`,
   *  or -1. */
  int getRemappedInputChannel(int inputChannelIndex) const {
    if (inputChannelIndex < 0 || inputChannelIndex >= AudioSource::kMaxChannels) return -1;
    std::lock_guard<std::mutex> lock(writer_);
    return edit_.inputs[inputChannelIndex];
  }

  /** Which result channel the source's channel `inputChannelIndex` goes to,
   *  or -1. */
  int getRemappedOutputChannel(int inputChannelIndex) const {
    if (inputChannelIndex < 0 || inputChannelIndex >= AudioSource::kMaxChannels) return -1;
    std::lock_guard<std::mutex> lock(writer_);
    return edit_.outputs[inputChannelIndex];
  }

  void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override {
    const int frames = samplesPerBlockExpected > 0 ? samplesPerBlockExpected : 512;
    scratch_.setSize(AudioSource::kMaxChannels, frames < kMaxChunk ? frames : kMaxChunk);
    source_->prepareToPlay(samplesPerBlockExpected, sampleRate);
  }
  void releaseResources() override {
    source_->releaseResources();
    scratch_.release();
  }

  /** [audio thread] */
  void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
    if (!info.buffer || info.numSamples <= 0) return;
    Map latest;
    if (published_.tryRead(latest)) active_ = latest;
    const int numChans = info.numChannels();
    const int required = active_.required;
    const int chunk = scratch_.numFrames();
    if (chunk == 0) {
      info.clearActiveBufferRegion();
      return;
    }
    for (int offset = 0; offset < info.numSamples; offset += chunk) {
      const int len = (info.numSamples - offset) < chunk ? (info.numSamples - offset) : chunk;
      for (int i = 0; i < required; ++i) {
        const int from = active_.inputs[i];
        if (from >= 0 && from < numChans)
          std::memcpy(scratch_.channel(i), info.channel(from) + offset, (size_t) len * sizeof(float));
        else
          std::memset(scratch_.channel(i), 0, (size_t) len * sizeof(float));
      }
      AudioBlock<float> block(scratch_.channels(), (size_t) required, (size_t) len);
      source_->getNextAudioBlock(AudioSourceChannelInfo(&block, 0, len));
      for (int c = 0; c < numChans; ++c) std::memset(info.channel(c) + offset, 0, (size_t) len * sizeof(float));
      for (int i = 0; i < required; ++i) {
        const int to = active_.outputs[i];
        if (to < 0 || to >= numChans) continue;
        float* d = info.channel(to) + offset;
        const float* s = scratch_.channel(i);
        for (int k = 0; k < len; ++k) d[k] += s[k];
      }
    }
  }

private:
  /** A routing, published whole: which input feeds each output channel and
   *  the other way round (-1 for none), and how many channels the source
   *  must be asked for to cover them. */
  struct Map {
    int8_t required = 2;
    int8_t inputs[AudioSource::kMaxChannels];
    int8_t outputs[AudioSource::kMaxChannels];
    Map() {
      for (int i = 0; i < AudioSource::kMaxChannels; ++i) inputs[i] = outputs[i] = -1;
    }
  };
  static int clampIndex(int index) { return index < 0 || index >= AudioSource::kMaxChannels ? -1 : index; }
  /** Long blocks go through in pieces this size, so scratch for every
   *  channel stays a bounded size whatever the host promised. */
  static constexpr int kMaxChunk = 1024;

  sourcedetail::MaybeOwned<AudioSource> source_;
  mutable std::mutex writer_; // the writing side only; the audio thread reads published_
  Map edit_;
  sourcedetail::SeqBox<Map> published_;
  Map active_;
  sourcedetail::PlanarBuffer scratch_;
};

/**
 * A source through a second-order IIR section per channel.
 *
 * The coefficients come from a Biquad -- design it with any of its cookbook
 * shapes, hand it over, and every channel's section copies the five numbers
 * and keeps its own state. That is the reference's IIRCoefficients split
 * without a second type: the design is the Biquad you configured, and the
 * channels' filters are the ones in here.
 *
 * A change lands at a block boundary with the filters' state kept, which is
 * what an IIR filter whose knob moved does. makeInactive() passes the audio
 * through untouched until coefficients are set again.
 */
class IIRFilterAudioSource : public AudioSource {
public:
  /** Filter `inputSource`, which must not be null: owned with
   *  `deleteInputWhenDeleted`, else borrowed and outliving this. Inactive,
   *  passing audio through, until coefficients are set. */
  IIRFilterAudioSource(AudioSource* inputSource, bool deleteInputWhenDeleted)
      : input_(inputSource, deleteInputWhenDeleted) {}

  /** Copy `design`'s coefficients into every channel's section. */
  void setCoefficients(const Biquad& design) {
    Coefficients next;
    design.getCoefficients(next.b0, next.b1, next.b2, next.a1, next.a2);
    next.active = true;
    publish(next);
  }
  /** Coefficients normalised so a0 is 1, as Biquad::setCoefficients takes. */
  void setCoefficients(float b0, float b1, float b2, float a1, float a2) {
    Coefficients next;
    next.b0 = b0;
    next.b1 = b1;
    next.b2 = b2;
    next.a1 = a1;
    next.a2 = a2;
    next.active = true;
    publish(next);
  }
  /** Pass the input through untouched from the next block, until
   *  coefficients are set again. Not for the audio thread: it takes the
   *  writers' lock, as the setters do. */
  void makeInactive() {
    std::lock_guard<std::mutex> lock(writer_);
    current_.active = false;
    published_.write(current_);
  }

  void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override {
    input_->prepareToPlay(samplesPerBlockExpected, sampleRate);
    for (Biquad& f : filters_) f.reset();
  }
  void releaseResources() override { input_->releaseResources(); }

  /** [audio thread] */
  void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
    if (!info.buffer || info.numSamples <= 0) return;
    input_->getNextAudioBlock(info);
    Coefficients latest;
    uint32_t version = 0;
    if (published_.tryRead(latest, &version) && version != applied_) {
      applied_ = version;
      active_ = latest.active;
      if (active_)
        for (Biquad& f : filters_) f.setCoefficients(latest.b0, latest.b1, latest.b2, latest.a1, latest.a2);
    }
    if (!active_) return;
    const int channels = info.numChannels() < AudioSource::kMaxChannels ? info.numChannels() : AudioSource::kMaxChannels;
    for (int c = 0; c < channels; ++c) {
      float* d = info.channel(c);
      Biquad& f = filters_[c];
      for (int i = 0; i < info.numSamples; ++i) d[i] = f.process(d[i]);
    }
  }

private:
  /** One biquad's coefficients, normalised (a0 = 1), published whole; an
   *  inactive set passes the input through. */
  struct Coefficients {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    bool active = false;
  };
  void publish(const Coefficients& next) {
    std::lock_guard<std::mutex> lock(writer_);
    current_ = next;
    published_.write(current_);
  }

  sourcedetail::MaybeOwned<AudioSource> input_;
  std::mutex writer_;
  Coefficients current_;
  sourcedetail::SeqBox<Coefficients> published_;
  uint32_t applied_ = ~0u;
  bool active_ = false;
  Biquad filters_[AudioSource::kMaxChannels];
};

/**
 * A source through the SDK's Reverb (effects.h).
 *
 * Stereo is the reverb's own stereo frame. Mono feeds the left side alone and
 * keeps the left output: the tank's input gain is applied to left plus right,
 * so a mono sample fed to both sides would arrive twice as loud as the
 * reference's mono path makes it. Channels past the second are left alone.
 *
 * Bypassing resets the tank on the way in and out, as the reference does, so
 * coming back from bypass does not replay a tail from before it.
 */
class ReverbAudioSource : public AudioSource {
public:
  /** Reverberate `inputSource`, which must not be null: owned with
   *  `deleteInputWhenDeleted`, else borrowed and outliving this. Allocates
   *  the reverb's tank. */
  ReverbAudioSource(AudioSource* inputSource, bool deleteInputWhenDeleted)
      : input_(inputSource, deleteInputWhenDeleted), reverb_(new Reverb()) {}

  /** The parameters last set, as the caller set them. */
  Reverb::Parameters getParameters() const {
    std::lock_guard<std::mutex> lock(writer_);
    return params_;
  }
  /** New settings, applied whole at the next block. Not for the audio
   *  thread: it takes the writers' lock. */
  void setParameters(const Reverb::Parameters& newParams) {
    std::lock_guard<std::mutex> lock(writer_);
    params_ = newParams;
    published_.write(params_);
  }
  /** [any thread] Pass the input through dry. The tank is cleared on the
   *  way in and out, so no old tail plays on return. */
  void setBypassed(bool shouldBeBypassed) noexcept { bypass_.store(shouldBeBypassed, std::memory_order_relaxed); }
  /** [any thread] Whether it is bypassed. */
  bool isBypassed() const noexcept { return bypass_.load(std::memory_order_relaxed); }

  void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override {
    input_->prepareToPlay(samplesPerBlockExpected, sampleRate);
    ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = (uint32_t) (samplesPerBlockExpected > 0 ? samplesPerBlockExpected : 512);
    spec.numChannels = 2;
    reverb_->prepare(spec);
    Reverb::Parameters p;
    uint32_t version = 0;
    if (published_.tryRead(p, &version)) {
      reverb_->setParameters(p);
      applied_ = version;
    }
    wasBypassed_ = bypass_.load(std::memory_order_relaxed);
  }
  void releaseResources() override { input_->releaseResources(); }

  /** [audio thread] */
  void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
    if (!info.buffer || info.numSamples <= 0) return;
    input_->getNextAudioBlock(info);
    const bool bypassed = bypass_.load(std::memory_order_relaxed);
    if (bypassed != wasBypassed_) {
      wasBypassed_ = bypassed;
      reverb_->reset();
    }
    if (bypassed) return;
    Reverb::Parameters p;
    uint32_t version = 0;
    if (published_.tryRead(p, &version) && version != applied_) {
      applied_ = version;
      reverb_->setParameters(p);
    }
    const int n = info.numSamples;
    if (info.numChannels() >= 2) {
      float* left = info.channel(0);
      float* right = info.channel(1);
      for (int i = 0; i < n; ++i) reverb_->process(left[i], right[i]);
    } else if (info.numChannels() == 1) {
      float* mono = info.channel(0);
      for (int i = 0; i < n; ++i) {
        float unused = 0.0f;
        reverb_->process(mono[i], unused);
      }
    }
  }

private:
  sourcedetail::MaybeOwned<AudioSource> input_;
  // On the heap: the delay lines are sized for 192 kHz and come to a few
  // hundred kilobytes, which is more than a source declared on a worker
  // thread's stack should have to carry.
  std::unique_ptr<Reverb> reverb_;
  mutable std::mutex writer_;
  Reverb::Parameters params_;
  sourcedetail::SeqBox<Reverb::Parameters> published_;
  uint32_t applied_ = ~0u;
  std::atomic<bool> bypass_{false};
  bool wasBypassed_ = false;
};

/**
 * Plays an input at a different rate: `samplesInPerOutputSample` input
 * samples per output sample, so 44100.0 / 48000.0 plays a 44.1 kHz source
 * at its own pitch in a 48 kHz stream, and 2.0 plays it an octave up.
 *
 * resample.h's Resampler is the SDK's rate converter, and it is the wrong
 * tool inside a callback by its own description: it converts a whole buffer
 * at once, evaluates its kernel with a sine per tap, and keeps nothing
 * between calls. This is its streaming counterpart, and the test holds it to
 * Resampler's output for the same material:
 *
 *   interpolation   interpolation.h's WindowedSincKernel<8>, read from its
 *                   table: no trigonometry per sample, and the first output
 *                   is aligned with the first input, because the kernel's
 *                   eight samples of look-ahead are pulled AHEAD rather than
 *                   paid for as latency.
 *   anti-aliasing   reading faster than one-for-one folds whatever lies above
 *                   the new Nyquist back down, and no interpolation kernel
 *                   undoes that. So above a ratio of 1 the input goes through
 *                   an eighth-order Butterworth lowpass (four of dsp.h's
 *                   double-precision SvfSections, the form whose corner may
 *                   move) at 0.45 of the OUTPUT rate first -- kept running at
 *                   every ratio and crossfaded in, so that crossing 1 is not a
 *                   click (see antiAlias()).
 *                   The reference uses a single second-order section and
 *                   linear interpolation; this measures better and costs a
 *                   table read.
 *   ratio changes   glide linearly across the block in which they are seen,
 *                   sample by sample. A ratio that steps is a pitch that
 *                   steps; the read position itself never jumps either way.
 *   ratio 1         exact. The fractional position stays at zero and every
 *                   output is the input sample, bit for bit.
 */
class ResamplingAudioSource : public AudioSource {
public:
  /** Resample `inputSource`, which must not be null: owned with
   *  `deleteInputWhenDeleted`, else borrowed and outliving this.
   *  `numChannels`, held to 1 .. AudioSource::kMaxChannels, is how many are
   *  resampled; a wider buffer's other channels get silence. Ratio 1 to
   *  start. */
  ResamplingAudioSource(AudioSource* inputSource, bool deleteInputWhenDeleted, int numChannels = 2)
      : input_(inputSource, deleteInputWhenDeleted), numChannels_(sourcedetail::clampChannels(numChannels)) {
    Kernel::warm(); // the table is built here, not inside the first block
    aa_.resize((size_t) numChannels_ * (size_t) kAaSections);
  }

  /** Any thread. Clamped to 1/256 .. 256, past which the request is a
   *  mistake rather than a rate. */
  void setResamplingRatio(double samplesInPerOutputSample) noexcept {
    double r = samplesInPerOutputSample;
    if (!(r > 0.0)) r = 1.0;
    if (r < 1.0 / 256.0) r = 1.0 / 256.0;
    if (r > 256.0) r = 256.0;
    ratio_.store(r, std::memory_order_relaxed);
  }
  /** [any thread] The ratio last set, after clamping: input samples per
   *  output sample. */
  double getResamplingRatio() const noexcept { return ratio_.load(std::memory_order_relaxed); }

  /** [audio thread] Forget the history: what a seek upstream needs, or the
   *  first samples after it are interpolated against the old position. */
  void flushBuffers() noexcept {
    if (buffer_.numFrames() == 0) return;
    for (int c = 0; c < numChannels_; ++c) std::memset(buffer_.channel(c), 0, (size_t) kBehind * sizeof(float));
    available_ = kBehind;
    index_ = kBehind;
    fraction_ = 0.0;
    // The history is silence now, so the filter's state is too, and the
    // crossfade lands where the ratio already is.
    for (SvfSection& sec : aa_) sec.reset();
    aaMix_ = current_ > 1.0 + 1e-9 ? 1.0f : 0.0f;
  }

  void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override {
    const double r = ratio_.load(std::memory_order_relaxed);
    current_ = r;
    const int block = samplesPerBlockExpected > 0 ? samplesPerBlockExpected : 512;
    const int inBlock = (int) std::ceil((double) block * r);
    input_->prepareToPlay(inBlock > 0 ? inBlock : 1, sampleRate * r);
    capacity_ = kPoints + (inBlock > 64 ? inBlock : 64) + kAhead + 1;
    buffer_.setSize(numChannels_, capacity_);
    aaRatio_ = 0.0;
    flushBuffers();
  }
  void releaseResources() override {
    input_->releaseResources();
    buffer_.release();
    capacity_ = 0;
  }

  /** [audio thread] */
  void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
    if (!info.buffer || info.numSamples <= 0) return;
    if (capacity_ == 0) {
      info.clearActiveBufferRegion();
      return;
    }
    const int n = info.numSamples;
    const int dstChannels = info.numChannels();
    const int channels = dstChannels < numChannels_ ? dstChannels : numChannels_;
    const double target = ratio_.load(std::memory_order_relaxed);
    const double from = current_;
    const bool steady = from == target;
    const double step = (target - from) / (double) n;
    const double fastest = from > target ? from : target;
    for (int i = 0; i < n; ++i) {
      const double r = steady ? from : from + step * (double) (i + 1);
      // The kernel reaches kAhead samples past the read point.
      while (index_ + kAhead >= available_) {
        const int want = (int) std::ceil(fastest * (double) (n - i) + fraction_) + kAhead + 1 -
                         (available_ - index_);
        if (!pull(want, r)) break;
      }
      if (fraction_ == 0.0 && r == 1.0) {
        for (int c = 0; c < channels; ++c) info.channel(c)[i] = buffer_.channel(c)[index_];
      } else {
        const float frac = (float) fraction_;
        for (int c = 0; c < channels; ++c) {
          const float* oldest = buffer_.channel(c) + index_ - kBehind;
          float newestFirst[kPoints];
          for (int k = 0; k < kPoints; ++k) newestFirst[k] = oldest[kPoints - 1 - k];
          info.channel(c)[i] = Kernel::at(newestFirst, frac);
        }
      }
      fraction_ += r;
      const double whole = std::floor(fraction_);
      index_ += (int) whole;
      fraction_ -= whole;
    }
    for (int c = channels; c < dstChannels; ++c) std::memset(info.channel(c), 0, (size_t) n * sizeof(float));
    current_ = target;
  }

private:
  using Kernel = WindowedSincKernel<8>;
  static constexpr int kPoints = Kernel::kPoints;
  /** Samples the kernel reads after the point it interpolates at... */
  static constexpr int kAhead = (int) Kernel::kLatency;
  /** ...and before it. 8 and 7: sixteen taps around the point. */
  static constexpr int kBehind = kPoints - 1 - kAhead;
  static constexpr int kAaSections = 4;
  /** Input samples over which the read crossfades between raw and filtered. */
  static constexpr int kAaFade = 64;

  /** [audio thread] Move what the kernel still needs to the front, then ask
   *  the input for up to `want` more. False only when there is no room,
   *  which the capacity rules out. */
  bool pull(int want, double r) noexcept {
    const int keepFrom = index_ - kBehind;
    if (keepFrom > 0) {
      const int kept = available_ - keepFrom;
      if (kept > 0)
        for (int c = 0; c < numChannels_; ++c)
          std::memmove(buffer_.channel(c), buffer_.channel(c) + keepFrom, (size_t) kept * sizeof(float));
      // The read point can be PAST everything held: one output step moves it
      // by the ratio, and once the buffer has been refilled inside a block --
      // a ratio raised above the one prepared for, or a block longer than
      // promised -- a step over 16 lands beyond the last frame pulled. The
      // frames in between are never interpolated, but the input still has to
      // pass them, in order and through the filter, or the stream would slip.
      for (int skip = kept < 0 ? -kept : 0; skip > 0;) {
        const int count = skip < capacity_ ? skip : capacity_;
        AudioBlock<float> block(buffer_.channels(), (size_t) numChannels_, (size_t) capacity_);
        input_->getNextAudioBlock(AudioSourceChannelInfo(&block, 0, count));
        antiAlias(0, count, r);
        skip -= count;
      }
      available_ = kept > 0 ? kept : 0;
      index_ -= keepFrom;
    }
    const int space = capacity_ - available_;
    const int count = want < 1 ? 1 : (want > space ? space : want);
    if (count <= 0) return false;
    AudioBlock<float> block(buffer_.channels(), (size_t) numChannels_, (size_t) capacity_);
    input_->getNextAudioBlock(AudioSourceChannelInfo(&block, available_, count));
    antiAlias(available_, count, r);
    available_ += count;
    return true;
  }

  /**
   * [audio thread] The lowpass that has to come BEFORE a faster read.
   *
   * The corner follows the ratio, so it moves whenever the ratio does, and
   * that decides the structure. The first version used IirSection, a
   * transposed direct form, redesigned per pull with its state carried over:
   * that state means nothing under the new coefficients, and a glide from
   * 1.0 to 1.5 put a 0.27 step into a 0.5 sine -- a click on every pitch bend.
   * SvfSection is the topology-preserving state-variable form, whose state
   * is the two integrators' and stays meaningful when the corner moves: the
   * same glide now steps no more than the sine itself does.
   *
   * It also runs at EVERY ratio, and what is read is a crossfade between the
   * raw input and the filtered copy -- all raw at or below 1, all filtered
   * above, sliding over kAaFade input samples when the ratio crosses 1. A
   * filter only started on crossing would start from zero state, and even a
   * warm one delays by about two samples, so a hard cut from raw to filtered
   * is a jump back in time. With the weight at exactly zero the raw sample is
   * passed as it is, which keeps ratio 1 bit-exact.
   */
  void antiAlias(int offset, int count, double r) noexcept {
    // Designed for the ratio, or for 1 when reading at or below unity --
    // nothing aliases then, but the filter is kept warm for the moment it
    // matters.
    const double designFor = r > 1.0 ? r : 1.0;
    if (std::fabs(designFor - aaRatio_) > 1e-6 * designFor) {
      aaRatio_ = designFor;
      // Designed against a nominal rate, since only the corner's FRACTION of
      // the input rate matters: 0.45 of the output rate (0.9 of its Nyquist)
      // is 0.45 / r of the input's. Butterworth Qs for order 8:
      // 1 / (2 cos((2k - 1) pi / 16)).
      const double nominal = 96000.0;
      const double corner = 0.45 * nominal / designFor;
      for (int k = 0; k < kAaSections; ++k) {
        const double q = 1.0 / (2.0 * std::cos((2.0 * k + 1.0) * 3.14159265358979323846 / 16.0));
        for (int c = 0; c < numChannels_; ++c) {
          SvfSection& sec = aa_[(size_t) c * kAaSections + (size_t) k];
          sec.setSampleRate((float) nominal);
          sec.lowpass(corner, q);
        }
      }
    }
    const float target = r > 1.0 + 1e-9 ? 1.0f : 0.0f;
    const float step = 1.0f / (float) kAaFade;
    float mixAfter = aaMix_;
    for (int c = 0; c < numChannels_; ++c) {
      float* d = buffer_.channel(c) + offset;
      SvfSection* sections = aa_.data() + (size_t) c * kAaSections;
      float mix = aaMix_;
      for (int i = 0; i < count; ++i) {
        const float x = d[i];
        float y = x;
        for (int k = 0; k < kAaSections; ++k) y = sections[k].process(y);
        if (mix == 1.0f) d[i] = y;
        else if (mix != 0.0f) d[i] = x + mix * (y - x);
        if (mix < target) mix = (mix + step) < target ? mix + step : target;
        else if (mix > target) mix = (mix - step) > target ? mix - step : target;
      }
      mixAfter = mix;
    }
    aaMix_ = mixAfter;
  }

  sourcedetail::MaybeOwned<AudioSource> input_;
  const int numChannels_;
  std::atomic<double> ratio_{1.0};
  double current_ = 1.0;
  sourcedetail::PlanarBuffer buffer_;
  int capacity_ = 0;
  int available_ = 0; // frames held, oldest at 0
  int index_ = 0;     // the integer part of the read point
  double fraction_ = 0.0;
  std::vector<SvfSection> aa_;
  double aaRatio_ = 0.0;
  float aaMix_ = 0.0f; // 0 reads the raw input, 1 the filtered
};

// ── Combining sources ────────────────────────────────────────────────────────

/**
 * Sums any number of sources.
 *
 * Inputs may be added and removed from any thread while audio runs, and the
 * audio thread takes no lock for it: the list is an immutable snapshot behind
 * an atomic pointer. A change copies the list, publishes the copy, then waits
 * out the block in flight (BlockEpoch) before it frees the old list or
 * releases a removed source -- so a removed source has finished its last
 * block before releaseResources() is called on it, and the CALLER waits for
 * that, never the audio thread. Removing a source from inside the mixer's own
 * callback cannot wait for itself, so the old list and the source are kept
 * and freed at the next change or when the mixer goes.
 *
 * The waiting is done with the writers' lock RELEASED. A source removing
 * itself from its own callback takes that lock on the audio thread, inside
 * the very block another thread's change may be waiting out; a wait holding
 * the lock is then a deadlock of both threads (the one-shot sample player
 * that removes itself while the UI adds the next one did exactly that).
 * Changes from other threads still go one at a time: the next one waits
 * until the last has freed what it replaced.
 *
 * The first input renders straight into the output and the rest are added
 * from scratch, in order, the reference's arithmetic. Scratch holds
 * AudioSource::kMaxChannels channels; a wider output gets only the first
 * input on the channels past that.
 */
class MixerAudioSource : public AudioSource {
public:
  /** No inputs: silence. */
  MixerAudioSource() = default;
  /** Removes every input, releasing each and deleting the ones it owns.
   *  Not while a block is being pulled from it. */
  ~MixerAudioSource() override {
    removeAllInputs();
    delete list_.exchange(nullptr, std::memory_order_seq_cst);
  }
  MixerAudioSource(const MixerAudioSource&) = delete;            ///< Not copyable: it may own its inputs.
  MixerAudioSource& operator=(const MixerAudioSource&) = delete; ///< Not copyable: it may own its inputs.

  /** Prepared at the mixer's rate first if the mixer is prepared, so it
   *  plays correctly from its first block. Adding one already present does
   *  nothing. */
  void addInputSource(AudioSource* newInput, bool deleteWhenRemoved) {
    if (!newInput) return;
    std::unique_lock<std::mutex> lock(writer_);
    awaitTurn(lock);
    const List* current = list_.load(std::memory_order_relaxed);
    if (current)
      for (const Input& in : current->inputs)
        if (in.source == newInput) return;
    if (sampleRate_ > 0.0) newInput->prepareToPlay(blockSize_, sampleRate_);
    std::unique_ptr<List> next(new List());
    if (current) next->inputs = current->inputs;
    next->inputs.push_back(Input{newInput, deleteWhenRemoved});
    swapIn(lock, std::move(next), nullptr, 0);
  }

  /** Take `input` out of the mix, then release it, and delete it if it was
   *  added with deleteWhenRemoved. From another thread this waits out the
   *  block in flight first, so the source has finished playing when it
   *  returns; from inside the mixer's own block it is freed at the next
   *  change instead. Not added, or null: nothing happens. */
  void removeInputSource(AudioSource* input) {
    if (!input) return;
    std::unique_lock<std::mutex> lock(writer_);
    awaitTurn(lock);
    const List* current = list_.load(std::memory_order_relaxed);
    if (!current) return;
    std::unique_ptr<List> next(new List());
    Input removed{nullptr, false};
    for (const Input& in : current->inputs) {
      if (in.source == input) removed = in;
      else next->inputs.push_back(in);
    }
    if (!removed.source) return;
    swapIn(lock, std::move(next), &removed, 1);
  }

  /** removeInputSource() for every input at once: one wait, then each is
   *  released and the owned ones deleted. */
  void removeAllInputs() {
    std::unique_lock<std::mutex> lock(writer_);
    awaitTurn(lock);
    const List* current = list_.load(std::memory_order_relaxed);
    if (!current) return;
    const std::vector<Input> all = current->inputs;
    swapIn(lock, std::unique_ptr<List>(new List()), all.data(), all.size());
  }

  /** How many inputs it is mixing. Takes the writers' lock: not for the
   *  audio thread. */
  int getNumInputs() const {
    std::lock_guard<std::mutex> lock(writer_);
    const List* current = list_.load(std::memory_order_relaxed);
    return current ? (int) current->inputs.size() : 0;
  }

  void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override {
    std::lock_guard<std::mutex> lock(writer_);
    blockSize_ = samplesPerBlockExpected > 0 ? samplesPerBlockExpected : 512;
    sampleRate_ = sampleRate;
    temp_.setSize(AudioSource::kMaxChannels, blockSize_ < kMaxChunk ? blockSize_ : kMaxChunk);
    if (const List* current = list_.load(std::memory_order_relaxed))
      for (const Input& in : current->inputs) in.source->prepareToPlay(blockSize_, sampleRate_);
  }

  void releaseResources() override {
    std::lock_guard<std::mutex> lock(writer_);
    if (const List* current = list_.load(std::memory_order_relaxed))
      for (const Input& in : current->inputs) in.source->releaseResources();
    temp_.release();
    sampleRate_ = 0.0;
  }

  /** [audio thread] */
  void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
    if (!info.buffer || info.numSamples <= 0) return;
    sourcedetail::InBlock inBlock(epoch_);
    const List* list = list_.load(std::memory_order_seq_cst);
    if (!list || list->inputs.empty()) {
      info.clearActiveBufferRegion();
      return;
    }
    list->inputs[0].source->getNextAudioBlock(info);
    const size_t count = list->inputs.size();
    const int chunk = temp_.numFrames();
    if (count < 2 || chunk == 0) return;
    const int channels = info.numChannels() < temp_.numChannels() ? info.numChannels() : temp_.numChannels();
    for (int offset = 0; offset < info.numSamples; offset += chunk) {
      const int len = (info.numSamples - offset) < chunk ? (info.numSamples - offset) : chunk;
      AudioBlock<float> block(temp_.channels(), (size_t) channels, (size_t) len);
      for (size_t k = 1; k < count; ++k) {
        list->inputs[k].source->getNextAudioBlock(AudioSourceChannelInfo(&block, 0, len));
        for (int c = 0; c < channels; ++c) {
          float* d = info.channel(c) + offset;
          const float* s = temp_.channel(c);
          for (int i = 0; i < len; ++i) d[i] += s[i];
        }
      }
    }
  }

private:
  /** A source the mixer plays, and whether the mixer deletes it. */
  struct Input {
    AudioSource* source;
    bool owned;
  };
  /** The inputs as one published list: a change builds a new one and swaps
   *  it in, so a block always reads a whole list. */
  struct List {
    std::vector<Input> inputs;
  };
  static constexpr int kMaxChunk = 1024;

  /** What a change replaced -- the old list, and the inputs it took out --
   *  with the block that may still be using it. */
  struct Retired {
    std::unique_ptr<List> list;
    Input input;
    uint64_t block;
  };

  /** Changes from other threads one at a time: wait, with writer_ released,
   *  until the change in progress has freed what it replaced. A change made
   *  from inside the mixer's own block does not wait -- the change in
   *  progress may be waiting for that very block -- and needs no turn: it
   *  only retires, never frees. Called with writer_ held. */
  void awaitTurn(std::unique_lock<std::mutex>& lock) {
    if (epoch_.isInside(epoch_.blockInFlight())) return;
    turn_.wait(lock, [this] { return !changing_; });
  }

  /** Publish `next` and retire the old list and `removed`; then, unless
   *  called from inside the mixer's own block (where nothing could be freed:
   *  the snapshot being iterated right now is the old one), wait out the
   *  block that may have seen them -- with writer_ released -- and free
   *  everything retired whose block is over. Called with writer_ held,
   *  returns with it held. */
  void swapIn(std::unique_lock<std::mutex>& lock, std::unique_ptr<List> next, const Input* removed,
              size_t removedCount) {
    std::unique_ptr<List> old(list_.exchange(next.release(), std::memory_order_seq_cst));
    const uint64_t block = epoch_.blockInFlight();
    retired_.push_back(Retired{std::move(old), Input{nullptr, false}, block});
    for (size_t i = 0; i < removedCount; ++i) retired_.push_back(Retired{nullptr, removed[i], block});
    if (epoch_.isInside(block)) return;
    changing_ = true;
    lock.unlock();
    epoch_.waitUntilOver(block);
    lock.lock();
    changing_ = false;
    size_t kept = 0;
    for (size_t i = 0; i < retired_.size(); ++i) {
      if (!epoch_.isOver(retired_[i].block)) {
        if (kept != i) retired_[kept] = std::move(retired_[i]);
        ++kept;
        continue;
      }
      retired_[i].list.reset();
      if (retired_[i].input.source) finish(retired_[i].input);
    }
    retired_.erase(retired_.begin() + (std::ptrdiff_t) kept, retired_.end());
    turn_.notify_all();
  }

  /** What the reference does with a removed input: release it, and delete
   *  it if it was handed over. */
  static void finish(const Input& in) {
    in.source->releaseResources();
    if (in.owned) delete in.source;
  }

  std::atomic<List*> list_{nullptr};
  mutable std::mutex writer_;
  sourcedetail::BlockEpoch epoch_;
  sourcedetail::PlanarBuffer temp_;
  double sampleRate_ = 0.0;
  int blockSize_ = 512;
  std::vector<Retired> retired_;
  std::condition_variable turn_;
  bool changing_ = false; // a change is waiting out a block, writer_ released
};

// ── Reading ahead ────────────────────────────────────────────────────────────

/**
 * One background thread that takes turns among the sources reading ahead.
 *
 * The reference's BufferingAudioSource takes a TimeSliceThread: one thread,
 * many clients, each called in turn and each saying how long it can wait
 * before it next needs a turn. thread_pool.h's ThreadPool is the SDK's
 * background work, and it is the wrong shape for this -- it runs jobs to
 * COMPLETION, and reading ahead never completes. Parked on a pool worker it
 * would take that worker for good, and the default pool has two.
 *
 * So this is the cooperative shape, for exactly this job. A client returning
 * 0 is called again at once (it has more to read); a positive number is a
 * nap in milliseconds; a negative one waits until moveToFrontOfQueue(). The
 * thread starts itself when the first client arrives -- a thread nobody
 * started is a buffer that never fills, which sounds exactly like a broken
 * file -- and must outlive every client registered with it.
 */
class ReadAheadThread {
public:
  /** Something serviced by the thread. */
  class Client {
  public:
    /** Removed from its thread first: removeTimeSliceClient(). */
    virtual ~Client() = default;
    /** Do one piece of work; return how many ms until the next turn. */
    virtual int useTimeSlice() = 0;
  };

  /** Not started yet: the first addTimeSliceClient() starts it. */
  ReadAheadThread() = default;
  /** Stops and joins the thread. It must outlive every client registered
   *  with it, so a client's own removal still finds it. */
  ~ReadAheadThread() { stopThread(); }
  ReadAheadThread(const ReadAheadThread&) = delete;            ///< Not copyable: it owns a thread.
  ReadAheadThread& operator=(const ReadAheadThread&) = delete; ///< Not copyable: it owns a thread.

  /** Start the thread if it is not running; harmless when it is. */
  void startThread() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable()) return;
    stopRequested_ = false;
    thread_ = std::thread([this] { run(); });
  }

  /** Stop and join. Clients stay registered and resume on the next start. */
  void stopThread() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopRequested_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
  }

  /** True between a start and a stop. */
  bool isThreadRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return thread_.joinable() && !stopRequested_;
  }

  /** Register `client`, not owned, for its first turn in
   *  `msToWaitBeforeFirstTurn` ms, and start the thread if it is not
   *  running. A client already registered, or null, is ignored. It must be
   *  removed with removeTimeSliceClient() before it is destroyed. */
  void addTimeSliceClient(Client* client, int msToWaitBeforeFirstTurn = 0) {
    if (!client) return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      bool present = false;
      for (const Entry& e : clients_) present = present || e.client == client;
      if (!present) clients_.push_back(Entry{client, Clock::now() + std::chrono::milliseconds(msToWaitBeforeFirstTurn)});
    }
    wake_.notify_all();
    if (!isThreadRunning()) startThread();
  }

  /** Remove, and if the client is in the middle of its turn, wait for the
   *  turn to end: after this returns the thread will not call it again. */
  void removeTimeSliceClient(Client* client) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (size_t i = 0; i < clients_.size(); ++i)
      if (clients_[i].client == client) {
        clients_.erase(clients_.begin() + (std::ptrdiff_t) i);
        break;
      }
    if (std::this_thread::get_id() == thread_.get_id()) return; // removing itself mid-turn
    serviced_.wait(lock, [&] { return current_ != client; });
  }

  /** [not the audio thread] Give `client` the next turn, now. */
  void moveToFrontOfQueue(Client* client) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (Entry& e : clients_)
        if (e.client == client) e.due = Clock::now();
    }
    wake_.notify_all();
  }

  /** How many clients are registered. */
  int getNumClients() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (int) clients_.size();
  }

private:
  using Clock = std::chrono::steady_clock;
  /** A client and when it next wants a turn. */
  struct Entry {
    Client* client;
    Clock::time_point due;
  };

  void run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopRequested_) {
      if (clients_.empty()) {
        wake_.wait(lock, [this] { return stopRequested_ || !clients_.empty(); });
        continue;
      }
      size_t next = 0;
      for (size_t i = 1; i < clients_.size(); ++i)
        if (clients_[i].due < clients_[next].due) next = i;
      if (clients_[next].due > Clock::now()) {
        // Woken early by a new client or a moveToFrontOfQueue(), or not: the
        // loop decides again either way.
        if (clients_[next].due == (Clock::time_point::max)()) wake_.wait(lock);
        else wake_.wait_until(lock, clients_[next].due);
        continue;
      }
      Client* client = clients_[next].client;
      current_ = client;
      lock.unlock();
      int wait = 0;
      try {
        wait = client->useTimeSlice();
      } catch (...) {
        wait = 100; // a client that throws is not allowed to take the thread with it
      }
      lock.lock();
      current_ = nullptr;
      for (Entry& e : clients_)
        if (e.client == client)
          e.due = wait < 0 ? (Clock::time_point::max)() : Clock::now() + std::chrono::milliseconds(wait);
      serviced_.notify_all();
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable serviced_;
  std::vector<Entry> clients_;
  Client* current_ = nullptr;
  std::thread thread_;
  bool stopRequested_ = false;
};

/**
 * Reads a positionable source ahead of playback on a ReadAheadThread, into a
 * ring the audio thread copies out of.
 *
 * The audio thread never waits for the reader. Frames that are not in the
 * ring yet come out as silence and are COUNTED (getUnderrunFrames()), the way
 * SampleStreamer counts them, rather than hidden: a disk that cannot keep up
 * should read as a number somebody can look at, not as a plugin that sounds
 * broken. waitForNextAudioBlockReady() is the blocking question for the
 * places that may block -- an offline render, a test.
 *
 * The ring is addressed by ABSOLUTE position: frame f lives in slot
 * f mod capacity, and the reader publishes which frames are valid as a range
 * [start, end). The audio thread copies whatever part of its block falls in
 * that range. The reader only ever overwrites a slot after raising `start`
 * past the frame that was in it and waiting out any block that might have
 * read the old range (BlockEpoch) -- so a slot is never written while it is
 * being read, which is the whole of the ring's correctness, and the waiting
 * is done by the reader thread.
 *
 * The reference does this with a lock that the audio callback takes. A seek
 * (setNextReadPosition) is a single atomic store here, from any thread,
 * including the audio thread; the reader notices on its next turn, which is
 * at most a few milliseconds while playback is moving.
 */
class BufferingAudioSource : public PositionableAudioSource, private ReadAheadThread::Client {
public:
  /** Read `source` ahead on `backgroundThread`, which must outlive this.
   *  `source` is owned with `deleteSourceWhenDeleted`, else borrowed and
   *  outliving this; null plays silence. The ring holds
   *  `numberOfSamplesToBuffer` frames (32 at least, and never less than two
   *  blocks) of `numberOfChannels` channels, held to 1 ..
   *  AudioSource::kMaxChannels. With `prefillBufferOnPrepareToPlay`,
   *  prepareToPlay() waits, up to 5 s, for a quarter of a second or half the
   *  ring to be read. */
  BufferingAudioSource(PositionableAudioSource* source, ReadAheadThread& backgroundThread,
                       bool deleteSourceWhenDeleted, int numberOfSamplesToBuffer, int numberOfChannels = 2,
                       bool prefillBufferOnPrepareToPlay = true)
      : source_(source, deleteSourceWhenDeleted), thread_(backgroundThread),
        bufferSize_(numberOfSamplesToBuffer > 32 ? numberOfSamplesToBuffer : 32),
        channels_(sourcedetail::clampChannels(numberOfChannels)), prefill_(prefillBufferOnPrepareToPlay) {}

  /** If prepared, stops reading -- off the thread, waiting out a turn in
   *  progress -- and releases the source; deletes it if it was handed over. */
  ~BufferingAudioSource() override { releaseResources(); }
  BufferingAudioSource(const BufferingAudioSource&) = delete;            ///< Not copyable: its thread holds it.
  BufferingAudioSource& operator=(const BufferingAudioSource&) = delete; ///< Not copyable: its thread holds it.

  void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override {
    stopReading();
    const int block = samplesPerBlockExpected > 0 ? samplesPerBlockExpected : 512;
    capacity_ = (block * 2) > bufferSize_ ? block * 2 : bufferSize_;
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    ring_.setSize(channels_, capacity_);
    if (source_) source_->prepareToPlay(block, sampleRate_);
    const int64_t at = nextPlayPos_.load(std::memory_order_relaxed);
    workerStart_ = workerEnd_ = at;
    range_.write(Range{at, at});
    sourcePos_ = -1;
    wasLooping_ = source_ && source_->isLooping();
    lastPolled_ = at;
    underruns_.store(0, std::memory_order_relaxed);
    prepared_.store(true, std::memory_order_seq_cst);
    if (!source_) return;
    thread_.addTimeSliceClient(this);
    if (prefill_) {
      // A quarter of a second or half the ring, whichever is less: enough
      // that the first blocks are not underruns, and not so much that
      // pressing play waits for a whole ring of disk.
      const int64_t wanted = (std::min)((int64_t) (sampleRate_ / 4.0), (int64_t) capacity_ / 2);
      waitUntil([&](const Range& r) { return r.start <= at && r.end - at >= wanted; }, 5000);
    }
  }

  void releaseResources() override {
    if (!prepared_.load(std::memory_order_relaxed)) return;
    stopReading();
    ring_.release();
    capacity_ = 0;
    if (source_) source_->releaseResources();
  }

  /** [audio thread] */
  void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
    if (!info.buffer || info.numSamples <= 0) return;
    sourcedetail::InBlock inBlock(epoch_);
    const int n = info.numSamples;
    const int dstChannels = info.numChannels();
    int64_t pos = nextPlayPos_.load(std::memory_order_acquire);
    int from = 0, to = 0;
    if (prepared_.load(std::memory_order_seq_cst)) {
      Range r{0, 0};
      bool have = false;
      // A writer mid-publish is on the other copy (see SeqBox); only one
      // that laps this read twice makes it lose, a couple of retries cover
      // that, and losing anyway means silence for this block, not a wait.
      for (int tries = 0; tries < 4 && !have; ++tries) have = range_.tryRead(r);
      if (have) {
        const int64_t lo = r.start > pos ? r.start : pos;
        const int64_t hi = r.end < pos + n ? r.end : pos + n;
        if (hi > lo) {
          from = (int) (lo - pos);
          to = (int) (hi - pos);
        }
      }
      underruns_.fetch_add((uint64_t) (n - (to - from)), std::memory_order_relaxed);
    }
    for (int c = 0; c < dstChannels; ++c) {
      float* d = info.channel(c);
      if (from > 0) std::memset(d, 0, (size_t) from * sizeof(float));
      if (to < n) std::memset(d + to, 0, (size_t) (n - to) * sizeof(float));
      if (to <= from) continue;
      if (c >= channels_) {
        std::memset(d + from, 0, (size_t) (to - from) * sizeof(float));
        continue;
      }
      const float* ring = ring_.channel(c);
      int64_t frame = pos + from;
      int at = from;
      while (at < to) {
        const int slot = (int) (frame % capacity_);
        const int run = (to - at) < (capacity_ - slot) ? (to - at) : (capacity_ - slot);
        std::memcpy(d + at, ring + slot, (size_t) run * sizeof(float));
        at += run;
        frame += run;
      }
    }
    // A seek that landed during this block wins over the advance.
    nextPlayPos_.compare_exchange_strong(pos, pos + n, std::memory_order_acq_rel);
  }

  /** Any thread, including the audio thread: one store. */
  void setNextReadPosition(int64_t newPosition) override {
    nextPlayPos_.store(newPosition > 0 ? newPosition : 0, std::memory_order_release);
  }
  int64_t getNextReadPosition() const override {
    const int64_t pos = nextPlayPos_.load(std::memory_order_acquire);
    const int64_t length = getTotalLength();
    return isLooping() && length > 0 && pos > 0 ? pos % length : pos;
  }
  int64_t getTotalLength() const override { return source_ ? source_->getTotalLength() : 0; }
  bool isLooping() const override { return source_ && source_->isLooping(); }
  void setLooping(bool shouldLoop) override {
    if (source_) source_->setLooping(shouldLoop);
  }

  /**
   * [not the audio thread] Wait until the next `info.numSamples` frames from
   * the play position are in the ring, or `timeoutMs` passes. True when they
   * are -- or when the position is already past the end of a source that
   * does not loop, where there is nothing left to wait for.
   */
  bool waitForNextAudioBlockReady(const AudioSourceChannelInfo& info, uint32_t timeoutMs) {
    if (!source_ || !prepared_.load(std::memory_order_acquire) || source_->getTotalLength() <= 0) return false;
    const int64_t pos = nextPlayPos_.load(std::memory_order_acquire);
    if (!isLooping() && pos > getTotalLength()) return true;
    return waitUntil(
        [&](const Range& r) {
          const int64_t now = nextPlayPos_.load(std::memory_order_acquire);
          return r.start <= now && r.end >= now + info.numSamples;
        },
        timeoutMs);
  }

  /** [not the audio thread] Give the reader its turn now rather than at its
   *  next poll: what a seek made from a thread that may block wants next. */
  void wakeReader() { thread_.moveToFrontOfQueue(this); }

  /** Frames the audio thread asked for that were not there. */
  uint64_t getUnderrunFrames() const noexcept { return underruns_.load(std::memory_order_relaxed); }
  /** The ring's length, in frames, once prepared. */
  int getBufferSize() const noexcept { return capacity_; }

private:
  /** The source frames the ring holds: [start, end). */
  struct Range {
    int64_t start, end;
  };
  /** The most read in one turn: big enough to be efficient, small enough
   *  that a seek is noticed between reads. */
  static constexpr int kMaxChunk = 4096;
  /** Not worth a turn for less than this while the ring is half full. */
  static constexpr int kMinChunk = 256;

  void stopReading() {
    thread_.removeTimeSliceClient(this);
    prepared_.store(false, std::memory_order_seq_cst);
    epoch_.waitForBlocksInFlight();
  }

  template <typename Condition>
  bool waitUntil(Condition ready, uint32_t timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    wakeReader();
    for (;;) {
      Range r{0, 0};
      if (range_.tryRead(r) && ready(r)) return true;
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::unique_lock<std::mutex> lock(readyMutex_);
      readyCv_.wait_for(lock, std::chrono::milliseconds(2));
    }
  }

  /** [read-ahead thread] */
  int useTimeSlice() override {
    if (!prepared_.load(std::memory_order_acquire)) return -1;
    if (readNextChunk()) return 0;
    // Idle. Poll briskly while playback moves -- a seek made on the audio
    // thread is only seen here -- and lazily while it does not.
    const int64_t play = nextPlayPos_.load(std::memory_order_acquire);
    const bool moving = play != lastPolled_;
    lastPolled_ = play;
    return moving ? 2 : 20;
  }

  /** [read-ahead thread] One read, if one is due. True when it read. */
  bool readNextChunk() {
    const bool looping = source_->isLooping();
    int64_t play = nextPlayPos_.load(std::memory_order_acquire);
    if (play < 0) play = 0;
    // Positions past the end mean different frames once looping changes, so
    // nothing held is valid any more.
    const bool loopChanged = looping != wasLooping_;
    wasLooping_ = looping;
    if (loopChanged || play < workerStart_ || play > workerEnd_) {
      workerStart_ = workerEnd_ = play;
      publish();
      epoch_.waitForBlocksInFlight();
    }
    const int64_t limit = play + capacity_;
    if (workerEnd_ >= limit) return false;
    const int64_t room = limit - workerEnd_;
    if (room < kMinChunk && workerEnd_ - play > capacity_ / 2) return false;
    const int chunk = (int) (room < kMaxChunk ? room : kMaxChunk);
    const int64_t first = workerEnd_;
    const int64_t last = first + chunk;
    // The frames this read overwrites leave the valid range BEFORE the slots
    // are touched, and any block that may have seen them finishes first.
    const int64_t newStart = (std::max)(workerStart_, last - capacity_);
    if (newStart > workerStart_) {
      workerStart_ = newStart;
      publish();
      epoch_.waitForBlocksInFlight();
    }
    if (sourcePos_ != first) source_->setNextReadPosition(first);
    AudioBlock<float> ring(ring_.channels(), (size_t) channels_, (size_t) capacity_);
    const int slot = (int) (first % capacity_);
    const int head = chunk < (capacity_ - slot) ? chunk : (capacity_ - slot);
    source_->getNextAudioBlock(AudioSourceChannelInfo(&ring, slot, head));
    if (head < chunk) source_->getNextAudioBlock(AudioSourceChannelInfo(&ring, 0, chunk - head));
    sourcePos_ = last;
    workerEnd_ = last;
    publish();
    readyCv_.notify_all();
    return true;
  }

  void publish() noexcept { range_.write(Range{workerStart_, workerEnd_}); }

  sourcedetail::MaybeOwned<PositionableAudioSource> source_;
  ReadAheadThread& thread_;
  const int bufferSize_;
  const int channels_;
  const bool prefill_;
  double sampleRate_ = 44100.0;
  int capacity_ = 0;
  sourcedetail::PlanarBuffer ring_;
  sourcedetail::BlockEpoch epoch_;
  sourcedetail::SeqBox<Range> range_{Range{0, 0}};
  std::atomic<int64_t> nextPlayPos_{0};
  std::atomic<bool> prepared_{false};
  std::atomic<uint64_t> underruns_{0};
  // The reader's own, touched only on its turn (or before it is registered).
  int64_t workerStart_ = 0, workerEnd_ = 0, sourcePos_ = -1, lastPolled_ = 0;
  bool wasLooping_ = false;
  std::mutex readyMutex_;
  std::condition_variable readyCv_;
};

// ── Playing ──────────────────────────────────────────────────────────────────

/**
 * Start, stop, seek and gain over a positionable source -- with the read-ahead
 * and the sample-rate correction set up for it -- as one source.
 *
 * setSource() builds the chain the reference builds: the caller's source,
 * then a BufferingAudioSource when a read-ahead size and a thread are given,
 * then a ResamplingAudioSource when the source's rate is given, correcting it
 * to whatever rate the transport is prepared at.
 *
 * What moves is RAMPED, where the reference cuts:
 *
 *   start()          fades in over kFadeSamples. The reference starts at full
 *                    level on whatever sample it lands on.
 *   stop()           fades out over kFadeSamples, across block boundaries if
 *                    the blocks are short, then stops pulling the source. It
 *                    returns at once; isPlaying() is the request and the
 *                    fade finishes on the audio thread.
 *   a seek           while playing, is carried to the audio thread and done
 *                    there: kSeekFadeSamples of fade-out at the old position,
 *                    the jump, then the fade back in. getCurrentPosition()
 *                    reports the new position at once.
 *   setGain()        ramps across the next block.
 *
 * The position is counted in OUTPUT samples as they are produced, so with a
 * resampler in the chain it is the position being heard rather than the
 * position the resampler has read up to. Finishing -- a non-looping source
 * played past its length -- stops the transport and sends a change message
 * (ChangeBroadcaster: one atomic flag, dispatched on the UI tick).
 */
class AudioTransportSource : public PositionableAudioSource, public ChangeBroadcaster {
public:
  static constexpr int kFadeSamples = 256;     ///< Output samples of fade at start() and stop().
  static constexpr int kSeekFadeSamples = 128; ///< Output samples of fade out before a seek's jump.

  /** No source, stopped, gain 1. */
  AudioTransportSource() = default;
  /** setSource(nullptr): the chain is released once the audio thread is done
   *  with it. Not while a block is being pulled from it. */
  ~AudioTransportSource() override { setSource(nullptr); }
  AudioTransportSource(const AudioTransportSource&) = delete;            ///< Not copyable.
  AudioTransportSource& operator=(const AudioTransportSource&) = delete; ///< Not copyable.

  /**
   * Play `newSource` (not owned), or nothing. `readAheadBufferSize` > 0 with a
   * `readAheadThread` puts a BufferingAudioSource of that many frames in front
   * of it; `sourceSampleRateToCorrectFor` > 0 resamples from that rate to the
   * transport's. Stops playback. The old chain is released after the audio
   * thread is provably done with it -- including releaseResources() on the
   * old source, so a source must outlive its time in the transport: replace
   * it, or setSource(nullptr), before destroying it, as with the reference.
   *
   * A source may call this from inside its own block -- one that hands over
   * to the next when it ends. The new source is prepared there and then (the
   * caller's trade, as in the reference); the old one is released by the next
   * call from any other thread, or by the destructor, since the block that
   * would have to finish first is the one making the call.
   */
  void setSource(PositionableAudioSource* newSource, int readAheadBufferSize = 0,
                 ReadAheadThread* readAheadThread = nullptr, double sourceSampleRateToCorrectFor = 0.0,
                 int maxNumChannels = 2) {
    if (epoch_.isInside(epoch_.blockInFlight())) {
      std::lock_guard<std::mutex> lock(control_);
      if (!chain_.load(std::memory_order_relaxed) && !newSource) return;
      std::unique_ptr<Chain> old = publishLocked(
          makeChainLocked(newSource, readAheadBufferSize, readAheadThread, sourceSampleRateToCorrectFor, maxNumChannels));
      if (old) retired_.push_back(Retired{std::move(old), epoch_.blockInFlight(), prepared_});
      return;
    }
    // Every other caller, one at a time. control_ itself is never held while
    // a block is waited out: the waiting thread held it, a block called in
    // here -- or only asked the position -- and each waited for the other
    // for good.
    std::lock_guard<std::mutex> serial(swap_);
    releaseRetired();
    std::unique_lock<std::mutex> lock(control_);
    const Chain* current = chain_.load(std::memory_order_relaxed);
    if (!current && !newSource) return;
    // The same source again: let go of the old chain first, so the source is
    // released by the old chain BEFORE the new one prepares it, not after.
    if (current && newSource && current->positionable == newSource) replaceAndRelease(lock, nullptr);
    replaceAndRelease(lock, makeChainLocked(newSource, readAheadBufferSize, readAheadThread,
                                            sourceSampleRateToCorrectFor, maxNumChannels));
  }

  /** [not the audio thread] Play from the current position, fading in over
   *  kFadeSamples, and send a change message. Nothing without a source, or
   *  when already playing. */
  void start() {
    std::lock_guard<std::mutex> lock(control_);
    if (!chain_.load(std::memory_order_relaxed)) return;
    if (state_.exchange(kPlaying, std::memory_order_acq_rel) == kPlaying) return;
    inputStreamEOF_.store(false, std::memory_order_relaxed);
    sendChangeMessage();
  }

  /** [any thread] Fade out over kFadeSamples and stop, sending a change
   *  message. Returns at once; the fade finishes on the audio thread. */
  void stop() {
    int expected = kPlaying;
    if (state_.compare_exchange_strong(expected, kStopping, std::memory_order_acq_rel)) sendChangeMessage();
  }

  /** Whether playback is requested: false from the moment stop() is called,
   *  while the fade-out is still finishing on the audio thread. */
  bool isPlaying() const noexcept { return state_.load(std::memory_order_acquire) == kPlaying; }
  /** [any thread] True once a source that does not loop has played past its
   *  end, which also stopped the transport; false again at the next start()
   *  or prepareToPlay(). */
  bool hasStreamFinished() const noexcept { return inputStreamEOF_.load(std::memory_order_acquire); }

  /** [not the audio thread] Seek, in seconds at the rate the transport is
   *  prepared at: setNextReadPosition() in output samples. Before the first
   *  prepareToPlay() there is no rate, and this seeks to 0. */
  void setPosition(double newPositionInSeconds) {
    double rate = 0.0;
    {
      std::lock_guard<std::mutex> lock(control_);
      rate = sampleRate_;
    }
    setNextReadPosition((int64_t) std::floor(newPositionInSeconds * rate + 0.5));
  }
  /** [not the audio thread] The position in seconds, as heard: a seek
   *  shows at once. 0 before prepareToPlay(). */
  double getCurrentPosition() const {
    std::lock_guard<std::mutex> lock(control_);
    return sampleRate_ > 0.0 ? (double) positionLocked() / sampleRate_ : 0.0;
  }
  /** [not the audio thread] The source's length in seconds, at the output
   *  rate; 0 with no source or before prepareToPlay(). */
  double getLengthInSeconds() const {
    std::lock_guard<std::mutex> lock(control_);
    return sampleRate_ > 0.0 ? (double) lengthLocked() / sampleRate_ : 0.0;
  }

  /** [any thread] Linear gain, 1 to start, ramped across the next block. */
  void setGain(float newGain) noexcept { gain_.store(newGain, std::memory_order_relaxed); }
  /** [any thread] The gain last set, linear. */
  float getGain() const noexcept { return gain_.load(std::memory_order_relaxed); }

  /** [not the audio thread] In output samples. While playing, the jump is
   *  made on the audio thread with a fade either side of it. */
  void setNextReadPosition(int64_t newPosition) override {
    std::lock_guard<std::mutex> lock(control_);
    Chain* chain = chain_.load(std::memory_order_relaxed);
    if (!chain) return;
    if (newPosition < 0) newPosition = 0;
    pendingSeek_.store(newPosition, std::memory_order_relaxed);
    if (state_.load(std::memory_order_acquire) == kStopped) {
      // Nothing is pulling the chain: move it here, and let a read-ahead
      // buffer start filling from the new place before anybody presses play.
      // The resampler's history belongs to the audio thread, which clears it
      // before its next pull.
      chain->reading->setNextReadPosition(toSource(*chain, newPosition));
      playPos_.store(newPosition, std::memory_order_relaxed);
      flushPending_.store(true, std::memory_order_release);
      if (chain->buffering) chain->buffering->wakeReader();
    } else {
      seekPending_.store(true, std::memory_order_release);
    }
  }
  /** [not the audio thread] In OUTPUT samples, at the prepared rate: a
   *  pending seek's target, else the position being heard, wrapped while
   *  looping. 0 with no source. */
  int64_t getNextReadPosition() const override {
    std::lock_guard<std::mutex> lock(control_);
    return positionLocked();
  }
  /** [not the audio thread] The source's length in OUTPUT samples, corrected
   *  for the resampler when there is one; 0 with no source. */
  int64_t getTotalLength() const override {
    std::lock_guard<std::mutex> lock(control_);
    return lengthLocked();
  }
  bool isLooping() const override {
    std::lock_guard<std::mutex> lock(control_);
    const Chain* chain = chain_.load(std::memory_order_relaxed);
    return chain && chain->reading->isLooping();
  }
  void setLooping(bool shouldLoop) override {
    std::lock_guard<std::mutex> lock(control_);
    if (Chain* chain = chain_.load(std::memory_order_relaxed)) chain->positionable->setLooping(shouldLoop);
  }

  /**
   * [not the audio thread] For a render that may block -- an offline bounce,
   * a test: wait until the read-ahead holds the source frames the next
   * `numSamples` output frames are made from, or `timeoutMs` passes. True at
   * once without read-ahead, or with nothing left to read. The callback itself
   * never waits: what is not there yet plays as silence and is counted as an
   * underrun. Call it between blocks, from the thread that pulls them; the
   * other controls wait while it does.
   */
  bool waitForNextAudioBlockReady(int numSamples, uint32_t timeoutMs) {
    std::lock_guard<std::mutex> lock(control_);
    Chain* chain = chain_.load(std::memory_order_relaxed);
    if (!chain || !chain->buffering || numSamples <= 0) return true;
    // The resampler reads a few frames past the ones it outputs.
    int64_t need = toSource(*chain, numSamples) + (chain->resampler ? 16 : 0);
    if (!chain->buffering->isLooping()) {
      const int64_t left = chain->buffering->getTotalLength() - chain->buffering->getNextReadPosition();
      if (left <= 0) return true;
      if (need > left) need = left;
    }
    const AudioSourceChannelInfo next(nullptr, 0, (int) need);
    return chain->buffering->waitForNextAudioBlockReady(next, timeoutMs);
  }

  void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override {
    std::lock_guard<std::mutex> lock(control_);
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    blockSize_ = samplesPerBlockExpected > 0 ? samplesPerBlockExpected : 512;
    prepared_ = true;
    inputStreamEOF_.store(false, std::memory_order_relaxed);
    if (Chain* chain = chain_.load(std::memory_order_relaxed)) prepareChain(*chain);
  }

  void releaseResources() override {
    std::lock_guard<std::mutex> lock(control_);
    if (Chain* chain = chain_.load(std::memory_order_relaxed))
      if (prepared_) chain->master->releaseResources();
    prepared_ = false;
  }

  /** [audio thread] */
  void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
    if (!info.buffer || info.numSamples <= 0) return;
    sourcedetail::InBlock inBlock(epoch_);
    Chain* chain = chain_.load(std::memory_order_seq_cst);
    const float gain = gain_.load(std::memory_order_relaxed);
    // A new chain fades in from nothing. Told apart by its serial number, not
    // its address: a chain freed by setSource(nullptr) and the next one made
    // straight after are the same size, and an allocator that hands the last
    // freed block back (glibc's, macOS's) puts the new one at the old address.
    const uint64_t serial = chain ? chain->serial : 0;
    if (serial != lastSerial_) {
      lastSerial_ = serial;
      fade_ = 0.0f;
    }
    if (chain && flushPending_.exchange(false, std::memory_order_acq_rel) && chain->resampler)
      chain->resampler->flushBuffers();
    if (!chain || state_.load(std::memory_order_acquire) == kStopped) {
      info.clearActiveBufferRegion();
      if (chain && seekPending_.exchange(false, std::memory_order_acq_rel)) applySeek(*chain);
      lastGain_ = gain;
      return;
    }
    const int n = info.numSamples;
    int done = 0;
    if (seekPending_.load(std::memory_order_acquire)) {
      // Out at the old position, then the jump, then back in below.
      if (fade_ > 0.0f) {
        const int k = n < kSeekFadeSamples ? n : kSeekFadeSamples;
        render(*chain, info, 0, k, 0.0f, fade_ / (float) k);
        fade_ = 0.0f;
        done = k;
      }
      if (seekPending_.exchange(false, std::memory_order_acq_rel)) applySeek(*chain);
    }
    if (done < n) {
      const float target = state_.load(std::memory_order_acquire) == kPlaying ? 1.0f : 0.0f;
      render(*chain, info, done, n - done, target, 1.0f / (float) kFadeSamples);
    }
    const PositionableAudioSource* reading = chain->reading;
    if (!reading->isLooping() &&
        playPos_.load(std::memory_order_relaxed) >= toOutput(*chain, reading->getTotalLength())) {
      int expected = kPlaying;
      if (state_.compare_exchange_strong(expected, kStopping, std::memory_order_acq_rel)) {
        inputStreamEOF_.store(true, std::memory_order_release);
        sendChangeMessage();
      }
    }
    // Stopped only once the fade is down -- and only if nobody pressed play
    // again in the meantime, which is why this is a compare-and-swap.
    if (fade_ <= 0.0f) {
      int expected = kStopping;
      state_.compare_exchange_strong(expected, kStopped, std::memory_order_acq_rel);
    }
    sourcedetail::rampGain(info, lastGain_, gain);
    lastGain_ = gain;
  }

private:
  enum : int { kStopped = 0, kPlaying = 1, kStopping = 2 };

  /** What setSource() builds around a source, published as one pointer: the
   *  read-ahead buffer and the resampler when asked for, and which of them
   *  positions are set on and blocks pulled from. */
  struct Chain {
    PositionableAudioSource* positionable = nullptr; // the caller's
    std::unique_ptr<BufferingAudioSource> buffering;
    std::unique_ptr<ResamplingAudioSource> resampler;
    PositionableAudioSource* reading = nullptr; // positions are set and read here
    AudioSource* master = nullptr;              // blocks are pulled from here
    double sourceSampleRate = 0.0;
    uint64_t serial = 0; // 1, 2, 3... in the order setSource() made them
  };

  /** A chain set down from inside its own block: freed, and released if it
   *  was prepared, once `block` is over. */
  struct Retired {
    std::unique_ptr<Chain> chain;
    uint64_t block = 0;
    bool prepared = false;
  };

  /** The chain that plays `newSource`, prepared if the transport is; null for
   *  no source. Called with control_ held. */
  std::unique_ptr<Chain> makeChainLocked(PositionableAudioSource* newSource, int readAheadBufferSize,
                                         ReadAheadThread* readAheadThread, double sourceSampleRateToCorrectFor,
                                         int maxNumChannels) {
    if (!newSource) return nullptr;
    std::unique_ptr<Chain> next(new Chain());
    next->serial = ++chainsMade_;
    next->positionable = newSource;
    next->sourceSampleRate = sourceSampleRateToCorrectFor > 0.0 ? sourceSampleRateToCorrectFor : 0.0;
    PositionableAudioSource* reading = newSource;
    if (readAheadBufferSize > 0 && readAheadThread) {
      next->buffering.reset(new BufferingAudioSource(newSource, *readAheadThread, false, readAheadBufferSize,
                                                     maxNumChannels));
      reading = next->buffering.get();
    }
    next->reading = reading;
    next->master = reading;
    if (next->sourceSampleRate > 0.0) {
      next->resampler.reset(new ResamplingAudioSource(reading, false, maxNumChannels));
      next->master = next->resampler.get();
    }
    if (prepared_) prepareChain(*next);
    return next;
  }

  /** Publish `next` (or nothing), stopped, and hand back the chain it
   *  replaced -- which a block may still be in. Called with control_ held. */
  std::unique_ptr<Chain> publishLocked(std::unique_ptr<Chain> next) {
    state_.store(kStopped, std::memory_order_relaxed);
    inputStreamEOF_.store(false, std::memory_order_relaxed);
    seekPending_.store(false, std::memory_order_relaxed);
    flushPending_.store(false, std::memory_order_relaxed);
    playPos_.store(next ? toOutput(*next, next->reading->getNextReadPosition()) : 0, std::memory_order_relaxed);
    return std::unique_ptr<Chain>(chain_.exchange(next.release(), std::memory_order_seq_cst));
  }

  /** Whether a chain other than the one being let go still plays `source`:
   *  then releasing it is that chain's business. Called with control_ held. */
  bool inUseLocked(const PositionableAudioSource* source) const {
    const Chain* current = chain_.load(std::memory_order_relaxed);
    if (current && current->positionable == source) return true;
    for (const Retired& r : retired_)
      if (r.chain->positionable == source) return true;
    return false;
  }

  /** [not the audio thread] Publish `next` (or nothing) and release the chain
   *  it replaced once no block can be in it. The wait is made with control_
   *  let go; `lock` holds it again on return. swap_ held. */
  void replaceAndRelease(std::unique_lock<std::mutex>& lock, std::unique_ptr<Chain> next) {
    std::unique_ptr<Chain> old = publishLocked(std::move(next));
    if (!old) return;
    const bool prepared = prepared_;
    const uint64_t seen = epoch_.blockInFlight();
    lock.unlock();
    epoch_.waitUntilOver(seen);
    lock.lock();
    // A block may have put the same source back while this waited. Checked
    // and released under one hold of control_, so it cannot do so between.
    if (prepared && !inUseLocked(old->positionable)) old->master->releaseResources();
  }

  /** [not the audio thread] Release and free every chain a block set down.
   *  swap_ held, control_ not. */
  void releaseRetired() {
    std::vector<Retired> done;
    {
      std::lock_guard<std::mutex> lock(control_);
      done.swap(retired_);
    }
    for (size_t i = 0; i < done.size(); ++i) {
      epoch_.waitUntilOver(done[i].block);
      bool release = done[i].prepared;
      for (size_t j = 0; j < i && release; ++j)
        if (done[j].chain->positionable == done[i].chain->positionable) release = false;
      if (!release) continue;
      std::lock_guard<std::mutex> lock(control_);
      if (!inUseLocked(done[i].chain->positionable)) done[i].chain->master->releaseResources();
    }
  }

  /** Output samples per source sample. The rate is only written by
   *  prepareToPlay(), which does not run alongside blocks, so the audio
   *  thread may read it too. */
  double ratio(const Chain& chain) const noexcept {
    return chain.sourceSampleRate > 0.0 && sampleRate_ > 0.0 ? sampleRate_ / chain.sourceSampleRate : 1.0;
  }
  int64_t toOutput(const Chain& chain, int64_t sourceFrames) const noexcept {
    const double r = ratio(chain);
    if (r == 1.0) return sourceFrames;
    // resample.h's arithmetic, so a transport and an offline conversion of
    // the same file agree on its length to the sample.
    return (int64_t) Resampler::outputLength((size_t) (sourceFrames > 0 ? sourceFrames : 0), r);
  }
  int64_t toSource(const Chain& chain, int64_t outputFrames) const noexcept {
    const double r = ratio(chain);
    return r == 1.0 ? outputFrames : (int64_t) std::floor((double) outputFrames / r + 0.5);
  }

  void prepareChain(Chain& chain) {
    if (chain.resampler) chain.resampler->setResamplingRatio(chain.sourceSampleRate / sampleRate_);
    chain.master->prepareToPlay(blockSize_, sampleRate_);
  }

  int64_t positionLocked() const {
    const Chain* chain = chain_.load(std::memory_order_relaxed);
    if (!chain) return 0;
    if (seekPending_.load(std::memory_order_acquire)) return pendingSeek_.load(std::memory_order_relaxed);
    const int64_t pos = playPos_.load(std::memory_order_relaxed);
    const int64_t length = lengthLocked();
    return chain->reading->isLooping() && length > 0 ? pos % length : pos;
  }
  int64_t lengthLocked() const {
    const Chain* chain = chain_.load(std::memory_order_relaxed);
    return chain ? toOutput(*chain, chain->reading->getTotalLength()) : 0;
  }

  /** [audio thread] */
  void applySeek(Chain& chain) noexcept {
    const int64_t target = pendingSeek_.load(std::memory_order_relaxed);
    chain.reading->setNextReadPosition(toSource(chain, target));
    if (chain.resampler) chain.resampler->flushBuffers();
    playPos_.store(target, std::memory_order_relaxed);
  }

  /** [audio thread] Pull `count` frames into the region at `offset`, scaling
   *  each frame by the fade and moving the fade toward `target` by `step`. */
  void render(Chain& chain, const AudioSourceChannelInfo& info, int offset, int count, float target,
              float step) noexcept {
    const AudioSourceChannelInfo part(info.buffer, info.startSample + offset, count);
    chain.master->getNextAudioBlock(part);
    playPos_.fetch_add(count, std::memory_order_relaxed);
    const int channels = part.numChannels();
    for (int i = 0; i < count; ++i) {
      if (fade_ == 1.0f && target == 1.0f) break;
      for (int c = 0; c < channels; ++c) part.channel(c)[i] *= fade_;
      if (fade_ < target) fade_ = (fade_ + step) < target ? fade_ + step : target;
      else if (fade_ > target) fade_ = (fade_ - step) > target ? fade_ - step : target;
    }
  }

  std::atomic<Chain*> chain_{nullptr};
  // Held only for short work, never across a wait: a source inside a block
  // may take it, to set the next source or ask the position.
  mutable std::mutex control_;
  std::mutex swap_; // setSource() callers other than a block, one at a time
  sourcedetail::BlockEpoch epoch_;
  std::vector<Retired> retired_; // under control_
  /** kPlaying, kStopping (fading out) or kStopped (not pulling the source). */
  std::atomic<int> state_{kStopped};
  std::atomic<bool> inputStreamEOF_{false};
  std::atomic<bool> seekPending_{false}, flushPending_{false};
  std::atomic<int64_t> pendingSeek_{0}, playPos_{0};
  std::atomic<float> gain_{1.0f};
  double sampleRate_ = 0.0;
  int blockSize_ = 512;
  bool prepared_ = false;
  uint64_t chainsMade_ = 0; // under control_
  // The audio thread's own.
  float fade_ = 0.0f;
  float lastGain_ = 1.0f;
  uint64_t lastSerial_ = 0;
};

/**
 * Plays a source through an audio device: the adapter between the pull model
 * and the SDK's device backends.
 *
 * Every output backend here calls one render function with interleaved
 * floats -- WasapiOutput, CoreAudioOutput, LinuxOutput, AlsaOutput and
 * JackOutput all take a RenderFn of (float* interleaved, frames, channels),
 * and AsioOutput the same with an input buffer in front. renderFunction()
 * and renderWithInputFunction() hand those over directly:
 *
 *     standalone::WasapiOutput out;
 *     out.open(player.renderFunction());
 *     player.audioDeviceAboutToStart(out.sampleRate(), 1024, (int) out.channels());
 *     out.run();
 *
 * audioDeviceIOCallback() is the reference's planar form, for a caller that
 * already has channel pointers. Every entry point declares ScopedNoDenormals,
 * as the DSP entry points do.
 *
 * A device may hand over more frames than it promised -- WASAPI's shared mode
 * fills whatever is free -- so the interleaved paths go through scratch in
 * pieces of the prepared size rather than trusting the promise. Input
 * channels are copied onto the first outputs before the source is called,
 * as the reference does, so a source can process them.
 */
class AudioSourcePlayer {
public:
  /** No source: the callbacks output silence. Gain 1. */
  AudioSourcePlayer() = default;
  /** setSource(nullptr). The device must have stopped calling it. */
  ~AudioSourcePlayer() { setSource(nullptr); }
  AudioSourcePlayer(const AudioSourcePlayer&) = delete;            ///< Not copyable: a device holds it.
  AudioSourcePlayer& operator=(const AudioSourcePlayer&) = delete; ///< Not copyable: a device holds it.

  /** Prepared first if the device is running; the old source is released
   *  once the callback is provably done with it. A source may call this from
   *  inside the callback -- one that hands over to the next when it ends --
   *  and its own release then waits for the next call from another thread,
   *  or the destructor: the callback it would wait for is the one calling. */
  void setSource(AudioSource* newSource) {
    if (epoch_.isInside(epoch_.blockInFlight())) {
      std::lock_guard<std::mutex> lock(control_);
      AudioSource* old = source_.load(std::memory_order_relaxed);
      if (old == newSource) return;
      if (newSource && sampleRate_ > 0.0 && bufferSize_ > 0) newSource->prepareToPlay(bufferSize_, sampleRate_);
      source_.store(newSource, std::memory_order_seq_cst);
      if (old) retired_.push_back(Retired{old, epoch_.blockInFlight(), sampleRate_ > 0.0});
      return;
    }
    // Other callers one at a time, and never holding control_ across the
    // wait: a callback that called in here would wait for it while the
    // caller waited for the callback.
    std::lock_guard<std::mutex> serial(swap_);
    releaseRetired();
    AudioSource* old = nullptr;
    uint64_t seen = 0;
    bool prepared = false;
    {
      std::lock_guard<std::mutex> lock(control_);
      old = source_.load(std::memory_order_relaxed);
      if (old == newSource) return;
      if (newSource && sampleRate_ > 0.0 && bufferSize_ > 0) newSource->prepareToPlay(bufferSize_, sampleRate_);
      source_.store(newSource, std::memory_order_seq_cst);
      seen = epoch_.blockInFlight();
      prepared = sampleRate_ > 0.0;
    }
    if (!old || !prepared) return;
    epoch_.waitUntilOver(seen);
    // The callback may have put it back while this waited. Checked and
    // released under one hold of control_, so it cannot do so between.
    std::lock_guard<std::mutex> lock(control_);
    if (!inUseLocked(old)) old->releaseResources();
  }
  /** [any thread] The source playing now, not owned; null for none. */
  AudioSource* getCurrentSource() const noexcept { return source_.load(std::memory_order_acquire); }

  /** [any thread] Linear output gain, 1 to start, ramped across the next
   *  callback. */
  void setGain(float newGain) noexcept { gain_.store(newGain, std::memory_order_relaxed); }
  /** [any thread] The gain last set, linear. */
  float getGain() const noexcept { return gain_.load(std::memory_order_relaxed); }

  /** The reference's form: two outputs, no inputs. */
  void prepareToPlay(double sampleRate, int blockSize) { audioDeviceAboutToStart(sampleRate, blockSize, 2, 0); }

  /** Before the first callback: the device's rate, the block size it
   *  promises, and its channel counts. */
  void audioDeviceAboutToStart(double sampleRate, int bufferSizeSamples, int numOutputChannels = 2,
                               int numInputChannels = 0) {
    std::lock_guard<std::mutex> serial(swap_);
    releaseRetired();
    waitOutCallbacks();
    std::lock_guard<std::mutex> lock(control_);
    sampleRate_ = sampleRate;
    bufferSize_ = bufferSizeSamples > 0 ? bufferSizeSamples : 512;
    const int widest = numOutputChannels > numInputChannels ? numOutputChannels : numInputChannels;
    scratch_.setSize(sourcedetail::clampChannels(widest), bufferSize_ < kMaxChunk ? bufferSize_ : kMaxChunk);
    if (AudioSource* source = source_.load(std::memory_order_relaxed)) source->prepareToPlay(bufferSize_, sampleRate_);
    lastGain_ = gain_.load(std::memory_order_relaxed);
    ready_.store(true, std::memory_order_seq_cst);
  }

  /** After the last callback: waits out one still running, then releases
   *  the source and the scratch. The callbacks output silence until the
   *  next audioDeviceAboutToStart(). */
  void audioDeviceStopped() {
    std::lock_guard<std::mutex> serial(swap_);
    releaseRetired();
    waitOutCallbacks();
    std::lock_guard<std::mutex> lock(control_);
    if (AudioSource* source = source_.load(std::memory_order_relaxed))
      if (sampleRate_ > 0.0) source->releaseResources();
    sampleRate_ = 0.0;
    bufferSize_ = 0;
    scratch_.release();
  }

  /** [audio thread] The reference's planar callback. Null channel pointers
   *  are skipped, as a device with a disabled channel hands them over. */
  void audioDeviceIOCallback(const float* const* inputChannelData, int numInputChannels,
                             float* const* outputChannelData, int numOutputChannels, int numSamples) {
    ScopedNoDenormals noDenormals;
    if (!outputChannelData || numOutputChannels <= 0 || numSamples <= 0) return;
    sourcedetail::InBlock inBlock(epoch_);
    float* outputs[AudioSource::kMaxChannels];
    int active = 0;
    for (int c = 0; c < numOutputChannels; ++c) {
      float* d = outputChannelData[c];
      if (!d) continue;
      if (active < AudioSource::kMaxChannels) outputs[active++] = d;
      else std::memset(d, 0, (size_t) numSamples * sizeof(float));
    }
    const float gain = gain_.load(std::memory_order_relaxed);
    AudioSource* source = source_.load(std::memory_order_seq_cst);
    if (!source || !ready_.load(std::memory_order_seq_cst)) {
      for (int c = 0; c < active; ++c) std::memset(outputs[c], 0, (size_t) numSamples * sizeof(float));
      lastGain_ = gain;
      return;
    }
    int copied = 0;
    for (int c = 0; inputChannelData && c < numInputChannels && copied < active; ++c)
      if (inputChannelData[c])
        std::memcpy(outputs[copied++], inputChannelData[c], (size_t) numSamples * sizeof(float));
    for (int c = copied; c < active; ++c) std::memset(outputs[c], 0, (size_t) numSamples * sizeof(float));
    AudioBlock<float> block(outputs, (size_t) active, (size_t) numSamples);
    const AudioSourceChannelInfo info(block);
    source->getNextAudioBlock(info);
    sourcedetail::rampGain(info, lastGain_, gain);
    lastGain_ = gain;
  }

  /** [audio thread] The output backends' RenderFn shape. */
  void render(float* interleaved, uint32_t frames, uint32_t channels) {
    ScopedNoDenormals noDenormals;
    renderInterleaved(nullptr, interleaved, frames, channels);
  }

  /** [audio thread] AsioOutput's RenderFn shape: `input` may be null. */
  void renderWithInput(const float* input, float* output, uint32_t frames, uint32_t channels) {
    ScopedNoDenormals noDenormals;
    renderInterleaved(input, output, frames, channels);
  }

  /** render() as a callable a backend's open() takes. */
  std::function<void(float*, uint32_t, uint32_t)> renderFunction() {
    return [this](float* interleaved, uint32_t frames, uint32_t channels) { render(interleaved, frames, channels); };
  }
  /** renderWithInput() as a callable AsioOutput::open() takes. */
  std::function<void(const float*, float*, uint32_t, uint32_t)> renderWithInputFunction() {
    return [this](const float* in, float* out, uint32_t frames, uint32_t channels) {
      renderWithInput(in, out, frames, channels);
    };
  }

private:
  static constexpr int kMaxChunk = 2048;

  /** [audio thread] */
  void renderInterleaved(const float* input, float* output, uint32_t frames, uint32_t channels) {
    if (!output || frames == 0 || channels == 0) return;
    sourcedetail::InBlock inBlock(epoch_);
    const int n = (int) frames;
    const int stride = (int) channels;
    const float gain = gain_.load(std::memory_order_relaxed);
    AudioSource* source = source_.load(std::memory_order_seq_cst);
    // ready_ first: while it is false the scratch is being resized.
    if (!source || !ready_.load(std::memory_order_seq_cst) || scratch_.numFrames() == 0) {
      std::memset(output, 0, (size_t) n * (size_t) stride * sizeof(float));
      lastGain_ = gain;
      return;
    }
    const int chunk = scratch_.numFrames();
    const int used = stride < scratch_.numChannels() ? stride : scratch_.numChannels();
    const float step = (gain - lastGain_) / (float) n;
    for (int offset = 0; offset < n; offset += chunk) {
      const int len = (n - offset) < chunk ? (n - offset) : chunk;
      for (int c = 0; c < used; ++c) {
        float* s = scratch_.channel(c);
        if (input)
          for (int i = 0; i < len; ++i) s[i] = input[(size_t) (offset + i) * (size_t) stride + (size_t) c];
        else
          std::memset(s, 0, (size_t) len * sizeof(float));
      }
      AudioBlock<float> block(scratch_.channels(), (size_t) used, (size_t) len);
      source->getNextAudioBlock(AudioSourceChannelInfo(&block, 0, len));
      for (int i = 0; i < len; ++i) {
        const float g = lastGain_ + step * (float) (offset + i);
        float* frame = output + (size_t) (offset + i) * (size_t) stride;
        for (int c = 0; c < used; ++c) frame[c] = scratch_.channel(c)[i] * g;
        for (int c = used; c < stride; ++c) frame[c] = 0.0f;
      }
    }
    lastGain_ = gain;
  }

  /** A source set down from inside the callback: released, if the device
   *  had prepared it, once `block` is over. */
  struct Retired {
    AudioSource* source = nullptr;
    uint64_t block = 0;
    bool prepared = false;
  };

  /** Whether the callback plays `source` again, or another retired entry
   *  holds it: then its release is someone else's. control_ held. */
  bool inUseLocked(const AudioSource* source) const {
    if (source_.load(std::memory_order_relaxed) == source) return true;
    for (const Retired& r : retired_)
      if (r.source == source) return true;
    return false;
  }

  /** [not the audio thread] Release every source the callback set down.
   *  swap_ held, control_ not. */
  void releaseRetired() {
    std::vector<Retired> done;
    {
      std::lock_guard<std::mutex> lock(control_);
      done.swap(retired_);
    }
    for (size_t i = 0; i < done.size(); ++i) {
      epoch_.waitUntilOver(done[i].block);
      bool release = done[i].prepared;
      for (size_t j = 0; j < i && release; ++j)
        if (done[j].source == done[i].source) release = false;
      if (!release) continue;
      std::lock_guard<std::mutex> lock(control_);
      if (!inUseLocked(done[i].source)) done[i].source->releaseResources();
    }
  }

  /** [not the audio thread] Stop the callback reaching the source, and wait
   *  until no callback is in it -- with control_ let go for the wait. */
  void waitOutCallbacks() {
    uint64_t seen = 0;
    {
      std::lock_guard<std::mutex> lock(control_);
      ready_.store(false, std::memory_order_seq_cst);
      seen = epoch_.blockInFlight();
    }
    epoch_.waitUntilOver(seen);
  }

  std::atomic<AudioSource*> source_{nullptr};
  // Held only for short work, never across a wait: a source inside the
  // callback may take it, to set the next source.
  std::mutex control_;
  std::mutex swap_; // the device's start and stop, and setSource() from outside the callback
  sourcedetail::BlockEpoch epoch_;
  std::vector<Retired> retired_; // under control_
  std::atomic<bool> ready_{false};
  std::atomic<float> gain_{1.0f};
  double sampleRate_ = 0.0;
  int bufferSize_ = 0;
  sourcedetail::PlanarBuffer scratch_;
  float lastGain_ = 1.0f; // the audio thread's
};

} // namespace sonore
