// SPDX-License-Identifier: Apache-2.0
// The macOS hosting work, RUN -- on a Mac, by whoever next has one.
//
// Everything this exercises was written without a Mac: Audio Unit hosting
// (host_au.h), audio workgroups and real-time helper threads
// (audio_workgroup.h, the realtime ThreadPool, CoreAudioOutput::ioWorkgroup),
// the hand-built blocks (apple_block.h) and NSViewComponentWithParent. It has
// been compiled for macOS against a shim of Apple's headers and never run.
// This program is the run. It uses only what every Mac has -- Apple's own
// AUDelay and DLSMusicDevice, the default output device -- and PRINTS what
// every step returned, so a failure arrives as a reason rather than a crash
// report. Built only on Apple (see the CMake lines in the commit that added
// it); compiled here, against the shim, so it cannot rot either.
//
//   ./mac_host_selftest            run everything
//
// "skip" is a step this machine cannot take (no audio device on a CI runner,
// an older macOS); "FAIL" is a step that ran and was wrong. The exit status
// is the number of FAILs.
#if !defined(__APPLE__)
#error "mac_host_selftest is macOS only: it calls Apple's frameworks"
#endif

#include <sonore/apple_block.h>
#include <sonore/audio_coreaudio.h>
#include <sonore/audio_workgroup.h>
#include <sonore/gfx/nsview_component.h>
#include <sonore/gfx/window_cocoa.h>
#include <sonore/host.h>
#include <sonore/thread_pool.h>

#include <dlfcn.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "mac_host_constants.h"

namespace {

int failures = 0;

void ok(bool good, const char* what, const std::string& detail = std::string()) {
  std::printf("  %s %s%s%s\n", good ? "ok  " : "FAIL", what, detail.empty() ? "" : ": ",
              detail.c_str());
  if (!good) ++failures;
}

void skip(const char* what, const std::string& why) {
  std::printf("  skip %s: %s\n", what, why.c_str());
}

void note(const char* what, const std::string& value) { std::printf("       %s: %s\n", what, value.c_str()); }

using sonore::gfx::cocoa::cls;
using sonore::gfx::cocoa::msg;

// ── Apple's numbers, against ours ──────────────────────────────────────────

void reportConstant(const mac_host_constants::Pair& pair, bool same) {
  if (!same)
    std::printf("  FAIL %s: ours %lld, Apple's %lld\n", pair.name, pair.ours, pair.apples);
}

void testConstants() {
  std::printf("\n-- every restated Apple constant --\n");
  const int differ = mac_host_constants::compareAll(&reportConstant);
  failures += differ;
  ok(differ == 0, "host_au.h, audio_workgroup.h and au_wrapper.h agree with Apple's headers");
  ok(sizeof(os_workgroup_join_token_s) <= sonore::detail::kWorkgroupTokenBytes,
     "WorkgroupToken has room for os_workgroup_join_token_s");
}

// ── The hand-built block ───────────────────────────────────────────────────

using TestInvoke = void (*)(void* block, int value);
void testInvoke(void* block, int value) {
  *sonore::detail::blockCapture<int*, TestInvoke>(block) = value;
}

void testBlocks() {
  std::printf("\n-- apple_block.h: a global block the runtime accepts --\n");
  int seen = 0;
  sonore::detail::BlockLiteral<int*, TestInvoke> block;
  ok(sonore::detail::makeGlobalBlock(block, &testInvoke, &seen), "_NSConcreteGlobalBlock found");
  using CopyFn = void* (*)(const void*);
  using ReleaseFn = void (*)(const void*);
  CopyFn copy = (CopyFn) dlsym(RTLD_DEFAULT, "_Block_copy");
  ReleaseFn release = (ReleaseFn) dlsym(RTLD_DEFAULT, "_Block_release");
  ok(copy && release, "_Block_copy and _Block_release found");
  if (!copy || !release) return;
  void* copied = copy(&block);
  ok(copied == &block, "_Block_copy returns a global block itself");
  auto* literal = static_cast<sonore::detail::BlockLiteral<int*, TestInvoke>*>(copied);
  literal->invoke(copied, 42);
  ok(seen == 42, "and calling it reaches the capture");
  release(copied);
  id asObject = (id) (void*) &block;
  id objcCopy = msg<id>(asObject, sel_registerName("copy"));
  ok((void*) objcCopy == (void*) &block, "-copy on it is the identity too");
  msg<void>(asObject, sel_registerName("release"));
  const char* className = class_getName(object_getClass(asObject));
  note("its class", className ? className : "(none)");
}

// ── Workgroups ─────────────────────────────────────────────────────────────

sonore::AudioWorkgroup deviceWorkgroup(sonore::standalone::CoreAudioOutput& output) {
  std::string why;
  if (!output.open([](float*, uint32_t, uint32_t) {})) {
    skip("the default output device", output.error());
    return sonore::AudioWorkgroup();
  }
  sonore::AudioWorkgroup workgroup = output.ioWorkgroup(&why);
  if (!workgroup) skip("kAudioDevicePropertyIOThreadOSWorkgroup", why);
  return workgroup;
}

void testWorkgroups() {
  std::printf("\n-- audio_workgroup.h: the device's workgroup, joined --\n");
  if (!sonore::AudioWorkgroup::isSupported()) {
    skip("os_workgroup", sonore::AudioWorkgroup::unavailableReason());
    return;
  }
  ok(true, "os_workgroup_* found");

  const sonore::RealtimeThreadHints hints = sonore::RealtimeThreadHints::forBlock(48000.0, 512);
  std::thread promote([&hints]() {
    std::string why;
    const bool promoted = sonore::promoteCurrentThreadToRealtime(hints, &why);
    ok(promoted, "thread_policy_set(THREAD_TIME_CONSTRAINT_POLICY) accepted 48 kHz / 512", why);
    thread_time_constraint_policy_data_t policy{};
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t isDefault = 0;
    const kern_return_t got = thread_policy_get(pthread_mach_thread_np(pthread_self()),
                                                THREAD_TIME_CONSTRAINT_POLICY,
                                                (thread_policy_t) &policy, &count, &isDefault);
    char text[160];
    std::snprintf(text, sizeof(text), "kr %d, default %d, period %u computation %u constraint %u",
                  (int) got, (int) isDefault, policy.period, policy.computation, policy.constraint);
    note("thread_policy_get reads back", text);
  });
  promote.join();

  sonore::standalone::CoreAudioOutput output;
  sonore::AudioWorkgroup workgroup = deviceWorkgroup(output);
  if (!workgroup) return;
  ok(true, "the device's IO workgroup");
  note("max parallel threads", std::to_string(workgroup.maxParallelThreadCount()));
  ok(!workgroup.isCancelled(), "not cancelled");

  // Joined twice: once as a plain thread, once as a real-time one, because
  // whether os_workgroup_join cares is one of the things this run is for.
  for (int realtime = 0; realtime < 2; ++realtime) {
    std::thread worker([&]() {
      std::string why;
      if (realtime) sonore::promoteCurrentThreadToRealtime(hints, &why);
      sonore::WorkgroupToken token;
      const bool joined = workgroup.join(token);
      const std::string label = realtime ? "a real-time thread joins" : "a plain thread joins";
      ok(joined, label.c_str(), std::string(token.describe()) + " (" + std::to_string(token.errorCode()) + ")");
      if (joined) {
        ok(token.isAttachedTo(workgroup), "the token knows its group");
        sonore::WorkgroupToken second;
        sonore::AudioWorkgroup same = workgroup;
        note("a second join on the same thread", same.join(second) ? "accepted" : second.describe());
        second.leave();
        ok(token.leave(), "and leaves on its own thread");
      }
    });
    worker.join();
  }

  std::printf("\n-- ThreadPool: realtime workers in the device's workgroup --\n");
  sonore::ThreadPool pool(2, hints);
  pool.setAudioWorkgroup(workgroup);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  sonore::ThreadPool::WorkgroupState state = pool.workgroupState();
  while ((state.settled != state.workers || state.generation < 2) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    state = pool.workgroupState();
  }
  ok(state.realtime == 2, "both workers are real-time", state.message);
  ok(state.joined == 2, "both workers joined", state.message);
  pool.offerAudioWorkgroup(sonore::AudioWorkgroup());
  std::atomic<int> ran{0};
  pool.addJob([&ran](const std::atomic<bool>&) { ran.fetch_add(1); });
  pool.waitForAll(3000);
  state = pool.workgroupState();
  ok(ran.load() == 1, "a job runs after an offer of no workgroup");
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (state.joined != 0 && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    state = pool.workgroupState();
  }
  ok(state.joined == 0, "and the workers left the group", state.message);
  output.stop();
}

// ── Audio Units ────────────────────────────────────────────────────────────

std::unique_ptr<sonore::host::HostedPlugin> loadApple(const char* id, std::string* error) {
  sonore::host::PluginDescription d;
  d.format = "AU";
  d.id = id;
  return sonore::host::loadAudioUnit(d, error);
}

void testRegistry() {
  std::printf("\n-- the AU registry --\n");
  std::string error;
  ok(sonore::host::AudioUnitPluginFormat::isAvailable(&error), "AudioToolbox and CoreFoundation found", error);
  const std::vector<sonore::host::PluginDescription> all =
      sonore::host::AudioUnitPluginFormat::findAllComponents(&error);
  ok(!all.empty(), "findAllComponents finds units", std::to_string(all.size()) + " found");
  bool delay = false, dls = false;
  for (const auto& d : all) {
    if (d.id == "aufx:dely:appl") {
      delay = true;
      note("AUDelay", d.vendor + " / " + d.name + " / " + d.version + " / " + d.features);
    }
    if (d.id == "aumu:dls :appl") dls = true;
  }
  ok(delay, "Apple's AUDelay is among them");
  ok(dls, "and DLSMusicDevice");
  sonore::host::PluginDescription viaHost;
  viaHost.format = "AU";
  viaHost.id = "aufx:dely:appl";
  ok(sonore::host::loadPlugin(viaHost) != nullptr, "host.h's loadPlugin routes format AU here");
  const std::vector<std::string> folders = sonore::host::AudioUnitPluginFormat::defaultSearchPaths();
  bool described = false;
  for (const std::string& folder : folders) {
    for (const std::string& file : sonore::host::pluginFilesIn(folder)) {
      if (file.size() < 10 || file.compare(file.size() - 10, 10, ".component") != 0) continue;
      std::string why;
      const auto found = sonore::host::AudioUnitPluginFormat::describeBundle(file, &why);
      ok(!found.empty(), "a .component is described from its Info.plist", file + (why.empty() ? "" : " -- " + why));
      for (const auto& d : found) note("  declares", d.id + " " + d.name + " " + d.features);
      described = true;
      break;
    }
    if (described) break;
  }
  if (!described) skip("describing a .component", "none installed in the standard folders");
}

void testAsyncInstantiate() {
  std::printf("\n-- AudioComponentInstantiate through a hand-built block --\n");
  const sonore::host::detail::AuApi& api = sonore::host::detail::auApi();
  if (!api.instantiate) {
    skip("AudioComponentInstantiate", "not found");
    return;
  }
  AudioComponentDescription wanted{};
  wanted.componentType = kAudioUnitType_Effect;
  wanted.componentSubType = sonore::host::detail::aucode::fourCC('d', 'e', 'l', 'y');
  wanted.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent component = api.findNext(nullptr, &wanted);
  if (!component) {
    skip("AUDelay", "not registered");
    return;
  }
  auto* request = new sonore::host::detail::AuInstantiateRequest();
  ok(sonore::detail::makeGlobalBlock(request->block, &sonore::host::detail::AuInstantiateRequest::completed,
                                     request),
     "the completion block is built");
  api.instantiate(component, 0, &request->block);
  const bool arrived = sonore::host::detail::auWaitWhile(request->state, 0, 10.0);
  ok(arrived, "the completion ran");
  if (arrived) {
    ok(request->status == noErr && request->instance, "with an instance",
       sonore::host::detail::auStatusText(request->status));
    if (request->instance) api.instanceDispose(request->instance);
  }
  request->drop();
}

double rms(const std::vector<float>& v) {
  double sum = 0.0;
  for (float x : v) sum += (double) x * x;
  return v.empty() ? 0.0 : std::sqrt(sum / (double) v.size());
}

void testEffect() {
  std::printf("\n-- AUDelay, loaded, rendered, automated, saved --\n");
  std::string error;
  std::unique_ptr<sonore::host::HostedPlugin> plugin = loadApple("aufx:dely:appl", &error);
  ok(plugin != nullptr, "loadAudioUnit(aufx:dely:appl)", error);
  if (!plugin) return;
  auto* au = static_cast<sonore::host::AuPlugin*>(plugin.get());
  note("v3", au->isV3() ? "yes" : "no");
  ok(au->numParameters() > 0, "parameters were read", std::to_string(au->numParameters()));
  for (int i = 0; i < au->numParameters(); ++i) {
    const sonore::host::HostedParam& p = au->parameter(i);
    char line[256];
    std::snprintf(line, sizeof(line), "id %u '%s' [%g .. %g] default %g now %g -> '%s'%s", p.id,
                  p.name.c_str(), p.minValue, p.maxValue, p.defaultValue, au->parameterValue(i),
                  au->parameterText(i, au->parameterValue(i)).c_str(), p.stepped ? " stepped" : "");
    note("  parameter", line);
  }
  ok(au->prepare(48000.0, 512, 2), "prepare(48 kHz, 512, stereo)", au->lastError());
  note("widths", std::to_string(au->inputChannels()) + " in, " + std::to_string(au->outputChannels()) + " out");
  ok(au->hasAudioInput(), "an effect has an input");

  std::vector<float> left(512, 0.0f), right(512, 0.0f);
  left[0] = right[0] = 1.0f;
  float* channels[2] = {left.data(), right.data()};
  sonore::AudioBlock<float> io(channels, 2, 512);
  au->process(io);
  ok(au->renderFailures() == 0, "AudioUnitRender accepted the block",
     sonore::host::detail::auStatusText(au->lastRenderStatus()));
  bool finite = true;
  for (float x : left) finite = finite && std::isfinite(x);
  ok(finite && rms(left) > 0.0, "the impulse comes out (the dry half of the mix)");

  if (au->numParameters() > 0) {
    const sonore::host::HostedParam& p = au->parameter(0);
    const double target = p.minValue + 0.25 * (p.maxValue - p.minValue);
    ok(au->setParameterValue(0, target), "setParameterValue", au->lastError());
    ok(std::fabs(au->parameterValue(0) - target) < 1e-3 * (1.0 + std::fabs(target)),
       "and parameterValue reads it back");
  }
  std::vector<uint8_t> state;
  ok(au->saveState(state) && state.size() > 5 && std::memcmp(state.data(), "<?xml", 5) == 0,
     "ClassInfo saves as an XML property list", au->lastError());
  if (!state.empty()) ok(au->loadState(state.data(), state.size()), "and loads back", au->lastError());
  note("latency (samples)", std::to_string(au->latencySamples()));
  note("tail (samples)", std::to_string(au->tailSamples()));
  note("factory presets", std::to_string(au->numPresets()));
  if (au->numPresets() > 0) ok(au->loadPreset(0), "the first factory preset loads", au->lastError());
  if (au->hasBypass()) {
    ok(au->setBypassed(true) && au->isBypassed(), "bypass on", au->lastError());
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    left[10] = 0.5f;
    au->process(io);
    ok(std::fabs(left[10] - 0.5f) < 1e-6f, "a bypassed unit passes its input");
    au->setBypassed(false);
  } else {
    skip("bypass", "AUDelay offers no kAudioUnitProperty_BypassEffect here");
  }
  std::vector<sonore::host::ParamEdit> edits;
  note("edits reported", std::to_string(au->drainParameterEdits(edits)));
  note("lastError", au->lastError().empty() ? "(none)" : au->lastError());
  au->release();
}

void testInstrument() {
  std::printf("\n-- DLSMusicDevice: MIDI in, sound out --\n");
  std::string error;
  std::unique_ptr<sonore::host::HostedPlugin> plugin = loadApple("aumu:dls :appl", &error);
  if (!plugin) {
    ok(false, "loadAudioUnit(aumu:dls :appl)", error);
    return;
  }
  auto* au = static_cast<sonore::host::AuPlugin*>(plugin.get());
  ok(au->acceptsMidi() && !au->hasAudioInput(), "an instrument: MIDI in, no audio in");
  ok(au->prepare(44100.0, 256, 2), "prepare(44.1 kHz, 256, stereo)", au->lastError());
  std::vector<float> left(256, 0.0f), right(256, 0.0f);
  float* channels[2] = {left.data(), right.data()};
  sonore::AudioBlock<float> io(channels, 2, 256);
  sonore::MidiBuffer midi;
  midi.addEvent(sonore::MidiMessage::noteOn(0, 60, 110), 0);
  double energy = 0.0;
  for (int block = 0; block < 20; ++block) {
    std::fill(left.begin(), left.end(), 0.0f);
    std::fill(right.begin(), right.end(), 0.0f);
    au->process(io, block == 0 ? &midi : nullptr);
    energy += rms(left);
  }
  ok(au->renderFailures() == 0, "every block rendered",
     sonore::host::detail::auStatusText(au->lastRenderStatus()));
  ok(energy > 0.0, "a note-on makes sound", std::to_string(energy));
  au->release();
}

void testEditorAndEmbedding() {
  std::printf("\n-- editors and NSViewComponentWithParent --\n");
  id app = msg<id>(cls("NSApplication"), sel_registerName("sharedApplication"));
  if (!app) {
    skip("AppKit", "no NSApplication");
    return;
  }
  // A bare NSView as the host's: no window, which a view does not need.
  id parent = msg<id>(cls("NSView"), sel_registerName("alloc"));
  const sonore::gfx::cocoa::CGRect2 frame{{0.0, 0.0}, {800.0, 600.0}};
  using InitFrame = id (*)(id, SEL, sonore::gfx::cocoa::CGRect2);
  parent = reinterpret_cast<InitFrame>(objc_msgSend)(parent, sel_registerName("initWithFrame:"), frame);
  ok(parent != nullptr, "an NSView to embed into");
  if (!parent) return;

  std::string error;
  std::unique_ptr<sonore::host::HostedPlugin> plugin = loadApple("aufx:dely:appl", &error);
  if (plugin && plugin->hasEditor()) {
    auto* au = static_cast<sonore::host::AuPlugin*>(plugin.get());
    const bool opened = au->openEditor(parent);
    ok(opened, "AUDelay's editor opens", au->lastError());
    uint32_t w = 0, h = 0;
    if (opened && au->editorSize(w, h)) note("its size", std::to_string(w) + " x " + std::to_string(h));
    au->closeEditor();
  } else {
    skip("an AU editor", plugin ? "AUDelay has no view of its own here" : error);
  }

  // The peer a native editor draws in, with no window around it, and a
  // component holding an NSView inside it.
  sonore::gfx::Component root;
  root.setBounds({0.0f, 0.0f, 400.0f, 300.0f});
  sonore::gfx::NSViewComponentWithParent embed;
  embed.setBounds({10.0f, 20.0f, 100.0f, 50.0f});
  root.addChild(&embed);
  sonore::gfx::NativeWindowCocoa peer;
  if (!sonore::gfx::NativeWindowCocoa::isAvailable() || !peer.open(nullptr, root, 400, 300)) {
    skip("the Cocoa peer", "it would not open without a window here");
  } else {
    ok(sonore::gfx::NativeWindowCocoa::viewShowing(&root) == peer.handle(), "the peer is found by its tree");
    id child = msg<id>(cls("NSView"), sel_registerName("alloc"));
    const sonore::gfx::cocoa::CGRect2 small{{0.0, 0.0}, {30.0, 30.0}};
    child = reinterpret_cast<InitFrame>(objc_msgSend)(child, sel_registerName("initWithFrame:"), small);
    ok(embed.setView(child), "setView takes an NSView", embed.lastError());
    msg<void>(child, sel_registerName("release")); // the component holds its own
    ok(embed.isAttached(), "the parent view went into the peer's view");
    id superview = msg<id>((id) embed.parentView(), sel_registerName("superview"));
    ok((void*) superview == peer.handle(), "as a subview of it");
    const auto placed = sonore::gfx::cocoa::msgRect<sonore::gfx::cocoa::CGRect2>(
        (id) embed.parentView(), sel_registerName("frame"));
    ok(placed.origin.x == 10.0 && placed.origin.y == 20.0 && placed.size.width == 100.0,
       "at the component's bounds, y down");
    const auto filled =
        sonore::gfx::cocoa::msgRect<sonore::gfx::cocoa::CGRect2>(child, sel_registerName("frame"));
    ok(filled.size.width == 100.0 && filled.size.height == 50.0, "the child fills it");
    embed.setVisible(false);
    embed.syncNativeView();
    ok(msg<BOOL>((id) embed.parentView(), sel_registerName("isHidden")) == YES, "hidden with the component");
    embed.setVisible(true);
    embed.syncNativeView();
    using SetSize = void (*)(id, SEL, sonore::gfx::cocoa::CGSize2);
    reinterpret_cast<SetSize>(objc_msgSend)(child, sel_registerName("setFrameSize:"),
                                            sonore::gfx::cocoa::CGSize2{200.0, 80.0});
    ok(embed.bounds().w == 200.0f && embed.bounds().h == 80.0f, "the component follows the child's size");
    peer.close();
    embed.syncNativeView();
    ok(!embed.isAttached(), "and leaves when the peer closes");
  }
  msg<void>(parent, sel_registerName("release"));
}

} // namespace

int main() {
  id pool = msg<id>(msg<id>(cls("NSAutoreleasePool"), sel_registerName("alloc")), sel_registerName("init"));
  std::printf("mac_host_selftest: AU hosting, workgroups and NSView embedding, RUN\n");
  testConstants();
  testBlocks();
  testWorkgroups();
  testRegistry();
  testAsyncInstantiate();
  testEffect();
  testInstrument();
  testEditorAndEmbedding();
  if (pool) msg<void>(pool, sel_registerName("drain"));
  std::printf("\n%d failure(s)\n%s\n", failures,
              failures == 0 ? "MAC HOST SELFTEST PASSED" : "MAC HOST SELFTEST FAILED");
  return failures;
}
