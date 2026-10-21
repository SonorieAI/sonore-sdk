// SPDX-License-Identifier: Apache-2.0
// The macOS hosting work compiled on Windows: Audio Unit hosting (host_au.h,
// through host.h), audio workgroups, the realtime ThreadPool, the device's
// workgroup, the hand-built block and NSViewComponentWithParent. Nothing here
// runs; the compiler reading it is the test, exactly as for
// mac_syntax_plugin.cpp -- see the note there and au_shim/README.md for what
// that proves (every name exists, every call fits its declaration) and what it
// does not (that the declarations are Apple's).
//
// Every template and every member is reached from sonoreMacHostSyntax() below,
// so nothing is left unread for being unused. mac_host_selftest.cpp is the
// same surface RUN, on a Mac.
#if !defined(__APPLE__) || defined(_WIN32)
#error "build with __APPLE__ defined and _WIN32 undefined: see sonore_mac_syntax_* in CMakeLists.txt"
#endif

#include <sonore/apple_block.h>
#include <sonore/audio_coreaudio.h>
#include <sonore/audio_workgroup.h>
#include <sonore/gfx/nsview_component.h>
#include <sonore/gfx/window_cocoa.h>
#include <sonore/host.h>
#include <sonore/plugin_cache.h>
#include <sonore/thread_pool.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "mac_host_constants.h"

namespace {

struct DspWithHook {
  sonore::AudioWorkgroup seen;
  void workgroupChanged(const sonore::AudioWorkgroup& workgroup) { seen = workgroup; }
};

struct DspWithoutHook {};

using TestInvoke = void (*)(void* block, int value);

void testInvoke(void* block, int value) {
  int* target = sonore::detail::blockCapture<int*, TestInvoke>(block);
  *target = value;
}

void reportPair(const mac_host_constants::Pair& pair, bool same) {
  if (!same) std::printf("%s: ours %lld, Apple's %lld\n", pair.name, pair.ours, pair.apples);
}

} // namespace

/** Reaches everything. Never called: there is no Apple runtime to call into. */
void sonoreMacHostSyntax(void* parentView, void* childView) {
  using namespace sonore;

  // ── apple_block.h ─────────────────────────────────────────────────────────
  int captured = 0;
  detail::BlockLiteral<int*, TestInvoke> block;
  if (detail::makeGlobalBlock(block, &testInvoke, &captured)) block.invoke(&block, 7);

  // ── audio_workgroup.h ─────────────────────────────────────────────────────
  AudioWorkgroup workgroup = AudioWorkgroup::retaining(parentView);
  AudioWorkgroup adopted = AudioWorkgroup::adopting(childView);
  WorkgroupToken token;
  if (workgroup.join(token)) token.leave();
  std::printf("%s %d %d %d %s\n", token.describe(), token.errorCode(), (int) token.status(),
              workgroup.maxParallelThreadCount(), workgroup.isCancelled() ? "c" : "");
  (void) token.isAttachedTo(adopted);
  void* handle = adopted.detachHandle();
  AudioWorkgroup back = AudioWorkgroup::adopting(handle);
  std::printf("%p %d %s\n", back.nativeHandle(), AudioWorkgroup::isSupported() ? 1 : 0,
              AudioWorkgroup::unavailableReason() ? "unsupported" : "supported");
  std::string why;
  const RealtimeThreadHints hints = RealtimeThreadHints::forBlock(48000.0, 256);
  (void) promoteCurrentThreadToRealtime(hints, &why);
  DspWithHook hooked;
  DspWithoutHook plain;
  dspWorkgroupChanged(hooked, workgroup);
  dspWorkgroupChanged(plain, workgroup);

  // ── the realtime ThreadPool ───────────────────────────────────────────────
  ThreadPool pool(4, hints);
  pool.setAudioWorkgroup(workgroup);
  pool.offerAudioWorkgroup(workgroup);
  pool.setRealtimeHints(RealtimeThreadHints::forBlock(44100.0, 128));
  const ThreadPool::WorkgroupState state = pool.workgroupState();
  std::printf("%d %d %d %s\n", state.workers, state.joined, state.realtime, state.message.c_str());

  // ── the device's workgroup ────────────────────────────────────────────────
  standalone::CoreAudioOutput output;
  AudioWorkgroup device = output.ioWorkgroup(&why);
  (void) device;

  // ── Audio Unit hosting ────────────────────────────────────────────────────
  std::string error;
  std::vector<host::PluginDescription> all = host::AudioUnitPluginFormat::findAllComponents(&error);
  std::vector<host::PluginDescription> bundle =
      host::AudioUnitPluginFormat::describeBundle("/Library/Audio/Plug-Ins/Components/X.component", &error);
  std::vector<std::string> paths = host::AudioUnitPluginFormat::defaultSearchPaths();
  (void) host::AudioUnitPluginFormat::isAvailable(&error);
  host::PluginDescription d;
  d.format = host::AudioUnitPluginFormat::formatName();
  d.id = "aufx:dely:appl";
  (void) host::AudioUnitPluginFormat::createsAsynchronously(d);
  std::unique_ptr<host::HostedPlugin> loaded = host::AudioUnitPluginFormat::createInstance(d, &error);
  std::unique_ptr<host::HostedPlugin> viaHost = host::loadPlugin(d);
  std::vector<host::PluginDescription> files = host::describeFile("/tmp/X.component");
  if (loaded) {
    auto* au = static_cast<host::AuPlugin*>(loaded.get());
    au->prepare(48000.0, 512, 2);
    float left[512] = {}, right[512] = {};
    float* channels[2] = {left, right};
    AudioBlock<float> io(channels, 2, 512);
    MidiBuffer midi;
    midi.addEvent(MidiMessage::noteOn(0, 60, 100), 0);
    au->process(io, &midi);
    au->reset();
    std::printf("%u %u %d %d %d\n", au->latencySamples(), au->tailSamples(),
                au->acceptsMidi() ? 1 : 0, au->hasAudioInput() ? 1 : 0, au->numParameters());
    if (au->numParameters() > 0) {
      const host::HostedParam& p = au->parameter(0);
      au->setParameterValue(0, au->parameterValue(0) + p.minValue);
      std::printf("%s\n", au->parameterText(0, p.defaultValue).c_str());
    }
    std::vector<uint8_t> blob;
    if (au->saveState(blob)) au->loadState(blob.data(), blob.size());
    for (int i = 0; i < au->numPresets(); ++i) std::printf("%s\n", au->presetName(i).c_str());
    au->loadPreset(0);
    if (au->hasBypass()) au->setBypassed(!au->isBypassed());
    std::vector<host::ParamEdit> edits;
    au->drainParameterEdits(edits);
    (void) au->takeRestartRequest();
    (void) au->takeStateDirty();
    (void) au->producedMidi();
    if (au->hasEditor() && au->openEditor(parentView)) {
      uint32_t w = 0, h = 0;
      au->editorSize(w, h);
      au->idleEditor();
      au->setEditorScale(2.0);
      au->closeEditor();
    }
    std::printf("%s %u %d %p %d %u %u %u\n", au->lastError().c_str(), au->renderFailures() + au->midiFailures(),
                (int) au->lastRenderStatus(), (void*) au->audioUnit(), au->isV3() ? 1 : 0,
                (unsigned) au->componentDescription().componentType, au->inputChannels(),
                au->outputChannels());
    au->release();
  }
  std::vector<uint8_t> preset;
  if (viaHost) host::savePresetFile(*viaHost, d, "/tmp/x.aupreset");
  (void) all;
  (void) bundle;
  (void) paths;
  (void) files;
  (void) preset;

  // ── NSViewComponentWithParent ─────────────────────────────────────────────
  gfx::Component root;
  gfx::NSViewComponentWithParent embed;
  root.addChild(&embed);
  embed.setBounds({10.0f, 10.0f, 300.0f, 200.0f});
  embed.setFollowsViewSize(true);
  if (embed.setView(childView)) {
    embed.resizeToFitView();
    embed.syncNativeView();
    embed.childFrameChanged();
  }
  std::printf("%p %p %d %s %p\n", embed.getView(), embed.parentView(), embed.isAttached() ? 1 : 0,
              embed.lastError().c_str(), gfx::NativeWindowCocoa::viewShowing(&root));

  // ── the constants, ours against the shim's ────────────────────────────────
  (void) mac_host_constants::compareAll(&reportPair);
  (void) captured;
}
