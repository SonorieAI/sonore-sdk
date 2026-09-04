// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: early reflections from a room, by the image-source method.
//
// A reverb tail says how big a space is; the first few echoes say where
// the walls are, and a "room" reverb without them is a hall with the lights
// off. The FDN and the spring give the tail; this gives the echoes, from a
// room rather than from a table of taps somebody typed.
//
// The method is Allen & Berkley's ("Image method for efficiently simulating
// small-room acoustics", JASA 1979): a rectangular room's walls are mirrors,
// so every reflection path is a straight line from an IMAGE of the source
// -- the source reflected across a wall, or across a wall and then another
// -- and the reflection arrives after the image's distance over the speed of
// sound, attenuated by 1/distance and by the walls' reflection coefficient
// once per bounce. Along each axis the images sit at i·L + s for even i and
// i·L + (L − s) for odd i, with |i| bounces; the three axes combine, and the
// order is the largest |i| + |j| + |k| kept. Two listening points an ear's
// width apart give the stereo pair, and the interaural delay of an
// off-axis source falls out of the geometry rather than being panned in.
//
// Taps are read from one delay line by linear interpolation (they are
// static) and the walls' high-frequency loss is one lowpass on the output,
// which is an approximation of per-bounce filtering and is said to be.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"

namespace sonore {

/**
 * SIZE: MaxDelay floats plus the taps -- 64 KB at the default, enough for a
 * 100 ms path at 96 kHz (order 2 in an 8 m room). A member, never a local.
 */
template <int MaxDelay = 16384, int MaxTaps = 256>
class EarlyReflections {
public:
  struct Room {
    float length = 8.0f, width = 6.0f, height = 3.0f;  // metres: x, y, z
    float sourceX = 2.0f, sourceY = 3.0f, sourceZ = 1.5f;
    float listenerX = 6.0f, listenerY = 3.0f, listenerZ = 1.5f;
    float earSpacing = 0.17f;       // the two listening points, along y
    float absorption = 0.3f;        // 0..1: energy lost per bounce
    float damping = 0.3f;           // 0..1: high-frequency loss on the output
    int order = 2;                  // reflections up to this many bounces
    bool includeDirect = false;     // the direct path is usually the dry signal
  };

  void prepare(const ProcessSpec& spec) {
    sr_ = (float) spec.sampleRate;
    for (int c = 0; c < 2; ++c) damp_[c].setSampleRate(sr_);
    setRoom(room_);
    reset();
  }
  void reset() {
    line_.reset();
    for (int c = 0; c < 2; ++c) damp_[c].reset();
  }

  void setRoom(const Room& r) {
    room_ = r;
    const float reflect = std::sqrt(1.0f - clampf(r.absorption, 0.0f, 0.99f));
    const int order = r.order < 0 ? 0 : (r.order > 4 ? 4 : r.order);
    const float ear[2] = {r.listenerY - 0.5f * r.earSpacing, r.listenerY + 0.5f * r.earSpacing};
    const float c = 343.0f;
    // The direct distance to the listener's centre normalises the gains.
    const float d0 = std::sqrt((r.sourceX - r.listenerX) * (r.sourceX - r.listenerX) +
                               (r.sourceY - r.listenerY) * (r.sourceY - r.listenerY) +
                               (r.sourceZ - r.listenerZ) * (r.sourceZ - r.listenerZ));
    for (int ch = 0; ch < 2; ++ch) {
      count_[ch] = 0;
      for (int i = -order; i <= order; ++i)
        for (int j = -order; j <= order; ++j)
          for (int k = -order; k <= order; ++k) {
            const int bounces = std::abs(i) + std::abs(j) + std::abs(k);
            if (bounces > order) continue;
            if (bounces == 0 && !r.includeDirect) continue;
            if (count_[ch] >= MaxTaps) continue;
            const float x = image(i, r.length, r.sourceX), y = image(j, r.width, r.sourceY), z = image(k, r.height, r.sourceZ);
            const float dx = x - r.listenerX, dy = y - ear[ch], dz = z - r.listenerZ;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float delay = d / c * sr_;
            if (delay < 1.0f || delay > (float) (MaxDelay - 2)) continue;
            delay_[ch][count_[ch]] = delay;
            gain_[ch][count_[ch]] = (d0 / (d > 0.1f ? d : 0.1f)) * std::pow(reflect, (float) bounces);
            ++count_[ch];
          }
    }
    const float d = clampf(r.damping, 0.0f, 1.0f);
    for (int ch = 0; ch < 2; ++ch) damp_[ch].setCutoff(20000.0f * std::pow(2000.0f / 20000.0f, d));
  }

  int tapCount(int channel = 0) const { return count_[channel & 1]; }
  /** The earliest tap's delay in samples, for the record. */
  float firstTapSamples(int channel = 0) const {
    const int ch = channel & 1;
    float first = 1e9f;
    for (int t = 0; t < count_[ch]; ++t) first = delay_[ch][t] < first ? delay_[ch][t] : first;
    return count_[ch] ? first : 0.0f;
  }

  /** Mono in, the two ears' reflections out (without the dry). */
  inline void process(float in, float& left, float& right) {
    float out[2] = {0.0f, 0.0f};
    for (int ch = 0; ch < 2; ++ch) {
      float acc = 0.0f;
      for (int t = 0; t < count_[ch]; ++t) acc += gain_[ch][t] * line_.read(delay_[ch][t]);
      out[ch] = damp_[ch].lp(acc);
    }
    line_.write(in);
    left = out[0];
    right = out[1];
  }

private:
  static inline float image(int i, float wall, float source) {
    return (float) i * wall + ((i & 1) ? (wall - source) : source);
  }

  DelayLine<MaxDelay> line_;
  OnePole damp_[2];
  float delay_[2][MaxTaps]{}, gain_[2][MaxTaps]{};
  int count_[2] = {0, 0};
  Room room_{};
  float sr_ = 48000.0f;
};

} // namespace sonore
