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
  static constexpr int kMaxColumns = 1024;

  /** How many columns of history to keep. Clamped, because the storage is fixed:
   *  a real-time producer cannot be handed a container that might reallocate
   *  underneath it. */
  void setColumns(int columns) {
    const int n = columns < 8 ? 8 : (columns > kMaxColumns ? kMaxColumns : columns);
    columns_.store(n, std::memory_order_relaxed);
    clear();
  }

  int columns() const { return columns_.load(std::memory_order_relaxed); }

  /** How many samples make one column. Larger is a slower sweep. */
  void setSamplesPerColumn(int samples) {
    samplesPerColumn_.store(samples < 1 ? 1 : samples, std::memory_order_relaxed);
  }

  int samplesPerColumn() const { return samplesPerColumn_.load(std::memory_order_relaxed); }

  void clear() {
    for (int i = 0; i < kMaxColumns; ++i) {
      low_[i].store(0.0f, std::memory_order_relaxed);
      high_[i].store(0.0f, std::memory_order_relaxed);
    }
    writeIndex_.store(0, std::memory_order_relaxed);
    accLow_ = 0.0f;
    accHigh_ = 0.0f;
    accCount_ = 0;
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
    const int perColumn = samplesPerColumn();
    const int n = columns();
    for (uint32_t f = 0; f < numFrames; ++f) {
      float value = 0.0f;
      if (channels && numChannels > 0) {
        // Summed to mono. A scope showing each channel separately needs as many
        // rows, and a plugin editor has room for one.
        Sample sum = (Sample) 0;
        for (uint32_t c = 0; c < numChannels; ++c)
          if (channels[c]) sum += channels[c][f];
        value = (float) (sum / (Sample) numChannels);
      }
      if (accCount_ == 0) {
        accLow_ = value;
        accHigh_ = value;
      } else {
        if (value < accLow_) accLow_ = value;
        if (value > accHigh_) accHigh_ = value;
      }
      if (++accCount_ >= perColumn) {
        const int at = writeIndex_.load(std::memory_order_relaxed);
        low_[at].store(accLow_, std::memory_order_relaxed);
        high_[at].store(accHigh_, std::memory_order_relaxed);
        writeIndex_.store((at + 1) % n, std::memory_order_release);
        accCount_ = 0;
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
  std::atomic<float> low_[kMaxColumns] = {};
  std::atomic<float> high_[kMaxColumns] = {};

  // Written only by the audio thread, between completed columns. Not atomic
  // because nothing else reads them.
  float accLow_ = 0.0f, accHigh_ = 0.0f;
  int accCount_ = 0;
};

} // namespace sonore
