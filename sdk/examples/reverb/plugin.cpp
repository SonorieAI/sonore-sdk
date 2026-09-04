// SPDX-License-Identifier: Apache-2.0
// Sonore SDK example: a convolution reverb, and the integration proof for
// the other half of the toolkit.
//
// The sampler example proved the sample path. This one proves the convolution
// path, and it is the first plugin here that has to be honest about BOTH of a
// host's timing questions at once:
//
//   LATENCY: a partitioned convolver answers one block late. A host that is
//   not told compensates by the wrong amount, and the reverb sits behind the
//   dry signal in every parallel mix it is used in.
//
//   TAIL: a reverb keeps sounding after its input stops. A host that is not
//   told cuts the decay dead when the transport stops or when it renders, and
//   the user hears a reverb that works live and is truncated on export.
//
// Both are declared here, and both are checked by the host tests against the
// numbers this plugin actually produces.
#define SONORE_NUM_PARAMS 4

#include <string>
#include <vector>

#include <sonore/dsp.h>
#include <sonore/fft.h>
#include <sonore/impulse.h>
#include <sonore/state_bag.h>
#include <sonore/waveform.h>

// 512-sample partitions, 48 of them: half a second of impulse at 48 kHz.
// Longer would mean more memory per instance for a tail most sources never
// reach; the partition size is the latency, so it is the other half of the
// same trade.
using RoomConvolver = sonore::Convolver<512, 48>;

struct SonoreDsp {
  RoomConvolver left, right;
  sonore::ImpulseResponse impulse;
  sonore::WaveformPeaks peaks;
  // Compensated, because the convolver answers a block late: see the comment
  // on CompensatedDryWetMixer for what mixing a late wet against an undelayed
  // dry actually sounds like. The pre-delay is deliberately NOT compensated:
  // it is the creative offset, not the technical one.
  sonore::CompensatedDryWetMixer<2, 1024> mixer;
  sonore::Smooth outSm;
  // 48000 samples covers the 200 ms control even at 192 kHz, and the line is
  // a fixed-size member: sizing it for a rate no one runs would cost every
  // instance the memory whether or not the control is ever moved.
  sonore::DelayLine<48000> preDelayL, preDelayR;
  std::string impulsePath; // empty means the built-in room

  double hostRate = 48000.0;

  /** Built at CONSTRUCTION, not at prepare(): a host may save state before it
   *  ever prepares, and state that depends on when it was asked for is state a
   *  host cannot trust. The sampler example learned this from clap-validator. */
  SonoreDsp() {
    buildDemoImpulse(48000.0);
    loadIntoConvolvers();
  }

  void prepare(const sonore::ProcessSpec& spec) {
    // An impulse recorded at one rate and played at another is TRANSPOSED, so
    // the response is rebuilt whenever the session rate changes.
    if (spec.sampleRate != hostRate || impulse.length() == 0) {
      hostRate = spec.sampleRate;
      if (impulsePath.empty()) buildDemoImpulse(hostRate);
      else if (!impulse.loadFromFile(impulsePath.c_str(), hostRate)) buildDemoImpulse(hostRate);
      loadIntoConvolvers();
    }
    left.reset();
    right.reset();
    preDelayL.reset();
    preDelayR.reset();
    outSm.setup((float) spec.sampleRate, 12.0f);
    outSm.snap(1.0f);
    mixer.setWetLatency((int) RoomConvolver::latency());
  }

  /** A synthetic room: an early reflection pattern over an exponentially
   *  decaying noise tail. Not a recording of anywhere, but a real impulse
   *  response: it has a direct hit, discrete early reflections and a diffuse
   *  tail, which is what makes the latency and tail numbers meaningful. */
  void buildDemoImpulse(double rate) {
    const size_t length = (size_t) (rate * 0.45);
    std::vector<float> ir(length * 2, 0.0f); // stereo, interleaved
    uint32_t seed = 0x9E3779B9;
    auto noise = [&seed]() {
      seed = seed * 1664525u + 1013904223u;
      return (float) ((int32_t) (seed >> 8) % 20001 - 10000) / 10000.0f;
    };

    for (size_t i = 0; i < length; ++i) {
      const double t = (double) i / rate;
      const float decay = (float) std::exp(-6.5 * t);
      // The tail is decorrelated between channels: identical noise on both
      // sides is a reverb that collapses to the centre.
      ir[i * 2] = noise() * decay * 0.35f;
      ir[i * 2 + 1] = noise() * decay * 0.35f;
    }
    ir[0] = ir[1] = 1.0f; // the direct hit
    // Early reflections, at times that are not multiples of each other, so
    // they do not stack into a ringing comb.
    const double taps[] = {0.011, 0.017, 0.023, 0.031, 0.043, 0.057};
    float level = 0.6f;
    for (double tap : taps) {
      const size_t at = (size_t) (tap * rate);
      if (at < length) {
        ir[at * 2] += level;
        ir[at * 2 + 1] += level * 0.8f;
      }
      level *= 0.72f;
    }

    impulse.loadFromSamples(ir.data(), length, 2, rate, rate, sonore::ImpulseNormalise::Energy);
    impulsePath.clear();
  }

  void loadIntoConvolvers() {
    if (impulse.length() == 0) return;
    left.loadImpulse(impulse.channel(0), impulse.length());
    right.loadImpulse(impulse.channel(impulse.numChannels() > 1 ? 1 : 0), impulse.length());
    peaks.build(impulse.channel(0), impulse.length(), 1, 256);
  }

  /** What the host must compensate for. One block, because that is what a
   *  partitioned convolver costs, whatever the impulse is. */
  int latencySamples() const { return (int) RoomConvolver::latency(); }

  /** How long the reverb keeps sounding after its input stops.
   *
   *  This started as `impulse.length() + latency()`, which reads right and is
   *  wrong twice. The host test measured 22682 samples against that
   *  declaration of 22112, and both halves of the 570-sample gap matter:
   *
   *  TWO blocks of latency, not one. A partitioned convolver buffers a whole
   *  block of input before it transforms anything, AND answers a block late.
   *  The last input sample's contribution therefore finishes two blocks after
   *  the impulse's own length, not one.
   *
   *  Plus the PRE-DELAY, which sits ahead of the convolution and so pushes the
   *  whole response later. It is a parameter, so the honest number is its
   *  MAXIMUM rather than its current value: reporting what it happens to be
   *  right now understates the tail the instant the user turns the control up,
   *  and a host believes the declaration. Overstating costs a fraction of a
   *  second of extra render; understating is a reverb that is clipped on
   *  export and fine while monitoring, which is the worst way for a user to
   *  find a bug. */
  int tailSamples() const {
    const size_t maxPreDelay = (size_t) (0.200 * hostRate); // the control's top end
    return (int) (maxPreDelay + impulse.length() + 2 * RoomConvolver::latency());
  }

  void saveState(sonore::StateBag& bag) const { bag.setString("impulsePath", impulsePath); }

  void loadState(const sonore::StateBag& bag) {
    const std::string path = bag.getString("impulsePath", "");
    if (path == impulsePath) return; // nothing changed; do not rebuild
    if (!path.empty() && impulse.loadFromFile(path.c_str(), hostRate)) {
      impulsePath = path;
      loadIntoConvolvers();
      return;
    }
    // The file moved, or the session came from another machine. The built-in
    // room means the plugin still reverberates, which beats a bypass the user
    // cannot diagnose.
    buildDemoImpulse(hostRate);
    loadIntoConvolvers();
  }

  void process(sonore::AudioBlock<float>& io, const float* p) {
    const size_t n = io.getNumSamples();
    float* L = io.getChannelPointer(0);
    float* R = io.getNumChannels() > 1 ? io.getChannelPointer(1) : L;

    const float preDelaySamples =
        1.0f + (float) (p[0] * 0.001 * hostRate); // milliseconds to samples
    mixer.setMix(p[1] * 0.01f);

    for (size_t i = 0; i < n; ++i) {
      const float dryL = L[i], dryR = R[i];

      // Pre-delay ahead of the convolution: delaying the WET path is what
      // separates a room from a slap, and delaying the whole thing instead
      // would just push the dry signal late too.
      preDelayL.write(dryL);
      preDelayR.write(dryR);
      const float sendL = preDelayL.readCubic(preDelaySamples);
      const float sendR = preDelayR.readCubic(preDelaySamples);

      const float wetL = left.process(sendL);
      const float wetR = right.process(sendR);

      const float gain = outSm.next(sonore::dbToGain(p[3]));
      L[i] = mixer.process(0, dryL, wetL * p[2]) * gain;
      R[i] = mixer.process(1, dryR, wetR * p[2]) * gain;
    }
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"predelay", "Pre-delay", "ms", 0.0f, 200.0f, 12.0f, 0, "Space"},
    {"mix", "Mix", "%", 0.0f, 100.0f, 30.0f, 0, "Space"},
    {"send", "Send", "", 0.0f, 4.0f, 1.0f, 0, "Space"},
    {"output", "Output", "dB", -24.0f, 12.0f, 0.0f, 0, "Output"},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.example.reverb",
    "Sonore Reverb",
    "Sonorie",
    "1.0.0",
    "Convolution reverb: a real impulse response, with honest latency and tail.",
    "https://sonorie.com",
    false, // effect
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr,
    0,
    "reverb",
};

#include <sonore/clap_wrapper.h>

#if defined(SONORE_BUILD_VST3)
#include <sonore/vst3_wrapper.h>
#endif

#if defined(SONORE_BUILD_AU)
#include <sonore/au_wrapper.h>
#include <sonore/au_view.h>
#endif

#if defined(SONORE_BUILD_LV2)
#include <sonore/lv2_wrapper.h>
#endif

#if defined(SONORE_BUILD_STANDALONE)
#include <sonore/standalone.h>
#endif
