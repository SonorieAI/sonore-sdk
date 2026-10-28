// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: convolution of any length, at zero latency, with impulse
// responses swapped while the audio runs.
//
// Convolver in fft.h is the small one: sized at compile time (16384 samples
// with its defaults), one block of latency, and an impulse that is loaded on
// the audio thread or handed over by swapping whole convolvers. That is the
// right tool for a cabinet. A convolution REVERB needs what it cannot do:
//
//   * an impulse of any length, sized when it is loaded -- a ten-second hall
//     is 480000 samples at 48 kHz;
//   * no latency at all, because a reverb sits on a send and a send that is
//     late against the dry signal smears every transient it touches;
//   * a new impulse while the music plays: built somewhere other than the
//     audio thread, handed over without a lock, and CROSSFADED in rather than
//     cut over, which clicks.
//
// How, in one paragraph each.
//
// ZERO LATENCY. The first `headBlock` samples of the impulse are a direct-
// form FIR, computed sample by sample, so the output for a sample exists the
// moment the sample does. Everything after them is convolved by FFT in
// blocks, and a block is only ever needed AFTER it has been filled: the
// segment of the impulse that a block size P serves starts at least P
// samples in, so its contribution is due no earlier than the block's end.
// With zeroLatency off the FIR is dropped and the output is one head block
// late, which costs a little less.
//
// ANY LENGTH, CHEAPLY. Non-uniform partitioning (Gardner, "Efficient
// convolution without input-output delay", JAES 1995): two partitions at the
// head block size, two at twice it, two at four times, ... up to `maxBlock`,
// and the whole rest of the impulse at maxBlock. Short blocks where latency
// matters, long blocks -- cheap per sample -- for the tail. Each size is
// uniformly partitioned (a frequency-domain delay line, as Convolver is),
// all of them on the real-signal transforms of DynamicFft.
//
// NO SPIKES. A long block's work used to arrive all at once: at the block's
// end, a multiply-accumulate over EVERY partition of the tail, which for a
// ten-second impulse is the whole impulse's spectrum. Here that sum is
// spread over the block it is for: the partitions that only need OLDER
// input are accumulated a slice at a time at each head-block boundary while
// the block fills, and its end has only the newest partition and the two
// transforms left.
//
// SWAPPING. An Engine is everything built from one impulse; it is made on
// the calling thread (or a background one, loadImpulseResponseAsync), handed
// to the audio thread through one atomic pointer, and crossfaded in over
// `crossfadeSeconds`. The engine it replaces goes back through a lock-free
// queue to be freed off the audio thread. Nothing on the audio thread
// allocates, locks, or frees.
//
// What it does NOT do, stated: a new engine starts with no input history,
// so for the length of the crossfade the new reverb's tail is building up
// while the old one's fades -- the early part of the new response is there
// at once, its late tail arrives over the impulse's own length.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#define SONORE_CONVOLUTION_THREADS 1
#include <condition_variable>
#include <thread>
#endif

#include "dsp.h" // first: impulse.h reaches resample.h, which is chained from it
#include "fft.h"
#include "impulse.h"
#include "simd.h"

namespace sonore {

/** How a Convolution partitions and swaps. Read when an impulse is LOADED
 *  (and by prepare()), so a change applies from the next load. */
struct ConvolutionOptions {
  /** The head block, a power of two from 16 to 4096: the FIR's length in
   *  zero-latency mode, the latency otherwise. 64 costs 64 multiply-adds a
   *  sample for the FIR and keeps every block boundary cheap. */
  size_t headBlock = 64;
  /** The largest partition, a power of two at least headBlock (up to 65536):
   *  the tail of a long impulse is convolved in blocks this size. Larger is
   *  cheaper per sample and costs more at each of its boundaries. */
  size_t maxBlock = 8192;
  /** No latency at all (the FIR head). Off: one headBlock of latency, and a
   *  little less work. */
  bool zeroLatency = true;
  /** How long a newly loaded impulse takes to replace the old one, in
   *  seconds. 0 cuts over at the next block -- and clicks. */
  float crossfadeSeconds = 0.05f;
};

namespace convdetail {

/** One uniformly partitioned segment of the impulse: `partitions` blocks of
 *  `block` samples, starting `offset` samples into it, whose output is
 *  written `offset + delay` samples after the input that made it. */
class Stage {
public:
  /** Build (allocates). `fft` is a transform of 2 * block points, shared
   *  between the channels and owned by the Engine. `slices` is how many
   *  head-block boundaries a block period has: the multiply-accumulate is
   *  spread over that many. */
  void build(const float* segment, size_t length, size_t block, size_t outputOffset,
             const DynamicFft* fft, size_t slices) {
    block_ = block;
    offset_ = outputOffset;
    fft_ = fft;
    bins_ = block + 1;
    row_ = (bins_ + 3) / 4 * 4;
    partitions_ = (length + block - 1) / block;
    slices_ = slices < 1 ? 1 : slices;
    irRe_.assign(partitions_ * row_, 0.0f);
    irIm_.assign(partitions_ * row_, 0.0f);
    fdlRe_.assign(partitions_ * row_, 0.0f);
    fdlIm_.assign(partitions_ * row_, 0.0f);
    accRe_.assign(row_, 0.0f);
    accIm_.assign(row_, 0.0f);
    time_.assign(2 * block, 0.0f);
    binRe_.assign(row_, 0.0f);
    binIm_.assign(row_, 0.0f);
    input_.assign(block, 0.0f);
    for (size_t p = 0; p < partitions_; ++p) {
      std::fill(time_.begin(), time_.end(), 0.0f);
      const size_t start = p * block;
      const size_t n = (std::min)(block, length - start);
      std::copy(segment + start, segment + start + n, time_.begin());
      fft_->realForward(time_.data(), &irRe_[p * row_], &irIm_[p * row_]);
    }
    reset();
  }

  /** Silence: history, the block being gathered, the sum in progress. */
  void reset() {
    std::fill(fdlRe_.begin(), fdlRe_.end(), 0.0f);
    std::fill(fdlIm_.begin(), fdlIm_.end(), 0.0f);
    std::fill(accRe_.begin(), accRe_.end(), 0.0f);
    std::fill(accIm_.begin(), accIm_.end(), 0.0f);
    std::fill(input_.begin(), input_.end(), 0.0f);
    head_ = 0;
    fill_ = 0;
    nextPartition_ = 1;
    slice_ = 0;
  }

  size_t block() const { return block_; }
  size_t partitions() const { return partitions_; }

  /** [audio-thread] Append `n` input samples: never more than the head
   *  block, and never past this stage's own block end -- the engine feeds
   *  head-block chunks, and every stage's block is a whole number of them. */
  void push(const float* x, size_t n) {
    std::copy(x, x + n, input_.begin() + (std::ptrdiff_t) fill_);
    fill_ += n;
  }

  /** [audio-thread] A head-block boundary at absolute input time `time`.
   *  Finishes the block if this stage's is now full (writing its output into
   *  `ring`), then does this period's next slice of older partitions. */
  void boundary(uint64_t time, float* ring, size_t ringMask) {
    workThisCall_ = 0;
    if (fill_ == block_) {
      fill_ = 0;
      completeBlock(time, ring, ringMask);
    }
    // A share of the partitions that read older input, for the block now
    // filling: every boundary of the period does one, and the last of them
    // finishes the rest, so the block end has only partition 0 left.
    const size_t remaining = partitions_ > nextPartition_ ? partitions_ - nextPartition_ : 0;
    if (remaining > 0) {
      const size_t slicesLeft = slices_ > slice_ ? slices_ - slice_ : 1;
      const size_t count = (remaining + slicesLeft - 1) / slicesLeft;
      accumulate(nextPartition_, nextPartition_ + count);
      nextPartition_ += count;
      ++slice_;
    }
    if (workThisCall_ > peakWork_) peakWork_ = workThisCall_;
  }

  /** The most partitions multiplied in any one boundary call so far: how
   *  well the work is spread (a test reads it; nothing depends on it). */
  size_t peakPartitionsPerBoundary() const { return peakWork_; }

private:
  void completeBlock(uint64_t time, float* ring, size_t ringMask) {
    // Whatever the period's slices did not reach, now.
    if (nextPartition_ < partitions_) accumulate(nextPartition_, partitions_);
    // The block's spectrum, zero-padded to 2P so the circular convolution
    // the FFT performs is the linear one, at the head of the delay line.
    std::copy(input_.begin(), input_.end(), time_.begin());
    std::fill(time_.begin() + (std::ptrdiff_t) block_, time_.end(), 0.0f);
    head_ = head_ == 0 ? partitions_ - 1 : head_ - 1;
    float* xr = &fdlRe_[head_ * row_];
    float* xi = &fdlIm_[head_ * row_];
    fft_->realForward(time_.data(), xr, xi);
    // ...times partition 0, onto the sum the period built.
    multiplyAccumulate(xr, xi, &irRe_[0], &irIm_[0]);
    ++workThisCall_;
    std::copy(accRe_.begin(), accRe_.begin() + (std::ptrdiff_t) bins_, binRe_.begin());
    std::copy(accIm_.begin(), accIm_.begin() + (std::ptrdiff_t) bins_, binIm_.begin());
    fft_->realInverse(binRe_.data(), binIm_.data(), time_.data());
    // 2P samples of output, the first of them due `offset_` after the
    // block's first input sample: added into the ring, overlap included.
    const uint64_t start = time - block_ + offset_;
    for (size_t i = 0; i < 2 * block_; ++i) ring[(size_t) (start + i) & ringMask] += time_[i];
    // The next block's sum starts empty, and reads the delay line from the
    // block just completed (partition 1) backwards.
    std::fill(accRe_.begin(), accRe_.end(), 0.0f);
    std::fill(accIm_.begin(), accIm_.end(), 0.0f);
    nextPartition_ = 1;
    slice_ = 0;
  }

  /** Partitions [from, to) against the history blocks they meet: partition
   *  p against the block p - 1 blocks before the newest one, which is what
   *  the block now filling will be p blocks after. */
  void accumulate(size_t from, size_t to) {
    workThisCall_ += to - from;
    for (size_t p = from; p < to; ++p) {
      const size_t slot = (head_ + p - 1) % partitions_;
      multiplyAccumulate(&fdlRe_[slot * row_], &fdlIm_[slot * row_], &irRe_[p * row_], &irIm_[p * row_]);
    }
  }

  /** acc += x * h over the bins, four at a time. */
  void multiplyAccumulate(const float* xr, const float* xi, const float* hr, const float* hi) {
    float* ar = accRe_.data();
    float* ai = accIm_.data();
    for (size_t i = 0; i < row_; i += 4) {
      const simd::Vec4f a = simd::Vec4f::load(xr + i), b = simd::Vec4f::load(xi + i);
      const simd::Vec4f c = simd::Vec4f::load(hr + i), d = simd::Vec4f::load(hi + i);
      (simd::Vec4f::load(ar + i) + a * c - b * d).store(ar + i);
      (simd::Vec4f::load(ai + i) + a * d + b * c).store(ai + i);
    }
  }

  size_t block_ = 0, offset_ = 0, bins_ = 0, row_ = 0, partitions_ = 0, slices_ = 1;
  const DynamicFft* fft_ = nullptr;
  std::vector<float> irRe_, irIm_, fdlRe_, fdlIm_, accRe_, accIm_, time_, binRe_, binIm_, input_;
  size_t head_ = 0, fill_ = 0, nextPartition_ = 1, slice_ = 0;
  size_t workThisCall_ = 0, peakWork_ = 0;
};

/** One channel's convolution: the FIR head, the stages, and the ring their
 *  outputs are summed in. */
class Channel {
public:
  void build(const float* ir, size_t length, const ConvolutionOptions& options,
             const std::vector<std::unique_ptr<DynamicFft>>& ffts) {
    const size_t p0 = options.headBlock;
    zeroLatency_ = options.zeroLatency;
    head_ = p0;
    const size_t delay = zeroLatency_ ? 0 : p0;
    // The FIR head: the first p0 taps, in the order the history is read
    // (newest first), so the sum is one dot product.
    fir_.assign(zeroLatency_ ? p0 : 0, 0.0f);
    for (size_t i = 0; i < fir_.size() && i < length; ++i) fir_[i] = ir[i];
    history_.assign(2 * p0, 0.0f);
    // Gardner's plan: two partitions a size, doubling, up to maxBlock.
    stages_.clear();
    size_t offset = zeroLatency_ ? p0 : 0, block = p0, ringNeed = 4 * p0;
    for (size_t index = 0; offset < length; ++index) {
      const size_t take = block >= options.maxBlock ? length - offset : (std::min)(length - offset, 2 * block);
      stages_.emplace_back();
      stages_.back().build(ir + offset, take, block, offset + delay, ffts[index].get(), block / p0);
      ringNeed = (std::max)(ringNeed, offset + delay + 2 * block + p0);
      offset += take;
      if (block < options.maxBlock) block *= 2;
    }
    size_t ring = 1;
    while (ring < ringNeed) ring <<= 1;
    ring_.assign(ring, 0.0f);
    mask_ = ring - 1;
    reset();
  }

  void reset() {
    std::fill(history_.begin(), history_.end(), 0.0f);
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    for (Stage& s : stages_) s.reset();
    time_ = 0;
    fill_ = 0;
    pos_ = 0;
  }

  /** [audio-thread] `n` samples of `in` convolved into `out` (which may be
   *  `in`). No allocation. */
  void process(const float* in, float* out, size_t n) {
    size_t done = 0;
    while (done < n) {
      const size_t chunk = (std::min)(n - done, head_ - fill_);
      for (Stage& s : stages_) s.push(in + done, chunk);
      for (size_t i = 0; i < chunk; ++i) {
        const float x = in[done + i];
        float y = 0.0f;
        if (zeroLatency_) {
          // Newest first and contiguous: written twice, half a buffer apart.
          pos_ = pos_ == 0 ? head_ - 1 : pos_ - 1;
          history_[pos_] = history_[pos_ + head_] = x;
          y = simd::dot(fir_.data(), history_.data() + pos_, head_);
        }
        const size_t at = (size_t) (time_ + i) & mask_;
        y += ring_[at];
        ring_[at] = 0.0f;
        out[done + i] = y;
      }
      time_ += chunk;
      fill_ += chunk;
      done += chunk;
      if (fill_ == head_) {
        fill_ = 0;
        for (Stage& s : stages_) s.boundary(time_, ring_.data(), mask_);
      }
    }
  }

  size_t stageCount() const { return stages_.size(); }
  const Stage& stage(size_t i) const { return stages_[i]; }

private:
  bool zeroLatency_ = true;
  size_t head_ = 64;
  std::vector<float> fir_, history_, ring_;
  std::vector<Stage> stages_;
  size_t mask_ = 0;
  uint64_t time_ = 0;
  size_t fill_ = 0, pos_ = 0;
};

} // namespace convdetail

/**
 * Everything built from one impulse response: a convolution per channel and
 * the transforms they share. Immutable once built except for the audio
 * state, which only the audio thread touches after it is handed over.
 */
class ConvolutionEngine {
public:
  /** Build for `channels` output channels from `ir` (a mono response
   *  serves every channel; a stereo one is L to L, R to R) at the options
   *  given. Allocates: never on the audio thread. An empty response builds
   *  the identity, delayed by the latency. */
  ConvolutionEngine(const ImpulseResponse& ir, size_t channels, const ConvolutionOptions& options)
      : options_(sanitise(options)) {
    length_ = ir.length();
    const size_t n = channels < 1 ? 1 : channels;
    if (length_ == 0) {
      identity_ = true;
      delay_.assign(n, std::vector<float>(options_.zeroLatency ? 1 : options_.headBlock, 0.0f));
      delayPos_.assign(n, 0);
      return;
    }
    // One transform per block size, shared by the channels: the tables are
    // read-only, and transforms are const.
    for (size_t block = options_.headBlock;; block *= 2) {
      ffts_.push_back(std::make_unique<DynamicFft>(2 * block));
      if (block >= options_.maxBlock) break;
    }
    channels_.resize(n);
    for (size_t c = 0; c < n; ++c) channels_[c].build(ir.channel(c), length_, options_, ffts_);
  }

  /** The output's latency in samples: 0 in zero-latency mode, the head
   *  block otherwise. */
  size_t latency() const { return options_.zeroLatency ? 0 : options_.headBlock; }
  /** The response's length in samples (0 for the identity). */
  size_t length() const { return length_; }
  /** Channels it was built for. */
  size_t numChannels() const { return identity_ ? delay_.size() : channels_.size(); }
  /** How the response was partitioned: stages for channel 0, and each one's
   *  block size and partition count. For a test or a log. */
  size_t stageCount() const { return channels_.empty() ? 0 : channels_[0].stageCount(); }
  size_t stageBlock(size_t i) const { return channels_[0].stage(i).block(); }
  size_t stagePartitions(size_t i) const { return channels_[0].stage(i).partitions(); }
  size_t stagePeakPartitionsPerBoundary(size_t i) const {
    return channels_[0].stage(i).peakPartitionsPerBoundary();
  }

  /** [audio-thread] Silence everything in flight. No allocation. */
  void reset() {
    for (convdetail::Channel& c : channels_) c.reset();
    for (std::vector<float>& d : delay_) std::fill(d.begin(), d.end(), 0.0f);
    for (size_t& p : delayPos_) p = 0;
  }

  /** [audio-thread] One channel of `n` samples, `in` to `out` (may be the
   *  same buffer). Channels past the ones built for are left alone. */
  void process(size_t channel, const float* in, float* out, size_t n) {
    if (identity_) {
      if (channel >= delay_.size()) return;
      std::vector<float>& d = delay_[channel];
      size_t& p = delayPos_[channel];
      if (options_.zeroLatency) {
        if (in != out) std::copy(in, in + n, out);
        return;
      }
      for (size_t i = 0; i < n; ++i) {
        const float x = in[i];
        out[i] = d[p];
        d[p] = x;
        if (++p == d.size()) p = 0;
      }
      return;
    }
    if (channel < channels_.size()) channels_[channel].process(in, out, n);
  }

private:
  static ConvolutionOptions sanitise(ConvolutionOptions o) {
    auto pow2 = [](size_t v, size_t lo, size_t hi) {
      v = (std::max)(lo, (std::min)(hi, v));
      size_t p = 1;
      while (p < v) p <<= 1;
      return p;
    };
    o.headBlock = pow2(o.headBlock, 16, 4096);
    o.maxBlock = pow2(o.maxBlock, o.headBlock, 65536);
    if (!(o.crossfadeSeconds >= 0.0f)) o.crossfadeSeconds = 0.0f;
    return o;
  }

  ConvolutionOptions options_;
  size_t length_ = 0;
  bool identity_ = false;
  std::vector<std::unique_ptr<DynamicFft>> ffts_;
  std::vector<convdetail::Channel> channels_;
  std::vector<std::vector<float>> delay_;
  std::vector<size_t> delayPos_;
};

/**
 * Convolution reverb and cabinet, of any length, at zero latency, with
 * impulse responses that change while the audio runs.
 *
 *   // prepare(): the rate the impulses are resampled to, and the scratch.
 *   conv.prepare(spec);
 *   // message thread (or a background one): build, then hand over.
 *   ImpulseResponse ir;
 *   ir.loadFromFile(path, spec.sampleRate);
 *   conv.loadImpulseResponse(ir);
 *   // audio thread:
 *   conv.process(block);
 *
 * Until an impulse is loaded it passes the signal through, at its latency.
 * A load builds on the calling thread and publishes wait-free; the audio
 * thread picks the new engine up at its next block and crossfades to it.
 */
class Convolution {
public:
  Convolution() = default;
  Convolution(const Convolution&) = delete;
  Convolution& operator=(const Convolution&) = delete;
  ~Convolution() {
    stopWorker();
    delete pending_.exchange(nullptr);
    delete current_;
    delete fading_;
    collectGarbage();
  }

  /** The options the next load (and prepare()) builds with. From the thread
   *  that loads, and not while a loadImpulseResponseAsync() is still
   *  building: the background loader reads them, unlocked. */
  void setOptions(const ConvolutionOptions& options) { options_ = options; }
  /** The options the next load builds with. */
  const ConvolutionOptions& options() const { return options_; }

  /**
   * Prepare for a sample rate, a block size and a channel count. Allocates,
   * and must not run while process() does (the wrappers never call them at
   * once). An impulse already loaded is rebuilt at the new rate, so a
   * session switched from 44.1 to 96 kHz keeps sounding the same.
   */
  void prepare(const ProcessSpec& spec) {
    stopWorker();
#if defined(SONORE_CONVOLUTION_THREADS)
    {
      // An async load that had not started becomes the impulse to build.
      std::lock_guard<std::mutex> lock(mutex_);
      if (hasJob_) {
        setSource(job_);
        job_ = ImpulseResponse();
        hasJob_ = false;
      }
    }
#endif
    sampleRate_ = spec.sampleRate > 0.0 ? spec.sampleRate : 48000.0;
    channels_ = spec.numChannels < 1 ? 1 : spec.numChannels;
    const size_t maxBlock = spec.maximumBlockSize < 1 ? 1 : spec.maximumBlockSize;
    input_.assign(channels_, std::vector<float>(maxBlock, 0.0f));
    faded_.assign(channels_, std::vector<float>(maxBlock, 0.0f));
    maxBlock_ = maxBlock;
    // Whatever was in flight is replaced outright: prepare() is not a time
    // to crossfade.
    delete pending_.exchange(nullptr);
    delete fading_;
    fading_ = nullptr;
    ImpulseResponse rated = source();
    rated.resampleTo(sampleRate_);
    delete current_;
    current_ = new ConvolutionEngine(rated, channels_, options_);
    latency_.store(current_->latency());
    tail_.store(current_->length());
    collectGarbage();
  }

  /**
   * Load `ir` -- at whatever rate it is held at; it is resampled to the
   * prepared rate here -- build the engine on THIS thread, and hand it to the
   * audio thread, which crossfades to it. Never call from the audio thread.
   * Returns false for an empty response (use clearImpulseResponse()).
   */
  bool loadImpulseResponse(const ImpulseResponse& ir) {
    if (ir.length() == 0) return false;
    publish(build(ir));
    return true;
  }

  /** As loadImpulseResponse(), from planar samples: `numChannels` arrays of
   *  `length` samples at `sampleRate`, normalised as asked and, with `trim`,
   *  stripped of leading and trailing silence first. */
  bool loadImpulseResponse(const float* const* samples, size_t numChannels, size_t length, double sampleRate,
                           ImpulseNormalise normalise = ImpulseNormalise::Energy, bool trim = false) {
    if (!samples || numChannels == 0 || length == 0) return false;
    std::vector<float> interleaved(numChannels * length);
    for (size_t c = 0; c < numChannels; ++c)
      for (size_t i = 0; i < length; ++i) interleaved[i * numChannels + c] = samples[c][i];
    ImpulseResponse ir;
    if (!ir.loadFromSamples(interleaved.data(), length, (uint16_t) numChannels, sampleRate, sampleRate, normalise))
      return false;
    if (trim) ir.trimSilence();
    return loadImpulseResponse(ir);
  }

  /** Back to no impulse: the signal passes through, crossfaded to. */
  void clearImpulseResponse() {
    setSource(ImpulseResponse());
    publish(new ConvolutionEngine(ImpulseResponse(), channels_, options_));
  }

  /**
   * Load on a BACKGROUND thread: returns at once, and the engine reaches the
   * audio thread when it is built. A newer request supersedes one not yet
   * started. Where threads do not exist (WebAssembly without pthreads) it
   * builds here instead, which is the same result later rather than sooner.
   */
  void loadImpulseResponseAsync(ImpulseResponse ir) {
#if defined(SONORE_CONVOLUTION_THREADS)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      job_ = std::move(ir);
      hasJob_ = true;
      if (!worker_.joinable()) {
        stop_ = false;
        worker_ = std::thread([this] { workerLoop(); });
      }
    }
    wake_.notify_one();
#else
    loadImpulseResponse(ir);
#endif
  }

  /** Wait until every async load requested so far has been built and handed
   *  over (not necessarily crossfaded in yet). For tests, and for a plugin
   *  that must know its impulse is in place before rendering offline. */
  void waitForAsyncLoads() {
#if defined(SONORE_CONVOLUTION_THREADS)
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return !hasJob_ && !building_; });
#endif
  }

  /**
   * [audio-thread] Convolve `block` in place. Picks up a newly loaded
   * engine and crossfades to it. No allocation, no lock, no free: blocks
   * longer than prepare()'s maximum are processed in pieces.
   */
  void process(AudioBlock<float>& block) {
    const size_t channels = (std::min)(block.getNumChannels(), channels_);
    size_t done = 0;
    const size_t total = block.getNumSamples();
    while (done < total) {
      const size_t n = (std::min)(total - done, maxBlock_);
      processPiece(block, done, n, channels);
      done += n;
    }
  }

  /** [audio-thread] Silence whatever is in flight: the history of the
   *  engine playing (and of the one fading out). */
  void reset() {
    if (current_) current_->reset();
    if (fading_) fading_->reset();
  }

  /** The output's latency in samples: report it to the host. */
  int latencySamples() const { return (int) latency_.load(); }
  /** The impulse's length in samples at the prepared rate: the tail to
   *  report. 0 with no impulse. */
  size_t tailSamples() const { return tail_.load(); }

  /** Free the engines the audio thread has finished with. load*, prepare()
   *  and the background loader all call it; a plugin that loads rarely and
   *  wants the memory back sooner can call it from a timer. Never from the
   *  audio thread. */
  void collectGarbage() {
    // One consumer at a time: the message thread and the background loader
    // can both get here, and the queue has one reading end.
    std::lock_guard<std::mutex> lock(garbageMutex_);
    ConvolutionEngine* e = nullptr;
    while (retired_.pop(e)) delete e;
  }

private:
  /** A fixed-size single-producer single-consumer queue of retired engines:
   *  the audio thread pushes, a non-audio thread pops and frees. */
  class RetireQueue {
  public:
    bool push(ConvolutionEngine* e) {
      const size_t head = head_.load(std::memory_order_relaxed);
      const size_t next = (head + 1) % kSize;
      if (next == tail_.load(std::memory_order_acquire)) return false;
      slots_[head] = e;
      head_.store(next, std::memory_order_release);
      return true;
    }
    bool pop(ConvolutionEngine*& e) {
      const size_t tail = tail_.load(std::memory_order_relaxed);
      if (tail == head_.load(std::memory_order_acquire)) return false;
      e = slots_[tail];
      tail_.store((tail + 1) % kSize, std::memory_order_release);
      return true;
    }

  private:
    static constexpr size_t kSize = 64;
    ConvolutionEngine* slots_[kSize] = {};
    std::atomic<size_t> head_{0}, tail_{0};
  };

  ConvolutionEngine* build(const ImpulseResponse& ir) {
    setSource(ir);
    ImpulseResponse rated = ir;
    rated.resampleTo(sampleRate_);
    return new ConvolutionEngine(rated, channels_, options_);
  }

  void publish(ConvolutionEngine* engine) {
    latency_.store(engine->latency());
    tail_.store(engine->length());
    // One slot: an engine the audio thread never took is simply replaced,
    // and it is freed here because nothing else can reach it any more.
    delete pending_.exchange(engine, std::memory_order_acq_rel);
    collectGarbage();
  }

  void processPiece(AudioBlock<float>& block, size_t offset, size_t n, size_t channels) {
    // A new engine, if one is waiting and no crossfade is under way.
    if (!fading_) {
      ConvolutionEngine* fresh = pending_.exchange(nullptr, std::memory_order_acq_rel);
      if (fresh) {
        const bool canFade = current_ && fadeLength() > 0 && input_.size() >= channels &&
                             !input_.empty() && input_[0].size() >= n;
        if (!canFade) {
          retire(current_);
          current_ = fresh;
        } else {
          fading_ = current_;
          current_ = fresh;
          fadePos_ = 0;
        }
      }
    }
    if (!current_) return; // not prepared: leave the audio alone
    for (size_t c = 0; c < channels; ++c) {
      float* data = block.getChannelPointer(c) + offset;
      if (fading_) std::copy(data, data + n, input_[c].begin());
      current_->process(c, data, data, n);
      if (fading_) fading_->process(c, input_[c].data(), faded_[c].data(), n);
    }
    if (!fading_) return;
    // Raised cosine, equal gain: the two outputs are the same input through
    // two responses, mostly alike, so their gains sum to one.
    const size_t length = fadeLength();
    for (size_t i = 0; i < n; ++i) {
      const size_t at = fadePos_ + i;
      const float g = at >= length ? 1.0f : 0.5f - 0.5f * std::cos(3.14159265358979f * (float) at / (float) length);
      for (size_t c = 0; c < channels; ++c) {
        float* data = block.getChannelPointer(c) + offset;
        data[i] = g * data[i] + (1.0f - g) * faded_[c][i];
      }
    }
    fadePos_ += n;
    if (fadePos_ >= length) {
      retire(fading_);
      fading_ = nullptr;
    }
  }

  size_t fadeLength() const { return (size_t) (options_.crossfadeSeconds * sampleRate_); }

  // The impulse as loaded, before resampling: what prepare() rebuilds from
  // at a new rate. Written by whichever thread loads, read by prepare().
  void setSource(const ImpulseResponse& ir) {
    std::lock_guard<std::mutex> lock(sourceMutex_);
    source_ = ir;
  }
  ImpulseResponse source() {
    std::lock_guard<std::mutex> lock(sourceMutex_);
    return source_;
  }

  /** [audio-thread] Hand an engine back to be freed elsewhere. If the queue
   *  is full -- nobody has collected for 64 swaps -- it is kept alive in a
   *  holding list instead of being freed here. */
  void retire(ConvolutionEngine* e) {
    if (!e) return;
    if (retired_.push(e)) return;
    for (ConvolutionEngine*& slot : held_)
      if (!slot) {
        slot = e;
        return;
      }
    // 64 + 16 engines unreclaimed: the oldest held one is leaked rather than
    // freed on the audio thread.
  }

#if defined(SONORE_CONVOLUTION_THREADS)
  void workerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      wake_.wait(lock, [this] { return stop_ || hasJob_; });
      if (stop_) break;
      ImpulseResponse ir = std::move(job_);
      hasJob_ = false;
      building_ = true;
      lock.unlock();
      ConvolutionEngine* engine = build(ir);
      publish(engine);
      lock.lock();
      building_ = false;
      idle_.notify_all();
    }
  }
#endif

  void stopWorker() {
#if defined(SONORE_CONVOLUTION_THREADS)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    stop_ = false;
#endif
    // The holding list is reachable again once nothing runs.
    for (ConvolutionEngine*& slot : held_) {
      delete slot;
      slot = nullptr;
    }
  }

  ConvolutionOptions options_;
  double sampleRate_ = 48000.0;
  size_t channels_ = 2, maxBlock_ = 512;
  ImpulseResponse source_;
  std::mutex sourceMutex_, garbageMutex_;
  std::atomic<ConvolutionEngine*> pending_{nullptr};
  ConvolutionEngine* current_ = nullptr;
  ConvolutionEngine* fading_ = nullptr;
  size_t fadePos_ = 0;
  RetireQueue retired_;
  ConvolutionEngine* held_[16] = {};
  std::vector<std::vector<float>> input_, faded_;
  std::atomic<size_t> latency_{0}, tail_{0};
#if defined(SONORE_CONVOLUTION_THREADS)
  std::mutex mutex_;
  std::condition_variable wake_, idle_;
  std::thread worker_;
  ImpulseResponse job_;
  bool hasJob_ = false, building_ = false, stop_ = false;
#endif
};

} // namespace sonore
