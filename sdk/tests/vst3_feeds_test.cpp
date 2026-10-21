// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a VST3 feeds the editor's load meter and scope, in both
// precisions, against the session's own deadline.
//
// The CLAP wrapper times every block and pushes it into the scope; the VST3
// process() did neither, so a VST3 editor's CPU read 0 and its scope never
// moved -- "measured in every format", said the Instance. And nothing in any
// format told the load meter the sample rate, so it judged every block
// against a 48 kHz deadline: at 96 kHz it read half the real load.
//
// And a host that delivers fewer channels than it negotiated, with the bypass
// engaged, must not send the crossfade through pointers nobody set.
//
// The trim example's VST3 is compiled into this test, as rt_safety_test does
// with CLAP, so the wrapper's own Instance can be read after driving it
// through its C API.
#define SONORE_BUILD_VST3 1
#include "../examples/trim/plugin.cpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

namespace {

template <typename Sample>
void runBlocks(Steinberg_Vst_IAudioProcessor* proc, int blocks, Steinberg_int32 frames,
               Steinberg_int32 sampleSize) {
  std::vector<Sample> inL((size_t) frames), inR((size_t) frames), outL((size_t) frames),
      outR((size_t) frames);
  Sample* in[2] = {inL.data(), inR.data()};
  Sample* out[2] = {outL.data(), outR.data()};
  Steinberg_Vst_AudioBusBuffers inBus{}, outBus{};
  inBus.numChannels = 2;
  outBus.numChannels = 2;
  if constexpr (sizeof(Sample) == 4) {
    inBus.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = in;
    outBus.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = out;
  } else {
    inBus.Steinberg_Vst_AudioBusBuffers_channelBuffers64 = in;
    outBus.Steinberg_Vst_AudioBusBuffers_channelBuffers64 = out;
  }
  Steinberg_Vst_ProcessData data{};
  data.processMode = Steinberg_Vst_ProcessModes_kRealtime;
  data.symbolicSampleSize = sampleSize;
  data.numSamples = frames;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &inBus;
  data.outputs = &outBus;
  static long phase = 0;
  for (int b = 0; b < blocks; ++b) {
    for (Steinberg_int32 i = 0; i < frames; ++i, ++phase)
      inL[(size_t) i] = inR[(size_t) i] = (Sample) (0.5 * std::sin(0.05 * (double) phase));
    proc->lpVtbl->process(proc, &data);
  }
}

/** Leaves a recognisable pattern where the next call's locals will be, so a
 *  pointer read before it is written is 0x4141... rather than whatever an
 *  earlier call happened to leave -- a crash, every time, instead of a write
 *  somewhere plausible. */
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void dirtyStack() {
  volatile unsigned char junk[32768];
  for (size_t i = 0; i < sizeof(junk); ++i) junk[i] = 0x41;
}

/** One block delivered ONE channel wide to a plugin negotiated at two. */
float narrowBlock(Steinberg_Vst_IAudioProcessor* proc) {
  static float in[256], out[256];
  for (int i = 0; i < 256; ++i) in[i] = 0.5f * (float) std::sin(0.05 * i);
  float* inPtr[1] = {in};
  float* outPtr[1] = {out};
  Steinberg_Vst_AudioBusBuffers inBus{}, outBus{};
  inBus.numChannels = 1;
  outBus.numChannels = 1;
  inBus.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = inPtr;
  outBus.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = outPtr;
  Steinberg_Vst_ProcessData data{};
  data.processMode = Steinberg_Vst_ProcessModes_kRealtime;
  data.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
  data.numSamples = 256;
  data.numInputs = 1;
  data.numOutputs = 1;
  data.inputs = &inBus;
  data.outputs = &outBus;
  dirtyStack();
  proc->lpVtbl->process(proc, &data);
  float peak = 0.0f;
  for (float v : out) peak = std::fabs(v) > peak ? std::fabs(v) : peak;
  return peak;
}

bool scopeMoved(const sonore::AudioScopeBuffer& scope) {
  for (int c = 0; c < scope.columns(); ++c) {
    float lo = 0.0f, hi = 0.0f;
    scope.columnAt(c, &lo, &hi);
    if (hi > 0.1f) return true;
  }
  return false;
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── VST3 load meter and scope ──\n");

  Steinberg_IPluginFactory* factory = GetPluginFactory();
  Steinberg_PClassInfo info{};
  bool found = false;
  for (Steinberg_int32 i = 0; i < factory->lpVtbl->countClasses(factory) && !found; ++i)
    found = factory->lpVtbl->getClassInfo(factory, i, &info) == Steinberg_kResultOk &&
            std::strcmp(info.category, "Audio Module Class") == 0;
  void* object = nullptr;
  check(found && factory->lpVtbl->createInstance(factory, (Steinberg_FIDString) info.cid,
                                                 (Steinberg_FIDString) Steinberg_Vst_IComponent_iid,
                                                 &object) == Steinberg_kResultOk,
        "the trim VST3 is created through its own factory");
  if (!object) return 1;
  auto* component = (Steinberg_Vst_IComponent*) object;
  component->lpVtbl->initialize(component, nullptr);
  Steinberg_Vst_IAudioProcessor* proc = nullptr;
  component->lpVtbl->queryInterface(component, Steinberg_Vst_IAudioProcessor_iid, (void**) &proc);
  check(proc != nullptr, "...with a processor");
  if (!proc) return 1;
  sonore::vst3::Plugin* plugin = sonore::vst3::ownerOfComponent(component);

  char line[200];
  const Steinberg_int32 sizes[2] = {Steinberg_Vst_SymbolicSampleSizes_kSample32,
                                    Steinberg_Vst_SymbolicSampleSizes_kSample64};
  for (Steinberg_int32 size : sizes) {
    const bool doubles = size == Steinberg_Vst_SymbolicSampleSizes_kSample64;
    if (doubles && proc->lpVtbl->canProcessSampleSize(proc, size) != Steinberg_kResultOk) continue;
    const char* label = doubles ? "64-bit" : "32-bit";
    Steinberg_Vst_ProcessSetup setup{};
    setup.processMode = Steinberg_Vst_ProcessModes_kRealtime;
    setup.symbolicSampleSize = size;
    setup.maxSamplesPerBlock = 256;
    setup.sampleRate = 96000.0;
    proc->lpVtbl->setupProcessing(proc, &setup);
    component->lpVtbl->setActive(component, 1);
    proc->lpVtbl->setProcessing(proc, 1);
    plugin->shared.scope.clear();
    const uint64_t before = plugin->shared.load.blocks();
    if (doubles) runBlocks<double>(proc, 100, 256, size);
    else runBlocks<float>(proc, 100, 256, size);
    const uint64_t timed = plugin->shared.load.blocks() - before;
    std::snprintf(line, sizeof(line), "%s: every block is timed for the load meter (%llu of 100)",
                  label, (unsigned long long) timed);
    check(timed == 100, line);
    std::snprintf(line, sizeof(line), "%s: ...and pushed into the scope", label);
    check(scopeMoved(plugin->shared.scope), line);

    // A block that takes a hundred of its deadlines at 96 kHz is a load of
    // 100 -- 50 had the meter kept 48 kHz. A hundred, not one: the peak is
    // the worst block since the reset, the hundred real blocks above among
    // them, and on a busy machine one of those took longer than its own
    // deadline and read 1.93 where this one should have read 1.
    plugin->shared.load.record(100.0 * 256.0 / 96000.0, 256);
    std::snprintf(line, sizeof(line), "%s: the deadline is the session's 96 kHz one, not 48 kHz "
                  "(a block of 100 x 256/96000 s reads %.2f, want 100.00)", label,
                  (double) plugin->shared.load.peakLoad());
    check(std::fabs(plugin->shared.load.peakLoad() - 100.0f) < 1.0f, line);
    proc->lpVtbl->setProcessing(proc, 0);
    component->lpVtbl->setActive(component, 0);
  }

  // A MONO track is kMono, VST3's bit 19 -- where our roles keep top side
  // right. The arrangement was copied as the role mask, so the DSP was told
  // its one channel was TSR and a mono-to-stereo matrix built from that
  // routed it nowhere. It is the centre.
  {
    Steinberg_Vst_SpeakerArrangement mono = Steinberg_Vst_SpeakerArr_kMono;
    Steinberg_Vst_SpeakerArrangement monoOut = Steinberg_Vst_SpeakerArr_kMono;
    const bool accepted =
        proc->lpVtbl->setBusArrangements(proc, &mono, 1, &monoOut, 1) == Steinberg_kResultOk;
    uint8_t roles[4] = {255, 255, 255, 255};
    const uint32_t n = sonore::rolesFromMask(plugin->shared.mainChannelMask, roles, 4);
    std::snprintf(line, sizeof(line), "a kMono bus is one channel whose role is the centre "
                  "(accepted %d, %u role(s), role %u)", (int) accepted, n, (unsigned) roles[0]);
    check(!accepted || (n == 1 && roles[0] == sonore::kChannelFC), line);
    check(accepted, "...and a mono bus is accepted at all");
    Steinberg_Vst_SpeakerArrangement stereo = Steinberg_Vst_SpeakerArr_kStereo;
    Steinberg_Vst_SpeakerArrangement stereoOut = Steinberg_Vst_SpeakerArr_kStereo;
    proc->lpVtbl->setBusArrangements(proc, &stereo, 1, &stereoOut, 1);
  }

  // A host may deliver fewer channels than were negotiated -- this call's
  // bus one wide on a stereo plugin. The wrapper processes the channels it
  // was given, but the bypass crossfade walked every NEGOTIATED channel, and
  // the pointers past the delivered ones were never set: with the bypass
  // engaged, it read and wrote through garbage.
  {
    Steinberg_Vst_ProcessSetup setup{};
    setup.processMode = Steinberg_Vst_ProcessModes_kRealtime;
    setup.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
    setup.maxSamplesPerBlock = 256;
    setup.sampleRate = 48000.0;
    proc->lpVtbl->setupProcessing(proc, &setup);
    component->lpVtbl->setActive(component, 1);
    proc->lpVtbl->setProcessing(proc, 1);
    plugin->shared.bypass.engaged = true;
    float peak = 0.0f;
    for (int block = 0; block < 8; ++block) peak = narrowBlock(proc);
    std::snprintf(line, sizeof(line), "a block narrower than the negotiated width, bypass engaged, "
                  "passes the dry signal through (peak %.3f, want 0.5)", (double) peak);
    check(std::fabs(peak - 0.5f) < 0.02f, line);
    plugin->shared.bypass.engaged = false;
    proc->lpVtbl->setProcessing(proc, 0);
    component->lpVtbl->setActive(component, 0);
  }

  proc->lpVtbl->release(proc);
  component->lpVtbl->terminate(component);
  component->lpVtbl->release(component);
  factory->lpVtbl->release(factory);

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE VST3 FEEDS PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
