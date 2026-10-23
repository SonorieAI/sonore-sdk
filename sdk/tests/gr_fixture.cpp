// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the smallest DSP that reports gain reduction, for the wasm
// build test. A 4:1 compressor with a -20 dB threshold and a hard knee, so a
// tone at -8 dBFS sits 12 dB over and the reading has a known answer: 9 dB.
// No example compresses, and an export that only ever returns 0 proves it
// links, not that it reads.
#define SONORE_NUM_PARAMS 1

#include <sonore/dsp.h>

struct SonoreDsp {
  sonore::Compressor comp;

  void prepare(const sonore::ProcessSpec& spec) {
    comp.setSampleRate((float) spec.sampleRate);
    comp.setThreshold(-20.0f);
    comp.setRatio(4.0f);
    comp.setKnee(0.0f);
    comp.setAttack(1.0f);
    comp.setRelease(50.0f);
  }

  /** What the meter reads, forwarded: the one line a plugin adds. */
  float gainReduction() const { return comp.gainReduction(); }

  void process(sonore::AudioBlock<float>& io, const float* p) {
    (void) p;
    const size_t n = io.getNumSamples();
    float* L = io.getChannelPointer(0);
    float* R = io.getNumChannels() > 1 ? io.getChannelPointer(1) : nullptr;
    for (size_t i = 0; i < n; ++i) {
      const float key = R && std::fabs(R[i]) > std::fabs(L[i]) ? R[i] : L[i];
      const float g = comp.computeGain(key);
      L[i] *= g;
      if (R) R[i] *= g;
    }
  }
};
