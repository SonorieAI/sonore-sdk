// SPDX-License-Identifier: Apache-2.0
// Sonore SDK example: a 3-way band splitter, the plainest possible reason to
// want EXTRA OUTPUT BUSES: one input, three destinations the host routes to
// three tracks. Low stays on the main output; Mid and High go out on aux buses.
//
// It is also the example that shows the ProcessContext signature: a DSP with
// aux buses takes process(ctx, params) rather than one of the simple forms,
// and reaches the extra buses through ctx.auxOut(i).
#define SONORE_NUM_PARAMS 3

#include <sonore/dsp.h>
#include <sonore/effects.h>

struct SonoreDsp {
  // Two crossovers in series: the low split feeds the high split, which is
  // what keeps the three bands summing back to the input.
  sonore::LinkwitzRiley lowSplit[2], highSplit[2];
  sonore::Smooth outSm;
  float lastLow = -1.0f, lastHigh = -1.0f;
  float sampleRate = 48000.0f;
  bool upperWasNeeded = true;

  /** A crossover rings after its input stops, and this plugin used to tell the
   *  host it did not: the host test measured 399 samples still coming out.
   *  The number quoted is for the LOWEST corner the control allows (40 Hz),
   *  not the one currently set: a tail that stops being true when the user
   *  turns a knob is not a declaration a host can act on. */
  int tailSamples() const {
    return sonore::LinkwitzRiley::tailSamples(sampleRate, 40.0f);
  }

  void prepare(const sonore::ProcessSpec& spec) {
    const float sr = (float) spec.sampleRate;
    sampleRate = sr;
    for (int c = 0; c < 2; ++c) {
      lowSplit[c].setSampleRate(sr);
      highSplit[c].setSampleRate(sr);
      lowSplit[c].reset();
      highSplit[c].reset();
    }
    outSm.setup(sr, 12.0f);
    outSm.snap(1.0f);
    lastLow = lastHigh = -1.0f;
  }

  void process(sonore::ProcessContext& ctx, const float* p) {
    const size_t n = ctx.main.getNumSamples();
    const size_t nch = ctx.main.getNumChannels();

    // Coefficients only when a control really moved.
    if (p[0] != lastLow || p[1] != lastHigh) {
      const float lowHz = p[0];
      // The upper crossover can never sit below the lower one, or the mid band
      // inverts and the three bands stop summing to the input.
      const float highHz = p[1] > lowHz * 1.05f ? p[1] : lowHz * 1.05f;
      for (int c = 0; c < 2; ++c) {
        lowSplit[c].setCrossover(lowHz);
        highSplit[c].setCrossover(highHz);
      }
      lastLow = p[0];
      lastHigh = p[1];
    }

    sonore::AudioBlock<float>& mid = ctx.auxOut(0);
    sonore::AudioBlock<float>& high = ctx.auxOut(1);
    // Aux contents are undefined on entry: a bus we do not fill must be
    // cleared, or the host plays whatever was left in its buffer.
    mid.clear();
    high.clear();

    // If the host is routing neither Mid nor High anywhere, the second
    // crossover is work nobody will ever hear. This is what
    // audio-ports-activation is FOR: the buffers still arrive, zero-filled,
    // so skipping is purely an optimisation and never a correctness question.
    const bool needUpper = ctx.isAuxActive(0) || ctx.isAuxActive(1);

    // Coming back after a skip, the upper crossover's state is STALE: it never
    // saw the samples that went by while nobody was listening, so resuming
    // from it would ring against history that no longer relates to the signal.
    // Starting clean costs one quiet block and is the only honest option.
    if (needUpper && !upperWasNeeded)
      for (int c = 0; c < 2; ++c) highSplit[c].reset();
    upperWasNeeded = needUpper;

    const size_t chans = nch < 2 ? nch : 2;
    float* io[2] = {nullptr, nullptr};
    float* midCh[2] = {nullptr, nullptr};
    float* highCh[2] = {nullptr, nullptr};
    for (size_t c = 0; c < chans; ++c) {
      io[c] = ctx.main.getChannelPointer(c);
      midCh[c] = c < mid.getNumChannels() ? mid.getChannelPointer(c) : nullptr;
      highCh[c] = c < high.getNumChannels() ? high.getChannelPointer(c) : nullptr;
    }
    // Once per block: the smoother does the ramp, so the dB-to-gain power
    // need not run per sample.
    const float target = sonore::dbToGain(p[2]);
    // Frames outside, channels inside, so the smoother takes ONE step per
    // frame and both channels share it. Stepped inside a per-channel loop it
    // advanced twice per frame -- left took the first half of the ramp, right
    // the second, and every block skipped as many steps as it had frames: a
    // zipper whenever the output moved, and a stereo image wobbling with it.
    for (size_t i = 0; i < n; ++i) {
      const float g = outSm.next(target);
      for (size_t c = 0; c < chans; ++c) {
        float low = 0.0f, rest = 0.0f, midBand = 0.0f, highBand = 0.0f;
        lowSplit[c].process(io[c][i], low, rest);
        if (needUpper) highSplit[c].process(rest, midBand, highBand);
        io[c][i] = low * g;
        if (midCh[c]) midCh[c][i] = midBand * g;
        if (highCh[c]) highCh[c][i] = highBand * g;
      }
    }
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"low", "Low/Mid", "Hz", 40.0f, 2000.0f, 200.0f, 0},
    {"high", "Mid/High", "Hz", 500.0f, 16000.0f, 3000.0f, 0},
    {"output", "Output", "dB", -24.0f, 12.0f, 0.0f, 0},
};

// The extra output buses, in the order the DSP indexes them.
static const sonore::AuxBusInfo kAuxOutputs[] = {
    {"Mid", 2},
    {"High", 2},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.example.splitter",
    "Sonore Splitter",
    "Sonorie",
    "1.0.0",
    "Three-way band splitter: low on the main output, mid and high on aux buses.",
    "https://sonorie.com",
    false, // effect
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr, // no factory presets
    0,
    "utility", // -> lv2:UtilityPlugin and friends per format
    nullptr,   // licence falls back to the vendor URL
    nullptr,   // no maintainer email
    2,         // stereo only: the bands are filtered per channel
    2,
    kAuxOutputs,
    (int) (sizeof(kAuxOutputs) / sizeof(kAuxOutputs[0])),
};

#include <sonore/formats.h> // CLAP, and every format this target is built as
