// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: LV2's bypass stays in time when the latency moves.
//
// LV2 has no restart. A host reads the latency port after every run and
// re-compensates there and then, so a plugin whose latency moves while it
// runs -- a look-ahead set by a parameter -- is re-aligned at once. Its
// bypass must follow, or the dry signal comes out of time with everything
// else: the dry path stayed at the latency of the last activation.
//
// The probe effect delays its input by its one parameter, in samples, and
// reports that as its latency. Bypassed, its output must be its input
// delayed by whatever the latency port says.
#define SONORE_NUM_PARAMS 1

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sonore/dsp.h>

struct SonoreDsp {
  std::vector<float> line[2];
  size_t pos = 0;
  float latency = 100.0f;
  void prepare(const sonore::ProcessSpec&) {
    for (auto& l : line) l.assign(1024, 0.0f);
    pos = 0;
  }
  uint32_t latencySamples() const { return (uint32_t) latency; }
  void process(sonore::AudioBlock<float>& io, const float* p) {
    latency = p[0];
    const size_t d = (size_t) p[0];
    for (size_t i = 0; i < io.getNumSamples(); ++i)
      for (size_t c = 0; c < 2; ++c) {
        float* x = io.getChannelPointer(c);
        line[c][(pos + i) % 1024] = x[i];
        x[i] = 0.5f * line[c][(pos + i + 1024 - d) % 1024];
      }
    pos = (pos + io.getNumSamples()) % 1024;
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"lat", "Latency", "", 0.0f, 500.0f, 100.0f, 0}};
static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.test.lv2bypass", "Sonore LV2 Bypass Probe", "Sonorie", "1.0.0", "",
    "https://sonorie.com", false, kParamTable, SONORE_NUM_PARAMS};

#include <sonore/clap_wrapper.h>
#include <sonore/lv2_wrapper.h>

using namespace sonore::lv2;

namespace {
int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

struct UridMap {
  std::vector<std::string> uris;
  static LV2_URID mapCb(LV2_URID_Map_Handle h, const char* uri) {
    auto* self = (UridMap*) h;
    for (size_t i = 0; i < self->uris.size(); ++i)
      if (self->uris[i] == uri) return (LV2_URID) (i + 1);
    self->uris.emplace_back(uri);
    return (LV2_URID) self->uris.size();
  }
};
} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── LV2 bypass with a moving latency ──\n");
  UridMap urids;
  LV2_URID_Map map{&urids, UridMap::mapCb};
  LV2_Feature mapFeature{LV2_URID__map, &map};
  const LV2_Feature* features[] = {&mapFeature, nullptr};
  const LV2_Descriptor* d = descriptor();
  LV2_Handle h = d->instantiate(d, 48000.0, ".", features);
  check(h != nullptr, "the probe effect instantiates");
  if (!h) return 1;
  const int ports = portTimeIn() + 2;
  std::vector<std::vector<uint8_t>> buf((size_t) ports, std::vector<uint8_t>(1 << 16, 0));
  for (int p = 0; p < ports; ++p) d->connect_port(h, (uint32_t) p, buf[(size_t) p].data());
  float* latencyControl = (float*) buf[0].data();
  float* enabled = (float*) buf[(size_t) portEnabled()].data();
  float* latencyPort = (float*) buf[(size_t) portLatency()].data();
  float* inL = (float*) buf[(size_t) portAudioInL()].data();
  float* inR = (float*) buf[(size_t) portAudioInL() + 1].data();
  float* outL = (float*) buf[(size_t) portAudioOutL()].data();
  *latencyControl = 100.0f;
  *enabled = 1.0f;
  d->activate(h);

  // The dry ring holds floats. This DSP has no 64-bit path, so floats are all
  // that can reach it, and a ring of doubles held the same values in twice the
  // memory: 100 ms of room plus LV2's 8192-frame slice, 208 KB of a stereo
  // instance at 48 kHz where 104 KB holds it exactly. The bypass checks below
  // then run through the float ring.
  {
    const auto& ring = ((Plugin*) h)->shared.bypass.ring;
    char sized[160];
    std::snprintf(sized, sizeof(sized), "the dry ring stores %zu-byte samples, %zu frames a channel",
                  sizeof(ring[0][0]), ring[0].size());
    check(sizeof(ring[0][0]) == sizeof(float) && ring[0].size() == 4800 + 8192, sized);
  }

  // A count, so the output says exactly how far behind the input it is.
  uint64_t t = 0;
  auto run = [&](int blocks) {
    for (int b = 0; b < blocks; ++b) {
      for (int i = 0; i < 64; ++i) inL[i] = inR[i] = (float) (t + (uint64_t) i + 1);
      d->run(h, 64);
      t += 64;
    }
  };
  // How far behind its input the last block's output is, over the whole block.
  auto lag = [&](double* spread) {
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i < 64; ++i) {
      const double behind = (double) (t - 64 + (uint64_t) i + 1) - (double) outL[i];
      lo = behind < lo ? behind : lo;
      hi = behind > hi ? behind : hi;
    }
    *spread = hi - lo;
    return lo;
  };

  double spread = 0.0;
  run(10);
  *enabled = 0.0f;
  run(40);
  const double atStart = lag(&spread);
  char line[200];
  std::snprintf(line, sizeof(line), "bypassed at the latency it activated with: %.0f behind, port %.0f",
                atStart, (double) *latencyPort);
  check(atStart == (double) *latencyPort && spread == 0.0, line);

  *enabled = 1.0f;
  run(40);
  *latencyControl = 300.0f; // automation moves the look-ahead while running
  run(10);
  *enabled = 0.0f;
  run(40);
  const double moved = lag(&spread);
  std::snprintf(line, sizeof(line),
                "...and after the latency moved to %.0f while running, %.0f behind (spread %.0f)",
                (double) *latencyPort, moved, spread);
  check(moved == (double) *latencyPort && spread == 0.0, line);

  d->deactivate(h);
  d->cleanup(h);
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE LV2 BYPASS PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
