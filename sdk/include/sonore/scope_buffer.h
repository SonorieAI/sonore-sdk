// SPDX-License-Identifier: Apache-2.0
//
// The scope's buffer, with no user interface anywhere near it.
//
// ── Why this is not part of the widget ──────────────────────────────────────
//
// The producer is the AUDIO THREAD and the consumer is a component. Keeping
// both inside one Widget would mean the wrapper -- which has to feed it from
// process() -- depending on the whole UI stack: fonts, look and feel, the
// rasteriser. It would also mean the buffer only existing while an editor is
// open, so a scope would start blank every time somebody opened the window
// instead of showing the last second of audio.
//
// So the buffer lives here, owned by whatever owns the audio, and the widget
// points at one. Exactly the split WaveformView and AudioThumbnail already
// have, and the same one MeterState has had since before there was a native UI.
//
// ── The real-time constraint ────────────────────────────────────────────────
//
// push() allocates nothing, takes no lock and does not block. The buffer is
// fixed-size and written with relaxed atomics; the UI reads whatever is there.
// A torn read shows one wrong column for one frame at thirty frames a second,
// which is invisible. A lock on the audio thread is a dropout, which is not.
#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>
#include "simd.h"

namespace sonore {

/**
 * A ring of min/max columns.
 *
 * Each column is the minimum and maximum of however many samples fell into it,
 * not one sample picked out of the group. A scope that plotted every Nth sample
 * would alias badly -- a 10 kHz tone at 48 kHz is under five samples a cycle,
 * and sampling that at pixel intervals draws a slow wobble that is not in the
 * audio. Min and max draws the envelope, which is what an analogue scope shows.
 */
class AudioScopeBuffer {
public:
  /** The most columns setColumns() keeps: the size of the fixed storage. */
  static constexpr int kMaxColumns = 1024;

  /** How many columns of history to keep. Clamped, because the storage is fixed:
   *  a real-time producer cannot be handed a container that might reallocate
   *  underneath it. */
  void setColumns(int columns) {
    const int n = columns < 8 ? 8 : (columns > kMaxColumns ? kMaxColumns : columns);
    columns_.store(n, std::memory_order_relaxed);
    clear();
  }

  /** [any thread] Columns of history in use, 8 .. kMaxColumns; 256 until
   *  setColumns(). */
  int columns() const { return columns_.load(std::memory_order_relaxed); }

  /** How many samples make one column. Larger is a slower sweep. */
  void setSamplesPerColumn(int samples) {
    samplesPerColumn_.store(samples < 1 ? 1 : samples, std::memory_order_relaxed);
  }

  /** [any thread] Samples folded into each column, at least 1; 128 until
   *  setSamplesPerColumn(). */
  int samplesPerColumn() const { return samplesPerColumn_.load(std::memory_order_relaxed); }

  /** [any thread] Empty the display. The columns are atomics and are zeroed
   *  here; the audio thread's own half-built column and write position are
   *  reset BY the audio thread, at the start of its next push. clear() --
   *  and setColumns(), which calls it from the editor -- wrote those plain
   *  fields from the main thread while push() was writing them, a data race
   *  on every resize of a running scope. */
  void clear() {
    for (int i = 0; i < kMaxColumns; ++i) {
      low_[i].store(0.0f, std::memory_order_relaxed);
      high_[i].store(0.0f, std::memory_order_relaxed);
    }
    resetRequested_.store(true, std::memory_order_release);
  }

  /**
   * [audio thread] Feed a block.
   *
   * `channels` may be null for silence. A bypassed plugin still has to keep the
   * sweep moving, or the scope freezes on the last thing it saw and looks
   * broken rather than quiet.
   */
  void push(const float* const* channels, uint32_t numChannels, uint32_t numFrames) {
    pushImpl(channels, numChannels, numFrames);
  }

  /**
   * [audio thread] Advance the sweep with silence.
   *
   * Its own name because push(nullptr, 0, n) is AMBIGUOUS between the two
   * overloads and will not compile -- which is a sharp edge every caller would
   * have to discover and then work around with a cast. A bypassed or idle
   * plugin still has to keep the sweep moving, or the scope freezes on the last
   * thing it saw and looks broken rather than quiet.
   */
  void pushSilence(uint32_t numFrames) {
    pushImpl<float>(nullptr, 0, numFrames);
  }

  /** [audio thread] The same, for a host running the DSP in double precision.
   *  Without it that path has a display that never moves, which reads as a
   *  broken plugin rather than as an unsupported mode. */
  void push(const double* const* channels, uint32_t numChannels, uint32_t numFrames) {
    pushImpl(channels, numChannels, numFrames);
  }

private:
  template <typename Sample>
  void pushImpl(const Sample* const* channels, uint32_t numChannels, uint32_t numFrames) {
    // Looked at before it is taken: an exchange is a locked instruction, a
    // full barrier on x86, paid on every block for a request that comes when
    // an editor resizes. A request made after the look is taken by the next
    // push, as one made just after the exchange always was.
    if (resetRequested_.load(std::memory_order_relaxed) &&
        resetRequested_.exchange(false, std::memory_order_acq_rel)) {
      writeIndex_.store(0, std::memory_order_release);
      accLow_ = 0.0f;
      accHigh_ = 0.0f;
      accCount_ = 0;
    }
    const int perColumn = samplesPerColumn();
    const int n = columns();
    // A column at a time rather than a sample at a time: the range of the
    // frames that finish (or continue) the current column in one pass, then
    // the store. Every wrapper pushes every block, editor open or not, and the
    // per-sample loop was 150 to 200 ns of each; the mono float feed they all
    // give it now goes four samples at a time.
    uint32_t f = 0;
    while (f < numFrames) {
      const int room = perColumn - accCount_;
      uint32_t take = room > 0 ? (uint32_t) room : 1u;
      if (take > numFrames - f) take = numFrames - f;
      float low = 0.0f, high = 0.0f;
      columnRange(channels, numChannels, f, take, low, high);
      if (accCount_ == 0) {
        accLow_ = low;
        accHigh_ = high;
      } else {
        if (low < accLow_) accLow_ = low;
        if (high > accHigh_) accHigh_ = high;
      }
      accCount_ += (int) take;
      f += take;
      if (accCount_ >= perColumn) {
        // Wrapped to THIS push's column count: the editor may have shrunk it
        // since the index was written, and the slot must stay inside it.
        const int at = writeIndex_.load(std::memory_order_relaxed) % n;
        low_[at].store(accLow_, std::memory_order_relaxed);
        high_[at].store(accHigh_, std::memory_order_relaxed);
        writeIndex_.store((at + 1) % n, std::memory_order_release);
        accCount_ = 0;
      }
    }
  }

  /** The lowest and highest of `count` frames from `from`, each frame summed
   *  to mono -- a scope showing each channel separately needs as many rows,
   *  and a plugin editor has room for one. */
  //
  // Two chains of each, as measureBlock (gui.h) takes them: one running min
  // and max per four samples waited three cycles on the step before, for
  // each of the 512 samples of a block. The range is the same either way --
  // a minimum does not depend on the order it is taken in -- for anything
  // but a NaN, which the output guard has silenced before the scope sees it.
  // (A column holding both zeros may keep the other one: -0 and +0 compare
  // equal, and are the same point on a scope.)
  template <typename Sample>
  static void columnRange(const Sample* const* channels, uint32_t numChannels, uint32_t from,
                          uint32_t count, float& low, float& high) {
    if constexpr (std::is_same<Sample, float>::value) {
      if (channels && numChannels == 1 && channels[0] && count >= 4) {
        const float* x = channels[0] + from;
        simd::Vec4f lo = simd::Vec4f::load(x), hi = lo;
        uint32_t k = 4;
        if (k + 4 <= count) {
          simd::Vec4f lo1 = simd::Vec4f::load(x + k), hi1 = lo1;
          for (k += 4; k + 8 <= count; k += 8) {
            const simd::Vec4f a = simd::Vec4f::load(x + k);
            const simd::Vec4f b = simd::Vec4f::load(x + k + 4);
            lo = (lo.min)(a);
            hi = (hi.max)(a);
            lo1 = (lo1.min)(b);
            hi1 = (hi1.max)(b);
          }
          lo = (lo.min)(lo1);
          hi = (hi.max)(hi1);
        }
        for (; k + 4 <= count; k += 4) {
          const simd::Vec4f v = simd::Vec4f::load(x + k);
          lo = (lo.min)(v);
          hi = (hi.max)(v);
        }
        float los[4], his[4];
        lo.store(los);
        hi.store(his);
        low = los[0];
        high = his[0];
        for (int j = 1; j < 4; ++j) {
          if (los[j] < low) low = los[j];
          if (his[j] > high) high = his[j];
        }
        for (; k < count; ++k) {
          if (x[k] < low) low = x[k];
          if (x[k] > high) high = x[k];
        }
        return;
      }
    } else {
      // The 64-bit feed, mono as every wrapper gives it: it took the general
      // loop below, a channel sum and a first-frame test per sample. Each
      // value is exactly what that loop makes of one channel -- 0 + x, which
      // turns -0 into +0, over a count of 1 -- in four chains.
      if (channels && numChannels == 1 && channels[0] && count >= 4) {
        const Sample* x = channels[0] + from;
        uint32_t k = 4;
#if defined(SONORE_SIMD_SSE)
        // In double, rounded to float at the end: rounding never puts two
        // values in the other order, so the float of the lowest is the lowest
        // of the floats.
        static_assert(std::is_same<Sample, double>::value, "the 64-bit feed is double");
        const __m128d zero = _mm_setzero_pd();
        __m128d lo = _mm_add_pd(zero, _mm_loadu_pd(x)), hi = lo;
        __m128d lo1 = _mm_add_pd(zero, _mm_loadu_pd(x + 2)), hi1 = lo1;
        for (; k + 4 <= count; k += 4) {
          const __m128d a = _mm_add_pd(zero, _mm_loadu_pd(x + k));
          const __m128d b = _mm_add_pd(zero, _mm_loadu_pd(x + k + 2));
          lo = _mm_min_pd(lo, a);
          hi = _mm_max_pd(hi, a);
          lo1 = _mm_min_pd(lo1, b);
          hi1 = _mm_max_pd(hi1, b);
        }
        double los[2], his[2];
        _mm_storeu_pd(los, _mm_min_pd(lo, lo1));
        _mm_storeu_pd(his, _mm_max_pd(hi, hi1));
        low = (float) (los[1] < los[0] ? los[1] : los[0]);
        high = (float) (his[1] > his[0] ? his[1] : his[0]);
#else
        float lo[4], hi[4];
        for (int j = 0; j < 4; ++j) lo[j] = hi[j] = (float) ((Sample) 0 + x[j]);
        for (; k + 4 <= count; k += 4)
          for (uint32_t j = 0; j < 4; ++j) {
            const float v = (float) ((Sample) 0 + x[k + j]);
            if (v < lo[j]) lo[j] = v;
            if (v > hi[j]) hi[j] = v;
          }
        low = lo[0];
        high = hi[0];
        for (int j = 1; j < 4; ++j) {
          if (lo[j] < low) low = lo[j];
          if (hi[j] > high) high = hi[j];
        }
#endif
        for (; k < count; ++k) {
          const float v = (float) ((Sample) 0 + x[k]);
          if (v < low) low = v;
          if (v > high) high = v;
        }
        return;
      }
    }
    for (uint32_t k = 0; k < count; ++k) {
      float value = 0.0f;
      if (channels && numChannels > 0) {
        Sample sum = (Sample) 0;
        for (uint32_t c = 0; c < numChannels; ++c)
          if (channels[c]) sum += channels[c][from + k];
        value = (float) (sum / (Sample) numChannels);
      }
      if (k == 0) {
        low = high = value;
      } else {
        if (value < low) low = value;
        if (value > high) high = value;
      }
    }
  }

public:
  /** [main thread] The column `i` positions back from the newest. */
  void columnAt(int i, float* low, float* high) const {
    const int n = columns();
    // The write index points at the slot about to be written, so the newest
    // FINISHED column is the one before it.
    const int newest = writeIndex_.load(std::memory_order_acquire);
    int at = (newest - 1 - i) % n;
    if (at < 0) at += n;
    *low = low_[at].load(std::memory_order_relaxed);
    *high = high_[at].load(std::memory_order_relaxed);
  }

private:
  std::atomic<int> columns_{256};
  std::atomic<int> samplesPerColumn_{128};
  std::atomic<int> writeIndex_{0};
  std::atomic<bool> resetRequested_{false};
  std::atomic<float> low_[kMaxColumns] = {};
  std::atomic<float> high_[kMaxColumns] = {};

  // Written only by the audio thread, between completed columns. Not atomic
  // because nothing else reads or writes them -- a clear() asks, through
  // resetRequested_, rather than writing them itself.
  float accLow_ = 0.0f, accHigh_ = 0.0f;
  int accCount_ = 0;
};

} // namespace sonore
