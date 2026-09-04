// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: hosting OTHER people's plugins.
//
// Everything else in this SDK is about being a plugin. This is the other
// direction: scanning a folder, finding what is in it, loading one, driving it
// with audio and MIDI, reading and writing its state. It is what an application
// needs to put third-party processors in its own signal path.
//
// The machinery is not new. The host tests in sdk/tests have been loading
// built artifacts and driving them as a host does for a long time, and this is
// that code made reusable and given a contract. What it adds is the part a
// test never needed: scanning, enumeration, and a lifetime that survives being
// handed around.
//
// WHAT THIS SUPPORTS, and what it does not:
//
//   CLAP: complete, and verified against every plugin this SDK builds.
//   VST3: complete, same. Compiled in only where Steinberg's C API has been
//           vendored, because a user who wants CLAP hosting should not have to
//           fetch a second SDK to make a header compile.
//   AU / AUv3: macOS only. Nothing here can be verified on a machine without
//           one, and an untested loader for someone else's binary is worse
//           than an honest gap.
//   LV2: complete, and the reason it took longest: an LV2 host cannot learn
//           a plugin's NAME without parsing RDF, so this needed turtle.h
//           first. Compiled in where the LV2 headers have been vendored.
//
// THREADING follows the same rules the wrappers obey. scan(), load(),
// prepare(), state and parameter reads are main-thread. process() is the audio
// thread and allocates nothing: every buffer it needs is sized in prepare().

#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <algorithm>
#include <string>
#include <vector>

#include <clap/clap.h>

// VST3 hosting is compiled in only where Steinberg's C API has been vendored.
// A user who wants CLAP hosting and nothing else should not have to fetch a
// second SDK to get a header to compile, and #if __has_include says so at the
// point of use rather than in a build file nobody reads.
#if defined(__has_include)
#if __has_include(<vst3_c_api.h>)
#define SONORE_HOST_VST3 1
#include <vst3_c_api.h>
#endif
#if __has_include(<lv2/core/lv2.h>)
#define SONORE_HOST_LV2 1
#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#endif
#endif

#include "audio.h"
#include "turtle.h"

#if defined(_WIN32)
// windows.h defines min and max as MACROS, which turn any later std::min or
// std::max into a syntax error -- and only when this header happens to be
// included FIRST. That made it an include-order landmine rather than a bug:
// gfx/viewport.h compiled for months and then stopped the day plugin_editor.h
// gained one more include, because the new one reached windows.h before
// viewport.h was seen.
//
// WIN32_LEAN_AND_MEAN for the ordinary reason: winsock, RPC, OLE and the shell
// are a large amount of preprocessing nobody here asked for.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#endif

namespace sonore {
namespace host {

// ── Dynamic libraries ────────────────────────────────────────────────────────

#if defined(_WIN32)
using LibHandle = HMODULE;
inline LibHandle openLib(const char* path) { return LoadLibraryA(path); }
inline void* libSymbol(LibHandle h, const char* name) {
  return h ? (void*) GetProcAddress(h, name) : nullptr;
}
inline void closeLib(LibHandle h) {
  if (h) FreeLibrary(h);
}
inline const char* clapExtension() { return ".clap"; }
#else
using LibHandle = void*;
inline LibHandle openLib(const char* path) { return dlopen(path, RTLD_LOCAL | RTLD_NOW); }
inline void* libSymbol(LibHandle h, const char* name) { return h ? dlsym(h, name) : nullptr; }
inline void closeLib(LibHandle h) {
  if (h) dlclose(h);
}
inline const char* clapExtension() { return ".clap"; }
#endif

// ── What a scan finds ────────────────────────────────────────────────────────

/** One plugin, described without loading it.
 *
 *  A scan may find several plugins inside one file: CLAP's factory returns a
 *  list, so `path` alone does not identify anything. The pair (path, id) does,
 *  and that pair is what a host should store in its own session. */
struct PluginDescription {
  std::string path;   // the file on disk
  std::string format; // "CLAP" today
  std::string id;     // the plugin's own id within that file
  std::string name;
  std::string vendor;
  std::string version;
  std::string features; // the format's own category words, semicolon separated
  bool isInstrument = false;
};

/** One parameter of a loaded plugin. */
struct HostedParam {
  /** The plugin's own handle for this parameter -- CLAP's clap_id, VST3's
   *  ParamID. Both are a 32-bit value the plugin chose; a host stores it and
   *  hands it back. Kept as a plain integer so talking about a VST3 parameter
   *  does not require CLAP's headers, or the other way round. */
  uint32_t id = 0;
  std::string name;
  std::string module; // the group path the plugin declared, may be empty
  double minValue = 0.0;
  double maxValue = 1.0;
  double defaultValue = 0.0;
  bool stepped = false;
};

// ── A loaded plugin ──────────────────────────────────────────────────────────

/**
 * One instantiated plugin, driven the way a host drives one.
 *
 * The object owns the module it came from, so it stays valid for as long as it
 * is held and unloads cleanly when it is not. Copying is disabled for the same
 * reason: two owners of one plugin instance is a double destroy.
 *
 * The interface is what the two formats have in COMMON, which is nearly
 * everything a host actually does: prepare, process, read and write
 * parameters, save and restore state, ask how late the output is. Where they
 * genuinely differ -- CLAP delivers MIDI as events, VST3 splits it between
 * typed events and hidden parameters -- the difference is absorbed by the
 * implementation rather than pushed onto the caller.
 */
/** Something the plugin did to one of its own parameters, on its way to the
 *  host.
 *
 *  Automation is not a one-way street. A host that only ever WRITES parameters
 *  records nothing when the user turns a knob on the plugin's own face, and
 *  shows stale values in its generic editor after a preset loads. The gesture
 *  marks matter as much as the values: they are what lets a host fold a whole
 *  mouse drag into one undo step, and what tells it to stop playing automation
 *  back over a knob the user is currently holding. */
struct ParamEdit {
  enum class Kind { kValue, kGestureBegin, kGestureEnd };
  Kind kind = Kind::kValue;
  /** Index into the plugin's parameter list, not the format's own id. */
  int index = -1;
  /** Plain value, in the parameter's own units. Unset for gesture marks. */
  double value = 0.0;
};

class HostedPlugin {
public:
  HostedPlugin() = default;
  HostedPlugin(const HostedPlugin&) = delete;
  HostedPlugin& operator=(const HostedPlugin&) = delete;
  virtual ~HostedPlugin() = default;

  virtual const PluginDescription& description() const = 0;
  virtual bool isValid() const = 0;

  virtual bool prepare(double sampleRate, uint32_t maxBlockSize, uint32_t numChannels = 2) = 0;
  virtual void release() = 0;
  virtual void reset() = 0;
  virtual void process(AudioBlock<float>& io, const MidiBuffer* midi = nullptr) = 0;

  virtual uint32_t latencySamples() const = 0;
  virtual uint32_t tailSamples() const = 0;
  virtual bool acceptsMidi() const = 0;
  virtual bool hasAudioInput() const = 0;

  virtual int numParameters() const = 0;
  virtual const HostedParam& parameter(int index) const = 0;
  virtual double parameterValue(int index) const = 0;
  virtual bool setParameterValue(int index, double value) = 0;
  virtual std::string parameterText(int index, double value) const = 0;

  virtual bool saveState(std::vector<uint8_t>& out) const = 0;
  virtual bool loadState(const uint8_t* data, size_t size) = 0;

  /** What the plugin EMITTED during the last process() call.
   *
   *  An arpeggiator's product is not audio, it is notes, and a host that drops
   *  them has hosted the plugin without using it. Cleared and refilled every
   *  block, so a caller reads it straight after process() or not at all. */
  virtual const MidiBuffer& producedMidi() const = 0;

  /** Bypass the plugin, using whatever the format calls it.
   *
   *  Every format has one and no two spell it alike: CLAP flags a parameter
   *  CLAP_PARAM_IS_BYPASS, VST3 flags one kIsBypass, and LV2 designates a
   *  control port lv2:enabled whose sense is INVERTED -- enabled 0 is
   *  bypassed. An application hosting a rack needs to switch one off without
   *  knowing which of those it has.
   *
   *  Returns false when the plugin exposes none, which is legal: an
   *  instrument has no dry signal to pass through, so bypassing it means
   *  nothing. A caller that gets false should route around the plugin itself
   *  rather than assume it worked. */
  virtual bool setBypassed(bool bypassed) = 0;
  virtual bool isBypassed() const = 0;
  /** Does this plugin have a bypass of its own at all? */
  virtual bool hasBypass() const = 0;

  /** The plugin's own factory presets.
   *
   *  Three formats, three completely different mechanisms. CLAP publishes them
   *  through a preset-discovery FACTORY at the entry level, which a host has
   *  to crawl before it knows one exists. VST3 hangs a program list off a unit
   *  and selects with a parameter flagged kIsProgramChange. LV2 writes
   *  pset:Preset into the bundle's Turtle, so finding out what a plugin ships
   *  with means reading RDF.
   *
   *  A caller wanting to show a preset menu should not have to know which of
   *  those it is holding. Zero is a normal answer, not a failure. */
  virtual int numPresets() const = 0;
  virtual std::string presetName(int index) const = 0;
  virtual bool loadPreset(int index) = 0;

  /** Take everything the plugin has said about its own parameters since the
   *  last call, and forget it.
   *
   *  Pull rather than callback, like producedMidi(): a host that is mid-block
   *  has nowhere safe to run someone else's code, and every other
   *  plugin-to-host channel here already works this way.
   *
   *  Appends; it does not clear the vector it is given. Returns how many it
   *  added, so a caller that only wants to know WHETHER anything moved does
   *  not have to diff a parameter list to find out. */
  virtual size_t drainParameterEdits(std::vector<ParamEdit>& out) = 0;

  /** The plugin's own face, embedded in a window the CALLER owns.
   *
   *  Embedded only, deliberately. A floating window would mean this host owns
   *  a second lifetime -- created where, closed by whom, destroyed in what
   *  order relative to the plugin -- for no gain a caller cannot get by
   *  making a window and passing it in. Every host worth naming embeds.
   *
   *  `parent` is the native handle for the platform: an HWND on Windows, an
   *  NSView* on macOS, an X11 Window on Linux. The caller keeps owning it and
   *  must not destroy it before closeEditor().
   *
   *  Zero-sized or absent is a normal answer: a plugin is allowed to have no
   *  editor, and a host that assumed otherwise would be a host that cannot
   *  load half of what exists. */
  virtual bool hasEditor() const = 0;
  virtual bool openEditor(void* parent) = 0;
  virtual void closeEditor() = 0;
  /** Call regularly on the main thread while an editor is open.
   *
   *  Only LV2 needs it, and it needs it badly: the format has no message loop
   *  of its own, so the bundle asks for ui:idleInterface and the host is what
   *  drives it. The other two run on the platform's own loop and this costs
   *  them nothing, which is why it is one call a caller can make blindly
   *  rather than a question it has to ask first. */
  virtual void idleEditor() = 0;
  /** The size the plugin wants, in pixels, only meaningful while open. */
  virtual bool editorSize(uint32_t& width, uint32_t& height) const = 0;

  /** Tell the plugin how many device pixels a logical one is worth here.
   *
   *  On a 150% display a plugin that is never told draws at the size it
   *  thinks it needs and gets a window a third too small, or the host scales
   *  the result and hands back something soft. Both formats have a way to say
   *  it and they disagree about who wins: CLAP lets a plugin REFUSE, meaning
   *  it handles DPI itself, and VST3 makes it a separate interface a view may
   *  simply not implement.
   *
   *  False means the plugin declined or has no way to be told, which is a
   *  normal answer and not a failure -- the caller then sizes its window from
   *  editorSize() as before. */
  virtual bool setEditorScale(double scale) = 0;
};

/** The CLAP side of it. */
class ClapPlugin final : public HostedPlugin {
public:
  ~ClapPlugin() override { unload(); }

  const PluginDescription& description() const override { return desc_; }
  bool isValid() const override { return plugin_ != nullptr; }

  // ── Lifecycle ─────────────────────────────────────────────────────────────

  /** Allocate for this rate and block size. Allocation happens HERE, which is
   *  the whole reason process() can promise not to. */
  bool prepare(double sampleRate, uint32_t maxBlockSize, uint32_t numChannels = 2) override {
    if (!plugin_) return false;
    if (active_) release();

    sampleRate_ = sampleRate;
    maxBlock_ = maxBlockSize > 0 ? maxBlockSize : 1;
    channels_ = numChannels > 0 ? (numChannels > 8 ? 8 : numChannels) : 1;

    if (!plugin_->activate(plugin_, sampleRate_, 1, maxBlock_)) return false;
    active_ = true;

    inStorage_.assign((size_t) channels_ * maxBlock_, 0.0f);
    outStorage_.assign((size_t) channels_ * maxBlock_, 0.0f);
    inPtrs_.assign(channels_, nullptr);
    outPtrs_.assign(channels_, nullptr);
    for (uint32_t c = 0; c < channels_; ++c) {
      inPtrs_[c] = inStorage_.data() + (size_t) c * maxBlock_;
      outPtrs_[c] = outStorage_.data() + (size_t) c * maxBlock_;
    }
    eventStorage_.assign(kMaxEventBytes, 0);
    // Reserved for MIDI events AND queued parameter changes, because
    // buildEvents() packs both into the same list. Sizing this for notes alone
    // would have it grow the first time a block carried a full set of both --
    // an allocation on the audio thread, in the one function that promises not
    // to make any.
    const size_t maxEvents = (size_t) MidiBuffer::kMaxEvents + kMaxQueuedParams;
    eventOffsets_.reserve(maxEvents);
    eventPtrs_.reserve(maxEvents);
    queuedParams_.reserve(kMaxQueuedParams);

    inBus_ = {inPtrs_.data(), nullptr, channels_, 0, 0};
    outBus_ = {outPtrs_.data(), nullptr, channels_, 0, 0};

    plugin_->start_processing(plugin_);
    processing_ = true;
    return true;
  }

  /** Stop and free. Called by prepare() before re-preparing and by the
   *  destructor; calling it twice is harmless. */
  void release() override {
    if (!plugin_) return;
    if (processing_) {
      plugin_->stop_processing(plugin_);
      processing_ = false;
    }
    if (active_) {
      plugin_->deactivate(plugin_);
      active_ = false;
    }
  }

  /** Clear whatever the plugin is holding: tails, voices, filter state. */
  void reset() override {
    if (plugin_ && active_) plugin_->reset(plugin_);
  }

  // ── Audio ─────────────────────────────────────────────────────────────────

  /**
   * Run one block. `io` is read AND written: the input is copied in, the
   * output copied back, so a caller can chain plugins over one buffer.
   *
   * Allocates nothing. Every vector this touches was sized in prepare(), and
   * the queued parameter changes go into a buffer reserved there, which is
   * what makes it legal to call from an audio callback.
   */
  void process(AudioBlock<float>& io, const MidiBuffer* midi = nullptr) override {
    if (!plugin_ || !processing_) return;
    const uint32_t frames = (uint32_t) io.getNumSamples();
    if (frames == 0 || frames > maxBlock_) return;

    const uint32_t chans = (uint32_t) io.getNumChannels();
    for (uint32_t c = 0; c < channels_; ++c) {
      const float* src = c < chans ? io.getChannelPointer(c) : nullptr;
      float* dst = inPtrs_[c];
      if (src) std::memcpy(dst, src, frames * sizeof(float));
      else std::memset(dst, 0, frames * sizeof(float));
      std::memset(outPtrs_[c], 0, frames * sizeof(float));
    }

    buildEvents(midi);
    produced_.clear(); // this block's emissions only

    clap_process_t p{};
    p.steady_time = steadyTime_;
    p.frames_count = frames;
    p.transport = nullptr;
    p.audio_inputs = &inBus_;
    p.audio_outputs = &outBus_;
    p.audio_inputs_count = hasAudioInput_ ? 1u : 0u;
    p.audio_outputs_count = 1;
    p.in_events = &inEvents_;
    p.out_events = &outEvents_;
    plugin_->process(plugin_, &p);
    steadyTime_ += frames;

    for (uint32_t c = 0; c < chans; ++c) {
      float* dst = io.getChannelPointer(c);
      const float* src = outPtrs_[c < channels_ ? c : channels_ - 1];
      std::memcpy(dst, src, frames * sizeof(float));
    }
    queuedParams_.clear();
  }

  /** What the host must compensate for, in samples. */
  uint32_t latencySamples() const override {
    if (!plugin_) return 0;
    const auto* ext = static_cast<const clap_plugin_latency_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_LATENCY));
    return ext ? ext->get(plugin_) : 0;
  }

  /** How long it keeps sounding after its input stops. */
  uint32_t tailSamples() const override {
    if (!plugin_) return 0;
    const auto* ext =
        static_cast<const clap_plugin_tail_t*>(plugin_->get_extension(plugin_, CLAP_EXT_TAIL));
    return ext ? ext->get(plugin_) : 0;
  }

  bool acceptsMidi() const override { return acceptsMidi_; }
  bool hasAudioInput() const override { return hasAudioInput_; }

  // ── Parameters ────────────────────────────────────────────────────────────

  int numParameters() const override { return (int) params_.size(); }
  const HostedParam& parameter(int index) const override { return params_[(size_t) index]; }

  double parameterValue(int index) const override {
    if (!paramsExt_ || index < 0 || index >= numParameters()) return 0.0;
    double v = 0.0;
    paramsExt_->get_value(plugin_, (clap_id) params_[(size_t) index].id, &v);
    return v;
  }

  /** Queue a change for the NEXT process() call.
   *
   *  Deliberately queued rather than applied: a parameter written straight
   *  into a plugin while it is processing is a race, and the whole reason
   *  CLAP delivers parameters as events is to avoid one. The queue is
   *  pre-reserved, so this is safe to call from the audio thread too. */
  bool setParameterValue(int index, double value) override {
    if (index < 0 || index >= numParameters()) return false;
    // Refused rather than grown: push_back past the reservation would be an
    // allocation, and this is documented as safe to call from the audio
    // thread. A host that queues more than 64 changes in one block is
    // automating faster than any block can express anyway.
    if (queuedParams_.size() >= kMaxQueuedParams) return false;
    queuedParams_.push_back({(clap_id) params_[(size_t) index].id, value});
    return true;
  }

  /** How the plugin itself would print a value ("-6.0 dB", "2.30 kHz"). */
  std::string parameterText(int index, double value) const override {
    if (!paramsExt_ || index < 0 || index >= numParameters()) return std::string();
    char buffer[128] = {};
    if (!paramsExt_->value_to_text(plugin_, (clap_id) params_[(size_t) index].id, value, buffer,
                                   sizeof(buffer)))
      return std::string();
    return std::string(buffer);
  }

  // ── State ─────────────────────────────────────────────────────────────────

  bool saveState(std::vector<uint8_t>& out) const override {
    const auto* ext =
        static_cast<const clap_plugin_state_t*>(plugin_->get_extension(plugin_, CLAP_EXT_STATE));
    if (!ext) return false;
    out.clear();
    clap_ostream_t stream{};
    stream.ctx = &out;
    stream.write = [](const clap_ostream_t* s, const void* buffer, uint64_t size) -> int64_t {
      auto* dest = static_cast<std::vector<uint8_t>*>(s->ctx);
      const auto* bytes = static_cast<const uint8_t*>(buffer);
      dest->insert(dest->end(), bytes, bytes + size);
      return (int64_t) size;
    };
    return ext->save(plugin_, &stream);
  }

  const MidiBuffer& producedMidi() const override { return produced_; }

  /** Which windowing API this platform speaks. Keyed on the PLATFORM rather
   *  than on what happens to be compiled in, because the question is what
   *  kind of window the caller would be handing over. */
  static const char* nativeWindowApi() {
#if defined(_WIN32)
    return CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
    return CLAP_WINDOW_API_COCOA;
#else
    return CLAP_WINDOW_API_X11;
#endif
  }

  bool hasEditor() const override {
    const auto* gui =
        static_cast<const clap_plugin_gui_t*>(plugin_->get_extension(plugin_, CLAP_EXT_GUI));
    // Having the extension is not the same as being able to embed HERE: a
    // plugin that only does Cocoa is faceless on Windows, and saying it has
    // an editor would put an empty window in front of a user.
    return gui && gui->is_api_supported(plugin_, nativeWindowApi(), false);
  }

  bool openEditor(void* parent) override {
    if (editorOpen_ || !parent) return false;
    guiExt_ = static_cast<const clap_plugin_gui_t*>(plugin_->get_extension(plugin_, CLAP_EXT_GUI));
    if (!guiExt_ || !guiExt_->is_api_supported(plugin_, nativeWindowApi(), false)) return false;
    if (!guiExt_->create(plugin_, nativeWindowApi(), false)) return false;
    // Before set_parent, because the scale decides how big the window the
    // plugin makes is, and correcting it afterwards is a visible jump.
    if (editorScale_ != 1.0 && guiExt_->set_scale) guiExt_->set_scale(plugin_, editorScale_);

    clap_window_t window{};
    window.api = nativeWindowApi();
#if defined(_WIN32)
    window.win32 = parent;
#elif defined(__APPLE__)
    window.cocoa = parent;
#else
    window.x11 = (unsigned long) (uintptr_t) parent;
#endif
    // set_parent then show, in that order. Showing an unparented editor is a
    // stray top-level window on the user's desktop, which is the failure that
    // looks like the plugin "opened somewhere else".
    if (!guiExt_->set_parent(plugin_, &window)) {
      guiExt_->destroy(plugin_);
      guiExt_ = nullptr;
      return false;
    }
    guiExt_->show(plugin_);
    editorOpen_ = true;
    return true;
  }

  void closeEditor() override {
    if (!editorOpen_ || !guiExt_) return;
    guiExt_->hide(plugin_);
    guiExt_->destroy(plugin_);
    guiExt_ = nullptr;
    editorOpen_ = false;
  }

  /** Nothing to do: a CLAP editor lives on the platform's own message loop,
   *  which the caller is already pumping to have a window at all. */
  void idleEditor() override {}

  bool setEditorScale(double scale) override {
    if (!(scale > 0.0)) return false;
    editorScale_ = scale;
    // Fetched on demand, not taken from the member: a host knows the scale
    // before it opens an editor, and guiExt_ is only filled in by
    // openEditor(). Requiring an open editor first made this return false for
    // exactly the case it exists to serve.
    const auto* gui = guiExt_ ? guiExt_
                              : static_cast<const clap_plugin_gui_t*>(
                                    plugin_->get_extension(plugin_, CLAP_EXT_GUI));
    if (!gui || !gui->set_scale) return false;
    // CLAP treats false here as "I deal with DPI myself", which is a real
    // answer and not an error -- passed straight through rather than
    // flattened into a success.
    return gui->set_scale(plugin_, scale);
  }

  bool editorSize(uint32_t& width, uint32_t& height) const override {
    if (!editorOpen_ || !guiExt_) return false;
    return guiExt_->get_size(plugin_, &width, &height);
  }

  size_t drainParameterEdits(std::vector<ParamEdit>& out) override {
    // Two things can be owed at this point, and both are settled before the
    // vector is handed over -- otherwise a caller that loads a preset and
    // drains immediately, without processing a block first, learns nothing.
    if (flushRequested_ && paramsExt_ && paramsExt_->flush) {
      flushRequested_ = false;
      buildEvents(nullptr); // the host's own pending writes ride the same flush
      paramsExt_->flush(plugin_, &inEvents_, &outEvents_);
      queuedParams_.clear();
    }
    if (valuesStale_ && paramsExt_) {
      valuesStale_ = false;
      // A rescan says WHICH values are stale only in the coarsest sense, so
      // every one is re-read. Reporting a value that did not move is harmless;
      // missing one that did is a lane of automation that never gets written.
      for (size_t i = 0; i < params_.size() && edits_.size() < kMaxEdits; ++i) {
        double value = 0.0;
        if (!paramsExt_->get_value(plugin_, (clap_id) params_[i].id, &value)) continue;
        ParamEdit edit;
        edit.kind = ParamEdit::Kind::kValue;
        edit.index = (int) i;
        edit.value = value;
        edits_.push_back(edit);
      }
    }
    const size_t added = edits_.size();
    out.insert(out.end(), edits_.begin(), edits_.end());
    edits_.clear();
    return added;
  }

  int numPresets() const override { return (int) presets_.size(); }
  std::string presetName(int index) const override {
    if (index < 0 || index >= numPresets()) return std::string();
    return presets_[(size_t) index].name;
  }
  bool loadPreset(int index) override {
    if (index < 0 || index >= numPresets()) return false;
    const auto* ext = static_cast<const clap_plugin_preset_load_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_PRESET_LOAD));
    if (!ext) return false;
    const Preset& p = presets_[(size_t) index];
    // The load key is opaque: it means something to the plugin and nothing to
    // a host, which is exactly why it goes back untouched.
    return ext->from_location(plugin_, p.locationKind,
                              p.location.empty() ? nullptr : p.location.c_str(),
                              p.loadKey.c_str());
  }

  bool hasBypass() const override { return bypassParam_ >= 0; }
  bool isBypassed() const override {
    return bypassParam_ >= 0 && parameterValue(bypassParam_) >= 0.5;
  }
  bool setBypassed(bool bypassed) override {
    if (bypassParam_ < 0) return false;
    // Queued like any other parameter: writing into a plugin while it is
    // processing is the race the event list exists to avoid, and a bypass
    // pressed mid-block is no different from a knob turned mid-block.
    return setParameterValue(bypassParam_, bypassed ? 1.0 : 0.0);
  }

  bool loadState(const uint8_t* data, size_t size) override {
    const auto* ext =
        static_cast<const clap_plugin_state_t*>(plugin_->get_extension(plugin_, CLAP_EXT_STATE));
    if (!ext || (!data && size > 0)) return false;
    struct Reader {
      const uint8_t* data;
      size_t size, pos;
    } reader{data, size, 0};
    clap_istream_t stream{};
    stream.ctx = &reader;
    stream.read = [](const clap_istream_t* s, void* buffer, uint64_t size) -> int64_t {
      auto* r = static_cast<Reader*>(s->ctx);
      const size_t left = r->size - r->pos;
      const size_t take = (size_t) size < left ? (size_t) size : left;
      if (take > 0) std::memcpy(buffer, r->data + r->pos, take);
      r->pos += take;
      return (int64_t) take;
    };
    return ext->load(plugin_, &stream);
  }

private:
  friend std::unique_ptr<HostedPlugin> loadPlugin(const PluginDescription&);

  ClapPlugin() = default;

  static constexpr size_t kMaxQueuedParams = 64;
  static constexpr size_t kMaxEventBytes = 8192;

  void unload() {
    release();
    // Before destroy(), not after: a GUI outliving the plugin that owns it is
    // a window drawing from freed memory.
    closeEditor();
    if (plugin_) {
      plugin_->destroy(plugin_);
      plugin_ = nullptr;
    }
    if (entry_) {
      entry_->deinit();
      entry_ = nullptr;
    }
    closeLib(lib_);
    lib_ = nullptr;
  }

  /** Pack this block's MIDI and queued parameter changes into one CLAP event
   *  list. Writes into buffers reserved by prepare(); nothing here grows. */
  void buildEvents(const MidiBuffer* midi) {
    eventOffsets_.clear();
    eventPtrs_.clear();
    size_t used = 0;

    auto append = [&](const void* data, size_t bytes) {
      if (used + bytes > eventStorage_.size()) return;
      std::memcpy(eventStorage_.data() + used, data, bytes);
      eventOffsets_.push_back(used);
      used += bytes;
    };

    for (const auto& q : queuedParams_) {
      clap_event_param_value_t e{};
      e.header.size = sizeof(e);
      e.header.type = CLAP_EVENT_PARAM_VALUE;
      e.header.time = 0;
      e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      e.param_id = q.id;
      e.port_index = -1;
      e.key = -1;
      e.channel = -1;
      e.note_id = -1;
      e.value = q.value;
      append(&e, sizeof(e));
    }

    if (midi && acceptsMidi_) {
      for (const auto& entry : *midi) {
        const MidiMessage& m = entry.getMessage();
        clap_event_midi_t e{};
        e.header.size = sizeof(e);
        e.header.type = CLAP_EVENT_MIDI;
        e.header.time = (uint32_t) (entry.samplePosition < 0 ? 0 : entry.samplePosition);
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.port_index = 0;
        e.data[0] = (uint8_t) m.getRawStatus();
        e.data[1] = (uint8_t) m.getRawData1();
        e.data[2] = (uint8_t) m.getRawData2();
        append(&e, sizeof(e));
      }
    }

    for (size_t off : eventOffsets_)
      eventPtrs_.push_back(reinterpret_cast<const clap_event_header_t*>(eventStorage_.data() + off));
  }

  struct QueuedParam {
    clap_id id;
    double value;
  };

  /** A parameter's id is the PLUGIN's numbering and need not be its position.
   *  Everything a caller sees here is indexed by position, so the two have to
   *  be kept apart -- reporting an id as an index would name the wrong knob on
   *  any plugin whose ids are not 0..n-1. */
  int indexOfParamId(clap_id id) const {
    for (size_t i = 0; i < params_.size(); ++i)
      if ((clap_id) params_[i].id == id) return (int) i;
    return -1;
  }

  /** Bounded because it is filled from the audio thread by a plugin that can
   *  emit as many events as it likes. A host that grew this without limit
   *  would allocate in the middle of a block on the plugin's say-so. */
  static constexpr size_t kMaxEdits = 4096;

  std::vector<ParamEdit> edits_;
  bool valuesStale_ = false;
  bool flushRequested_ = false;
  const clap_plugin_gui_t* guiExt_ = nullptr;
  double editorScale_ = 1.0;
  bool editorOpen_ = false;

  LibHandle lib_ = nullptr;
  const clap_plugin_entry_t* entry_ = nullptr;
  const clap_plugin_t* plugin_ = nullptr;
  const clap_plugin_params_t* paramsExt_ = nullptr;
  clap_host_t hostFacade_{};
  PluginDescription desc_;
  std::vector<HostedParam> params_;

  double sampleRate_ = 48000.0;
  uint32_t maxBlock_ = 0;
  uint32_t channels_ = 2;
  bool active_ = false, processing_ = false;
  bool acceptsMidi_ = false, hasAudioInput_ = true;
  int bypassParam_ = -1;
  int64_t steadyTime_ = 0;

  std::vector<float> inStorage_, outStorage_;
  std::vector<float*> inPtrs_, outPtrs_;
  clap_audio_buffer_t inBus_{}, outBus_{};

  std::vector<uint8_t> eventStorage_;
  std::vector<size_t> eventOffsets_;
  std::vector<const clap_event_header_t*> eventPtrs_;
  /** One preset, as the discovery crawl reported it. */
  struct Preset {
    std::string name;
    std::string location;
    std::string loadKey;
    uint32_t locationKind = 0;
  };
  std::vector<Preset> presets_;

  std::vector<QueuedParam> queuedParams_;
  MidiBuffer produced_;

  clap_input_events_t inEvents_{};
  clap_output_events_t outEvents_{};

  // ── The callbacks CLAP reaches back through ───────────────────────────────
  static uint32_t eventsSize(const clap_input_events_t* list) {
    return (uint32_t) static_cast<const ClapPlugin*>(list->ctx)->eventPtrs_.size();
  }
  static const clap_event_header_t* eventsGet(const clap_input_events_t* list, uint32_t index) {
    const auto* self = static_cast<const ClapPlugin*>(list->ctx);
    return index < self->eventPtrs_.size() ? self->eventPtrs_[index] : nullptr;
  }
  /** What the plugin emits, kept.
   *
   *  This used to return false and drop everything, which was the honest
   *  answer while there was nowhere to put it -- telling an arpeggiator its
   *  notes were taken when they were thrown away would have been worse. Now
   *  there is somewhere: PluginGraph routes them to another node.
   *
   *  Only 3-byte channel messages. Anything else is a dialect this host has no
   *  MidiBuffer shape for, and refusing it is still better than truncating it
   *  into something that means a different thing. */
  static bool eventsPush(const clap_output_events_t* list, const clap_event_header_t* header) {
    if (!header || header->space_id != CLAP_CORE_EVENT_SPACE_ID) return false;
    auto* self = static_cast<ClapPlugin*>(list->ctx);

    // What the plugin did to its OWN parameters -- a knob turned on its face,
    // or every knob moved at once by a preset. A host that ignores these
    // records no automation from the plugin's editor and shows stale numbers
    // afterwards, which is the failure a user reads as "the preset did not
    // load" when it loaded perfectly well.
    if (header->type == CLAP_EVENT_PARAM_VALUE) {
      const auto* e = reinterpret_cast<const clap_event_param_value_t*>(header);
      if (self->edits_.size() >= kMaxEdits) return false;
      ParamEdit edit;
      edit.kind = ParamEdit::Kind::kValue;
      edit.index = self->indexOfParamId(e->param_id);
      edit.value = e->value;
      if (edit.index < 0) return false; // a parameter this host never saw
      self->edits_.push_back(edit);
      return true;
    }
    if (header->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ||
        header->type == CLAP_EVENT_PARAM_GESTURE_END) {
      const auto* e = reinterpret_cast<const clap_event_param_gesture_t*>(header);
      if (self->edits_.size() >= kMaxEdits) return false;
      ParamEdit edit;
      edit.kind = header->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? ParamEdit::Kind::kGestureBegin
                                                                 : ParamEdit::Kind::kGestureEnd;
      edit.index = self->indexOfParamId(e->param_id);
      if (edit.index < 0) return false;
      self->edits_.push_back(edit);
      return true;
    }

    if (header->type == CLAP_EVENT_MIDI) {
      const auto* e = reinterpret_cast<const clap_event_midi_t*>(header);
      const int status = e->data[0];
      if (status < 0x80 || status >= 0xf0) return false;
      self->produced_.addEvent(MidiMessage(status, e->data[1], e->data[2]), (int) header->time);
      return true;
    }
    // CLAP hosts may also receive typed notes. Translated rather than dropped,
    // because which spelling a plugin chooses is its business and a graph
    // downstream should see the same notes either way.
    if (header->type == CLAP_EVENT_NOTE_ON || header->type == CLAP_EVENT_NOTE_OFF) {
      const auto* e = reinterpret_cast<const clap_event_note_t*>(header);
      const int channel = e->channel < 0 ? 0 : e->channel;
      if (header->type == CLAP_EVENT_NOTE_ON) {
        int velocity = (int) (e->velocity * 127.0 + 0.5);
        if (velocity < 1) velocity = 1;
        self->produced_.addEvent(MidiMessage::noteOn(channel, e->key, velocity),
                                 (int) header->time);
      } else {
        self->produced_.addEvent(MidiMessage::noteOff(channel, e->key), (int) header->time);
      }
      return true;
    }
    return false;
  }

  /** [main-thread] The plugin says its values are stale.
   *
   *  Recorded rather than acted on. Answering here would mean calling back
   *  into a plugin that is still inside its own state.load() or preset load,
   *  and re-entering a plugin during its own callback is a good way to find
   *  out which plugins never expected it. The re-read happens on the next
   *  drain, which is a plain main-thread call with nothing on the stack. */
  static void hostParamsRescan(const clap_host_t* host, clap_param_rescan_flags flags) {
    auto* self = static_cast<ClapPlugin*>(host->host_data);
    if (!self) return;
    if (flags & (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_ALL)) self->valuesStale_ = true;
  }
  static void hostParamsClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
  /** [thread-safe] The plugin has parameter events waiting and is not being
   *  processed. Honoured on the next drain by calling params.flush(), which is
   *  the only way an edit made while the transport is stopped ever arrives. */
  static void hostParamsRequestFlush(const clap_host_t* host) {
    auto* self = static_cast<ClapPlugin*>(host->host_data);
    if (self) self->flushRequested_ = true;
  }

  /** [main-thread] The plugin wants the window a different size.
   *
   *  Declined, and honestly. The window belongs to the CALLER: this host was
   *  handed a handle, not ownership, and resizing someone else's window from
   *  under them is not a favour. A caller who wants to follow the plugin asks
   *  editorSize() and resizes its own window, which it is allowed to do and
   *  this host is not. Returning true and doing nothing would leave the
   *  plugin drawing at a size the window is not. */
  static bool hostGuiRequestResize(const clap_host_t*, uint32_t, uint32_t) { return false; }
  static bool hostGuiRequestShow(const clap_host_t*) { return false; }
  static bool hostGuiRequestHide(const clap_host_t*) { return false; }
  static void hostGuiResizeHintsChanged(const clap_host_t*) {}
  static void hostGuiClosed(const clap_host_t* host, bool) {
    // The plugin closed its own editor. Whatever the caller does about it,
    // this side must stop believing one is open or the next closeEditor()
    // destroys a GUI that is already gone.
    auto* self = static_cast<ClapPlugin*>(host->host_data);
    if (self) self->editorOpen_ = false;
  }

  static const void* hostGetExtension(const clap_host_t*, const char* id) {
    if (id && std::strcmp(id, CLAP_EXT_GUI) == 0) {
      static const clap_host_gui_t ext = {hostGuiResizeHintsChanged, hostGuiRequestResize,
                                          hostGuiRequestShow, hostGuiRequestHide, hostGuiClosed};
      return &ext;
    }
    if (id && std::strcmp(id, CLAP_EXT_PARAMS) == 0) {
      static const clap_host_params_t ext = {hostParamsRescan, hostParamsClear,
                                             hostParamsRequestFlush};
      return &ext;
    }
    return nullptr;
  }
  static void hostRequestRestart(const clap_host_t*) {}
  static void hostRequestProcess(const clap_host_t*) {}
  static void hostRequestCallback(const clap_host_t*) {}

  void wireCallbacks() {
    inEvents_.ctx = this;
    inEvents_.size = eventsSize;
    inEvents_.get = eventsGet;
    outEvents_.ctx = this;
    outEvents_.try_push = eventsPush;

    hostFacade_.clap_version = CLAP_VERSION;
    hostFacade_.host_data = this;
    hostFacade_.name = "Sonore Host";
    hostFacade_.vendor = "Sonorie";
    hostFacade_.url = "";
    hostFacade_.version = "1.0";
    hostFacade_.get_extension = hostGetExtension;
    hostFacade_.request_restart = hostRequestRestart;
    hostFacade_.request_process = hostRequestProcess;
    hostFacade_.request_callback = hostRequestCallback;
  }

  /** Walk the preset-discovery factory and record what it declares.
   *
   *  This is the most indirect of the three formats by a distance. The factory
   *  lives at the ENTRY, not on the plugin, so a host asks the module what
   *  preset PROVIDERS it has, creates each one with an indexer of its own,
   *  lets the provider declare LOCATIONS through that indexer, and only then
   *  asks each location for its metadata -- which arrives through a third
   *  callback object. Four objects to learn a plugin ships three presets.
   *
   *  Done once at load, on the main thread, because every step of it can
   *  allocate and read files. */
  void crawlPresets(const char* pluginId) {
    if (!entry_ || !pluginId) return;
    const auto* factory = static_cast<const clap_preset_discovery_factory_t*>(
        entry_->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    if (!factory) return;

    struct Crawl {
      std::vector<Preset>* into = nullptr;
      const char* wantPluginId = nullptr;
      std::vector<clap_preset_discovery_location_t> locations;
      std::vector<std::string> locationStrings;
      std::vector<std::string> locationNames;
      // The preset being described right now. begin_preset names it and the
      // calls that follow describe it, so this holds the one in progress.
      std::string pendingName, pendingKey;
      bool pendingWanted = false;
      uint32_t currentKind = 0;
      std::string currentLocation;
    } crawl;
    crawl.into = &presets_;
    crawl.wantPluginId = pluginId;

    clap_preset_discovery_indexer_t indexer{};
    indexer.clap_version = CLAP_VERSION;
    indexer.name = "Sonore Host";
    indexer.vendor = "Sonorie";
    indexer.url = "";
    indexer.version = "1.0";
    indexer.indexer_data = &crawl;
    indexer.declare_filetype = [](const clap_preset_discovery_indexer_t*,
                                  const clap_preset_discovery_filetype_t*) { return true; };
    indexer.declare_soundpack = [](const clap_preset_discovery_indexer_t*,
                                   const clap_preset_discovery_soundpack_t*) { return true; };
    indexer.get_extension = [](const clap_preset_discovery_indexer_t*, const char*) -> const void* {
      return nullptr;
    };
    indexer.declare_location = [](const clap_preset_discovery_indexer_t* self,
                                  const clap_preset_discovery_location_t* location) {
      auto* c = static_cast<Crawl*>(self->indexer_data);
      if (!c || !location) return false;
      // The strings are the PROVIDER's and are only guaranteed for the length
      // of this call, so they are copied before anything else happens.
      c->locationStrings.push_back(location->location ? location->location : "");
      c->locationNames.push_back(location->name ? location->name : "");
      clap_preset_discovery_location_t copy = *location;
      copy.location = nullptr; // resolved from the copies below, by index
      copy.name = nullptr;
      c->locations.push_back(copy);
      return true;
    };

    clap_preset_discovery_metadata_receiver_t receiver{};
    receiver.receiver_data = &crawl;
    receiver.on_error = [](const clap_preset_discovery_metadata_receiver_t*, int32_t,
                           const char*) {};
    receiver.begin_preset = [](const clap_preset_discovery_metadata_receiver_t* self,
                               const char* name, const char* loadKey) {
      auto* c = static_cast<Crawl*>(self->receiver_data);
      if (!c) return false;
      // Whatever the previous preset was, it is finished: commit it before
      // starting another, or the last one described is the only one kept.
      if (c->pendingWanted && !c->pendingKey.empty())
        c->into->push_back({c->pendingName, c->currentLocation, c->pendingKey, c->currentKind});
      c->pendingName = name ? name : "";
      c->pendingKey = loadKey ? loadKey : "";
      // A preset with no plugin id declared belongs to whatever is crawling
      // it; add_plugin_id narrows that if the provider bothers to say.
      c->pendingWanted = true;
      return true;
    };
    receiver.add_plugin_id = [](const clap_preset_discovery_metadata_receiver_t* self,
                                const clap_universal_plugin_id_t* id) {
      auto* c = static_cast<Crawl*>(self->receiver_data);
      if (!c || !id || !id->id) return;
      // A provider may list presets for several plugins in one location. Only
      // the ones naming THIS plugin are ours; taking the rest would put
      // another product's presets in this one's menu.
      if (std::strcmp(id->id, c->wantPluginId) != 0) c->pendingWanted = false;
    };
    receiver.set_soundpack_id = [](const clap_preset_discovery_metadata_receiver_t*,
                                   const char*) {};
    receiver.set_flags = [](const clap_preset_discovery_metadata_receiver_t*, uint32_t) {};
    receiver.add_creator = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
    receiver.set_description = [](const clap_preset_discovery_metadata_receiver_t*,
                                  const char*) {};
    receiver.set_timestamps = [](const clap_preset_discovery_metadata_receiver_t*, clap_timestamp,
                                 clap_timestamp) {};
    receiver.add_feature = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
    receiver.add_extra_info = [](const clap_preset_discovery_metadata_receiver_t*, const char*,
                                 const char*) {};

    const uint32_t providers = factory->count(factory);
    for (uint32_t i = 0; i < providers; ++i) {
      const clap_preset_discovery_provider_descriptor_t* pd =
          factory->get_descriptor(factory, i);
      if (!pd || !pd->id) continue;
      const clap_preset_discovery_provider_t* provider =
          factory->create(factory, &indexer, pd->id);
      if (!provider) continue;
      if (provider->init(provider)) {
        for (size_t k = 0; k < crawl.locations.size(); ++k) {
          crawl.currentKind = crawl.locations[k].kind;
          crawl.currentLocation = crawl.locationStrings[k];
          crawl.pendingWanted = false;
          crawl.pendingKey.clear();
          provider->get_metadata(provider, crawl.currentKind,
                                 crawl.currentLocation.empty() ? nullptr
                                                               : crawl.currentLocation.c_str(),
                                 &receiver);
          // The last preset described has no begin_preset after it to commit
          // it, so it is committed here.
          if (crawl.pendingWanted && !crawl.pendingKey.empty())
            presets_.push_back({crawl.pendingName, crawl.currentLocation, crawl.pendingKey,
                                crawl.currentKind});
        }
      }
      provider->destroy(provider);
      crawl.locations.clear();
      crawl.locationStrings.clear();
      crawl.locationNames.clear();
    }
  }

  void readCapabilities() {
    paramsExt_ = static_cast<const clap_plugin_params_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_PARAMS));
    if (paramsExt_) {
      const uint32_t n = paramsExt_->count(plugin_);
      params_.reserve(n);
      for (uint32_t i = 0; i < n; ++i) {
        clap_param_info_t info{};
        if (!paramsExt_->get_info(plugin_, i, &info)) continue;
        HostedParam p;
        p.id = (uint32_t) info.id;
        p.name = info.name;
        p.module = info.module;
        p.minValue = info.min_value;
        p.maxValue = info.max_value;
        p.defaultValue = info.default_value;
        p.stepped = (info.flags & CLAP_PARAM_IS_STEPPED) != 0;
        // Found by its FLAG. Which index carries the bypass is the plugin's
        // business, and looking for a parameter NAMED "Bypass" would find a
        // knob that merely happens to be called that.
        if (info.flags & CLAP_PARAM_IS_BYPASS) bypassParam_ = (int) params_.size();
        params_.push_back(p);
      }
    }
    const auto* notePorts = static_cast<const clap_plugin_note_ports_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_NOTE_PORTS));
    acceptsMidi_ = notePorts && notePorts->count(plugin_, true) > 0;
    const auto* audioPorts = static_cast<const clap_plugin_audio_ports_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_AUDIO_PORTS));
    hasAudioInput_ = !audioPorts || audioPorts->count(plugin_, true) > 0;
  }
};

#if defined(SONORE_HOST_VST3)

// ── The VST3 side ────────────────────────────────────────────────────────────
//
// A different shape from CLAP in every respect that matters to a host.
//
//   * The module is usually a BUNDLE -- a directory whose real binary lives at
//     Contents/<arch>/<name> -- rather than the file the user points at. A
//     scanner that only opens files finds almost nothing in a real VST3 folder.
//   * Objects are COM-style: created through a factory by class id, reference
//     counted, and queried for interfaces rather than having extensions.
//   * The processor and the editor are two objects that may or may not be the
//     same one, and a host has to handle both cases.
//   * MIDI does not arrive as MIDI. Notes are typed events; control change and
//     pitch bend are parameter changes on ids published through IMidiMapping,
//     which this host looks up and uses -- the same route our own wrapper
//     publishes on the other side.

/** Where the real binary lives inside a .vst3, which may be a plain file. */
inline std::string vst3BinaryPath(const std::string& path) {
#if defined(_WIN32)
  const char* arch = "x86_64-win";
  const char* leafSuffix = ".vst3";
#elif defined(__APPLE__)
  const char* arch = "MacOS";
  const char* leafSuffix = "";
#else
  const char* arch = "x86_64-linux";
  const char* leafSuffix = ".so";
#endif
  // A bare file is already the binary; this is what our own build produces and
  // what many Linux plugins ship.
  bool isDirectory = false;
#if defined(_WIN32)
  const DWORD attrs = GetFileAttributesA(path.c_str());
  isDirectory = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat st {};
  isDirectory = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
  if (!isDirectory) return path;

  // Otherwise it is a bundle, and the leaf name is the folder's own name minus
  // the extension.
  size_t slash = path.find_last_of("/\\");
  std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
  if (leaf.size() > 5 && leaf.compare(leaf.size() - 5, 5, ".vst3") == 0)
    leaf = leaf.substr(0, leaf.size() - 5);
#if defined(_WIN32)
  return path + "\\Contents\\" + arch + "\\" + leaf + leafSuffix;
#else
  return path + "/Contents/" + arch + "/" + leaf + leafSuffix;
#endif
}

inline bool sameTuid(const Steinberg_TUID a, const Steinberg_TUID b) {
  return std::memcmp(a, b, sizeof(Steinberg_TUID)) == 0;
}

/** Call the module's init symbol, which is named differently on every
 *  platform and which a plugin is entitled to refuse. */
inline bool vst3ModuleInit(LibHandle lib) {
#if defined(_WIN32)
  auto init = (bool (*)()) libSymbol(lib, "InitDll");
  return !init || init(); // absent is legal; failing is not
#elif defined(__APPLE__)
  auto init = (bool (*)(void*)) libSymbol(lib, "bundleEntry");
  return !init || init(nullptr);
#else
  auto init = (bool (*)(void*)) libSymbol(lib, "ModuleEntry");
  return !init || init(nullptr);
#endif
}

/** Open a .vst3 and read its factory's class list without instantiating
 *  anything. Shared by the scanner and the loader so the two cannot drift. */
template <typename OnClass>
inline void withVst3Factory(const std::string& path, OnClass&& onClass) {
  LibHandle lib = openLib(vst3BinaryPath(path).c_str());
  if (!lib) return;
  if (vst3ModuleInit(lib)) {
    auto getFactory = (Steinberg_IPluginFactory * (*) ()) libSymbol(lib, "GetPluginFactory");
    if (getFactory) {
      if (Steinberg_IPluginFactory* factory = getFactory()) {
        const Steinberg_int32 count = factory->lpVtbl->countClasses(factory);
        for (Steinberg_int32 i = 0; i < count; ++i) {
          Steinberg_PClassInfo info{};
          if (factory->lpVtbl->getClassInfo(factory, i, &info) != Steinberg_kResultOk) continue;
          // Only audio processors. A module also publishes its CONTROLLER as a
          // class, and a scanner that listed those would show every plugin
          // twice -- once as itself and once as its own user interface.
          if (std::strcmp(info.category, "Audio Module Class") != 0) continue;
          onClass(factory, info);
        }
        factory->lpVtbl->release(factory);
      }
    }
  }
  if (auto exitFn = (bool (*)()) libSymbol(lib, "ExitDll")) exitFn();
  closeLib(lib);
}

// ── The COM objects a host has to provide ────────────────────────────────────
//
// VST3 does not take a callback; it takes objects. Parameter automation
// arrives as an IParameterChanges holding one IParamValueQueue per parameter,
// and events as an IEventList. These are the smallest honest implementations:
// stack-lifetime, no reference counting to speak of, and no allocation once
// the vectors behind them are reserved.

struct ParamQueue {
  Steinberg_Vst_IParamValueQueueVtbl* lpVtbl = nullptr;
  Steinberg_Vst_ParamID id = 0;
  double value = 0.0;
};

inline Steinberg_tresult SMTG_STDMETHODCALLTYPE hostQueryNoInterface(void*, const Steinberg_TUID,
                                                                     void** obj) {
  if (obj) *obj = nullptr;
  return Steinberg_kNoInterface;
}
inline Steinberg_uint32 SMTG_STDMETHODCALLTYPE hostAddRefStatic(void*) { return 1; }
inline Steinberg_uint32 SMTG_STDMETHODCALLTYPE hostReleaseStatic(void*) { return 1; }

inline Steinberg_Vst_ParamID SMTG_STDMETHODCALLTYPE pqGetId(void* self) {
  return static_cast<ParamQueue*>(self)->id;
}
inline Steinberg_int32 SMTG_STDMETHODCALLTYPE pqGetPointCount(void*) { return 1; }
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE pqGetPoint(void* self, Steinberg_int32 index,
                                                           Steinberg_int32* offset,
                                                           Steinberg_Vst_ParamValue* value) {
  if (index != 0 || !offset || !value) return Steinberg_kResultFalse;
  *offset = 0; // block-granular, like every other automation path in this SDK
  *value = static_cast<ParamQueue*>(self)->value;
  return Steinberg_kResultOk;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE pqAddPoint(void*, Steinberg_int32,
                                                           Steinberg_Vst_ParamValue,
                                                           Steinberg_int32*) {
  return Steinberg_kNotImplemented; // a plugin does not write into the host's queue
}

inline Steinberg_Vst_IParamValueQueueVtbl* paramQueueVtbl() {
  static Steinberg_Vst_IParamValueQueueVtbl v = {
      hostQueryNoInterface, hostAddRefStatic, hostReleaseStatic,
      pqGetId,              pqGetPointCount,  pqGetPoint,
      pqAddPoint,
  };
  return &v;
}

struct ParameterChanges {
  Steinberg_Vst_IParameterChangesVtbl* lpVtbl = nullptr;
  std::vector<ParamQueue>* queues = nullptr;
};

inline Steinberg_int32 SMTG_STDMETHODCALLTYPE pcCount(void* self) {
  auto* pc = static_cast<ParameterChanges*>(self);
  return pc->queues ? (Steinberg_int32) pc->queues->size() : 0;
}
inline Steinberg_Vst_IParamValueQueue* SMTG_STDMETHODCALLTYPE pcGetData(void* self,
                                                                        Steinberg_int32 index) {
  auto* pc = static_cast<ParameterChanges*>(self);
  if (!pc->queues || index < 0 || index >= (Steinberg_int32) pc->queues->size()) return nullptr;
  return (Steinberg_Vst_IParamValueQueue*) &(*pc->queues)[(size_t) index];
}
inline Steinberg_Vst_IParamValueQueue* SMTG_STDMETHODCALLTYPE
pcAddData(void*, const Steinberg_Vst_ParamID*, Steinberg_int32*) {
  return nullptr; // the plugin does not add to the host's own list
}

inline Steinberg_Vst_IParameterChangesVtbl* paramChangesVtbl() {
  static Steinberg_Vst_IParameterChangesVtbl v = {
      hostQueryNoInterface, hostAddRefStatic, hostReleaseStatic,
      pcCount,              pcGetData,        pcAddData,
  };
  return &v;
}

/** The object a VST3 plugin tells when its own editor moves something.
 *
 *  This is the direction the format makes a host implement rather than call.
 *  Without it a plugin has nowhere to report an edit, which is why some
 *  refuse to initialise their controller at all -- and why, for the ones that
 *  do not, every knob turned on the plugin's face is invisible to automation.
 *
 *  restartComponent is the other half and the less obvious one: it is how a
 *  plugin says "I changed values you did not write", which is exactly what
 *  happens when a program is selected. A host that ignores it keeps showing
 *  the numbers from before the preset. */
struct ComponentHandler {
  Steinberg_Vst_IComponentHandlerVtbl* lpVtbl = nullptr;
  /** Set when the plugin reports kParamValuesChanged; the owner re-reads. */
  bool valuesStale = false;
  std::vector<ParamEdit>* edits = nullptr;
  /** id -> index, because the edits a caller sees are indexed by position. */
  const std::vector<HostedParam>* params = nullptr;
  /** Plain values, not normalised: the owner converts, since only it holds the
   *  controller needed to do it. Null until the plugin is fully loaded. */
  Steinberg_Vst_IEditController* controller = nullptr;
};

inline int chIndexOf(const ComponentHandler* h, Steinberg_Vst_ParamID id) {
  if (!h->params) return -1;
  for (size_t i = 0; i < h->params->size(); ++i)
    if ((Steinberg_Vst_ParamID) (*h->params)[i].id == id) return (int) i;
  return -1;
}

inline Steinberg_tresult SMTG_STDMETHODCALLTYPE chQuery(void* self, const Steinberg_TUID iid,
                                                        void** obj) {
  if (!obj) return Steinberg_kInvalidArgument;
  // FUnknown and IComponentHandler are the same object here. Handing back
  // nothing for FUnknown would be a host that cannot be asked what it is,
  // which some plugins treat as a reason to give up.
  if (std::memcmp(iid, Steinberg_FUnknown_iid, sizeof(Steinberg_TUID)) == 0 ||
      std::memcmp(iid, Steinberg_Vst_IComponentHandler_iid, sizeof(Steinberg_TUID)) == 0) {
    *obj = self;
    return Steinberg_kResultOk;
  }
  *obj = nullptr;
  return Steinberg_kNoInterface;
}

inline Steinberg_tresult SMTG_STDMETHODCALLTYPE chBeginEdit(void* self,
                                                            Steinberg_Vst_ParamID id) {
  auto* h = static_cast<ComponentHandler*>(self);
  if (!h->edits) return Steinberg_kResultFalse;
  const int index = chIndexOf(h, id);
  if (index < 0) return Steinberg_kResultFalse;
  h->edits->push_back({ParamEdit::Kind::kGestureBegin, index, 0.0});
  return Steinberg_kResultOk;
}

inline Steinberg_tresult SMTG_STDMETHODCALLTYPE chPerformEdit(void* self, Steinberg_Vst_ParamID id,
                                                              Steinberg_Vst_ParamValue value) {
  auto* h = static_cast<ComponentHandler*>(self);
  if (!h->edits) return Steinberg_kResultFalse;
  const int index = chIndexOf(h, id);
  if (index < 0) return Steinberg_kResultFalse;
  // VST3 reports normalised; everything a caller sees here is plain, so that
  // one hosting API means the same thing whichever format is underneath.
  double plain = value;
  if (h->controller)
    plain = h->controller->lpVtbl->normalizedParamToPlain(h->controller, id, value);
  h->edits->push_back({ParamEdit::Kind::kValue, index, plain});
  return Steinberg_kResultOk;
}

inline Steinberg_tresult SMTG_STDMETHODCALLTYPE chEndEdit(void* self, Steinberg_Vst_ParamID id) {
  auto* h = static_cast<ComponentHandler*>(self);
  if (!h->edits) return Steinberg_kResultFalse;
  const int index = chIndexOf(h, id);
  if (index < 0) return Steinberg_kResultFalse;
  h->edits->push_back({ParamEdit::Kind::kGestureEnd, index, 0.0});
  return Steinberg_kResultOk;
}

inline Steinberg_tresult SMTG_STDMETHODCALLTYPE chRestartComponent(void* self,
                                                                   Steinberg_int32 flags) {
  auto* h = static_cast<ComponentHandler*>(self);
  // Recorded, not answered on the spot: the plugin is inside its own call and
  // reading every parameter back out of it from here would re-enter it.
  if (flags & Steinberg_Vst_RestartFlags_kParamValuesChanged) h->valuesStale = true;
  return Steinberg_kResultOk;
}

inline Steinberg_Vst_IComponentHandlerVtbl* componentHandlerVtbl() {
  static Steinberg_Vst_IComponentHandlerVtbl v = {
      chQuery,    hostAddRefStatic, hostReleaseStatic,
      chBeginEdit, chPerformEdit,  chEndEdit,
      chRestartComponent,
  };
  return &v;
}

/** The other object VST3 makes a host implement: what a view talks to.
 *
 *  A plugin is entitled to ask for a different size, and the way it asks is
 *  through the frame. Attaching a view WITHOUT one is legal and common, and
 *  it is also how a plugin ends up permanently stuck at its initial size --
 *  including plugins whose editor computes its own size on the first frame.
 *
 *  This one declines, for the same reason the CLAP side does: the window
 *  belongs to the caller. Declining is a different thing from not being
 *  asked, though; the request is recorded so a caller can act on it. */
struct PlugFrame {
  Steinberg_IPlugFrameVtbl* lpVtbl = nullptr;
  uint32_t requestedWidth = 0, requestedHeight = 0;
  bool resizeRequested = false;
};

inline Steinberg_tresult SMTG_STDMETHODCALLTYPE pfQuery(void* self, const Steinberg_TUID iid,
                                                        void** obj) {
  if (!obj) return Steinberg_kInvalidArgument;
  if (std::memcmp(iid, Steinberg_FUnknown_iid, sizeof(Steinberg_TUID)) == 0 ||
      std::memcmp(iid, Steinberg_IPlugFrame_iid, sizeof(Steinberg_TUID)) == 0) {
    *obj = self;
    return Steinberg_kResultOk;
  }
  *obj = nullptr;
  return Steinberg_kNoInterface;
}

inline Steinberg_tresult SMTG_STDMETHODCALLTYPE pfResizeView(void* self,
                                                             struct Steinberg_IPlugView*,
                                                             struct Steinberg_ViewRect* rect) {
  auto* frame = static_cast<PlugFrame*>(self);
  if (!rect) return Steinberg_kInvalidArgument;
  frame->requestedWidth = (uint32_t) (rect->right - rect->left);
  frame->requestedHeight = (uint32_t) (rect->bottom - rect->top);
  frame->resizeRequested = true;
  // kResultFalse, and meant: this host did not resize anything. Answering
  // true would tell the plugin the window is now that size when it is not,
  // and it would draw accordingly.
  return Steinberg_kResultFalse;
}

inline Steinberg_IPlugFrameVtbl* plugFrameVtbl() {
  static Steinberg_IPlugFrameVtbl v = {pfQuery, hostAddRefStatic, hostReleaseStatic, pfResizeView};
  return &v;
}

/** An IBStream over a std::vector.
 *
 *  VST3 reads and writes state through a stream object rather than a buffer,
 *  so a host that wants the bytes has to provide one. Seek is implemented
 *  because setComponentState needs the stream rewound between the component
 *  and the controller -- both read it from the start. */
class MemoryStream {
public:
  explicit MemoryStream(std::vector<uint8_t>* buffer) : buffer_(buffer) { lpVtbl = vtbl(); }
  void rewind() { pos_ = 0; }

  Steinberg_IBStreamVtbl* lpVtbl = nullptr;

private:
  static MemoryStream* self(void* p) { return static_cast<MemoryStream*>(p); }

  static Steinberg_tresult SMTG_STDMETHODCALLTYPE msRead(void* p, void* dest,
                                                         Steinberg_int32 bytes,
                                                         Steinberg_int32* read) {
    MemoryStream* s = self(p);
    if (!dest || bytes < 0) return Steinberg_kInvalidArgument;
    const size_t left = s->buffer_->size() - s->pos_;
    const size_t take = (size_t) bytes < left ? (size_t) bytes : left;
    if (take > 0) std::memcpy(dest, s->buffer_->data() + s->pos_, take);
    s->pos_ += take;
    if (read) *read = (Steinberg_int32) take;
    return Steinberg_kResultOk;
  }
  static Steinberg_tresult SMTG_STDMETHODCALLTYPE msWrite(void* p, void* src,
                                                          Steinberg_int32 bytes,
                                                          Steinberg_int32* written) {
    MemoryStream* s = self(p);
    if (!src || bytes < 0) return Steinberg_kInvalidArgument;
    const auto* data = static_cast<const uint8_t*>(src);
    s->buffer_->insert(s->buffer_->end(), data, data + bytes);
    s->pos_ = s->buffer_->size();
    if (written) *written = bytes;
    return Steinberg_kResultOk;
  }
  static Steinberg_tresult SMTG_STDMETHODCALLTYPE msSeek(void* p, Steinberg_int64 pos,
                                                         Steinberg_int32 mode,
                                                         Steinberg_int64* result) {
    MemoryStream* s = self(p);
    Steinberg_int64 target = pos;
    if (mode == Steinberg_IBStream_IStreamSeekMode_kIBSeekCur) target += (Steinberg_int64) s->pos_;
    else if (mode == Steinberg_IBStream_IStreamSeekMode_kIBSeekEnd)
      target += (Steinberg_int64) s->buffer_->size();
    if (target < 0) target = 0;
    if (target > (Steinberg_int64) s->buffer_->size()) target = (Steinberg_int64) s->buffer_->size();
    s->pos_ = (size_t) target;
    if (result) *result = target;
    return Steinberg_kResultOk;
  }
  static Steinberg_tresult SMTG_STDMETHODCALLTYPE msTell(void* p, Steinberg_int64* result) {
    if (result) *result = (Steinberg_int64) self(p)->pos_;
    return Steinberg_kResultOk;
  }
  static Steinberg_IBStreamVtbl* vtbl() {
    static Steinberg_IBStreamVtbl v = {
        hostQueryNoInterface, hostAddRefStatic, hostReleaseStatic,
        msRead,               msWrite,          msSeek,
        msTell,
    };
    return &v;
  }

  std::vector<uint8_t>* buffer_ = nullptr;
  size_t pos_ = 0;
};

struct EventList {
  Steinberg_Vst_IEventListVtbl* lpVtbl = nullptr;
  std::vector<Steinberg_Vst_Event>* events = nullptr;
  /** Where a plugin's OWN events go when this list is used as an output. Null
   *  on an input list, which is why addEvent checks. */
  MidiBuffer* produced = nullptr;
};

inline Steinberg_int32 SMTG_STDMETHODCALLTYPE elCount(void* self) {
  auto* el = static_cast<EventList*>(self);
  return el->events ? (Steinberg_int32) el->events->size() : 0;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE elGetEvent(void* self, Steinberg_int32 index,
                                                           struct Steinberg_Vst_Event* e) {
  auto* el = static_cast<EventList*>(self);
  if (!el->events || !e || index < 0 || index >= (Steinberg_int32) el->events->size())
    return Steinberg_kResultFalse;
  *e = (*el->events)[(size_t) index];
  return Steinberg_kResultOk;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE elAddEvent(void* self,
                                                           struct Steinberg_Vst_Event* e) {
  // A plugin's own events, kept. This refused everything while there was
  // nowhere to route an arpeggiator's notes; PluginGraph is that somewhere.
  auto* el = static_cast<EventList*>(self);
  if (!el || !el->produced || !e) return Steinberg_kNotImplemented;
  const int offset = e->sampleOffset < 0 ? 0 : e->sampleOffset;
  if (e->type == Steinberg_Vst_Event_EventTypes_kNoteOnEvent) {
    const auto& on = e->Steinberg_Vst_Event_noteOn;
    int velocity = (int) (on.velocity * 127.0f + 0.5f);
    if (velocity < 1) velocity = 1;
    el->produced->addEvent(
        MidiMessage::noteOn(on.channel < 0 ? 0 : on.channel, on.pitch, velocity), offset);
    return Steinberg_kResultOk;
  }
  if (e->type == Steinberg_Vst_Event_EventTypes_kNoteOffEvent) {
    const auto& off = e->Steinberg_Vst_Event_noteOff;
    el->produced->addEvent(MidiMessage::noteOff(off.channel < 0 ? 0 : off.channel, off.pitch),
                           offset);
    return Steinberg_kResultOk;
  }
  // Everything else has no MidiBuffer shape here. Refused rather than
  // truncated into something that means a different thing.
  return Steinberg_kNotImplemented;
}

inline Steinberg_Vst_IEventListVtbl* eventListVtbl() {
  static Steinberg_Vst_IEventListVtbl v = {
      hostQueryNoInterface, hostAddRefStatic, hostReleaseStatic,
      elCount,              elGetEvent,       elAddEvent,
  };
  return &v;
}

class Vst3Plugin;
inline std::unique_ptr<HostedPlugin> loadVst3(const PluginDescription&);

class Vst3Plugin final : public HostedPlugin {
public:
  ~Vst3Plugin() override { unload(); }

  const PluginDescription& description() const override { return desc_; }
  bool isValid() const override { return processor_ != nullptr; }

  bool prepare(double sampleRate, uint32_t maxBlockSize, uint32_t numChannels = 2) override {
    if (!processor_ || !component_) return false;
    if (active_) release();

    maxBlock_ = maxBlockSize > 0 ? maxBlockSize : 1;
    channels_ = numChannels > 0 ? (numChannels > 8 ? 8 : numChannels) : 1;

    Steinberg_Vst_SpeakerArrangement arrangement =
        channels_ == 1 ? (Steinberg_Vst_SpeakerArrangement) 1 // mono
                       : (Steinberg_Vst_SpeakerArrangement) 3; // stereo
    Steinberg_Vst_SpeakerArrangement in = arrangement, out = arrangement;
    processor_->lpVtbl->setBusArrangements(processor_, hasAudioInput_ ? &in : nullptr,
                                           hasAudioInput_ ? 1 : 0, &out, 1);

    Steinberg_Vst_ProcessSetup setup{};
    setup.processMode = Steinberg_Vst_ProcessModes_kRealtime;
    setup.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
    setup.maxSamplesPerBlock = (Steinberg_int32) maxBlock_;
    setup.sampleRate = sampleRate;
    if (processor_->lpVtbl->setupProcessing(processor_, &setup) != Steinberg_kResultOk)
      return false;

    // Every bus on, or a host gets silence out of a plugin that is working.
    for (Steinberg_int32 dir = 0; dir < 2; ++dir) {
      const Steinberg_Vst_BusDirection d =
          dir == 0 ? Steinberg_Vst_BusDirections_kInput : Steinberg_Vst_BusDirections_kOutput;
      const Steinberg_int32 n =
          component_->lpVtbl->getBusCount(component_, Steinberg_Vst_MediaTypes_kAudio, d);
      for (Steinberg_int32 i = 0; i < n; ++i)
        component_->lpVtbl->activateBus(component_, Steinberg_Vst_MediaTypes_kAudio, d, i, 1);
      const Steinberg_int32 e =
          component_->lpVtbl->getBusCount(component_, Steinberg_Vst_MediaTypes_kEvent, d);
      for (Steinberg_int32 i = 0; i < e; ++i)
        component_->lpVtbl->activateBus(component_, Steinberg_Vst_MediaTypes_kEvent, d, i, 1);
    }

    component_->lpVtbl->setActive(component_, 1);
    active_ = true;

    storage_.assign((size_t) channels_ * maxBlock_ * 2, 0.0f);
    inPtrs_.assign(channels_, nullptr);
    outPtrs_.assign(channels_, nullptr);
    for (uint32_t c = 0; c < channels_; ++c) {
      inPtrs_[c] = storage_.data() + (size_t) c * maxBlock_;
      outPtrs_[c] = storage_.data() + (size_t) (channels_ + c) * maxBlock_;
    }
    events_.reserve(MidiBuffer::kMaxEvents);
    // queues_ carries BOTH the caller's queued parameter changes and the
    // controller messages translated out of this block's MIDI, so it is sized
    // for both. Sizing it for one would silently drop the other's tail.
    queues_.reserve(kMaxQueuedParams + (size_t) MidiBuffer::kMaxEvents);
    // And queued_ itself, which the CLAP path reserves and this one did not.
    // The allocation audit found it immediately: three allocations across 200
    // blocks, which is a vector doubling from empty as the first few parameter
    // changes arrived. Exactly the same omission as the CLAP side had, in the
    // code written to mirror it.
    queued_.reserve(kMaxQueuedParams);

    processor_->lpVtbl->setProcessing(processor_, 1);
    processing_ = true;
    return true;
  }

  void release() override {
    if (processing_ && processor_) {
      processor_->lpVtbl->setProcessing(processor_, 0);
      processing_ = false;
    }
    if (active_ && component_) {
      component_->lpVtbl->setActive(component_, 0);
      active_ = false;
    }
  }

  void reset() override {
    // VST3 has no reset(); a host cycles processing off and on, which is what
    // the spec says clears a plugin's internal state.
    if (!processor_ || !processing_) return;
    processor_->lpVtbl->setProcessing(processor_, 0);
    processor_->lpVtbl->setProcessing(processor_, 1);
  }

  void process(AudioBlock<float>& io, const MidiBuffer* midi = nullptr) override;

  uint32_t latencySamples() const override {
    return processor_ ? (uint32_t) processor_->lpVtbl->getLatencySamples(processor_) : 0;
  }
  uint32_t tailSamples() const override {
    return processor_ ? (uint32_t) processor_->lpVtbl->getTailSamples(processor_) : 0;
  }
  bool acceptsMidi() const override { return acceptsMidi_; }
  bool hasAudioInput() const override { return hasAudioInput_; }

  int numParameters() const override { return (int) params_.size(); }
  const HostedParam& parameter(int index) const override { return params_[(size_t) index]; }

  double parameterValue(int index) const override {
    if (!controller_ || index < 0 || index >= numParameters()) return 0.0;
    // VST3 speaks NORMALISED everywhere; the plain value is the plugin's own
    // idea and has to be asked for separately.
    const double norm = controller_->lpVtbl->getParamNormalized(
        controller_, (Steinberg_Vst_ParamID) params_[(size_t) index].id);
    return controller_->lpVtbl->normalizedParamToPlain(
        controller_, (Steinberg_Vst_ParamID) params_[(size_t) index].id, norm);
  }

  bool setParameterValue(int index, double value) override {
    if (!controller_ || index < 0 || index >= numParameters()) return false;
    if (queued_.size() >= kMaxQueuedParams) return false;
    const auto id = (Steinberg_Vst_ParamID) params_[(size_t) index].id;
    double norm = controller_->lpVtbl->plainParamToNormalized(controller_, id, value);
    norm = norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm);
    // The controller is told immediately so a read-back is coherent, and the
    // PROCESSOR is told through the queue -- writing into a plugin that is
    // processing is the race the event list exists to avoid.
    controller_->lpVtbl->setParamNormalized(controller_, id, norm);
    queued_.push_back({id, norm});
    return true;
  }

  std::string parameterText(int index, double value) const override {
    if (!controller_ || index < 0 || index >= numParameters()) return std::string();
    const auto id = (Steinberg_Vst_ParamID) params_[(size_t) index].id;
    const double norm = controller_->lpVtbl->plainParamToNormalized(controller_, id, value);
    Steinberg_Vst_String128 text{};
    if (controller_->lpVtbl->getParamStringByValue(controller_, id, norm, text) !=
        Steinberg_kResultOk)
      return std::string();
    std::string out;
    for (int i = 0; i < 128 && text[i]; ++i) out.push_back((char) text[i]);
    return out;
  }

  const MidiBuffer& producedMidi() const override { return produced_; }

  bool hasBypass() const override { return bypassParam_ >= 0; }
  bool isBypassed() const override {
    return bypassParam_ >= 0 && parameterValue(bypassParam_) >= 0.5;
  }
  bool setBypassed(bool bypassed) override {
    if (bypassParam_ < 0) return false;
    return setParameterValue(bypassParam_, bypassed ? 1.0 : 0.0);
  }

  /** The window kind this platform hands over. */
  static Steinberg_FIDString nativePlatformType() {
#if defined(_WIN32)
    return Steinberg_kPlatformTypeHWND;
#elif defined(__APPLE__)
    return Steinberg_kPlatformTypeNSView;
#else
    return Steinberg_kPlatformTypeX11EmbedWindowID;
#endif
  }

  bool hasEditor() const override { return hasEditor_; }

  bool openEditor(void* parent) override {
    if (view_ || !parent || !controller_) return false;
    // The scale goes on BEFORE attach: that is what decides how big the
    // window it creates is.
    const bool applyScale = editorScale_ != 1.0;
    // "editor" is the name the format reserves for the main one. Asking for
    // any other name is asking for a view a host has no contract with.
    view_ = controller_->lpVtbl->createView(controller_, Steinberg_Vst_ViewType_kEditor);
    if (!view_) return false;
    if (view_->lpVtbl->isPlatformTypeSupported(view_, nativePlatformType()) !=
        Steinberg_kResultOk) {
      view_->lpVtbl->release(view_);
      view_ = nullptr;
      return false;
    }
    // The frame goes on BEFORE attach: a plugin that wants to size itself
    // does it during attach, and a view with no frame at that moment has
    // nobody to ask.
    frame_.lpVtbl = plugFrameVtbl();
    frame_.resizeRequested = false;
    view_->lpVtbl->setFrame(view_, (Steinberg_IPlugFrame*) &frame_);
    if (applyScale) applyScaleToView();
    if (view_->lpVtbl->attached(view_, parent, nativePlatformType()) != Steinberg_kResultOk) {
      view_->lpVtbl->setFrame(view_, nullptr);
      view_->lpVtbl->release(view_);
      view_ = nullptr;
      return false;
    }
    return true;
  }

  void closeEditor() override {
    if (!view_) return;
    // removed() before the frame is taken away and before release: the view
    // is still attached to a window the caller owns, and detaching it after
    // dropping the reference means detaching nothing.
    view_->lpVtbl->removed(view_);
    view_->lpVtbl->setFrame(view_, nullptr);
    view_->lpVtbl->release(view_);
    view_ = nullptr;
  }

  /** Nothing to do, for the same reason as CLAP: the view is a platform
   *  window and the platform's loop is already running it. */
  void idleEditor() override {}

  bool setEditorScale(double scale) override {
    if (!(scale > 0.0)) return false;
    editorScale_ = scale;
    // Remembered even with no view open, because a host knows the scale
    // before it knows the window -- and applying it at attach is the only way
    // the FIRST window comes up the right size rather than being corrected a
    // frame later.
    if (!view_) return true;
    return applyScaleToView();
  }

  bool editorSize(uint32_t& width, uint32_t& height) const override {
    if (!view_) return false;
    Steinberg_ViewRect rect{};
    if (view_->lpVtbl->getSize(view_, &rect) != Steinberg_kResultOk) return false;
    width = (uint32_t) (rect.right - rect.left);
    height = (uint32_t) (rect.bottom - rect.top);
    return true;
  }

  size_t drainParameterEdits(std::vector<ParamEdit>& out) override {
    // A plugin that reported kParamValuesChanged is owed a re-read. Doing it
    // here rather than inside restartComponent keeps the host off the
    // plugin's own stack, and this is a plain main-thread call.
    if (handler_.valuesStale && controller_) {
      handler_.valuesStale = false;
      for (size_t i = 0; i < params_.size(); ++i) {
        const auto id = (Steinberg_Vst_ParamID) params_[i].id;
        const double norm = controller_->lpVtbl->getParamNormalized(controller_, id);
        edits_.push_back({ParamEdit::Kind::kValue, (int) i,
                          controller_->lpVtbl->normalizedParamToPlain(controller_, id, norm)});
      }
    }
    const size_t added = edits_.size();
    out.insert(out.end(), edits_.begin(), edits_.end());
    edits_.clear();
    return added;
  }

  int numPresets() const override { return (int) presetNames_.size(); }
  std::string presetName(int index) const override {
    if (index < 0 || index >= numPresets()) return std::string();
    return presetNames_[(size_t) index];
  }
  bool loadPreset(int index) override {
    if (index < 0 || index >= numPresets() || programParam_ < 0 || !controller_) return false;
    if (queued_.size() >= kMaxQueuedParams) return false;
    // Selected through the parameter the plugin flagged kIsProgramChange,
    // which is the only way VST3 offers -- there is no "load program" call.
    //
    // And it goes to BOTH sides, exactly like any other parameter. Telling
    // only the controller moves the editor and leaves the processor on the
    // old preset, which is a plugin whose knobs say one thing and whose
    // sound says another. In a separate-component plugin that is the whole
    // failure; in a single-component one it happens to work, which is worse,
    // because it works until the first plugin that is not built like ours.
    const double steps = numPresets() > 1 ? (double) (numPresets() - 1) : 1.0;
    const double norm = (double) index / steps;
    const auto id = (Steinberg_Vst_ParamID) params_[(size_t) programParam_].id;
    controller_->lpVtbl->setParamNormalized(controller_, id, norm);
    queued_.push_back({id, norm});
    return true;
  }

  bool saveState(std::vector<uint8_t>& out) const override;
  bool loadState(const uint8_t* data, size_t size) override;

private:
  friend std::unique_ptr<HostedPlugin> loadVst3(const PluginDescription&);
  Vst3Plugin() = default;

  static constexpr size_t kMaxQueuedParams = 64;

  void unload();
  void readCapabilities();

  struct QueuedParam {
    Steinberg_Vst_ParamID id;
    double value;
  };

  LibHandle lib_ = nullptr;
  Steinberg_IPluginFactory* factory_ = nullptr;
  Steinberg_Vst_IComponent* component_ = nullptr;
  Steinberg_Vst_IAudioProcessor* processor_ = nullptr;
  Steinberg_Vst_IEditController* controller_ = nullptr;
  /** Looked up once, because CC and pitch bend have no event form in this
   *  format and every block that carries one needs the mapping. */
  Steinberg_Vst_IMidiMapping* midiMapping_ = nullptr;
  bool controllerIsSeparate_ = false;
  PluginDescription desc_;
  std::vector<HostedParam> params_;

  uint32_t maxBlock_ = 0, channels_ = 2;
  bool active_ = false, processing_ = false;
  bool acceptsMidi_ = false, hasAudioInput_ = true;
  int bypassParam_ = -1;
  /** Index into params_ of the program-change control, or -1. The presets
   *  themselves are named by the unit's program list, not by the parameter. */
  int programParam_ = -1;
  std::vector<std::string> presetNames_;
  /** Ask the view for its scale interface and hand the factor over. A view
   *  that does not implement it is saying it is not DPI aware, which is a
   *  legitimate thing to say. */
  bool applyScaleToView() {
    if (!view_) return false;
    Steinberg_IPlugViewContentScaleSupport* support = nullptr;
    if (view_->lpVtbl->queryInterface(view_, Steinberg_IPlugViewContentScaleSupport_iid,
                                      (void**) &support) != Steinberg_kResultOk ||
        !support)
      return false;
    const Steinberg_tresult result =
        support->lpVtbl->setContentScaleFactor(support, (float) editorScale_);
    support->lpVtbl->release(support);
    return result == Steinberg_kResultOk;
  }

  ComponentHandler handler_;
  std::vector<ParamEdit> edits_;
  double editorScale_ = 1.0;
  PlugFrame frame_;
  Steinberg_IPlugView* view_ = nullptr;
  bool hasEditor_ = false;
  int64_t projectTime_ = 0;

  std::vector<float> storage_;
  std::vector<float*> inPtrs_, outPtrs_;
  std::vector<Steinberg_Vst_Event> events_;
  std::vector<QueuedParam> queued_;
  std::vector<ParamQueue> queues_;
  MidiBuffer produced_;
};

inline void Vst3Plugin::unload() {
  release();
  // Released in the reverse order they were made, and the controller only if
  // it is a SEPARATE object -- when the component also implements
  // IEditController, releasing it twice is one release too many.
  // The view belongs to the controller and must not outlive it.
  closeEditor();
  // The handler is a member of THIS object and the plugin holds a raw pointer
  // to it. Taking it back before the object goes away is the difference
  // between a clean teardown and a plugin calling into freed memory.
  if (controller_) {
    controller_->lpVtbl->setComponentHandler(controller_, nullptr);
    handler_.edits = nullptr;
    handler_.params = nullptr;
    handler_.controller = nullptr;
  }
  if (controller_ && controllerIsSeparate_) {
    controller_->lpVtbl->terminate(controller_);
    controller_->lpVtbl->release(controller_);
  }
  controller_ = nullptr;
  if (processor_) processor_->lpVtbl->release(processor_);
  processor_ = nullptr;
  if (component_) {
    component_->lpVtbl->terminate(component_);
    component_->lpVtbl->release(component_);
  }
  component_ = nullptr;
  if (factory_) factory_->lpVtbl->release(factory_);
  factory_ = nullptr;
  if (lib_) {
    if (auto exitFn = (bool (*)()) libSymbol(lib_, "ExitDll")) exitFn();
    closeLib(lib_);
  }
  lib_ = nullptr;
}

inline void Vst3Plugin::readCapabilities() {
  // Asked once and remembered, because asking means CREATING a view: a plugin
  // that is queried every time a menu is drawn would build and tear down its
  // editor for the privilege of being greyed out.
  if (controller_) {
    Steinberg_IPlugView* probe =
        controller_->lpVtbl->createView(controller_, Steinberg_Vst_ViewType_kEditor);
    if (probe) {
      hasEditor_ =
          probe->lpVtbl->isPlatformTypeSupported(probe, nativePlatformType()) ==
          Steinberg_kResultOk;
      probe->lpVtbl->release(probe);
    }
  }

  hasAudioInput_ = component_->lpVtbl->getBusCount(component_, Steinberg_Vst_MediaTypes_kAudio,
                                                   Steinberg_Vst_BusDirections_kInput) > 0;
  acceptsMidi_ = component_->lpVtbl->getBusCount(component_, Steinberg_Vst_MediaTypes_kEvent,
                                                 Steinberg_Vst_BusDirections_kInput) > 0;
  if (!controller_) return;

  // The program list, which lives on a UNIT rather than on the plugin: a host
  // asks a unit which list it owns and then asks the list for its names.
  {
    Steinberg_Vst_IUnitInfo* units = nullptr;
    if (controller_->lpVtbl->queryInterface(controller_, Steinberg_Vst_IUnitInfo_iid,
                                            (void**) &units) == Steinberg_kResultOk &&
        units) {
      if (units->lpVtbl->getProgramListCount(units) > 0) {
        Steinberg_Vst_ProgramListInfo listInfo{};
        if (units->lpVtbl->getProgramListInfo(units, 0, &listInfo) == Steinberg_kResultOk) {
          for (Steinberg_int32 i = 0; i < listInfo.programCount; ++i) {
            Steinberg_Vst_String128 name{};
            if (units->lpVtbl->getProgramName(units, listInfo.id, i, name) !=
                Steinberg_kResultOk)
              continue;
            std::string narrow;
            for (int k = 0; k < 128 && name[k]; ++k) narrow.push_back((char) name[k]);
            presetNames_.push_back(narrow);
          }
        }
      }
      units->lpVtbl->release(units);
    }
  }

  const Steinberg_int32 n = controller_->lpVtbl->getParameterCount(controller_);
  params_.reserve((size_t) (n > 0 ? n : 0));
  for (Steinberg_int32 i = 0; i < n; ++i) {
    Steinberg_Vst_ParameterInfo info{};
    if (controller_->lpVtbl->getParameterInfo(controller_, i, &info) != Steinberg_kResultOk)
      continue;
    // Hidden parameters are skipped, and this matters more than it sounds: an
    // instrument publishes 2080 of them for MIDI controller routing, and a
    // host that put those in a user's parameter list would be unusable.
    if (info.flags & Steinberg_Vst_ParameterInfo_ParameterFlags_kIsHidden) continue;
    HostedParam p;
    p.id = (uint32_t) info.id;
    for (int c = 0; c < 128 && info.title[c]; ++c) p.name.push_back((char) info.title[c]);
    // VST3 parameters are normalised 0..1 on the wire; the range a user sees
    // is whatever the plugin maps that onto, so ask it.
    p.minValue = controller_->lpVtbl->normalizedParamToPlain(controller_, info.id, 0.0);
    p.maxValue = controller_->lpVtbl->normalizedParamToPlain(controller_, info.id, 1.0);
    p.defaultValue = controller_->lpVtbl->normalizedParamToPlain(controller_, info.id,
                                                                 info.defaultNormalizedValue);
    p.stepped = info.stepCount > 0;
    if (info.flags & Steinberg_Vst_ParameterInfo_ParameterFlags_kIsBypass)
      bypassParam_ = (int) params_.size();
    if (info.flags & Steinberg_Vst_ParameterInfo_ParameterFlags_kIsProgramChange)
      programParam_ = (int) params_.size();
    params_.push_back(p);
  }
}

inline void Vst3Plugin::process(AudioBlock<float>& io, const MidiBuffer* midi) {
  if (!processor_ || !processing_) return;
  const uint32_t frames = (uint32_t) io.getNumSamples();
  if (frames == 0 || frames > maxBlock_) return;

  const uint32_t chans = (uint32_t) io.getNumChannels();
  for (uint32_t c = 0; c < channels_; ++c) {
    const float* src = c < chans ? io.getChannelPointer(c) : nullptr;
    if (src) std::memcpy(inPtrs_[c], src, frames * sizeof(float));
    else std::memset(inPtrs_[c], 0, frames * sizeof(float));
    std::memset(outPtrs_[c], 0, frames * sizeof(float));
  }

  events_.clear();
  if (midi && acceptsMidi_) {
    for (const auto& entry : *midi) {
      const MidiMessage& m = entry.getMessage();
      Steinberg_Vst_Event e{};
      e.busIndex = 0;
      e.sampleOffset = entry.samplePosition < 0 ? 0 : entry.samplePosition;
      e.flags = Steinberg_Vst_Event_EventFlags_kIsLive;
      if (m.isNoteOn()) {
        e.type = Steinberg_Vst_Event_EventTypes_kNoteOnEvent;
        e.Steinberg_Vst_Event_noteOn.channel = (Steinberg_int16) (m.getChannel() - 1);
        e.Steinberg_Vst_Event_noteOn.pitch = (Steinberg_int16) m.getNoteNumber();
        e.Steinberg_Vst_Event_noteOn.velocity = m.getFloatVelocity();
        e.Steinberg_Vst_Event_noteOn.noteId = -1;
      } else if (m.isNoteOff()) {
        e.type = Steinberg_Vst_Event_EventTypes_kNoteOffEvent;
        e.Steinberg_Vst_Event_noteOff.channel = (Steinberg_int16) (m.getChannel() - 1);
        e.Steinberg_Vst_Event_noteOff.pitch = (Steinberg_int16) m.getNoteNumber();
        e.Steinberg_Vst_Event_noteOff.noteId = -1;
      } else {
        // Everything else -- CC, pitch bend, aftertouch -- is NOT an event in
        // this format. It travels as a parameter change on an id the plugin
        // publishes through IMidiMapping, so it is turned into one below.
        continue;
      }
      if (events_.size() < events_.capacity()) events_.push_back(e);
    }
  }

  queues_.clear();
  for (const auto& q : queued_) {
    if (queues_.size() >= queues_.capacity()) break;
    ParamQueue pq;
    pq.lpVtbl = paramQueueVtbl();
    pq.id = q.id;
    pq.value = q.value;
    queues_.push_back(pq);
  }
  // The controller messages that have no event form.
  if (midi && midiMapping_) {
    for (const auto& entry : *midi) {
      const MidiMessage& m = entry.getMessage();
      int controller = -1;
      double normalised = 0.0;
      if (m.isController()) {
        controller = m.getControllerNumber();
        normalised = m.getControllerValue() / 127.0;
      } else if (m.isPitchWheel()) {
        controller = Steinberg_Vst_ControllerNumbers_kPitchBend;
        normalised = m.getPitchWheelValue() / 16383.0;
      } else if (m.isChannelPressure()) {
        controller = Steinberg_Vst_ControllerNumbers_kAfterTouch;
        normalised = m.getChannelPressureValue() / 127.0;
      }
      if (controller < 0 || queues_.size() >= queues_.capacity()) continue;
      Steinberg_Vst_ParamID id = 0;
      if (midiMapping_->lpVtbl->getMidiControllerAssignment(
              midiMapping_, 0, (Steinberg_int16) (m.getChannel() - 1),
              (Steinberg_Vst_CtrlNumber) controller, &id) != Steinberg_kResultOk)
        continue;
      ParamQueue pq;
      pq.lpVtbl = paramQueueVtbl();
      pq.id = id;
      pq.value = normalised;
      queues_.push_back(pq);
    }
  }

  ParameterChanges changes{};
  changes.lpVtbl = paramChangesVtbl();
  changes.queues = &queues_;
  EventList inEvents{};
  inEvents.lpVtbl = eventListVtbl();
  inEvents.events = &events_;
  produced_.clear(); // this block's emissions only
  EventList outEvents{};
  outEvents.lpVtbl = eventListVtbl();
  outEvents.produced = &produced_;

  Steinberg_Vst_AudioBusBuffers inBus{}, outBus{};
  inBus.numChannels = (Steinberg_int32) channels_;
  inBus.silenceFlags = 0;
  inBus.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = inPtrs_.data();
  outBus.numChannels = (Steinberg_int32) channels_;
  outBus.silenceFlags = 0;
  outBus.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = outPtrs_.data();

  Steinberg_Vst_ProcessContext context{};
  context.state = Steinberg_Vst_ProcessContext_StatesAndFlags_kPlaying |
                  Steinberg_Vst_ProcessContext_StatesAndFlags_kTempoValid |
                  Steinberg_Vst_ProcessContext_StatesAndFlags_kTimeSigValid |
                  Steinberg_Vst_ProcessContext_StatesAndFlags_kProjectTimeMusicValid;
  context.sampleRate = 48000.0;
  context.projectTimeSamples = projectTime_;
  context.tempo = 120.0;
  context.timeSigNumerator = 4;
  context.timeSigDenominator = 4;

  Steinberg_Vst_ProcessData data{};
  data.processMode = Steinberg_Vst_ProcessModes_kRealtime;
  data.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
  data.numSamples = (Steinberg_int32) frames;
  data.numInputs = hasAudioInput_ ? 1 : 0;
  data.numOutputs = 1;
  data.inputs = hasAudioInput_ ? &inBus : nullptr;
  data.outputs = &outBus;
  data.inputParameterChanges = (Steinberg_Vst_IParameterChanges*) &changes;
  data.outputParameterChanges = nullptr;
  data.inputEvents = events_.empty() ? nullptr : (Steinberg_Vst_IEventList*) &inEvents;
  data.outputEvents = (Steinberg_Vst_IEventList*) &outEvents;
  data.processContext = &context;

  processor_->lpVtbl->process(processor_, &data);
  projectTime_ += frames;

  for (uint32_t c = 0; c < chans; ++c)
    std::memcpy(io.getChannelPointer(c), outPtrs_[c < channels_ ? c : channels_ - 1],
                frames * sizeof(float));
  queued_.clear();
}

inline bool Vst3Plugin::saveState(std::vector<uint8_t>& out) const {
  if (!component_) return false;
  out.clear();
  MemoryStream stream(&out);
  return component_->lpVtbl->getState(component_, (Steinberg_IBStream*) &stream) ==
         Steinberg_kResultOk;
}

inline bool Vst3Plugin::loadState(const uint8_t* data, size_t size) {
  if (!component_ || (!data && size > 0)) return false;
  std::vector<uint8_t> copy(data, data + size);
  MemoryStream stream(&copy);
  if (component_->lpVtbl->setState(component_, (Steinberg_IBStream*) &stream) !=
      Steinberg_kResultOk)
    return false;
  // The controller has its own copy of the values and will keep showing the
  // old ones unless it is told. A host that skips this gets a plugin that
  // SOUNDS restored and LOOKS unchanged.
  if (controller_ && controllerIsSeparate_) {
    stream.rewind();
    controller_->lpVtbl->setComponentState(controller_, (Steinberg_IBStream*) &stream);
  }
  return true;
}

#endif // SONORE_HOST_VST3

#if defined(SONORE_HOST_LV2)

// ── The LV2 side ─────────────────────────────────────────────────────────────
//
// The format that describes itself in RDF. A CLAP module tells you what is
// inside it through a factory call and a VST3 through a class list; an LV2
// bundle tells you through a Turtle document beside the binary, and a host
// that cannot read that cannot learn a plugin's name, let alone its ports.
//
// So the shape here is different again: parse manifest.ttl, follow
// rdfs:seeAlso to the file with the port descriptions, then dlopen the binary
// and match by URI. The plugin's own code is barely involved until the last
// step.

// -- The LV2 UI ABI, from the host's side ------------------------------------
//
// Spelled again rather than included. The plugin-side copy in lv2_ui.h drags
// in a webview backend and a compiled-in plugin descriptor, neither of which
// belongs anywhere near a host, and the extension's own header is not part of
// the core LV2 package this SDK carries. These five types are the entire
// surface a host touches.

#define SONORE_HOST_LV2_UI_URI "http://lv2plug.in/ns/extensions/ui"
#define SONORE_HOST_LV2_UI__parent SONORE_HOST_LV2_UI_URI "#parent"
#define SONORE_HOST_LV2_UI__idleInterface SONORE_HOST_LV2_UI_URI "#idleInterface"

struct HostLv2Feature {
  const char* URI;
  void* data;
};

/** How an interface tells the host a control moved. Format 0 means the buffer
 *  is one float and the port is an ordinary control port. */
using HostLv2UiWrite = void (*)(void* controller, uint32_t portIndex, uint32_t bufferSize,
                                uint32_t format, const void* buffer);

struct HostLv2UiDescriptor {
  const char* URI;
  void* (*instantiate)(const struct HostLv2UiDescriptor* descriptor, const char* pluginUri,
                       const char* bundlePath, HostLv2UiWrite writeFunction, void* controller,
                       void** widget, const HostLv2Feature* const* features);
  void (*cleanup)(void* ui);
  void (*port_event)(void* ui, uint32_t portIndex, uint32_t bufferSize, uint32_t format,
                     const void* buffer);
  const void* (*extension_data)(const char* uri);
};

struct HostLv2UiIdle {
  int (*idle)(void* ui);
};

/** The one type of UI this platform can embed. A bundle built for X11 is
 *  faceless on Windows and saying otherwise puts an empty window in front of
 *  someone. */
inline const char* nativeLv2UiType() {
#if defined(_WIN32)
  return "http://lv2plug.in/ns/extensions/ui#WindowsUI";
#elif defined(__APPLE__)
  return "http://lv2plug.in/ns/extensions/ui#CocoaUI";
#else
  return "http://lv2plug.in/ns/extensions/ui#X11UI";
#endif
}

/** The file name out of a resolved lv2:binary reference. The manifest writes
 *  it relative to the bundle and the parser resolves it, so this takes the
 *  leaf back off rather than guessing at path separators twice. */
inline std::string binaryLeaf(const std::string& resolved) {
  const size_t slash = resolved.find_last_of("/\\");
  return slash == std::string::npos ? resolved : resolved.substr(slash + 1);
}

struct Lv2Port {
  int index = -1;
  std::string symbol;
  bool isControl = false, isAudio = false, isAtom = false;
  bool isInput = false, isOutput = false;
  float defaultValue = 0.0f, minValue = 0.0f, maxValue = 1.0f;
  std::string name;
};

/** One pset:Preset, reduced to what applying it costs: port indices and the
 *  values they take.
 *
 *  A preset that says nothing about a port leaves it alone -- that is LV2's
 *  rule, not a shortcut here, and it is why this is a sparse list rather than
 *  a full control vector. State properties (state:state) are not read: our
 *  own plugins put everything in ports, and silently applying half a preset
 *  would be worse than declining a kind we cannot honour. */
struct Lv2Preset {
  std::string uri;
  std::string name;
  std::vector<std::pair<int, float>> values;
};

class Lv2Plugin;
inline std::unique_ptr<HostedPlugin> loadLv2(const PluginDescription&);

class Lv2Plugin final : public HostedPlugin {
public:
  ~Lv2Plugin() override { unload(); }

  const PluginDescription& description() const override { return desc_; }
  bool isValid() const override { return descriptor_ != nullptr; }

  bool prepare(double sampleRate, uint32_t maxBlockSize, uint32_t numChannels = 2) override;
  void release() override {
    if (handle_ && descriptor_ && descriptor_->deactivate) descriptor_->deactivate(handle_);
    active_ = false;
  }
  void reset() override {
    // LV2 has no reset entry point. Deactivating and reactivating is what the
    // specification says clears a plugin's state, and it is what a host does.
    if (!handle_ || !descriptor_ || !active_) return;
    if (descriptor_->deactivate) descriptor_->deactivate(handle_);
    if (descriptor_->activate) descriptor_->activate(handle_);
  }

  void process(AudioBlock<float>& io, const MidiBuffer* midi = nullptr) override;

  uint32_t latencySamples() const override {
    if (latencyPort_ < 0) return 0;
    const float value = controls_[(size_t) latencyPort_];
    return value > 0.0f ? (uint32_t) value : 0u;
  }
  uint32_t tailSamples() const override { return 0; } // LV2 core has no tail
  bool acceptsMidi() const override { return atomInPort_ >= 0; }
  bool hasAudioInput() const override { return !audioIn_.empty(); }

  int numParameters() const override { return (int) params_.size(); }
  const HostedParam& parameter(int index) const override { return params_[(size_t) index]; }

  double parameterValue(int index) const override {
    if (index < 0 || index >= numParameters()) return 0.0;
    return controls_[(size_t) params_[(size_t) index].id];
  }

  bool setParameterValue(int index, double value) override {
    if (index < 0 || index >= numParameters()) return false;
    const HostedParam& p = params_[(size_t) index];
    const double clamped = value < p.minValue ? p.minValue : (value > p.maxValue ? p.maxValue : value);
    // Written straight in, and that is correct HERE where it is a race
    // everywhere else: an LV2 control port IS a float the host owns and the
    // plugin reads once a block. There is no event list to go through because
    // the format does not have one.
    controls_[(size_t) p.id] = (float) clamped;
    return true;
  }

  std::string parameterText(int, double value) const override {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return std::string(buffer);
  }

  bool saveState(std::vector<uint8_t>& out) const override;
  bool loadState(const uint8_t* data, size_t size) override;

  const MidiBuffer& producedMidi() const override { return produced_; }

  /** Nothing, unless an interface is open.
   *
   *  The PLUGIN has no channel for this and cannot have one: an LV2 control
   *  port is a float the host owns and the plugin only reads. What can report
   *  is the UI, which is a separate binary with a separate lifetime, talking
   *  through the write function it was handed at instantiate. So a closed
   *  editor reports nothing and an open one reports what the user did to it,
   *  which is the truth about the format rather than a shape borrowed from
   *  the other two. */
  size_t drainParameterEdits(std::vector<ParamEdit>& out) override {
    const size_t added = edits_.size();
    out.insert(out.end(), edits_.begin(), edits_.end());
    edits_.clear();
    return added;
  }

  bool hasEditor() const override { return !uiBinary_.empty() && !uiUri_.empty(); }

  bool openEditor(void* parent) override {
    if (uiHandle_ || !parent || !hasEditor()) return false;
    uiLib_ = openLib(uiBinary_.c_str());
    if (!uiLib_) return false;

    auto entry =
        (const HostLv2UiDescriptor* (*) (uint32_t)) libSymbol(uiLib_, "lv2ui_descriptor");
    if (!entry) {
      closeLib(uiLib_);
      uiLib_ = nullptr;
      return false;
    }
    // By URI, not by index. One binary is allowed to carry several interfaces
    // and taking the first would be taking whichever happened to be built
    // first -- a different plugin's face, in a bundle that ships two.
    for (uint32_t i = 0;; ++i) {
      const HostLv2UiDescriptor* d = entry(i);
      if (!d) break;
      if (d->URI && uiUri_ == d->URI) {
        uiDesc_ = d;
        break;
      }
    }
    if (!uiDesc_ || !uiDesc_->instantiate) {
      closeLib(uiLib_);
      uiLib_ = nullptr;
      return false;
    }

    HostLv2Feature parentFeature{SONORE_HOST_LV2_UI__parent, parent};
    const HostLv2Feature* features[] = {&parentFeature, nullptr};

    uiWidget_ = nullptr;
    uiHandle_ = uiDesc_->instantiate(uiDesc_, desc_.id.c_str(), (desc_.path + "/").c_str(),
                                     uiWrite, this, &uiWidget_, features);
    if (!uiHandle_) {
      closeLib(uiLib_);
      uiLib_ = nullptr;
      uiDesc_ = nullptr;
      return false;
    }

    if (uiDesc_->extension_data)
      uiIdle_ = static_cast<const HostLv2UiIdle*>(
          uiDesc_->extension_data(SONORE_HOST_LV2_UI__idleInterface));

    // The interface starts blank and the ports are not at their defaults if
    // this plugin has been played with. Sending the current value of every
    // control is what a host owes a UI it just opened.
    uiEchoed_.assign(controls_.size(), 0.0f);
    for (const Lv2Port& p : ports_)
      if (p.isControl && p.isInput) sendPortToUi(p.index, true);
    return true;
  }

  void closeEditor() override {
    if (uiHandle_ && uiDesc_ && uiDesc_->cleanup) uiDesc_->cleanup(uiHandle_);
    uiHandle_ = nullptr;
    uiWidget_ = nullptr;
    uiIdle_ = nullptr;
    uiDesc_ = nullptr;
    if (uiLib_) closeLib(uiLib_);
    uiLib_ = nullptr;
  }

  /** LV2 is the one format here that needs the host to keep the interface
   *  alive. There is no message loop of the plugin's own: the bundle asks for
   *  ui:idleInterface and the host calls it, which is where a webview gets
   *  pumped and where anything the page said becomes a port write. Skipping
   *  it gives a face that draws once and then stops responding. */
  void idleEditor() override {
    if (uiIdle_ && uiIdle_->idle && uiHandle_) uiIdle_->idle(uiHandle_);
    // The other direction, on the same beat: a control that moved for any
    // reason -- automation, a preset, another editor -- has to reach the
    // interface, or it shows numbers that stopped being true.
    for (const Lv2Port& p : ports_)
      if (p.isControl && p.isInput) sendPortToUi(p.index, false);
  }

  bool editorSize(uint32_t& width, uint32_t& height) const override {
    if (!uiHandle_ || !uiWidget_) return false;
#if defined(_WIN32)
    RECT rect{};
    if (!GetClientRect((HWND) uiWidget_, &rect)) return false;
    width = (uint32_t) (rect.right - rect.left);
    height = (uint32_t) (rect.bottom - rect.top);
    return width > 0 && height > 0;
#else
    // The widget is a GtkWidget* or an NSView* and asking it its size means
    // linking the toolkit it belongs to. A host that has one should ask it;
    // this one does not, and says so rather than inventing a number.
    (void) width;
    (void) height;
    return false;
#endif
  }

  /** LV2's UI extension has ui:scaleFactor as an optional FEATURE handed over
   *  at instantiate, not a call a host can make afterwards -- so there is
   *  nothing to say once the interface exists. */
  bool setEditorScale(double) override { return false; }

  int numPresets() const override { return (int) presets_.size(); }
  std::string presetName(int index) const override {
    if (index < 0 || index >= numPresets()) return std::string();
    return presets_[(size_t) index].name;
  }
  bool loadPreset(int index) override {
    if (index < 0 || index >= numPresets()) return false;
    // Nothing to call: an LV2 preset IS a set of port values, and the ports
    // are floats this host owns. Writing them is the whole operation.
    for (const auto& pv : presets_[(size_t) index].values)
      if (pv.first >= 0 && (size_t) pv.first < controls_.size())
        controls_[(size_t) pv.first] = pv.second;
    return true;
  }

  bool hasBypass() const override { return enabledPort_ >= 0; }
  /** INVERTED. lv2:enabled is not a bypass switch, it is the opposite one, and
   *  a host that copied the value across formats would bypass every plugin it
   *  meant to leave alone. */
  bool isBypassed() const override {
    return enabledPort_ >= 0 && controls_[(size_t) enabledPort_] < 0.5f;
  }
  bool setBypassed(bool bypassed) override {
    if (enabledPort_ < 0) return false;
    // Written straight in, which is safe here for the reason it is not
    // elsewhere: an LV2 control port IS a float the host owns and the plugin
    // reads once a block. The format has no event list to go through.
    controls_[(size_t) enabledPort_] = bypassed ? 0.0f : 1.0f;
    return true;
  }

private:
  friend std::unique_ptr<HostedPlugin> loadLv2(const PluginDescription&);
  Lv2Plugin() = default;

  /** The interface saying a control moved.
   *
   *  Two things happen, and a host that does only one of them is broken in a
   *  way users describe as "the knob springs back": the value is written to
   *  the PORT, because in LV2 the host owns that float and nobody else will
   *  write it, and it is recorded as an edit so automation can see it. */
  static void uiWrite(void* controller, uint32_t portIndex, uint32_t bufferSize, uint32_t format,
                      const void* buffer) {
    auto* self = static_cast<Lv2Plugin*>(controller);
    if (!self || format != 0 || bufferSize != sizeof(float) || !buffer) return;
    if (portIndex >= self->controls_.size()) return;
    const float value = *static_cast<const float*>(buffer);
    self->controls_[portIndex] = value;
    if (portIndex < self->uiEchoed_.size()) self->uiEchoed_[portIndex] = value;

    for (size_t i = 0; i < self->params_.size(); ++i) {
      if (self->params_[i].id != portIndex) continue;
      if (self->edits_.size() < 4096)
        self->edits_.push_back({ParamEdit::Kind::kValue, (int) i, (double) value});
      return;
    }
  }

  /** Tell the interface what a port is at, if it does not already know.
   *
   *  `force` is for the moment an editor opens, when it knows nothing. After
   *  that only changes go out: a host that re-sent every value on every idle
   *  would be fighting the mouse that is dragging one. */
  void sendPortToUi(int index, bool force) {
    if (!uiHandle_ || !uiDesc_ || !uiDesc_->port_event) return;
    if (index < 0 || (size_t) index >= controls_.size()) return;
    if ((size_t) index >= uiEchoed_.size()) uiEchoed_.resize(controls_.size(), 0.0f);
    const float value = controls_[(size_t) index];
    if (!force && uiEchoed_[(size_t) index] == value) return;
    uiEchoed_[(size_t) index] = value;
    uiDesc_->port_event(uiHandle_, (uint32_t) index, sizeof(float), 0, &value);
  }

  std::vector<Lv2Preset> presets_;
  std::vector<ParamEdit> edits_;
  std::string uiUri_, uiBinary_;
  LibHandle uiLib_ = nullptr;
  const HostLv2UiDescriptor* uiDesc_ = nullptr;
  const HostLv2UiIdle* uiIdle_ = nullptr;
  void* uiHandle_ = nullptr;
  void* uiWidget_ = nullptr;
  std::vector<float> uiEchoed_;

  void unload() {
    release();
    // The interface reads the ports this object owns, so it goes first.
    closeEditor();
    if (handle_ && descriptor_ && descriptor_->cleanup) descriptor_->cleanup(handle_);
    handle_ = nullptr;
    descriptor_ = nullptr;
    if (lib_) closeLib(lib_);
    lib_ = nullptr;
  }

  /** One property from the state: extension.
   *
   *  A control port is a float and nothing else. Everything a plugin knows
   *  beyond its knobs -- which sample a sampler loaded, which impulse a
   *  reverb read -- travels through this extension as typed key/value pairs,
   *  and a host that saved only the ports would restore a session with the
   *  knobs right and the sound wrong. */
  struct StateProperty {
    LV2_URID key = 0;
    LV2_URID type = 0;
    std::vector<uint8_t> value;
  };

  /** The URI behind a URID, so a saved blob survives being reopened.
   *
   *  URIDs are only required to be stable within ONE host's lifetime, so
   *  writing the numbers into a file would produce state that restores as
   *  something else next time. The URI strings go in the blob and are mapped
   *  again on the way back. */
  std::string uriFor(LV2_URID id) const {
    for (const auto& entry : urids_)
      if (entry.second == id) return entry.first;
    return std::string();
  }

  static LV2_State_Status storeProperty(LV2_State_Handle handle, uint32_t key, const void* value,
                                        size_t size, uint32_t type, uint32_t flags) {
    auto* properties = static_cast<std::vector<StateProperty>*>(handle);
    if (!properties || !value || size == 0) return LV2_STATE_ERR_UNKNOWN;
    // POD only. A plugin may ask to store something that is only valid while
    // it is running; taking a copy of that and writing it to a file would
    // restore a pointer into a process that no longer exists.
    if ((flags & LV2_STATE_IS_POD) == 0) return LV2_STATE_ERR_BAD_FLAGS;
    StateProperty p;
    p.key = key;
    p.type = type;
    const auto* bytes = static_cast<const uint8_t*>(value);
    p.value.assign(bytes, bytes + size);
    properties->push_back(std::move(p));
    return LV2_STATE_SUCCESS;
  }

  static const void* retrieveProperty(LV2_State_Handle handle, uint32_t key, size_t* size,
                                      uint32_t* type, uint32_t* flags) {
    auto* properties = static_cast<std::vector<StateProperty>*>(handle);
    if (!properties) return nullptr;
    for (const StateProperty& p : *properties) {
      if (p.key != key) continue;
      if (size) *size = p.value.size();
      if (type) *type = p.type;
      if (flags) *flags = LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE;
      return p.value.data();
    }
    return nullptr;
  }

  const LV2_State_Interface* stateInterface() const {
    if (!descriptor_ || !descriptor_->extension_data) return nullptr;
    return static_cast<const LV2_State_Interface*>(
        descriptor_->extension_data(LV2_STATE__interface));
  }

  /** The URID map every LV2 plugin asks for. A plain string table: ids are
   *  only required to be stable within one host's lifetime. */
  static LV2_URID mapUri(LV2_URID_Map_Handle handle, const char* uri) {
    auto* self = static_cast<Lv2Plugin*>(handle);
    if (!uri) return 0;
    auto found = self->urids_.find(uri);
    if (found != self->urids_.end()) return found->second;
    const LV2_URID id = (LV2_URID) (self->urids_.size() + 1);
    self->urids_[uri] = id;
    return id;
  }

  LibHandle lib_ = nullptr;
  const LV2_Descriptor* descriptor_ = nullptr;
  LV2_Handle handle_ = nullptr;
  PluginDescription desc_;
  std::vector<HostedParam> params_;
  std::vector<Lv2Port> ports_;
  std::map<std::string, LV2_URID> urids_;

  std::vector<float> controls_;
  std::vector<int> audioIn_, audioOut_;
  int latencyPort_ = -1, atomInPort_ = -1, atomOutPort_ = -1, enabledPort_ = -1;

  std::vector<float> storage_;
  std::vector<float*> inPtrs_, outPtrs_;
  std::vector<uint8_t> atomIn_, atomOut_;
  MidiBuffer produced_;
  /** The two URIDs process() needs, looked up ONCE.
   *
   *  Calling mapUri() per block cost four allocations a block: the map is
   *  keyed by std::string and a const char* lookup builds a temporary, and
   *  these URIs are far too long for the small-string optimisation. The
   *  allocation audit reported 800 across 200 blocks, which is exactly that. */
  LV2_URID uridSequence_ = 0, uridMidiEvent_ = 0;
  LV2_URID_Map map_{};
  LV2_Feature mapFeature_{};
  std::vector<const LV2_Feature*> features_;

  uint32_t maxBlock_ = 0, channels_ = 2;
  bool active_ = false;
};

inline bool Lv2Plugin::prepare(double sampleRate, uint32_t maxBlockSize, uint32_t numChannels) {
  if (!descriptor_) return false;
  if (handle_) {
    release();
    if (descriptor_->cleanup) descriptor_->cleanup(handle_);
    handle_ = nullptr;
  }
  maxBlock_ = maxBlockSize > 0 ? maxBlockSize : 1;
  channels_ = numChannels > 0 ? (numChannels > 8 ? 8 : numChannels) : 1;

  // The rate is fixed at INSTANTIATE in this format, so re-preparing means a
  // whole new instance. Nothing else here works that way.
  map_.handle = this;
  map_.map = mapUri;
  mapFeature_.URI = LV2_URID__map;
  mapFeature_.data = &map_;
  features_.clear();
  features_.push_back(&mapFeature_);
  features_.push_back(nullptr);

  handle_ = descriptor_->instantiate(descriptor_, sampleRate, desc_.path.c_str(),
                                     features_.data());
  if (!handle_) return false;

  // Resolved here, where allocating is what prepare() is for.
  uridSequence_ = mapUri(this, LV2_ATOM__Sequence);
  uridMidiEvent_ = mapUri(this, LV2_MIDI__MidiEvent);

  storage_.assign((size_t) channels_ * maxBlock_ * 2, 0.0f);
  inPtrs_.assign(channels_, nullptr);
  outPtrs_.assign(channels_, nullptr);
  for (uint32_t c = 0; c < channels_; ++c) {
    inPtrs_[c] = storage_.data() + (size_t) c * maxBlock_;
    outPtrs_[c] = storage_.data() + (size_t) (channels_ + c) * maxBlock_;
  }
  atomIn_.assign(8192, 0);
  atomOut_.assign(8192, 0);

  // Every port gets a pointer, including the ones this host does not use. A
  // plugin is entitled to dereference anything it declared, and connecting
  // only what we care about is how a host crashes a plugin that did nothing
  // wrong.
  int audioInSeen = 0, audioOutSeen = 0;
  for (const Lv2Port& p : ports_) {
    if (p.isControl) {
      descriptor_->connect_port(handle_, (uint32_t) p.index, &controls_[(size_t) p.index]);
    } else if (p.isAudio && p.isInput) {
      descriptor_->connect_port(handle_, (uint32_t) p.index,
                                inPtrs_[audioInSeen < (int) channels_ ? audioInSeen : 0]);
      ++audioInSeen;
    } else if (p.isAudio && p.isOutput) {
      descriptor_->connect_port(handle_, (uint32_t) p.index,
                                outPtrs_[audioOutSeen < (int) channels_ ? audioOutSeen : 0]);
      ++audioOutSeen;
    } else if (p.isAtom && p.isInput) {
      descriptor_->connect_port(handle_, (uint32_t) p.index, atomIn_.data());
    } else if (p.isAtom && p.isOutput) {
      descriptor_->connect_port(handle_, (uint32_t) p.index, atomOut_.data());
    }
  }

  if (descriptor_->activate) descriptor_->activate(handle_);
  active_ = true;

  // One silent sample, so the plugin publishes its latency.
  //
  // lv2:latency is an OUTPUT control port, which means the plugin writes it
  // during run() and not before. A host asking latencySamples() straight
  // after prepare -- which is exactly when a host needs it, to lay the track
  // out -- would read the zero the port was initialised to. Every other
  // format answers immediately, and a caller should not have to know which
  // one it is holding.
  //
  // A single sample rather than a whole block: it costs nothing and any
  // plugin that reports latency at all reports it on its first run.
  if (latencyPort_ >= 0) {
    for (uint32_t c = 0; c < channels_; ++c) {
      inPtrs_[c][0] = 0.0f;
      outPtrs_[c][0] = 0.0f;
    }
    if (atomInPort_ >= 0) {
      auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(atomIn_.data());
      seq->atom.type = uridSequence_;
      seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
      seq->body.unit = 0;
      seq->body.pad = 0;
    }
    descriptor_->run(handle_, 1);
  }
  return true;
}

inline void Lv2Plugin::process(AudioBlock<float>& io, const MidiBuffer* midi) {
  if (!handle_ || !descriptor_ || !active_) return;
  const uint32_t frames = (uint32_t) io.getNumSamples();
  if (frames == 0 || frames > maxBlock_) return;

  const uint32_t chans = (uint32_t) io.getNumChannels();
  for (uint32_t c = 0; c < channels_; ++c) {
    const float* src = c < chans ? io.getChannelPointer(c) : nullptr;
    if (src) std::memcpy(inPtrs_[c], src, frames * sizeof(float));
    else std::memset(inPtrs_[c], 0, frames * sizeof(float));
    std::memset(outPtrs_[c], 0, frames * sizeof(float));
  }

  // MIDI in, as an atom sequence. Built by hand because the atom-forge header
  // is not vendored and the sequence layout for 3-byte events is small and
  // fixed -- a header, then one aligned event per message.
  if (atomInPort_ >= 0) {
    auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(atomIn_.data());
    seq->atom.type = uridSequence_;
    seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
    seq->body.unit = 0;
    seq->body.pad = 0;
    if (midi && acceptsMidi()) {
      const LV2_URID midiEvent = uridMidiEvent_;
      uint8_t* base = reinterpret_cast<uint8_t*>(&seq->body);
      for (const auto& entry : *midi) {
        const uint32_t need = (uint32_t) (sizeof(LV2_Atom_Event) + 8u);
        // Summed on the left, so an atom buffer shorter than a header can
        // never underflow the subtraction into a bound that admits anything.
        if (sizeof(LV2_Atom) + seq->atom.size + need > atomIn_.size()) break;
        auto* ev = reinterpret_cast<LV2_Atom_Event*>(base + seq->atom.size);
        ev->time.frames = entry.samplePosition < 0 ? 0 : entry.samplePosition;
        ev->body.size = 3;
        ev->body.type = midiEvent;
        uint8_t* bytes = reinterpret_cast<uint8_t*>(ev) + sizeof(LV2_Atom_Event);
        const MidiMessage& m = entry.getMessage();
        bytes[0] = (uint8_t) m.getRawStatus();
        bytes[1] = (uint8_t) m.getRawData1();
        bytes[2] = (uint8_t) m.getRawData2();
        seq->atom.size += need;
      }
    }
  }
  if (atomOutPort_ >= 0) {
    // The plugin needs to be told how much room it has, which it reads from
    // the atom's own size field before writing.
    auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(atomOut_.data());
    seq->atom.type = uridSequence_;
    seq->atom.size = (uint32_t) (atomOut_.size() - sizeof(LV2_Atom));
    seq->body.unit = 0;
    seq->body.pad = 0;
  }

  descriptor_->run(handle_, frames);

  produced_.clear();
  if (atomOutPort_ >= 0) {
    const auto* seq = reinterpret_cast<const LV2_Atom_Sequence*>(atomOut_.data());
    const LV2_URID midiEvent = uridMidiEvent_;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(&seq->body);
    uint32_t offset = sizeof(LV2_Atom_Sequence_Body);
    while (offset + sizeof(LV2_Atom_Event) <= seq->atom.size) {
      const auto* ev = reinterpret_cast<const LV2_Atom_Event*>(base + offset);
      if (ev->body.type == midiEvent && ev->body.size >= 3) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(ev) + sizeof(LV2_Atom_Event);
        if (bytes[0] >= 0x80 && bytes[0] < 0xf0)
          produced_.addEvent(MidiMessage(bytes[0], bytes[1], bytes[2]), (int) ev->time.frames);
      }
      const uint32_t step =
          (uint32_t) (sizeof(LV2_Atom_Event) + ((ev->body.size + 7u) & ~7u));
      if (step == 0) break;
      offset += step;
    }
  }

  for (uint32_t c = 0; c < chans; ++c)
    std::memcpy(io.getChannelPointer(c), outPtrs_[c < channels_ ? c : channels_ - 1],
                frames * sizeof(float));
}

namespace detail {

inline void appendU32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back((uint8_t) ((v >> (8 * i)) & 0xff));
}
inline bool readU32(const uint8_t* data, size_t size, size_t* pos, uint32_t* out) {
  if (*pos + 4 > size) return false;
  *out = (uint32_t) data[*pos] | ((uint32_t) data[*pos + 1] << 8) |
         ((uint32_t) data[*pos + 2] << 16) | ((uint32_t) data[*pos + 3] << 24);
  *pos += 4;
  return true;
}

} // namespace detail

inline bool Lv2Plugin::saveState(std::vector<uint8_t>& out) const {
  out.clear();
  // Ports first, then the extension's properties. Length-prefixed rather than
  // terminated, because a stored value is arbitrary bytes and any terminator
  // would eventually appear inside one.
  detail::appendU32(out, (uint32_t) params_.size());
  for (const HostedParam& p : params_) {
    const float v = controls_[(size_t) p.id];
    const auto* bytes = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), bytes, bytes + sizeof(float));
  }

  std::vector<StateProperty> properties;
  const LV2_State_Interface* state = stateInterface();
  if (state && state->save && handle_) {
    const LV2_Feature* const features[] = {nullptr};
    state->save(handle_, storeProperty, &properties, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE,
                features);
  }

  detail::appendU32(out, (uint32_t) properties.size());
  for (const StateProperty& p : properties) {
    // The URI, not the URID. Ids are only stable within one host's lifetime,
    // and a file full of them restores as something else the next time.
    const std::string keyUri = uriFor(p.key);
    const std::string typeUri = uriFor(p.type);
    detail::appendU32(out, (uint32_t) keyUri.size());
    out.insert(out.end(), keyUri.begin(), keyUri.end());
    detail::appendU32(out, (uint32_t) typeUri.size());
    out.insert(out.end(), typeUri.begin(), typeUri.end());
    detail::appendU32(out, (uint32_t) p.value.size());
    out.insert(out.end(), p.value.begin(), p.value.end());
  }
  return true;
}

inline bool Lv2Plugin::loadState(const uint8_t* data, size_t size) {
  if (!data && size > 0) return false;
  size_t pos = 0;
  uint32_t count = 0;
  if (!detail::readU32(data, size, &pos, &count)) return false;
  if (count != params_.size()) return false; // a blob for a different plugin
  for (uint32_t i = 0; i < count; ++i) {
    if (pos + sizeof(float) > size) return false;
    float v = 0.0f;
    std::memcpy(&v, data + pos, sizeof(float));
    pos += sizeof(float);
    controls_[(size_t) params_[i].id] = v;
  }

  uint32_t properties = 0;
  // A blob written before this host understood the state extension simply
  // ends here, and that is a restore rather than a failure.
  if (!detail::readU32(data, size, &pos, &properties)) return true;

  std::vector<StateProperty> restored;
  for (uint32_t i = 0; i < properties; ++i) {
    uint32_t keyLength = 0, typeLength = 0, valueLength = 0;
    if (!detail::readU32(data, size, &pos, &keyLength)) return false;
    if (pos + keyLength > size) return false;
    const std::string keyUri((const char*) data + pos, keyLength);
    pos += keyLength;
    if (!detail::readU32(data, size, &pos, &typeLength)) return false;
    if (pos + typeLength > size) return false;
    const std::string typeUri((const char*) data + pos, typeLength);
    pos += typeLength;
    if (!detail::readU32(data, size, &pos, &valueLength)) return false;
    if (pos + valueLength > size) return false;

    StateProperty p;
    p.key = mapUri(this, keyUri.c_str());
    p.type = typeUri.empty() ? 0 : mapUri(this, typeUri.c_str());
    p.value.assign(data + pos, data + pos + valueLength);
    pos += valueLength;
    restored.push_back(std::move(p));
  }

  const LV2_State_Interface* state = stateInterface();
  if (state && state->restore && handle_) {
    const LV2_Feature* const features[] = {nullptr};
    state->restore(handle_, retrieveProperty, &restored,
                   LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, features);
  }
  return true;
}

/** Read a bundle's Turtle and describe what is in it, without loading code. */
inline void describeLv2Bundle(const std::string& bundlePath,
                              std::vector<PluginDescription>* found) {
  const std::string manifestPath = bundlePath + "/manifest.ttl";
  std::FILE* file = std::fopen(manifestPath.c_str(), "rb");
  if (!file) return;
  std::string text;
  char buffer[8192];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
  std::fclose(file);

  turtle::Document manifest;
  // A manifest that will not parse is a bundle a host skips, which is exactly
  // what every other LV2 host does with one.
  if (!turtle::parse(text, bundlePath, &manifest)) return;

  const std::string lv2 = "http://lv2plug.in/ns/lv2core#";
  const std::string rdfs = "http://www.w3.org/2000/01/rdf-schema#";
  const std::string doap = "http://usefulinc.com/ns/doap#";

  for (const std::string& subject : manifest.subjectsOfType(lv2 + "Plugin")) {
    PluginDescription d;
    d.path = bundlePath;
    d.format = "LV2";
    d.id = subject; // the plugin URI, which is what identifies it everywhere
    d.name = manifest.object(subject, doap + "name");

    // The real description usually lives in a second file. Read it for the
    // name and the category, because a manifest is deliberately thin.
    for (const std::string& also : manifest.objects(subject, rdfs + "seeAlso")) {
      std::FILE* extra = std::fopen(also.c_str(), "rb");
      if (!extra) continue;
      std::string more;
      while ((got = std::fread(buffer, 1, sizeof(buffer), extra)) > 0) more.append(buffer, got);
      std::fclose(extra);
      turtle::Document doc;
      if (!turtle::parse(more, bundlePath, &doc)) continue;
      if (d.name.empty()) d.name = doc.object(subject, doap + "name");
      for (const auto& t : doc.triples) {
        if (t.subject != subject) continue;
        if (t.predicate != "http://www.w3.org/1999/02/22-rdf-syntax-ns#type") continue;
        if (t.object == lv2 + "Plugin") continue;
        if (!d.features.empty()) d.features += ";";
        d.features += t.object;
        if (t.object.find("Instrument") != std::string::npos) d.isInstrument = true;
      }
    }
    if (d.name.empty()) d.name = subject;
    found->push_back(d);
  }
}

inline std::unique_ptr<HostedPlugin> loadLv2(const PluginDescription& description) {
  std::unique_ptr<Lv2Plugin> hosted(new Lv2Plugin());
  hosted->desc_ = description;

  const std::string manifestPath = description.path + "/manifest.ttl";
  std::FILE* file = std::fopen(manifestPath.c_str(), "rb");
  if (!file) return nullptr;
  std::string text;
  char buffer[8192];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
  std::fclose(file);

  turtle::Document manifest;
  if (!turtle::parse(text, description.path, &manifest)) return nullptr;

  const std::string lv2 = "http://lv2plug.in/ns/lv2core#";
  const std::string rdfs = "http://www.w3.org/2000/01/rdf-schema#";
  const std::string atom = "http://lv2plug.in/ns/ext/atom#";

  const std::string binary = manifest.object(description.id, lv2 + "binary");
  if (binary.empty()) return nullptr;

  // Ports come from the seeAlso document; gather every one of them.
  turtle::Document all;
  all.triples = manifest.triples;
  for (const std::string& also : manifest.objects(description.id, rdfs + "seeAlso")) {
    std::FILE* extra = std::fopen(also.c_str(), "rb");
    if (!extra) continue;
    std::string more;
    while ((got = std::fread(buffer, 1, sizeof(buffer), extra)) > 0) more.append(buffer, got);
    std::fclose(extra);
    turtle::Document doc;
    if (!turtle::parse(more, description.path, &doc)) continue;
    all.triples.insert(all.triples.end(), doc.triples.begin(), doc.triples.end());
  }

  int highest = -1;
  for (const std::string& portNode : all.objects(description.id, lv2 + "port")) {
    Lv2Port p;
    const std::string index = all.object(portNode, lv2 + "index");
    if (index.empty()) continue;
    p.index = std::atoi(index.c_str());
    p.symbol = all.object(portNode, lv2 + "symbol");
    p.name = all.object(portNode, lv2 + "name");
    for (const auto& t : all.triples) {
      if (t.subject != portNode) continue;
      if (t.predicate != "http://www.w3.org/1999/02/22-rdf-syntax-ns#type") continue;
      if (t.object == lv2 + "ControlPort") p.isControl = true;
      if (t.object == lv2 + "AudioPort") p.isAudio = true;
      if (t.object == atom + "AtomPort") p.isAtom = true;
      if (t.object == lv2 + "InputPort") p.isInput = true;
      if (t.object == lv2 + "OutputPort") p.isOutput = true;
    }
    const std::string def = all.object(portNode, lv2 + "default");
    const std::string lo = all.object(portNode, lv2 + "minimum");
    const std::string hi = all.object(portNode, lv2 + "maximum");
    if (!def.empty()) p.defaultValue = (float) std::atof(def.c_str());
    if (!lo.empty()) p.minValue = (float) std::atof(lo.c_str());
    if (!hi.empty()) p.maxValue = (float) std::atof(hi.c_str());
    if (p.index > highest) highest = p.index;
    hosted->ports_.push_back(p);
  }
  if (highest < 0) return nullptr; // a plugin with no ports is not one

  hosted->controls_.assign((size_t) highest + 1, 0.0f);
  for (const Lv2Port& p : hosted->ports_) {
    if (p.isControl) hosted->controls_[(size_t) p.index] = p.defaultValue;
    if (p.isControl && p.isOutput && p.symbol == "latency") hosted->latencyPort_ = p.index;
    if (p.isControl && p.isInput && p.symbol == "enabled") hosted->enabledPort_ = p.index;
    if (p.isAtom && p.isInput) hosted->atomInPort_ = p.index;
    if (p.isAtom && p.isOutput) hosted->atomOutPort_ = p.index;
    if (p.isAudio && p.isInput) hosted->audioIn_.push_back(p.index);
    if (p.isAudio && p.isOutput) hosted->audioOut_.push_back(p.index);
    // Input control ports are the plugin's PARAMETERS. The designated ones --
    // enabled, freewheel, latency -- are the host's own machinery and belong
    // in a user's parameter list no more than a bypass button does.
    if (p.isControl && p.isInput && p.symbol != "enabled" && p.symbol != "freewheel") {
      HostedParam hp;
      hp.id = (uint32_t) p.index;
      hp.name = p.name.empty() ? p.symbol : p.name;
      hp.minValue = p.minValue;
      hp.maxValue = p.maxValue;
      hp.defaultValue = p.defaultValue;
      hosted->params_.push_back(hp);
    }
  }

  // ── Factory presets, which LV2 keeps in the RDF rather than in the code ──
  //
  // The manifest announces each pset:Preset that applies to this plugin and
  // says which file defines it; the file holds the port values. That split is
  // deliberate on the plugin's side -- a host scanning a hundred bundles
  // should not read every preset of every one of them -- and it means finding
  // out what a preset DOES costs a second parse, done here once at load.
  {
    const std::string pset = "http://lv2plug.in/ns/ext/presets#";
    const std::string rdfType = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";

    // Symbol -> port index, because a preset names ports the way a human
    // wrote them and the control vector is indexed by number.
    std::map<std::string, int> bySymbol;
    for (const Lv2Port& p : hosted->ports_)
      if (p.isControl && p.isInput) bySymbol[p.symbol] = p.index;

    for (const auto& t : manifest.triples) {
      if (t.predicate != rdfType || t.object != pset + "Preset") continue;
      // appliesTo is not optional here: one bundle can hold several plugins,
      // and a preset for the other one is not this one's to show.
      bool applies = false;
      for (const std::string& to : manifest.objects(t.subject, lv2 + "appliesTo"))
        if (to == description.id) applies = true;
      if (!applies) continue;

      Lv2Preset preset;
      preset.uri = t.subject;
      preset.name = manifest.object(t.subject, rdfs + "label");
      if (preset.name.empty()) preset.name = t.subject;
      hosted->presets_.push_back(preset);
    }

    // The values, from whichever document actually carries them. A preset
    // defined inline in the manifest needs no second read; the usual case
    // points at presets.ttl through its own seeAlso.
    std::map<std::string, turtle::Document> parsed;
    for (Lv2Preset& preset : hosted->presets_) {
      std::vector<const turtle::Document*> sources;
      sources.push_back(&manifest);
      for (const std::string& also : manifest.objects(preset.uri, rdfs + "seeAlso")) {
        auto it = parsed.find(also);
        if (it == parsed.end()) {
          std::FILE* extra = std::fopen(also.c_str(), "rb");
          if (!extra) continue;
          std::string more;
          while ((got = std::fread(buffer, 1, sizeof(buffer), extra)) > 0) more.append(buffer, got);
          std::fclose(extra);
          turtle::Document doc;
          if (!turtle::parse(more, description.path, &doc)) continue;
          it = parsed.insert(std::make_pair(also, doc)).first;
        }
        sources.push_back(&it->second);
      }

      for (const turtle::Document* doc : sources) {
        for (const std::string& portNode : doc->objects(preset.uri, lv2 + "port")) {
          const std::string symbol = doc->object(portNode, lv2 + "symbol");
          const std::string value = doc->object(portNode, pset + "value");
          if (symbol.empty() || value.empty()) continue;
          auto found = bySymbol.find(symbol);
          if (found == bySymbol.end()) continue; // a port this build does not have
          preset.values.push_back(std::make_pair(found->second, (float) std::atof(value.c_str())));
        }
        if (!preset.values.empty()) break;
      }
    }
  }

  // -- The interface the bundle ships, if it ships one this platform can use --
  //
  // ui:ui points at a node; the node says what KIND it is and which binary
  // carries it. The kind matters more than it looks: a bundle built for X11
  // declares ui:X11UI, and embedding that into an HWND would produce a window
  // that never draws. Better to report no editor than a broken one.
  {
    const std::string ui = "http://lv2plug.in/ns/extensions/ui#";
    const std::string rdfType = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";
    for (const std::string& uiNode : all.objects(description.id, ui + "ui")) {
      bool rightKind = false;
      for (const auto& t : all.triples)
        if (t.subject == uiNode && t.predicate == rdfType && t.object == nativeLv2UiType())
          rightKind = true;
      if (!rightKind) continue;
      const std::string uiBinary = all.object(uiNode, ui + "binary");
      if (uiBinary.empty()) continue;
      hosted->uiUri_ = uiNode;
      // The same treatment lv2:binary gets: the parser resolved it against
      // the bundle, and the leaf goes back on this side of the separator.
      hosted->uiBinary_ = description.path + "/" + binaryLeaf(uiBinary);
      break;
    }
  }

  hosted->lib_ = openLib((description.path + "/" + binaryLeaf(binary)).c_str());
  if (!hosted->lib_) return nullptr;
  auto entry = (const LV2_Descriptor* (*) (uint32_t)) libSymbol(hosted->lib_, "lv2_descriptor");
  if (!entry) return nullptr;
  for (uint32_t i = 0;; ++i) {
    const LV2_Descriptor* d = entry(i);
    if (!d) break;
    if (d->URI && description.id == d->URI) {
      hosted->descriptor_ = d;
      break;
    }
  }
  if (!hosted->descriptor_) return nullptr;
  return std::unique_ptr<HostedPlugin>(hosted.release());
}

#endif // SONORE_HOST_LV2

// ── Scanning ─────────────────────────────────────────────────────────────────

/** Where a .clap's real binary lives.
 *
 *  On macOS a .clap is a BUNDLE -- a directory whose binary sits at
 *  Contents/MacOS/<name> -- exactly like a .vst3. On Windows and Linux the
 *  file IS the binary. dlopen on a directory simply fails, so without this a
 *  macOS scan finds every plugin by NAME and can describe none of them: the
 *  first macOS CI run reported an empty scan and a segfault in the host, both
 *  from this one missing hop.
 *
 *  The .vst3 resolver next to this one has always done it. CLAP never did,
 *  because CLAP was only ever loaded on the two platforms where the path and
 *  the binary are the same thing. */
inline std::string clapBinaryPath(const std::string& path) {
#if defined(__APPLE__)
  struct stat st {};
  if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return path;
  const size_t slash = path.find_last_of('/');
  std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
  if (leaf.size() > 5 && leaf.compare(leaf.size() - 5, 5, ".clap") == 0)
    leaf = leaf.substr(0, leaf.size() - 5);
  return path + "/Contents/MacOS/" + leaf;
#else
  return path;
#endif
}

/** Every plugin in one directory, described without instantiating any of them.
 *
 *  Not recursive, and that is on purpose: a scan that walks a whole drive is
 *  how a host ends up loading code it was never pointed at. A caller that
 *  wants a tree can walk it and call this per folder, which keeps the decision
 *  where it belongs. */
/** Every file in a folder that MIGHT be a plugin, by name alone.
 *
 *  Nothing is opened here. That separation is what lets a caller decide, per
 *  file, whether to load it -- which is the difference between a scan that
 *  can skip a plugin known to crash and one that cannot. */
inline std::vector<std::string> pluginFilesIn(const std::string& directory) {
  std::vector<std::string> candidates;

#if defined(_WIN32)
  WIN32_FIND_DATAA fd{};
  const std::string pattern = directory + "\\*";
  HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      const std::string name = fd.cFileName;
      if (name == "." || name == "..") continue;
      if ((name.size() > 5 && name.compare(name.size() - 5, 5, ".clap") == 0) ||
          (name.size() > 5 && name.compare(name.size() - 5, 5, ".vst3") == 0) ||
          (name.size() > 4 && name.compare(name.size() - 4, 4, ".lv2") == 0))
        candidates.push_back(directory + "\\" + name);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
  }
#else
  if (DIR* dir = opendir(directory.c_str())) {
    while (struct dirent* e = readdir(dir)) {
      const std::string name = e->d_name;
      if (name == "." || name == "..") continue;
      if ((name.size() > 5 && name.compare(name.size() - 5, 5, ".clap") == 0) ||
          (name.size() > 5 && name.compare(name.size() - 5, 5, ".vst3") == 0) ||
          (name.size() > 4 && name.compare(name.size() - 4, 4, ".lv2") == 0))
        candidates.push_back(directory + "/" + name);
    }
    closedir(dir);
  }
#endif
  // SORTED, because readdir hands them back in whatever order the filesystem
  // finds convenient -- inode order on ext4, which is neither alphabetical nor
  // stable. A host that scans a folder would list a user's plugins in a
  // different order on every machine, and a test that picks "the first one
  // called Synth" would pick a different FORMAT of the same plugin depending
  // on the platform. Windows happened to look sorted because NTFS keeps its
  // directory index that way; Linux does not.
  std::sort(candidates.begin(), candidates.end());
  return candidates;
}

/** Open ONE file and describe whatever plugins are inside it.
 *
 *  This is the dangerous call: it runs the file's own code. An empty result
 *  means the file is not a plugin this host understands, which is not an
 *  error and is not reported -- a scan that surfaces every unrelated file it
 *  walked past is a scan nobody reads. */
inline std::vector<PluginDescription> describeFile(const std::string& path) {
  std::vector<PluginDescription> found;
  const bool isClap = path.size() > 5 && path.compare(path.size() - 5, 5, ".clap") == 0;
  const bool isVst3 = path.size() > 5 && path.compare(path.size() - 5, 5, ".vst3") == 0;
  const bool isLv2 = path.size() > 4 && path.compare(path.size() - 4, 4, ".lv2") == 0;

  if (isClap) {
    LibHandle lib = openLib(clapBinaryPath(path).c_str());
    if (!lib) return found;
    const auto* entry = static_cast<const clap_plugin_entry_t*>(libSymbol(lib, "clap_entry"));
    if (entry && entry->init(path.c_str())) {
      const auto* factory =
          static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
      if (factory) {
        const uint32_t count = factory->get_plugin_count(factory);
        for (uint32_t i = 0; i < count; ++i) {
          const clap_plugin_descriptor_t* d = factory->get_plugin_descriptor(factory, i);
          if (!d || !d->id) continue;
          PluginDescription info;
          info.path = path;
          info.format = "CLAP";
          info.id = d->id;
          info.name = d->name ? d->name : "";
          info.vendor = d->vendor ? d->vendor : "";
          info.version = d->version ? d->version : "";
          for (const char* const* f = d->features; f && *f; ++f) {
            if (!info.features.empty()) info.features += ";";
            info.features += *f;
            if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0) info.isInstrument = true;
          }
          found.push_back(info);
        }
      }
      entry->deinit();
    }
    closeLib(lib);
    return found;
  }

#if defined(SONORE_HOST_VST3)
  if (isVst3) {
    withVst3Factory(path, [&](Steinberg_IPluginFactory* factory, const Steinberg_PClassInfo& info) {
      PluginDescription d;
      d.path = path;
      d.format = "VST3";
      // The class id as hex, because that is what identifies a plugin inside a
      // module and it has to survive being written into a session file.
      char hex[33] = {};
      for (int b = 0; b < 16; ++b)
        std::snprintf(hex + b * 2, 3, "%02X", (unsigned char) info.cid[b]);
      d.id = hex;
      d.name = info.name;
      // PClassInfo has no vendor or version; the richer PClassInfo2 does, and
      // asking for it is optional. Left empty rather than guessed.
      Steinberg_PFactoryInfo fi{};
      if (factory->lpVtbl->getFactoryInfo(factory, &fi) == Steinberg_kResultOk) d.vendor = fi.vendor;
      d.isInstrument = false; // decided below, from the subcategories
      Steinberg_IPluginFactory2* f2 = nullptr;
      if (factory->lpVtbl->queryInterface(factory, Steinberg_IPluginFactory2_iid, (void**) &f2) ==
              Steinberg_kResultOk &&
          f2) {
        const Steinberg_int32 n = f2->lpVtbl->countClasses(f2);
        for (Steinberg_int32 i = 0; i < n; ++i) {
          Steinberg_PClassInfo2 info2{};
          if (f2->lpVtbl->getClassInfo2(f2, i, &info2) != Steinberg_kResultOk) continue;
          if (std::memcmp(info2.cid, info.cid, sizeof(Steinberg_TUID)) != 0) continue;
          d.features = info2.subCategories;
          d.version = info2.version;
          if (d.vendor.empty()) d.vendor = info2.vendor;
          d.isInstrument = d.features.find("Instrument") != std::string::npos;
          break;
        }
        f2->lpVtbl->release(f2);
      }
      found.push_back(d);
    });
    return found;
  }
#endif
#if defined(SONORE_HOST_LV2)
  if (isLv2) {
    describeLv2Bundle(path, &found);
    return found;
  }
#endif
  (void) isVst3;
  (void) isLv2;
  return found;
}

/** Everything in a folder, loaded and described. One implementation, shared
 *  with the cached scan: the walk and the per-file load are the same code. */
inline std::vector<PluginDescription> scanDirectory(const std::string& directory) {
  std::vector<PluginDescription> found;
  for (const std::string& path : pluginFilesIn(directory)) {
    const std::vector<PluginDescription> inFile = describeFile(path);
    for (const PluginDescription& d : inFile) found.push_back(d);
  }
  return found;
}

// ── Loading ──────────────────────────────────────────────────────────────────

#if defined(SONORE_HOST_VST3)
/** Instantiate a VST3. Separate from the CLAP path because almost nothing
 *  about it is the same: a factory by class id, reference-counted objects, and
 *  an editor that may or may not be a second object. */
inline std::unique_ptr<HostedPlugin> loadVst3(const PluginDescription& description) {
  std::unique_ptr<Vst3Plugin> hosted(new Vst3Plugin());
  hosted->desc_ = description;

  hosted->lib_ = openLib(vst3BinaryPath(description.path).c_str());
  if (!hosted->lib_ || !vst3ModuleInit(hosted->lib_)) return nullptr;

  auto getFactory = (Steinberg_IPluginFactory * (*) ()) libSymbol(hosted->lib_,
                                                                  "GetPluginFactory");
  if (!getFactory) return nullptr;
  hosted->factory_ = getFactory();
  if (!hosted->factory_) return nullptr;

  // The description's id is the class id, printed as hex; find the class that
  // matches rather than trusting an index, which changes between versions.
  const Steinberg_int32 count = hosted->factory_->lpVtbl->countClasses(hosted->factory_);
  bool found = false;
  Steinberg_PClassInfo chosen{};
  for (Steinberg_int32 i = 0; i < count; ++i) {
    Steinberg_PClassInfo info{};
    if (hosted->factory_->lpVtbl->getClassInfo(hosted->factory_, i, &info) != Steinberg_kResultOk)
      continue;
    if (std::strcmp(info.category, "Audio Module Class") != 0) continue;
    char hex[33] = {};
    for (int b = 0; b < 16; ++b)
      std::snprintf(hex + b * 2, 3, "%02X", (unsigned char) info.cid[b]);
    if (description.id == hex) {
      chosen = info;
      found = true;
      break;
    }
  }
  if (!found) return nullptr;

  if (hosted->factory_->lpVtbl->createInstance(hosted->factory_, chosen.cid,
                                               Steinberg_Vst_IComponent_iid,
                                               (void**) &hosted->component_) !=
          Steinberg_kResultOk ||
      !hosted->component_)
    return nullptr;
  if (hosted->component_->lpVtbl->initialize(hosted->component_, nullptr) != Steinberg_kResultOk)
    return nullptr;
  if (hosted->component_->lpVtbl->queryInterface(hosted->component_,
                                                 Steinberg_Vst_IAudioProcessor_iid,
                                                 (void**) &hosted->processor_) !=
          Steinberg_kResultOk ||
      !hosted->processor_)
    return nullptr;

  // The editor may BE the component or may be a class of its own. Both are
  // legal and a host has to handle both, because which one it is decides
  // whether terminate() and release() are owed twice or once.
  if (hosted->component_->lpVtbl->queryInterface(hosted->component_,
                                                 Steinberg_Vst_IEditController_iid,
                                                 (void**) &hosted->controller_) ==
          Steinberg_kResultOk &&
      hosted->controller_) {
    hosted->controllerIsSeparate_ = false;
  } else {
    Steinberg_TUID controllerCid{};
    if (hosted->component_->lpVtbl->getControllerClassId(hosted->component_, controllerCid) ==
            Steinberg_kResultOk &&
        hosted->factory_->lpVtbl->createInstance(hosted->factory_, controllerCid,
                                                 Steinberg_Vst_IEditController_iid,
                                                 (void**) &hosted->controller_) ==
            Steinberg_kResultOk &&
        hosted->controller_) {
      hosted->controllerIsSeparate_ = true;
      hosted->controller_->lpVtbl->initialize(hosted->controller_, nullptr);
      // A separate controller starts blank: it has to be handed the
      // component's state or it will show defaults for a plugin that is not
      // at its defaults.
      std::vector<uint8_t> initial;
      MemoryStream stream(&initial);
      if (hosted->component_->lpVtbl->getState(hosted->component_,
                                               (Steinberg_IBStream*) &stream) ==
          Steinberg_kResultOk) {
        stream.rewind();
        hosted->controller_->lpVtbl->setComponentState(hosted->controller_,
                                                       (Steinberg_IBStream*) &stream);
      }
    }
  }

  if (hosted->controller_)
    hosted->controller_->lpVtbl->queryInterface(hosted->controller_,
                                                Steinberg_Vst_IMidiMapping_iid,
                                                (void**) &hosted->midiMapping_);

  // The handler goes in before anything else is asked of the controller. This
  // host had none at all, which meant a hosted plugin had nowhere to report an
  // edit made on its own face -- and plugins that check for one at
  // initialise time have every right to refuse without it.
  //
  // The pointers are to members, so they outlive every call the plugin can
  // make through them; params_ is still empty here and fills in a moment,
  // which costs nothing because an edit arriving before then simply names a
  // parameter the host has not read yet and is declined.
  if (hosted->controller_) {
    hosted->handler_.lpVtbl = componentHandlerVtbl();
    hosted->handler_.edits = &hosted->edits_;
    hosted->handler_.params = &hosted->params_;
    hosted->handler_.controller = hosted->controller_;
    hosted->controller_->lpVtbl->setComponentHandler(
        hosted->controller_, (Steinberg_Vst_IComponentHandler*) &hosted->handler_);
  }

  hosted->readCapabilities();
  return std::unique_ptr<HostedPlugin>(hosted.release());
}
#endif

/** Instantiate what a scan described. Returns null if the file moved, the id
 *  is gone, or the plugin refused to initialise: all of which happen to a
 *  real session opened on a different machine. */
inline std::unique_ptr<HostedPlugin> loadPlugin(const PluginDescription& description) {
#if defined(SONORE_HOST_VST3)
  if (description.format == "VST3") return loadVst3(description);
#endif
#if defined(SONORE_HOST_LV2)
  if (description.format == "LV2") return loadLv2(description);
#endif
  if (description.format != "CLAP") return nullptr;

  std::unique_ptr<ClapPlugin> hosted(new ClapPlugin());
  hosted->desc_ = description;
  hosted->wireCallbacks();

  hosted->lib_ = openLib(clapBinaryPath(description.path).c_str());
  if (!hosted->lib_) return nullptr;

  hosted->entry_ =
      static_cast<const clap_plugin_entry_t*>(libSymbol(hosted->lib_, "clap_entry"));
  if (!hosted->entry_ || !hosted->entry_->init(description.path.c_str())) {
    hosted->entry_ = nullptr; // never deinit an entry that failed to init
    return nullptr;
  }

  const auto* factory = static_cast<const clap_plugin_factory_t*>(
      hosted->entry_->get_factory(CLAP_PLUGIN_FACTORY_ID));
  if (!factory) return nullptr;

  hosted->plugin_ = factory->create_plugin(factory, &hosted->hostFacade_, description.id.c_str());
  if (!hosted->plugin_) return nullptr;
  if (!hosted->plugin_->init(hosted->plugin_)) {
    // init() failing means the plugin never came up; destroy is still owed.
    hosted->plugin_->destroy(hosted->plugin_);
    hosted->plugin_ = nullptr;
    return nullptr;
  }

  hosted->readCapabilities();
  hosted->crawlPresets(description.id.c_str());
  return std::unique_ptr<HostedPlugin>(hosted.release());
}

} // namespace host
} // namespace sonore
