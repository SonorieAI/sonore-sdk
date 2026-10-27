// SPDX-License-Identifier: Apache-2.0
//
// The spectrum a plugin's interface draws, measured where the audio is.
//
// ── One rule, wherever it is drawn ──────────────────────────────────────────
//
// A Sonorie plugin's page mounts an analyzer with sonore.spectrum(), and the
// studio's browser preview feeds it from a Web Audio AnalyserNode. A shipped
// plugin feeds it from here. Both follow ONE rule, so a display written once
// looks the same in the studio and in a DAW:
//
//   * mono, the average of the channels (an AnalyserNode's own down-mix);
//   * the newest windowFor(rate) samples -- 8192 at 44.1 / 48 kHz, doubled per
//     doubling of the rate, so every rate sees ~170 ms and the same hertz per
//     bin -- under a Blackman window, the one an AnalyserNode uses;
//   * |X[k]| / N in dB plus kCalibrationDb, so a full-scale sine centred on a
//     bin reads 0 dBFS (a sine's energy is split between +f and -f, and 0.42
//     is the Blackman window's coherent gain);
//   * resampled onto a FIXED grid of kPoints points, log-spaced kLowHz ..
//     kHighHz. A point whose band (half a grid step each side) holds bin
//     centres reads the LOUDEST of them, so a narrow peak is never averaged
//     away between pixels; a point below the bin spacing (the bass) is
//     interpolated in dB between its two neighbouring bins; above Nyquist, or
//     quieter than kFloorDb, it reads kFloorDb.
//
// The grid is the contract, not a picture size. The browser side is
// spectrum.ts in the Sonorie app; change one and change the other.
//
// ── Two threads ─────────────────────────────────────────────────────────────
//
// push() is the audio thread's: it writes a lock-free ring with relaxed
// atomics, allocates nothing and does nothing at all until something asked
// (setActive). analyse() is the interface's: it copies the newest window, runs
// the transform and fills the grid. A torn read shows one slightly wrong frame
// for one tick, which nobody can see; a lock on the audio thread is a dropout.
// The same arrangement as AudioScopeBuffer, and for the same reason the feed
// is owned by whatever owns the audio, not by the window that shows it.
//
// ── Nothing until asked ─────────────────────────────────────────────────────
//
// Every instance of every plugin carries two of these (its output and its
// input), and most never draw a spectrum. So the ring -- 128 KB, the longest
// window -- is made on the main thread the first time setActive(true) is
// called, published to the audio thread through an atomic pointer, and freed
// only with the feed. An unused feed is a few dozen bytes; the wrapper's
// Instance has a size budget, and two inline rings would have spent it five
// times over.
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "fft.h"

namespace sonore {

/**
 * A plugin's spectrum, for a display: fed by the audio thread, read on the
 * interface's clock as kPoints calibrated dBFS values on a fixed log grid.
 */
class SpectrumFeed {
public:
  /** Points on the grid. */
  static constexpr int kPoints = 256;
  /** The grid's two ends, in hertz. */
  static constexpr double kLowHz = 20.0, kHighHz = 20000.0;
  /** What "nothing there" reads, in dBFS. */
  static constexpr float kFloorDb = -140.0f;
  /** The longest window, and so the ring's size: 32768 samples, ~170 ms at
   *  192 kHz. A power of two, so the ring index is a mask. */
  static constexpr uint32_t kCapacity = 32768;

  /** 20 log10(2 / 0.42): a full-scale sine reads 0 dBFS through the window. */
  static double calibrationDb() { return 20.0 * std::log10(2.0 / 0.42); }

  /** The window length for `sampleRate`: about 170 ms at every rate. */
  static uint32_t windowFor(double sampleRate) {
    return sampleRate <= 50000.0 ? 8192u : sampleRate <= 100000.0 ? 16384u : 32768u;
  }

  /** Grid point `i`'s frequency, in hertz. */
  static double pointHz(int i) { return kLowHz * std::pow(kHighHz / kLowHz, (double) i / (kPoints - 1)); }

  SpectrumFeed() = default;
  SpectrumFeed(const SpectrumFeed&) = delete;
  SpectrumFeed& operator=(const SpectrumFeed&) = delete;

  /** [main thread] Start or stop feeding. Until something turns it on, push()
   *  returns at once and the ring does not exist: a plugin whose page shows no
   *  analyzer pays nothing. The first start makes the ring (an allocation, on
   *  this thread) and publishes it before the audio thread is told to fill it. */
  void setActive(bool on) {
    if (on && !storage_) {
      storage_.reset(new std::atomic<float>[kCapacity]);
      for (uint32_t i = 0; i < kCapacity; ++i) storage_[i].store(0.0f, std::memory_order_relaxed);
      ring_.store(storage_.get(), std::memory_order_release);
    }
    active_.store(on, std::memory_order_release);
  }
  /** [any thread] Whether push() is feeding the ring. */
  bool active() const { return active_.load(std::memory_order_relaxed); }

  /** [audio thread] One block: the average of `numChannels` channels (null
   *  channel pointers are skipped), into the ring. No allocation, no lock. */
  void push(const float* const* channels, uint32_t numChannels, uint32_t numFrames) {
    pushImpl(channels, numChannels, numFrames);
  }
  /** [audio thread] The same for a host running 64-bit audio. */
  void push(const double* const* channels, uint32_t numChannels, uint32_t numFrames) {
    pushImpl(channels, numChannels, numFrames);
  }

  /**
   * [main thread] The newest window at `sampleRate`, measured onto the grid:
   * `out` receives kPoints dBFS values. The first call at a new window length
   * builds that length's transform and window (an allocation, here and never
   * on the audio thread).
   */
  void analyse(double sampleRate, float* out) {
    const std::atomic<float>* ring = ring_.load(std::memory_order_acquire);
    if (!ring) {
      for (int i = 0; i < kPoints; ++i) out[i] = kFloorDb;
      return;
    }
    const uint32_t n = windowFor(sampleRate);
    if (n != size_) prepare(n);
    const uint32_t w = write_.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i) {
      re_[i] = ring[(w - n + i) & (kCapacity - 1)].load(std::memory_order_relaxed) * window_[i];
      im_[i] = 0.0f;
    }
    if (n == 8192) fft8_->transform(re_.data(), im_.data());
    else if (n == 16384) fft16_->transform(re_.data(), im_.data());
    else fft32_->transform(re_.data(), im_.data());
    // The browser's analyser reports N/2 bins, DC up to one short of Nyquist:
    // the same count here, so the two grids are filled from the same bins.
    const uint32_t bins = n / 2;
    for (uint32_t k = 0; k < bins; ++k) {
      const double mag = std::sqrt((double) re_[k] * re_[k] + (double) im_[k] * im_[k]) / n;
      bins_[k] = mag > 0.0 ? 20.0 * std::log10(mag) : -1e9;
    }
    grid(bins_.data(), bins, sampleRate, n, out);
  }

  /**
   * The grid rule on its own: `bins` (dB of |X[k]| / N, `count` of them from
   * DC) of an `n`-point transform at `sampleRate`, into kPoints calibrated
   * dBFS values. Public so a test can hold it to the rule without audio.
   */
  static void grid(const double* bins, uint32_t count, double sampleRate, uint32_t n, float* out) {
    const double cal = calibrationDb();
    const double ratio = std::pow(kHighHz / kLowHz, 1.0 / (kPoints - 1));
    const double half = std::sqrt(ratio), binHz = sampleRate / n, nyquist = sampleRate / 2.0;
    const int last = (int) count - 1;
    const auto at = [&](int k) {
      const double v = bins[k] + cal;
      return v > kFloorDb ? v : (double) kFloorDb;
    };
    for (int i = 0; i < kPoints; ++i) {
      const double f = kLowHz * std::pow(ratio, i);
      if (f >= nyquist || last < 1) {
        out[i] = kFloorDb;
        continue;
      }
      const int k0 = (int) std::ceil(f / half / binHz);
      int k1 = (int) std::ceil(f * half / binHz) - 1;
      if (k1 > last) k1 = last;
      double v;
      if (k0 <= k1) {
        v = kFloorDb;
        for (int k = k0; k <= k1; ++k) {
          const double b = at(k);
          if (b > v) v = b;
        }
      } else {
        const double x = f / binHz;
        const int ka = (int) std::floor(x), kb = ka + 1 > last ? last : ka + 1;
        v = at(ka) + (at(kb) - at(ka)) * (x - ka);
      }
      out[i] = v > kFloorDb ? (float) v : kFloorDb;
    }
  }

  /**
   * `count` values as JavaScript numbers in tenths of a decibel, comma
   * separated, appended to `out`: "-23.4,-140.0". Written digit by digit
   * because printf's %f follows setlocale(), hosts really call it, and a
   * German locale's "-23,4" inside generated JavaScript is a syntax error that
   * takes the whole page's bridge down (see jsNumber in gui.h).
   */
  static void appendTenths(std::string& out, const float* values, int count) {
    for (int i = 0; i < count; ++i) {
      if (i) out += ',';
      const double v = values[i] == values[i] ? (double) values[i] : (double) kFloorDb;
      long tenths = std::lround(v * 10.0);
      if (tenths < 0) {
        out += '-';
        tenths = -tenths;
      }
      out += std::to_string(tenths / 10);
      out += '.';
      out += (char) ('0' + (int) (tenths % 10));
    }
  }

private:
  template <typename Sample>
  void pushImpl(const Sample* const* channels, uint32_t numChannels, uint32_t numFrames) {
    if (!active_.load(std::memory_order_acquire) || !channels || numChannels == 0) return;
    std::atomic<float>* ring = ring_.load(std::memory_order_acquire);
    if (!ring) return;
    uint32_t used = 0;
    for (uint32_t c = 0; c < numChannels; ++c) used += channels[c] ? 1u : 0u;
    if (!used) return;
    const float scale = 1.0f / (float) used;
    uint32_t w = write_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < numFrames; ++i, ++w) {
      float sum = 0.0f;
      for (uint32_t c = 0; c < numChannels; ++c)
        if (channels[c]) sum += (float) channels[c][i];
      ring[w & (kCapacity - 1)].store(sum * scale, std::memory_order_relaxed);
    }
    write_.store(w, std::memory_order_release);
  }

  void prepare(uint32_t n) {
    size_ = n;
    window_.assign(n, 0.0f);
    // Blackman, periodic -- the window an AnalyserNode applies, so the leakage
    // round a tone looks the same in the studio and in the plugin.
    const double tau = 6.283185307179586;
    for (uint32_t i = 0; i < n; ++i)
      window_[i] = (float) (0.42 - 0.5 * std::cos(tau * i / n) + 0.08 * std::cos(2.0 * tau * i / n));
    re_.assign(n, 0.0f);
    im_.assign(n, 0.0f);
    bins_.assign(n / 2, 0.0);
    if (n == 8192 && !fft8_) fft8_.reset(new Fft<8192>());
    if (n == 16384 && !fft16_) fft16_.reset(new Fft<16384>());
    if (n == 32768 && !fft32_) fft32_.reset(new Fft<32768>());
  }

  std::atomic<bool> active_{false};
  std::atomic<uint32_t> write_{0};
  /** The ring, made by the first setActive(true) and kept until the feed
   *  goes: the audio thread reads it through ring_, never storage_. */
  std::unique_ptr<std::atomic<float>[]> storage_;
  std::atomic<std::atomic<float>*> ring_{nullptr};

  // The interface thread's own, built on the first analyse() at a length.
  uint32_t size_ = 0;
  std::vector<float> window_, re_, im_;
  std::vector<double> bins_;
  std::unique_ptr<Fft<8192>> fft8_;
  std::unique_ptr<Fft<16384>> fft16_;
  std::unique_ptr<Fft<32768>> fft32_;
};

} // namespace sonore
