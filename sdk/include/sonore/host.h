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
//   CLAP:   complete, and verified against every plugin this SDK builds.
//   VST3:   complete, same. Compiled in only where Steinberg's C API has been
//           vendored, because a user who wants CLAP hosting should not have to
//           fetch a second SDK to make a header compile.
//   AU / AUv3: macOS only, in host_au.h, which this file includes at its
//           end: the system registry scanned by type, AUv2 and AUv3 loaded,
//           rendered, automated, saved and edited. COMPILED for macOS and
//           never run -- there is no Mac in this loop -- and every Apple call
//           reports its failure by name; tests/mac_host_selftest.cpp is the
//           run a Mac owes it.
//   LV2:    complete, and the reason it took longest: an LV2 host cannot learn
//           a plugin's NAME without parsing RDF, so this needed turtle.h
//           first. Compiled in where the LV2 headers have been vendored.
//   LADSPA: complete for what the format has: audio and control ports, and
//           nothing else -- no MIDI, no editor, no presets. Always compiled
//           in, because ladspa_abi.h declares its ABI and there is no SDK to
//           vendor.
//   VST2:   never. Steinberg no longer licenses its SDK, and a host built on a
//           reconstruction of it carries the same risk a plugin would.
//
// THREADING follows the same rules the wrappers obey. scan(), load(),
// prepare(), state and parameter reads are main-thread. process() is the audio
// thread and allocates nothing: every buffer it needs is sized in prepare().

#pragma once

#include <cstdint>
#include <cstring>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <thread>
#include <map>
#include <memory>
#include <mutex>
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
#include "vst3_uid.h"
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
#include "ladspa_abi.h"
#include "number_text.h"
#include "turtle.h"
#include "path_io.h"
#include "files.h"

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

// ── Between threads ──────────────────────────────────────────────────────────

/** A bounded multi-producer, single-consumer queue: Vyukov's ring, each slot
 *  carrying a sequence number that says whose turn it is. A producer never
 *  waits -- a full queue refuses -- and nothing allocates after
 *  construction, so a UI thread, an automation thread and the audio thread
 *  itself can all push while whoever is running the plugin pops. */
template <typename T, size_t Capacity>
class MpscQueue {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "a power of two");

public:
  /** Empty, with room for Capacity values. The ring is a member array, so
   *  nothing is allocated now or later. */
  MpscQueue() {
    for (size_t i = 0; i < Capacity; ++i) slots_[i].seq.store(i, std::memory_order_relaxed);
  }

  /** [any thread] Copy `value` in. False, with nothing written, when the
   *  queue is full. Lock-free; it never waits for the consumer. */
  bool push(const T& value) {
    size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
      Slot& slot = slots_[pos & (Capacity - 1)];
      const size_t seq = slot.seq.load(std::memory_order_acquire);
      const std::ptrdiff_t lag = (std::ptrdiff_t) (seq - pos);
      if (lag == 0) {
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          slot.value = value;
          slot.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (lag < 0) {
        return false; // full: the consumer has not reached this slot yet
      } else {
        pos = head_.load(std::memory_order_relaxed);
      }
    }
  }

  /** The one consumer. More than one thread may take that role, but never at
   *  the same time, and the hand-over must itself synchronise. */
  bool pop(T& out) {
    Slot& slot = slots_[tail_ & (Capacity - 1)];
    const size_t seq = slot.seq.load(std::memory_order_acquire);
    if ((std::ptrdiff_t) (seq - (tail_ + 1)) < 0) return false; // empty
    out = slot.value;
    slot.seq.store(tail_ + Capacity, std::memory_order_release);
    ++tail_;
    return true;
  }

private:
  /** One cell of the ring. `seq` is whose turn it is: equal to the position,
   *  a producer may write it; one past, the consumer may read it. */
  struct Slot {
    std::atomic<size_t> seq{0};
    T value{};
  };
  Slot slots_[Capacity];
  std::atomic<size_t> head_{0};
  size_t tail_ = 0;
};

// ── Dynamic libraries ────────────────────────────────────────────────────────

#if defined(_WIN32)
/** A loaded library: an HMODULE on Windows, a dlopen() handle elsewhere.
 *  Null is none. */
using LibHandle = HMODULE;
// By UTF-8 path, as every path here is: LoadLibraryA read it as ANSI, so a
// plugin in a folder named in another script did not load.
//
// A file that is not a loadable image -- a truncated download, a plugin built
// for another architecture, a dependency missing -- made Windows raise its
// "Bad Image" hard-error box from inside LoadLibrary: modal, waiting for a
// click, on a desktop a scanning process may not even show, and the scan
// waited with it. A test scanning a corrupt plugin stood still for 33 minutes
// under ctest. The failure is a null handle instead: hard errors are turned
// off for this thread, for this call only, and the host's own mode restored.
/** Open a library by its UTF-8 path, running its initialisers. Null when
 *  `path` is null or the file does not load; closeLib() undoes it. */
inline LibHandle openLib(const char* path) {
  if (!path) return nullptr;
  DWORD previous = 0;
  const BOOL changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous);
  const LibHandle h = LoadLibraryW(pathio::widen(path).c_str());
  if (changed) SetThreadErrorMode(previous, nullptr);
  return h;
}
/** The address of an exported symbol; null when `h` is null or exports no
 *  such name. */
inline void* libSymbol(LibHandle h, const char* name) {
  return h ? (void*) GetProcAddress(h, name) : nullptr;
}
/** Close what openLib() opened; null is ignored. Every pointer into the
 *  library is dangling afterwards. */
inline void closeLib(LibHandle h) {
  if (h) FreeLibrary(h);
}
/** ".clap", a CLAP file's extension on every platform. */
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

// ── A CLAP module's entry, initialised once ──────────────────────────────────
//
// clap_entry.init() is called once per module before anything else and
// deinit() once after the last use. The OS shares one loaded module between
// every handle opened on it, so two instances of a plugin -- or an instance
// and a rescan -- share one entry. Each initialised it for itself and the
// first to finish deinitialised it: init while initialised, deinit while
// instances were alive, and a plugin that keeps module-wide state without a
// count of its own freed it under a live instance. Counted here instead, by
// the entry's address, which is the module's.
namespace detail {
struct ClapEntryCount {
  const clap_plugin_entry_t* entry;
  int users;
};
// Made once and never destroyed. A ClapPlugin can itself be a static -- or
// live in one -- and be destroyed during static destruction after these
// were: its release then locked a destroyed mutex and walked a destroyed
// vector, or skipped the deinit() it owed.
inline std::mutex& clapEntryLock() {
  static std::mutex* lock = new std::mutex;
  return *lock;
}
inline std::vector<ClapEntryCount>& clapEntryCounts() {
  static auto* counts = new std::vector<ClapEntryCount>;
  return *counts;
}
/** [main-thread] init() if this is the module's first user, else count one
 *  more. False if init() refused, and nothing is counted then. */
inline bool clapEntryAcquire(const clap_plugin_entry_t* entry, const char* path) {
  if (!entry) return false;
  std::lock_guard<std::mutex> guard(clapEntryLock());
  for (ClapEntryCount& c : clapEntryCounts())
    if (c.entry == entry) {
      ++c.users;
      return true;
    }
  if (!entry->init || !entry->init(path)) return false;
  clapEntryCounts().push_back({entry, 1});
  return true;
}
/** [main-thread] deinit() when the module's last user lets go. Before the
 *  library is closed, never after. */
inline void clapEntryRelease(const clap_plugin_entry_t* entry) {
  if (!entry) return;
  std::lock_guard<std::mutex> guard(clapEntryLock());
  auto& counts = clapEntryCounts();
  for (size_t i = 0; i < counts.size(); ++i)
    if (counts[i].entry == entry) {
      if (--counts[i].users == 0) {
        if (entry->deinit) entry->deinit();
        counts.erase(counts.begin() + (long) i);
      }
      return;
    }
}

/** A string a plugin wrote into a fixed-size array of its own: up to its
 *  terminator, or the array's end if it has none. A plugin that copies a
 *  string as long as the array -- strncpy does -- leaves no terminator, and
 *  read to the first zero byte a CLAP parameter's name ran on into its
 *  module, and a VST3 class's name, PClassInfo's last field, off the end of
 *  the struct into the host's stack. */
template <size_t N>
inline std::string fixedString(const char (&field)[N]) {
  return std::string(field, (size_t) (std::find(field, field + N, '\0') - field));
}

/** The most of each thing a plugin may say it has. A count is the plugin's
 *  word as much as a string is: one read from a field it never set says
 *  four billion, and the host reserved a HostedParam for each -- loadPlugin()
 *  threw std::bad_alloc -- or called a plugin's getClassInfo two billion
 *  times in a scan. Past these, a plugin is taken to have this many and is
 *  asked about no more: four thousand plugins in one file (LADSPA's bound
 *  already), a million parameters, sixty-five thousand presets. */
constexpr uint32_t kMaxPluginsPerFile = 4096;
constexpr uint32_t kMaxParameters = 1u << 20;
constexpr uint32_t kMaxPresets = 1u << 16;
/** A plugin's count, held to `limit`, and never below zero. */
template <typename Count>
inline uint32_t heldCount(Count reported, uint32_t limit) {
  if (!(reported > 0)) return 0;
  return (uint64_t) reported < limit ? (uint32_t) reported : limit;
}

/** The name this host gives plugins, and the lock it is read under. Made
 *  once and never destroyed, for the reason clapEntryLock() is. */
inline std::mutex& hostNameLock() {
  static std::mutex* lock = new std::mutex;
  return *lock;
}
inline std::string& hostNameStorage() {
  static auto* name = new std::string("Sonore Host");
  return *name;
}
} // namespace detail

/** [main-thread] The name a hosted plugin is told this host goes by: what a
 *  CLAP plugin reads in clap_host.name and a VST3 plugin asks its
 *  IHostApplication for. "Sonore Host" until an application says otherwise
 *  -- its own name is the useful answer, since plugins keep workarounds per
 *  host and show the name in their about boxes. A plugin loaded afterwards
 *  hears the new one; one already loaded keeps the name it was given. */
inline void setHostName(const std::string& utf8Name) {
  std::lock_guard<std::mutex> guard(detail::hostNameLock());
  detail::hostNameStorage() = utf8Name.empty() ? std::string("Sonore Host") : utf8Name;
}
/** [any thread] The name setHostName() last gave, or "Sonore Host". A copy,
 *  read under the lock. */
inline std::string hostName() {
  std::lock_guard<std::mutex> guard(detail::hostNameLock());
  return detail::hostNameStorage();
}

// ── What a scan finds ────────────────────────────────────────────────────────

/** One plugin, described without loading it.
 *
 *  A scan may find several plugins inside one file (CLAP's factory returns a
 *  list), so `path` alone does not identify anything. The pair (path, id) does,
 *  and that pair is what a host should store in its own session. */
struct PluginDescription {
  std::string path;   ///< The file or bundle on disk, UTF-8, as the scan found it.
  std::string format; ///< "CLAP", "VST3", "LV2", "LADSPA" or "AU": what loadPlugin() dispatches on.
  /** The plugin's own id within that file: CLAP's descriptor id, a VST3
   *  class id as text, an LV2 plugin URI, a LADSPA unique id in decimal, or
   *  an AU's "type:subtype:manufacturer". */
  std::string id;
  std::string name;    ///< The name to show a user, UTF-8.
  std::string vendor;  ///< Who made it; empty when the format or the plugin does not say.
  std::string version; ///< The plugin's version as text; empty when not given.
  std::string features; ///< The format's own category words, semicolon separated.
  bool isInstrument = false; ///< True when its categories say it is an instrument.
};

/** One parameter of a loaded plugin. */
struct HostedParam {
  /** The plugin's own handle for this parameter -- CLAP's clap_id, VST3's
   *  ParamID. Both are a 32-bit value the plugin chose; a host stores it and
   *  hands it back. Kept as a plain integer so talking about a VST3 parameter
   *  does not require CLAP's headers, or the other way round. */
  uint32_t id = 0;
  std::string name;   ///< The name the plugin gives it, UTF-8.
  std::string module; // the group path the plugin declared, may be empty
  double minValue = 0.0;     ///< The lowest PLAIN value, in the parameter's own units.
  double maxValue = 1.0;     ///< The highest PLAIN value.
  double defaultValue = 0.0; ///< The PLAIN value the plugin starts at.
  bool stepped = false; ///< True for a parameter that takes whole steps only: a switch, a choice.
};

/** A parameter's position, given the plugin's id for it.
 *
 *  Ids need not be 0..n-1, nor in declaration order, and the lookup runs once
 *  per event a plugin reports -- on the audio thread, for CLAP. Scanning the
 *  list made one event cost the parameter count, so a preset load reporting
 *  every one of 4000 parameters was millions of comparisons in one block.
 *  Hashed once when the list is read, at most half full so every probe run is
 *  short and ends; a lookup after that costs the same at any size. */
class ParamIdIndex {
public:
  /** Index `params` by id, replacing whatever was indexed before. Allocates:
   *  call it where the list is read, not on the audio thread. */
  void build(const std::vector<HostedParam>& params) {
    size_t size = 16;
    while (size < params.size() * 2) size <<= 1;
    slots_.assign(size, Slot{0, -1});
    mask_ = (uint32_t) (size - 1);
    for (size_t i = 0; i < params.size(); ++i) {
      const uint32_t id = params[i].id;
      uint32_t s = mix(id) & mask_;
      while (slots_[s].index >= 0 && slots_[s].id != id) s = (s + 1) & mask_;
      // An id declared twice keeps its first parameter, as scanning found.
      if (slots_[s].index < 0) slots_[s] = Slot{id, (int) i};
    }
  }

  /** -1 for an id the list does not hold, or before build(). */
  int find(uint32_t id) const {
    if (slots_.empty()) return -1;
    for (uint32_t s = mix(id) & mask_;; s = (s + 1) & mask_) {
      const Slot& slot = slots_[s];
      if (slot.index < 0) return -1;
      if (slot.id == id) return slot.index;
    }
  }

private:
  /** Murmur3's finaliser: ids a plugin numbered in steps, or by some hash of
   *  its own, still spread over the table. */
  static uint32_t mix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
  }

  /** The plugin's id and its position in the list; an index below zero is an
   *  empty slot, where a probe for an id that is not there stops. */
  struct Slot {
    uint32_t id;
    int index;
  };
  std::vector<Slot> slots_;
  uint32_t mask_ = 0;
};

// ── A loaded plugin ──────────────────────────────────────────────────────────

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
  /** What happened. */
  enum class Kind {
    kValue,        ///< The parameter moved to `value`.
    kGestureBegin, ///< The user took hold of it: what follows is one gesture.
    kGestureEnd    ///< The user let go.
  };
  Kind kind = Kind::kValue; ///< Which of the three this is.
  /** Index into the plugin's parameter list, not the format's own id. */
  int index = -1;
  /** Plain value, in the parameter's own units. Unset for gesture marks. */
  double value = 0.0;
};

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
class HostedPlugin {
public:
  /** For a format's own class. A host gets its plugins from loadPlugin(),
   *  as a std::unique_ptr that owns them. */
  HostedPlugin() = default;
  HostedPlugin(const HostedPlugin&) = delete;            ///< Not copyable: one instance, one owner.
  HostedPlugin& operator=(const HostedPlugin&) = delete; ///< Not copyable: one instance, one owner.
  /** [main-thread, audio stopped] Releases the plugin, closes its editor,
   *  destroys the instance and closes its library, in that order. */
  virtual ~HostedPlugin() = default;

  /** The description it was loaded from, as the caller passed it to
   *  loadPlugin(). Lives as long as the plugin. */
  virtual const PluginDescription& description() const = 0;
  /** True while an instance exists. loadPlugin() returns null rather than an
   *  invalid plugin, so anything it handed back answers true. */
  virtual bool isValid() const = 0;

  /** [main-thread, audio stopped] Allocate for this rate, the LARGEST block
   *  process() will be given, and this many channels. */
  virtual bool prepare(double sampleRate, uint32_t maxBlockSize, uint32_t numChannels = 2) = 0;
  /** [main-thread, audio stopped] The reverse of prepare(); harmless twice. */
  virtual void release() = 0;
  /** Clear tails, voices and filter state. With audio stopped, or from the
   *  thread that calls process() -- never beside a process() call. A format
   *  whose reset belongs to the audio thread (CLAP) applies it at the start
   *  of the next process() call. */
  virtual void reset() = 0;
  /** [audio-thread] One block of at most the maxBlockSize given to prepare().
   *  A longer block, or one before prepare(), is not processed: `io` is left
   *  as it came in and `midi` is not delivered, note-offs included. Split it
   *  instead -- PluginGraph does, in slices with their events -- because
   *  growing a buffer here would be an allocation on the audio thread. A
   *  CLAP plugin is started here, on the first block, and one that refuses
   *  to start is not processed either, and is asked again next block. Nor
   *  is an LV2 plugin during a loadState() on another thread: its format
   *  gives a state restore the instance to itself. */
  virtual void process(AudioBlock<float>& io, const MidiBuffer* midi = nullptr) = 0;

  /** [main-thread] How late the output is, in samples at the prepared rate:
   *  what a host delays its other paths by. 0 when the plugin reports none.
   *  It can change after a restart request; see takeRestartRequest(). */
  virtual uint32_t latencySamples() const = 0;
  /** [main-thread] True once since the plugin asked to be restarted -- CLAP's
   *  request_restart or latency.changed, VST3's restartComponent with
   *  kLatencyChanged -- which is how a plugin whose latency moved gets it
   *  applied: the host prepares it again. A format with no such request
   *  answers false, and a graph still notices the latency itself moving. */
  virtual bool takeRestartRequest() { return false; }
  /** [main-thread] True once since the plugin said its state changed in a way
   *  no parameter shows -- a sampler loaded a file through its own browser --
   *  which is the host's cue to mark the session unsaved. CLAP's
   *  state.mark_dirty, VST3's IComponentHandler2::setDirty; a format that
   *  cannot say so answers false. */
  virtual bool takeStateDirty() { return false; }
  /** [main-thread] How long the plugin keeps sounding after its input stops,
   *  in samples: CLAP's tail extension, VST3's getTailSamples(). 0 when the
   *  format has no way to say (LV2, LADSPA) or the plugin reports none. */
  virtual uint32_t tailSamples() const = 0;
  /** True when the plugin takes the notes of `midi`: a CLAP note port (one
   *  that speaks only CLAP's own note events is handed the note-ons and
   *  note-offs, and nothing else), a VST3 event input, an LV2 atom input.
   *  False is not the whole story: a VST3 plugin's MIDI-mapped controllers
   *  reach its parameters either way, with or without an event input. */
  virtual bool acceptsMidi() const = 0;
  /** False for a plugin with no audio input, an instrument or a generator:
   *  what comes in through `io` is not heard. */
  virtual bool hasAudioInput() const = 0;

  /** How many parameters the host lists; an index runs 0 to this minus one.
   *  Not listed: VST3's hidden parameters, and an LV2 plugin's enabled and
   *  freewheel ports, which are the host's machinery. */
  virtual int numParameters() const = 0;
  /** The parameter at `index`, which must be in range: it is not checked.
   *  The reference lasts until the list is rebuilt: by a CLAP plugin that
   *  rescans all of its parameters while released, or by a LADSPA one
   *  prepared at a new rate. */
  virtual const HostedParam& parameter(int index) const = 0;
  /** [main-thread] The current value, PLAIN, in the parameter's own units
   *  between its minValue and maxValue. 0 for an index out of range. */
  virtual double parameterValue(int index) const = 0;
  /** [main-thread] Set a parameter to a PLAIN value, in its own units. CLAP
   *  and VST3 queue it for the next process() call (CLAP's queue may also be
   *  pushed from the audio thread); LV2 and LADSPA clamp it to the range and
   *  the plugin reads it at its next block. False for an index out of range,
   *  or, for CLAP, when 64 changes are already waiting for the next block. */
  virtual bool setParameterValue(int index, double value) = 0;
  /** [main-thread] How the plugin would print `value`, a plain value of this
   *  parameter: "-6.0 dB", "2.30 kHz", "On". CLAP and VST3 ask the plugin;
   *  LV2 and LADSPA build it from what the port declares. Empty when the
   *  plugin has no text for it or the index is out of range. */
  virtual std::string parameterText(int index, double value) const = 0;

  /** [main-thread] Write everything the plugin needs to come back as it is
   *  into `out`, which is cleared first. Give the bytes back to loadState()
   *  of the same plugin: for VST3 (with a controller state), LV2 and LADSPA
   *  they are framed in this host's own layout. False when the plugin keeps
   *  no state or refused to save it. */
  virtual bool saveState(std::vector<uint8_t>& out) const = 0;
  /** [main-thread] Restore what saveState() wrote, `size` bytes at `data`;
   *  nothing is kept once this returns. False for a blob the plugin or this
   *  host rejects; LV2 and LADSPA then apply none of it. */
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
  /** [main-thread] True when the plugin's own bypass is on. False when it is
   *  off or the plugin has none (see hasBypass()). */
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
  /** The name of preset `index`, 0 to numPresets() minus one; empty out of
   *  range. */
  virtual std::string presetName(int index) const = 0;
  /** [main-thread] Recall preset `index`. False out of range, or when the
   *  plugin refuses or has no way to load one. Not every format reports the
   *  values that moved, so a caller showing them reads them again with
   *  parameterValue(). */
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
  /** [main-thread] Create the editor inside `parent` and show it. False when
   *  one is already open, `parent` is null, or the plugin has no editor for
   *  this platform's windows or refused to make one; nothing is left open
   *  then. */
  virtual bool openEditor(void* parent) = 0;
  /** [main-thread] Take the editor out of `parent` and destroy it. Harmless
   *  when none is open; the plugin's destructor calls it too. */
  virtual void closeEditor() = 0;
  /** Call regularly on the main thread while an editor is open.
   *
   *  Only LV2 needs it, and it needs it badly: the format has no message loop
   *  of its own, so the bundle asks for ui:idleInterface and the host is what
   *  drives it. The other two run on the platform's own loop and this costs
   *  them nothing, which is why it is one call a caller can make blindly
   *  rather than a question it has to ask first.
   *
   *  It is also where an editor that went away by itself is found out -- a
   *  CLAP one that reported itself destroyed, an LV2 one whose idle() said
   *  it closed: it is cleaned up here, editorSize() answers false from then
   *  on, and openEditor() makes a new one. */
  virtual void idleEditor() = 0;
  /** The size the plugin wants, in pixels, only meaningful while open. An
   *  editor that resizes itself -- its corner dragged, its zoom changed -- is
   *  let (CLAP's request_resize, VST3's resizeView), and this is where the
   *  caller hears of it: it sizes its own window to follow. */
  virtual bool editorSize(uint32_t& width, uint32_t& height) const = 0;

  /** [main-thread] The caller's window was resized -- its corner dragged,
   *  the user's choice of size: ask the open editor to be `width` x `height`
   *  pixels. The plugin may refuse, or take the nearest size it can: true
   *  when it took a size, and editorSize() then answers which. An editor
   *  that cannot be resized answers false, and the caller keeps its window
   *  at editorSize(). Before this a host could only follow an editor's own
   *  resizing, never ask for one. */
  virtual bool setEditorSize(uint32_t width, uint32_t height) {
    (void) width;
    (void) height;
    return false;
  }

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
  /** [main-thread, audio stopped] Release, close the editor, destroy the
   *  instance, give up the module's entry and close the library. */
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

    activating_ = true;
    const bool activated = plugin_->activate(plugin_, sampleRate_, 1, maxBlock_);
    activating_ = false;
    if (!activated) return false;
    active_ = true;

    // A buffer for every port the plugin declared, as wide as that port --
    // not one each way as wide as the caller: CLAP requires the count and the
    // widths to match, and a plugin reading its sidechain or its second
    // channel without checking is within its rights. A plugin with no
    // audio-ports extension has declared nothing, and gets what it always got
    // here: one port each way, the caller's width.
    if (portsDeclared_) {
      layoutPorts(inPortWidth_, inStorage_, inPtrs_, inBuses_);
      layoutPorts(outPortWidth_, outStorage_, outPtrs_, outBuses_);
    } else {
      const std::vector<uint32_t> one(1, channels_);
      layoutPorts(one, inStorage_, inPtrs_, inBuses_);
      layoutPorts(one, outStorage_, outPtrs_, outBuses_);
    }
    // The event buffers were sized at load, for every flush and block alike:
    // see reserveEvents().

    // Not started here: start_processing() is the audio thread's, and the
    // first process() makes the call. A plugin may set that thread up in it
    // -- its denormal mode, its priority, the id its own checks compare
    // against -- and from prepare() all of that went to the main thread.
    resetPending_.store(false, std::memory_order_relaxed);
    return true;
  }

  /** Stop and free. Called by prepare() before re-preparing and by the
   *  destructor; calling it twice is harmless. */
  void release() override {
    if (!plugin_) return;
    if (processing_) {
      // CLAP asks for this on the audio thread too, but audio has stopped
      // by now and there is no audio thread left to ask: it is called here,
      // after the last block has returned, and before deactivate() as CLAP
      // requires.
      plugin_->stop_processing(plugin_);
      processing_ = false;
    }
    if (active_) {
      plugin_->deactivate(plugin_);
      active_ = false;
    }
  }

  /** Clear whatever the plugin is holding: tails, voices, filter state.
   *  CLAP's reset() is the audio thread's, so it is made at the start of the
   *  next block, before the plugin processes it. */
  void reset() override {
    if (plugin_ && active_) resetPending_.store(true, std::memory_order_release);
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
    if (!plugin_) return;

    // The plugin is never in process() and flush() at once. Only a
    // main-thread flush holds this against us, and drainParameterEdits()
    // takes it only once audio has been idle for a tenth of a second -- so
    // the most this ever waits is the first block after a stop, for the
    // microseconds a flush takes.
    int expected = kGateFree;
    while (!gate_.compare_exchange_weak(expected, kGateProcessing, std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
      expected = kGateFree;
      std::this_thread::yield();
    }
    // This block's emissions only -- cleared before anything can return, so a
    // block that is not processed hands on nothing, not the MIDI of the last
    // one that was: an arpeggio's last notes, replayed downstream on every
    // block the plugin refused to start for, or that was too long or empty.
    // Under the gate, because a main-thread flush writes here too.
    produced_.clear();
    const uint32_t frames = (uint32_t) io.getNumSamples();
    if (!active_ || frames == 0 || frames > maxBlock_) {
      gate_.store(kGateFree, std::memory_order_release);
      return;
    }

    // Started on this thread, which is the one CLAP gives it to. A plugin
    // may refuse, and then it is not processed: the block is left as it came
    // in, and it is asked again on the next one.
    if (!processing_) {
      if (!plugin_->start_processing(plugin_)) {
        gate_.store(kGateFree, std::memory_order_release);
        return;
      }
      processing_ = true;
    }
    if (resetPending_.exchange(false, std::memory_order_acquire)) plugin_->reset(plugin_);

    // The caller's channels meet the MAIN input; every other input is
    // silent. A mono caller is heard on every channel of a wider port, and a
    // narrower port takes the caller's first channels.
    const uint32_t chans = (uint32_t) io.getNumChannels();
    for (size_t port = 0; port < inBuses_.size(); ++port) {
      const clap_audio_buffer_t& bus = inBuses_[port];
      for (uint32_t c = 0; c < bus.channel_count; ++c) {
        const uint32_t from = chans == 1 ? 0 : c;
        const float* src = (int) port == mainIn_ && from < chans ? io.getChannelPointer(from)
                                                                 : nullptr;
        if (src) std::memcpy(bus.data32[c], src, frames * sizeof(float));
        else std::memset(bus.data32[c], 0, frames * sizeof(float));
      }
    }
    for (float* out : outPtrs_) std::memset(out, 0, frames * sizeof(float));

    buildEvents(midi, frames);

    clap_process_t p{};
    p.steady_time = steadyTime_;
    p.frames_count = frames;
    p.transport = nullptr;
    p.audio_inputs = inBuses_.data();
    p.audio_outputs = outBuses_.data();
    p.audio_inputs_count = (uint32_t) inBuses_.size();
    p.audio_outputs_count = (uint32_t) outBuses_.size();
    p.in_events = &inEvents_;
    p.out_events = &outEvents_;
    plugin_->process(plugin_, &p);
    steadyTime_ += frames;

    // Back from the MAIN output only; aux outputs are the plugin's own. A
    // caller wider than the port gets its last channel repeated -- so a mono
    // output fills every channel -- and a plugin with no output port at all
    // leaves silence.
    const clap_audio_buffer_t* mainOut =
        mainOut_ >= 0 && outBuses_[(size_t) mainOut_].channel_count > 0
            ? &outBuses_[(size_t) mainOut_]
            : nullptr;
    for (uint32_t c = 0; c < chans; ++c) {
      float* dst = io.getChannelPointer(c);
      if (mainOut) {
        const uint32_t from = c < mainOut->channel_count ? c : mainOut->channel_count - 1;
        std::memcpy(dst, mainOut->data32[from], frames * sizeof(float));
      } else {
        std::memset(dst, 0, frames * sizeof(float));
      }
    }
    lastProcessNs_.store(steadyNs(), std::memory_order_relaxed);
    gate_.store(kGateFree, std::memory_order_release);
  }

  /** What the host must compensate for, in samples. */
  uint32_t latencySamples() const override {
    if (!plugin_) return 0;
    const auto* ext = static_cast<const clap_plugin_latency_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_LATENCY));
    return ext ? ext->get(plugin_) : 0;
  }

  bool takeRestartRequest() override {
    return restartRequested_.exchange(false, std::memory_order_acq_rel);
  }

  bool takeStateDirty() override {
    serviceMainThreadCallback();
    return stateDirty_.exchange(false, std::memory_order_acq_rel);
  }

  /** How long it keeps sounding after its input stops. */
  uint32_t tailSamples() const override {
    if (!plugin_) return 0;
    const auto* ext =
        static_cast<const clap_plugin_tail_t*>(plugin_->get_extension(plugin_, CLAP_EXT_TAIL));
    return ext ? ext->get(plugin_) : 0;
  }

  // A port that speaks only CLAP's note dialect is handed the notes of
  // `midi` too, as CLAP note events, so it takes MIDI as a VST3 event input
  // does (notes, and nothing else as events). It answered false, and a caller
  // that asks first -- what this is for -- sent such an instrument no notes.
  bool acceptsMidi() const override { return acceptsMidi_ || notesAsClap_; }
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
    // Refused rather than grown: growing would be an allocation, and this is
    // documented as safe to call from the audio thread. A host that queues
    // more than 64 changes in one block is automating faster than any block
    // can express anyway.
    //
    // From ANY thread, while the plugin processes: the queue is lock-free
    // and process() takes what it holds at the start of the block. It was a
    // vector, cleared at the END of the block, so a change pushed from the
    // UI while the plugin was inside process() -- most of the time, on a
    // busy audio thread -- was thrown away: 98 of 100 in the test.
    return queuedParams_.push({(clap_id) params_[(size_t) index].id, value});
  }

  /** How the plugin itself would print a value ("-6.0 dB", "2.30 kHz"). */
  std::string parameterText(int index, double value) const override {
    if (!paramsExt_ || index < 0 || index >= numParameters()) return std::string();
    char buffer[128] = {};
    if (!paramsExt_->value_to_text(plugin_, (clap_id) params_[(size_t) index].id, value, buffer,
                                   sizeof(buffer)))
      return std::string();
    return detail::fixedString(buffer); // the plugin may fill all of it
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
    acknowledgeLostEditor();
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
    // Before show(), which is where many an editor first asks for its size.
    resizeWanted_.store(false, std::memory_order_relaxed);
    editorShown_.store(true, std::memory_order_release);
    guiExt_->show(plugin_);
    editorOpen_ = true;
    applyResizeRequest();
    return true;
  }

  void closeEditor() override {
    acknowledgeLostEditor();
    if (!editorOpen_ || !guiExt_) return;
    editorShown_.store(false, std::memory_order_release);
    guiExt_->hide(plugin_);
    guiExt_->destroy(plugin_);
    guiExt_ = nullptr;
    editorOpen_ = false;
  }

  /** A CLAP editor lives on the platform's own message loop, which the caller
   *  is already pumping to have a window at all. What is owed here is
   *  answering an editor that destroyed itself, and one that asked to be
   *  another size (hostGuiRequestResize). */
  void idleEditor() override {
    acknowledgeLostEditor();
    applyResizeRequest();
    serviceMainThreadCallback();
  }

  /** [main-thread] Answer a request_callback: call the plugin's
   *  on_main_thread(). Nothing did, so a plugin that asked -- the SDK's own,
   *  to tell the host its state changed -- was never called back, and the
   *  news it was holding never arrived. Done here, from idleEditor() and
   *  drainParameterEdits(), the two calls a host makes on its main thread
   *  regularly, and from takeStateDirty(). */
  void serviceMainThreadCallback() {
    if (!plugin_ || !callbackRequested_.exchange(false, std::memory_order_acq_rel)) return;
    if (plugin_->on_main_thread) plugin_->on_main_thread(plugin_);
  }

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
    if (!editorOpen_ || !guiExt_ || editorLost_.load(std::memory_order_acquire)) return false;
    return guiExt_->get_size(plugin_, &width, &height);
  }

  /** CLAP's way: an editor that can_resize() is asked to adjust the size to
   *  one it can take, then given it with set_size(). */
  bool setEditorSize(uint32_t width, uint32_t height) override {
    if (!editorOpen_ || !guiExt_ || editorLost_.load(std::memory_order_acquire)) return false;
    if (width == 0 || height == 0 || !guiExt_->can_resize || !guiExt_->set_size) return false;
    if (!guiExt_->can_resize(plugin_)) return false;
    if (guiExt_->adjust_size) guiExt_->adjust_size(plugin_, &width, &height);
    return guiExt_->set_size(plugin_, width, height);
  }

  size_t drainParameterEdits(std::vector<ParamEdit>& out) override {
    serviceMainThreadCallback();
    // Two things can be owed at this point, and both are settled before the
    // vector is handed over -- otherwise a caller that loads a preset and
    // drains immediately, without processing a block first, learns nothing.
    //
    // The flush runs HERE only while audio is idle. An active plugin's flush
    // belongs to the audio thread and must never overlap process(): while
    // blocks are flowing, the next process() delivers and collects exactly
    // what a flush would, and the request stays up until audio stops. When
    // it has stopped -- the transport-stopped preset load -- the gate keeps
    // a process() that starts meanwhile from running beside it.
    if (paramsExt_ && paramsExt_->flush && flushRequested_.load(std::memory_order_acquire)) {
      const int64_t idleFor = steadyNs() - lastProcessNs_.load(std::memory_order_relaxed);
      int expected = kGateFree;
      if (idleFor > kIdleNs &&
          gate_.compare_exchange_strong(expected, kGateFlushing, std::memory_order_acquire)) {
        flushRequested_.store(false, std::memory_order_relaxed);
        buildEvents(nullptr, 0); // the host's own pending writes ride the same flush
        paramsExt_->flush(plugin_, &inEvents_, &outEvents_);
        gate_.store(kGateFree, std::memory_order_release);
      }
    }
    // What the plugin said, in order, collected by whichever thread ran it.
    size_t added = 0;
    ParamEdit edit;
    while (edits_.pop(edit)) {
      out.push_back(edit);
      ++added;
    }
    if (valuesStale_ && paramsExt_) {
      valuesStale_ = false;
      // A rescan says WHICH values are stale only in the coarsest sense, so
      // every one is re-read. Reporting a value that did not move is harmless;
      // missing one that did is a lane of automation that never gets written.
      for (size_t i = 0; i < params_.size(); ++i) {
        double value = 0.0;
        if (!paramsExt_->get_value(plugin_, (clap_id) params_[i].id, &value)) continue;
        edit.kind = ParamEdit::Kind::kValue;
        edit.index = (int) i;
        edit.value = value;
        out.push_back(edit);
        ++added;
      }
    }
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
    // a host, which is exactly why it goes back untouched -- and a preset
    // that is a file of its own has none, which the extension spells null.
    return ext->from_location(plugin_, p.locationKind,
                              p.location.empty() ? nullptr : p.location.c_str(),
                              p.loadKey.empty() ? nullptr : p.loadKey.c_str());
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
  /** Room for everything one block can carry: a full MidiBuffer and a full
   *  parameter queue. It was 8 KB, which holds 341 MIDI events, and what did
   *  not fit was dropped without a word -- the block's last note-offs first,
   *  so a dense block left notes hanging. */
  static constexpr size_t kMaxEventBytes =
      (size_t) MidiBuffer::kMaxEvents *
          (sizeof(clap_event_note_t) > sizeof(clap_event_midi_t) ? sizeof(clap_event_note_t)
                                                                  : sizeof(clap_event_midi_t)) +
      kMaxQueuedParams * sizeof(clap_event_param_value_t);
  static constexpr int kGateFree = 0, kGateProcessing = 1, kGateFlushing = 2;
  static constexpr int64_t kIdleNs = 100000000; // audio idle this long: flush here

  static int64_t steadyNs() {
    return (int64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  /** [main-thread] The plugin said its editor was destroyed -- a crashed
   *  renderer, a lost display connection -- and "the host must call
   *  clap_plugin_gui->destroy() to acknowledge the gui destruction". Not
   *  hide(): there is nothing left to hide. closed() is [thread-safe], so it
   *  only raises the flag and the acknowledgement waits for this thread. */
  void acknowledgeLostEditor() {
    if (!editorLost_.exchange(false, std::memory_order_acq_rel)) return;
    editorShown_.store(false, std::memory_order_release);
    if (editorOpen_ && guiExt_) guiExt_->destroy(plugin_);
    guiExt_ = nullptr;
    editorOpen_ = false;
  }

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
      detail::clapEntryRelease(entry_);
      entry_ = nullptr;
    }
    closeLib(lib_);
    lib_ = nullptr;
  }

  /** [main-thread] The event list's storage, sized once, at load.
   *
   *  Reserved for MIDI events AND queued parameter changes, because
   *  buildEvents() packs both into the same list. Sizing this for notes alone
   *  would have it grow the first time a block carried a full set of both --
   *  an allocation on the audio thread, in the one function that promises not
   *  to make any.
   *
   *  At load and not in prepare(), where it was: a flush the plugin asked
   *  for before its first prepare() -- from init(), or its editor -- packed
   *  the caller's queued changes into a buffer of no size. Nothing fitted,
   *  every change was popped off the queue anyway, and the plugin heard none
   *  of them, then or in the first block.
   *
   *  Once the note ports are read: the notes' share is for a plugin with one,
   *  because buildEvents() packs MIDI for no other, and the 57 KB of it was
   *  held by every effect for nothing. */
  void reserveEvents() {
    const bool notes = acceptsMidi_ || notesAsClap_;
    eventStorage_.assign(notes ? kMaxEventBytes : kMaxQueuedParams * sizeof(clap_event_param_value_t), 0);
    const size_t maxEvents = (notes ? (size_t) MidiBuffer::kMaxEvents : 0) + kMaxQueuedParams;
    eventOffsets_.reserve(maxEvents);
    eventPtrs_.reserve(maxEvents);
  }

  /** Pack this block's MIDI and queued parameter changes into one CLAP event
   *  list. Writes into buffers reserved by reserveEvents(); nothing here grows. */
  void buildEvents(const MidiBuffer* midi, uint32_t frames) {
    eventOffsets_.clear();
    eventPtrs_.clear();
    size_t used = 0;

    auto append = [&](const void* data, size_t bytes) {
      if (used + bytes > eventStorage_.size()) return;
      std::memcpy(eventStorage_.data() + used, data, bytes);
      eventOffsets_.push_back(used);
      used += bytes;
    };

    // At most what the queue holds, a block: the storage and the lists are
    // sized for that. A writer refilling the queue while this empties it
    // leaves the rest for the next block. They were popped regardless, and
    // dropped once the storage was full, or carried the lists past the room
    // reserved for them -- an allocation, here.
    QueuedParam q;
    for (size_t taken = 0; taken < kMaxQueuedParams && queuedParams_.pop(q); ++taken) {
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

    if (midi && (acceptsMidi_ || notesAsClap_)) {
      for (const auto& entry : *midi) {
        const MidiMessage& m = entry.getMessage();
        const int last = frames > 0 ? (int) frames - 1 : 0;
        const int at = entry.samplePosition < 0 ? 0 : entry.samplePosition;
        // A note port that speaks only CLAP's own dialect is given its notes
        // as CLAP note events; whatever has no CLAP spelling (a controller, a
        // bend) has nowhere to go on such a port and is left out.
        if (notesAsClap_) {
          if (!m.isNoteOn() && !m.isNoteOff()) continue;
          clap_event_note_t n{};
          n.header.size = sizeof(n);
          n.header.type = (uint16_t) (m.isNoteOn() ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF);
          n.header.time = (uint32_t) (at > last ? last : at);
          n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
          n.note_id = -1;
          n.port_index = 0;
          n.channel = (int16_t) (m.getChannel() - 1);
          n.key = (int16_t) m.getNoteNumber();
          n.velocity = m.getVelocity() / 127.0;
          append(&n, sizeof(n));
          continue;
        }
        clap_event_midi_t e{};
        e.header.size = sizeof(e);
        e.header.type = CLAP_EVENT_MIDI;
        // Inside the block, which CLAP requires of every event: a MidiBuffer
        // filled by a caller can hold a position at or past the end, and the
        // plugin was handed it as it was.
        e.header.time = (uint32_t) (at > last ? last : at);
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

  /** A parameter written between blocks, from any thread. CLAP takes
   *  parameter changes as events inside a block, so it waits here and becomes
   *  a CLAP_EVENT_PARAM_VALUE at the start of the next process(). */
  struct QueuedParam {
    clap_id id;
    double value;
  };

  /** A parameter's id is the PLUGIN's numbering and need not be its position.
   *  Everything a caller sees here is indexed by position, so the two have to
   *  be kept apart -- reporting an id as an index would name the wrong knob on
   *  any plugin whose ids are not 0..n-1. */
  int indexOfParamId(clap_id id) const { return paramIndex_.find((uint32_t) id); }

  /** Bounded because it is filled from the audio thread by a plugin that can
   *  emit as many events as it likes. A host that grew this without limit
   *  would allocate in the middle of a block on the plugin's say-so. */
  static constexpr size_t kMaxEdits = 4096;

  /** Filled by whichever thread runs the plugin -- the audio thread in
   *  process(), the main thread in an idle flush -- and emptied on the main
   *  thread by drainParameterEdits(). A vector shared that way was a race. */
  MpscQueue<ParamEdit, kMaxEdits> edits_;
  bool valuesStale_ = false;
  /** Atomic because request_flush is [thread-safe]: a plugin may ask from its
   *  own GUI or worker thread while the caller's main thread reads it. */
  std::atomic<bool> flushRequested_{false};
  /** request_restart is [thread-safe] too, and latency.changed sets it. */
  std::atomic<bool> restartRequested_{false};
  /** request_callback is [thread-safe]: a plugin asks from its audio thread
   *  to be called on the main one. Answered by serviceMainThreadCallback(). */
  std::atomic<bool> callbackRequested_{false};
  /** state.mark_dirty, [main-thread]; read by takeStateDirty(). */
  std::atomic<bool> stateDirty_{false};
  /** Inside activate(), where latency.changed() is how a plugin says what
   *  its latency now is -- not a request to be activated again. */
  bool activating_ = false;
  const clap_plugin_gui_t* guiExt_ = nullptr;
  double editorScale_ = 1.0;
  bool editorOpen_ = false;
  /** An editor is parented and shown: request_resize is answered. */
  std::atomic<bool> editorShown_{false};
  /** The size request_resize last accepted, for set_size() on the main
   *  thread; past 32768 pixels a side is no size a screen has. */
  static constexpr uint32_t kMaxEditorSide = 32768;
  std::atomic<bool> resizeWanted_{false};
  std::atomic<uint32_t> wantedWidth_{0}, wantedHeight_{0};
  /** [main-thread] What request_resize accepted, confirmed to the plugin. */
  void applyResizeRequest() {
    if (!resizeWanted_.exchange(false, std::memory_order_acq_rel)) return;
    if (!editorOpen_ || !guiExt_ || !guiExt_->set_size) return;
    guiExt_->set_size(plugin_, wantedWidth_.load(std::memory_order_relaxed),
                      wantedHeight_.load(std::memory_order_relaxed));
  }
  /** Raised by gui.closed(was_destroyed), from any thread; answered by
   *  acknowledgeLostEditor() on the main one. */
  std::atomic<bool> editorLost_{false};

  LibHandle lib_ = nullptr;
  const clap_plugin_entry_t* entry_ = nullptr;
  const clap_plugin_t* plugin_ = nullptr;
  const clap_plugin_params_t* paramsExt_ = nullptr;
  clap_host_t hostFacade_{};
  std::string hostName_;
  PluginDescription desc_;
  std::vector<HostedParam> params_;
  ParamIdIndex paramIndex_;

  double sampleRate_ = 48000.0;
  uint32_t maxBlock_ = 0;
  uint32_t channels_ = 2;
  bool active_ = false, processing_ = false;
  /** A reset() asked for, made by the next process(). */
  std::atomic<bool> resetPending_{false};
  bool acceptsMidi_ = false, hasAudioInput_ = true;
  /** The note port takes only CLAP's own note events, not MIDI. */
  bool notesAsClap_ = false;
  int bypassParam_ = -1;
  int64_t steadyTime_ = 0;

  std::vector<float> inStorage_, outStorage_;
  /** Every port's channel pointers, port after port; the buses index in. */
  std::vector<float*> inPtrs_, outPtrs_;
  std::vector<clap_audio_buffer_t> inBuses_, outBuses_;
  /** The audio ports as the plugin declared them: each one's width, in its
   *  own order, and which is main (-1 for none). Bounded, so a plugin that
   *  declares absurd numbers gets a host that runs out of ports, not memory. */
  static constexpr uint32_t kMaxPorts = 32, kMaxPortChannels = 64;
  bool portsDeclared_ = false;
  std::vector<uint32_t> inPortWidth_, outPortWidth_;
  int mainIn_ = 0, mainOut_ = 0;

  /** Storage, pointers and bus descriptions for ports of these widths. */
  void layoutPorts(const std::vector<uint32_t>& widths, std::vector<float>& storage,
                   std::vector<float*>& ptrs, std::vector<clap_audio_buffer_t>& buses) {
    size_t total = 0;
    for (uint32_t w : widths) total += w;
    storage.assign(total * maxBlock_, 0.0f);
    ptrs.assign(total, nullptr);
    for (size_t c = 0; c < total; ++c) ptrs[c] = storage.data() + c * maxBlock_;
    buses.assign(widths.size(), clap_audio_buffer_t{});
    size_t at = 0;
    for (size_t port = 0; port < widths.size(); ++port) {
      buses[port].data32 = ptrs.data() + at;
      buses[port].channel_count = widths[port];
      at += widths[port];
    }
  }

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

  MpscQueue<QueuedParam, kMaxQueuedParams> queuedParams_;
  /** Who is running the plugin: nobody, process() or a main-thread flush. */
  std::atomic<int> gate_{kGateFree};
  /** When the last block finished; zero until the first. */
  std::atomic<int64_t> lastProcessNs_{0};
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
      ParamEdit edit;
      edit.kind = ParamEdit::Kind::kValue;
      edit.index = self->indexOfParamId(e->param_id);
      edit.value = e->value;
      if (edit.index < 0) return false; // a parameter this host never saw
      return self->edits_.push(edit);   // false when full: bounded, never grown
    }
    if (header->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ||
        header->type == CLAP_EVENT_PARAM_GESTURE_END) {
      const auto* e = reinterpret_cast<const clap_event_param_gesture_t*>(header);
      ParamEdit edit;
      edit.kind = header->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? ParamEdit::Kind::kGestureBegin
                                                                 : ParamEdit::Kind::kGestureEnd;
      edit.index = self->indexOfParamId(e->param_id);
      if (edit.index < 0) return false;
      return self->edits_.push(edit);
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
    // ALL says the parameters themselves changed -- their number, ids,
    // ranges -- and CLAP allows it only while the plugin is deactivated, so
    // nothing on the audio thread is reading the list. It was taken for a
    // values rescan only, and the host went on with the old list.
    if ((flags & CLAP_PARAM_RESCAN_ALL) && !self->active_) self->readParams();
  }
  static void hostParamsClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
  /** [thread-safe] The plugin has parameter events waiting and is not being
   *  processed. Honoured on the next drain by calling params.flush(), which is
   *  the only way an edit made while the transport is stopped ever arrives. */
  static void hostParamsRequestFlush(const clap_host_t* host) {
    auto* self = static_cast<ClapPlugin*>(host->host_data);
    if (self) self->flushRequested_ = true;
  }

  /** [thread-safe] The plugin wants its editor a different size: its corner
   *  dragged, its zoom changed, a new scale.
   *
   *  The caller's window is still the caller's -- it follows the plugin
   *  through editorSize(), and resizes itself -- but the EDITOR is the
   *  plugin's, and is let change. This declined every request, so a plugin
   *  refused kept its old size, editorSize() never moved, and the caller had
   *  nothing to follow: no editor in this host could change size at all.
   *  Accepted while an editor is shown, and confirmed with set_size() on the
   *  main thread (applyResizeRequest), for a plugin that waits to be told
   *  rather than taking the size an accepted request gives it. */
  static bool hostGuiRequestResize(const clap_host_t* host, uint32_t width, uint32_t height) {
    auto* self = host ? static_cast<ClapPlugin*>(host->host_data) : nullptr;
    if (!self || !self->editorShown_.load(std::memory_order_acquire)) return false;
    if (width == 0 || height == 0 || width > kMaxEditorSide || height > kMaxEditorSide) return false;
    self->wantedWidth_.store(width, std::memory_order_relaxed);
    self->wantedHeight_.store(height, std::memory_order_relaxed);
    self->resizeWanted_.store(true, std::memory_order_release);
    return true;
  }
  static bool hostGuiRequestShow(const clap_host_t*) { return false; }
  static bool hostGuiRequestHide(const clap_host_t*) { return false; }
  static void hostGuiResizeHintsChanged(const clap_host_t*) {}
  static void hostGuiClosed(const clap_host_t* host, bool wasDestroyed) {
    // [thread-safe], so nothing here touches the editor: the main thread
    // answers on its next call. The first version cleared editorOpen_ for
    // both cases, from whatever thread this came on -- so a destroyed GUI was
    // never destroyed, the next openEditor() called create() on top of it,
    // and a merely CLOSED one (floating windows, which this host never asks
    // for) was never destroyed at all. Closed without being destroyed, the
    // GUI still exists, and closeEditor() still owes it hide and destroy.
    auto* self = static_cast<ClapPlugin*>(host->host_data);
    if (self && wasDestroyed) self->editorLost_.store(true, std::memory_order_release);
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
    if (id && std::strcmp(id, CLAP_EXT_LATENCY) == 0) {
      static const clap_host_latency_t ext = {hostLatencyChanged};
      return &ext;
    }
    if (id && std::strcmp(id, CLAP_EXT_STATE) == 0) {
      static const clap_host_state_t ext = {hostMarkDirty};
      return &ext;
    }
    return nullptr;
  }
  /** [thread-safe] The plugin needs deactivating and activating again --
   *  above all because its latency moved, which CLAP only lets it change
   *  while inactive. This did nothing, so a plugin with an oversampling
   *  switch kept the latency it was activated with for good. Recorded, for
   *  whoever owns the plugin to act on from the main thread. */
  static void hostRequestRestart(const clap_host_t* host) {
    if (host && host->host_data)
      static_cast<ClapPlugin*>(host->host_data)->restartRequested_.store(true, std::memory_order_release);
  }
  /** [main-thread & being-activated] The latency changed. CLAP allows that
   *  only inside activate(), and latencySamples() asks the plugin every time,
   *  so there it needs nothing. It was taken for a restart request: every
   *  plugin that announces its latency as it activates -- the SDK's own do --
   *  asked to be restarted by every prepare, the restart's own included.
   *  Outside activate(), where the format does not allow it, it still asks. */
  static void hostLatencyChanged(const clap_host_t* host) {
    auto* self = host ? static_cast<ClapPlugin*>(host->host_data) : nullptr;
    if (self && self->activating_) return;
    hostRequestRestart(host);
  }
  static void hostRequestProcess(const clap_host_t*) {}
  static void hostRequestCallback(const clap_host_t* host) {
    if (host && host->host_data)
      static_cast<ClapPlugin*>(host->host_data)->callbackRequested_.store(true, std::memory_order_release);
  }
  static void hostMarkDirty(const clap_host_t* host) {
    if (host && host->host_data)
      static_cast<ClapPlugin*>(host->host_data)->stateDirty_.store(true, std::memory_order_release);
  }

  void wireCallbacks() {
    inEvents_.ctx = this;
    inEvents_.size = eventsSize;
    inEvents_.get = eventsGet;
    outEvents_.ctx = this;
    outEvents_.try_push = eventsPush;

    hostFacade_.clap_version = CLAP_VERSION;
    hostFacade_.host_data = this;
    // A copy of its own: the plugin reads the pointer for as long as it
    // lives, and setHostName() may change the original meanwhile.
    hostName_ = hostName();
    hostFacade_.name = hostName_.c_str();
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
   *  A location is either inside the plugin (a container: every preset has a
   *  load key) or on disk. A FILE location that is a folder is the HOST's to
   *  crawl -- every file in it of a type the provider declared, folders
   *  inside it too -- and each file is asked about on its own: one preset,
   *  no load key, named by the provider or else by the file. This read only
   *  containers, and passed a folder's path to get_metadata as if it were a
   *  preset, which is how a plugin whose presets are files (the SDK's own,
   *  user_presets.h) came to publish none.
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
      /** The file types the provider declared, with their dots. */
      std::vector<std::string> extensions;

      /** The preset being described is finished: keep it if it is ours. A
       *  container's needs its load key; a file's is the file. */
      void commit() {
        const bool file = currentKind == CLAP_PRESET_DISCOVERY_LOCATION_FILE;
        if (pendingWanted && (!pendingKey.empty() || file)) {
          std::string name = pendingName;
          if (name.empty() && file) {
            name = fileName(currentLocation);
            const std::string ext = fileExtension(name);
            if (!ext.empty()) name.resize(name.size() - ext.size());
          }
          into->push_back({name, currentLocation, pendingKey, currentKind});
        }
        pendingWanted = false;
        pendingName.clear();
        pendingKey.clear();
      }
    } crawl;
    crawl.into = &presets_;
    crawl.wantPluginId = pluginId;

    clap_preset_discovery_indexer_t indexer{};
    indexer.clap_version = CLAP_VERSION;
    indexer.name = hostName_.c_str();
    indexer.vendor = "Sonorie";
    indexer.url = "";
    indexer.version = "1.0";
    indexer.indexer_data = &crawl;
    indexer.declare_filetype = [](const clap_preset_discovery_indexer_t* self,
                                  const clap_preset_discovery_filetype_t* type) {
      auto* c = static_cast<Crawl*>(self->indexer_data);
      if (c && type && type->file_extension && type->file_extension[0])
        c->extensions.push_back(std::string(".") + type->file_extension);
      return true;
    };
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
      c->commit();
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

    const uint32_t providers =
        detail::heldCount(factory->count(factory), detail::kMaxPluginsPerFile);
    for (uint32_t i = 0; i < providers; ++i) {
      const clap_preset_discovery_provider_descriptor_t* pd =
          factory->get_descriptor(factory, i);
      if (!pd || !pd->id) continue;
      const clap_preset_discovery_provider_t* provider =
          factory->create(factory, &indexer, pd->id);
      if (!provider) continue;
      if (provider->init(provider)) {
        auto describe = [&](uint32_t kind, const std::string& location) {
          crawl.currentKind = kind;
          crawl.currentLocation = location;
          crawl.pendingWanted = false;
          crawl.pendingKey.clear();
          crawl.pendingName.clear();
          provider->get_metadata(provider, kind, location.empty() ? nullptr : location.c_str(),
                                 &receiver);
          // The last preset described has no begin_preset after it to commit
          // it, so it is committed here.
          crawl.commit();
        };
        // A folder's files, and its folders' -- sorted, so the order is the
        // same on every filesystem, and only so deep, so a link back up the
        // tree cannot keep this walking.
        std::function<void(const std::string&, int)> walk = [&](const std::string& folder, int depth) {
          for (const FileEntry& e : listDirectory(folder, crawl.extensions)) {
            if (e.isDirectory) {
              if (depth < 8) walk(e.path, depth + 1);
            } else {
              describe(CLAP_PRESET_DISCOVERY_LOCATION_FILE, e.path);
            }
          }
        };
        for (size_t k = 0; k < crawl.locations.size(); ++k) {
          const uint32_t kind = crawl.locations[k].kind;
          const std::string& where = crawl.locationStrings[k];
          if (kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && isDirectory(where)) walk(where, 0);
          else describe(kind, where);
        }
      }
      provider->destroy(provider);
      crawl.locations.clear();
      crawl.locationStrings.clear();
      crawl.locationNames.clear();
      crawl.extensions.clear();
    }
  }

  /** [main-thread] The parameter list, read again from scratch.
   *
   *  Edits still waiting for the caller's drain name places in the list
   *  being replaced, so they go with it. A plugin going from 250 parameters
   *  to 10 (CLAP_PARAM_RESCAN_ALL, after a state load) handed the caller
   *  edits for places 10 to 249 of a list now 10 long, and a caller looking
   *  each one up read past its end. The values worth knowing are reported
   *  again: ALL marks every value stale, and the next drain re-reads them.
   *  Popped here, on the main thread, which is the queue's one consumer --
   *  CLAP allows ALL only while the plugin is inactive, so nothing pushes. */
  void readParams() {
    ParamEdit stale;
    while (edits_.pop(stale)) {
    }
    params_.clear();
    bypassParam_ = -1;
    paramsExt_ = static_cast<const clap_plugin_params_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_PARAMS));
    if (paramsExt_) {
      const uint32_t n = detail::heldCount(paramsExt_->count(plugin_), detail::kMaxParameters);
      params_.reserve(n < 4096 ? n : 4096); // past that, grown as the plugin proves it
      for (uint32_t i = 0; i < n; ++i) {
        clap_param_info_t info{};
        if (!paramsExt_->get_info(plugin_, i, &info)) continue;
        HostedParam p;
        p.id = (uint32_t) info.id;
        p.name = detail::fixedString(info.name);
        p.module = detail::fixedString(info.module);
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
    paramIndex_.build(params_); // even empty: an old index must not outlive its list
  }

  void readCapabilities() {
    readParams();
    const auto* notePorts = static_cast<const clap_plugin_note_ports_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_NOTE_PORTS));
    // The dialect the plugin's note port speaks. A host must send a port only
    // what it declares, and this one sent MIDI to every port -- a plugin
    // whose port speaks only CLAP's dialect heard no notes at all. MIDI where
    // the port takes it, as before; CLAP note events where it takes only
    // those; and a port that will not describe itself gets MIDI, as it did.
    acceptsMidi_ = notesAsClap_ = false;
    if (notePorts && notePorts->count(plugin_, true) > 0) {
      clap_note_port_info_t port{};
      if (notePorts->get(plugin_, 0, true, &port)) {
        const bool midi =
            (port.supported_dialects & (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE)) != 0;
        acceptsMidi_ = midi;
        notesAsClap_ = !midi && (port.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
      } else {
        acceptsMidi_ = true;
      }
    }
    const auto* audioPorts = static_cast<const clap_plugin_audio_ports_t*>(
        plugin_->get_extension(plugin_, CLAP_EXT_AUDIO_PORTS));
    portsDeclared_ = audioPorts != nullptr;
    inPortWidth_.clear();
    outPortWidth_.clear();
    mainIn_ = mainOut_ = 0;
    if (audioPorts) {
      for (int dir = 0; dir < 2; ++dir) {
        const bool isInput = dir == 0;
        std::vector<uint32_t>& widths = isInput ? inPortWidth_ : outPortWidth_;
        int& main = isInput ? mainIn_ : mainOut_;
        main = -1;
        const uint32_t n = (std::min)(audioPorts->count(plugin_, isInput), kMaxPorts);
        for (uint32_t i = 0; i < n; ++i) {
          clap_audio_port_info_t info{};
          // A port that will not describe itself is still a port: it keeps
          // its index, with no channels.
          const bool described = audioPorts->get(plugin_, i, isInput, &info);
          widths.push_back(described ? (std::min)(info.channel_count, kMaxPortChannels) : 0u);
          if (main < 0 && described && (info.flags & CLAP_AUDIO_PORT_IS_MAIN)) main = (int) i;
        }
        // The spec puts a main port first; one that flags none still has
        // the caller's audio routed to its first port rather than nowhere.
        if (main < 0 && !widths.empty()) main = 0;
      }
    }
    hasAudioInput_ = !audioPorts || !inPortWidth_.empty();
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

/** The folder inside a bundle's Contents that holds the binary THIS host can
 *  load: the VST3 module layout's name for the architecture the host was
 *  built for. It was x86_64 always, so a 32-bit host, or one built for ARM64
 *  -- Windows on Snapdragon, Linux on a Raspberry Pi -- looked in the x86_64
 *  folder and found nothing it could load. */
inline const char* vst3ArchFolder() {
#if defined(_WIN32)
#if defined(_M_ARM64EC)
  return "arm64ec-win";
#elif defined(_M_ARM64)
  return "arm64-win";
#elif defined(_M_IX86)
  return "x86-win";
#else
  return "x86_64-win";
#endif
#elif defined(__APPLE__)
  return "MacOS"; // one universal binary
#else
#if defined(__aarch64__)
  return "aarch64-linux";
#elif defined(__arm__)
  return "armv7l-linux";
#elif defined(__i386__)
  return "i386-linux";
#else
  return "x86_64-linux";
#endif
#endif
}

/** Where the real binary lives inside a .vst3, which may be a plain file. */
inline std::string vst3BinaryPath(const std::string& path) {
#if defined(_WIN32)
  const char* arch = vst3ArchFolder();
  const char* leafSuffix = ".vst3";
#elif defined(__APPLE__)
  const char* arch = vst3ArchFolder();
  const char* leafSuffix = "";
#else
  const char* arch = vst3ArchFolder();
  const char* leafSuffix = ".so";
#endif
  // A bare file is already the binary; this is what our own build produces and
  // what many Linux plugins ship.
  bool isDirectory = false;
#if defined(_WIN32)
  const DWORD attrs = GetFileAttributesW(pathio::widen(path.c_str()).c_str());
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
  const std::string own = path + "\\Contents\\" + arch + "\\" + leaf + leafSuffix;
#if defined(_M_ARM64) || defined(_M_ARM64EC)
  // An ARM64 process also loads an ARM64X binary, which the layout keeps in
  // a folder of its own: the one there is, when there is no plain one.
  if (GetFileAttributesW(pathio::widen(own.c_str()).c_str()) == INVALID_FILE_ATTRIBUTES) {
    const std::string arm64x = path + "\\Contents\\arm64x-win\\" + leaf + leafSuffix;
    if (GetFileAttributesW(pathio::widen(arm64x.c_str()).c_str()) != INVALID_FILE_ATTRIBUTES) return arm64x;
  }
#endif
  return own;
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
  // The library's own handle: the VST3 SDK keeps it to find the module's
  // path and resources. It was passed null.
  auto init = (bool (*)(void*)) libSymbol(lib, "ModuleEntry");
  return !init || init(lib);
#endif
}

/** ...and its exit, once for every init that SUCCEEDED and never otherwise.
 *  Only ExitDll was ever looked up, so on macOS and Linux a module was never
 *  told it was leaving; and it was called after an InitDll that had refused,
 *  telling a module that never started to shut down. */
inline void vst3ModuleExit(LibHandle lib) {
#if defined(_WIN32)
  if (auto exitFn = (bool (*)()) libSymbol(lib, "ExitDll")) exitFn();
#elif defined(__APPLE__)
  if (auto exitFn = (bool (*)()) libSymbol(lib, "bundleExit")) exitFn();
#else
  if (auto exitFn = (bool (*)()) libSymbol(lib, "ModuleExit")) exitFn();
#endif
}

/** Open a .vst3 and read its factory's class list without instantiating
 *  anything. Shared by the scanner and the loader so the two cannot drift. */
template <typename OnClass>
inline void withVst3Factory(const std::string& path, OnClass&& onClass) {
  LibHandle lib = openLib(vst3BinaryPath(path).c_str());
  if (!lib) return;
  const bool entered = vst3ModuleInit(lib);
  if (entered) {
    auto getFactory = (Steinberg_IPluginFactory * (*) ()) libSymbol(lib, "GetPluginFactory");
    if (getFactory) {
      if (Steinberg_IPluginFactory* factory = getFactory()) {
        const Steinberg_int32 count = (Steinberg_int32) detail::heldCount(
            factory->lpVtbl->countClasses(factory), detail::kMaxPluginsPerFile);
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
  if (entered) vst3ModuleExit(lib);
  closeLib(lib);
}

// ── The COM objects a host has to provide ────────────────────────────────────
//
// VST3 does not take a callback; it takes objects. Parameter automation
// arrives as an IParameterChanges holding one IParamValueQueue per parameter,
// and events as an IEventList. These are the smallest honest implementations:
// stack-lifetime, no reference counting to speak of, and no allocation once
// the vectors behind them are reserved.

/** IParamValueQueue: one parameter's points for this block, at sample
 *  offsets inside it. Kept in a vector reserved before processing, so a
 *  block's automation allocates nothing. */
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

/** IParameterChanges, the host's side: every parameter queue this block
 *  carries into process() as ProcessData::inputParameterChanges. */
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

/** The other direction: what the PROCESSOR changed during a block -- a meter
 *  it publishes as a read-only parameter, a value its DSP moved. Only the
 *  last point of each is kept, because this host is block-granular and what
 *  the controller and the caller need is where the value ended. */
struct OutParamQueue {
  Steinberg_Vst_IParamValueQueueVtbl* lpVtbl = nullptr;
  Steinberg_Vst_ParamID id = 0;
  double value = 0.0;
  Steinberg_int32 offset = 0;
  Steinberg_int32 points = 0;
  /** Index into the host's parameter list, or -1 for one it does not list. */
  int listed = -1;
};

inline Steinberg_Vst_ParamID SMTG_STDMETHODCALLTYPE opqGetId(void* self) {
  return static_cast<OutParamQueue*>(self)->id;
}
inline Steinberg_int32 SMTG_STDMETHODCALLTYPE opqGetPointCount(void* self) {
  return static_cast<OutParamQueue*>(self)->points;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE opqGetPoint(void* self, Steinberg_int32 index,
                                                            Steinberg_int32* offset,
                                                            Steinberg_Vst_ParamValue* value) {
  auto* q = static_cast<OutParamQueue*>(self);
  if (index < 0 || index >= q->points || !offset || !value) return Steinberg_kResultFalse;
  *offset = q->offset; // every index reads the last point: the only one kept
  *value = q->value;
  return Steinberg_kResultOk;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE opqAddPoint(void* self, Steinberg_int32 offset,
                                                            Steinberg_Vst_ParamValue value,
                                                            Steinberg_int32* index) {
  auto* q = static_cast<OutParamQueue*>(self);
  q->offset = offset;
  q->value = value;
  if (index) *index = q->points;
  ++q->points;
  return Steinberg_kResultOk;
}

inline Steinberg_Vst_IParamValueQueueVtbl* outParamQueueVtbl() {
  static Steinberg_Vst_IParamValueQueueVtbl v = {
      hostQueryNoInterface, hostAddRefStatic, hostReleaseStatic,
      opqGetId,             opqGetPointCount, opqGetPoint,
      opqAddPoint,
  };
  return &v;
}

/** IParameterChanges, the plugin's side: ProcessData::outputParameterChanges,
 *  where the DSP reports the parameters it moved itself. Read after the block
 *  and sent on to the main thread, where the controller and drainParameterEdits()
 *  hear about them. */
struct OutParameterChanges {
  Steinberg_Vst_IParameterChangesVtbl* lpVtbl = nullptr;
  /** Reserved before the block and never grown in it: a plugin reporting
   *  more parameters than there is room for is refused the rest. */
  std::vector<OutParamQueue>* queues = nullptr;
  /** Finding the queue a parameter already has, in constant time: the id's
   *  place in the list, then that place's queue (-1 for none yet). It was a
   *  walk over every queue so far, and a plugin reporting 4,000 parameters in
   *  one block -- a preset recalled on the audio thread -- spent 2.6 ms of a
   *  2.7 ms block in it. */
  const ParamIdIndex* index = nullptr;
  int32_t* slotOf = nullptr;
  size_t listedCount = 0;
  /** The few a host does not list, walked: capped, so they stay few. */
  static constexpr int kMaxUnlisted = 64;
  int32_t unlisted[kMaxUnlisted] = {};
  int unlistedCount = 0;
};

inline Steinberg_int32 SMTG_STDMETHODCALLTYPE opcCount(void* self) {
  auto* pc = static_cast<OutParameterChanges*>(self);
  return pc->queues ? (Steinberg_int32) pc->queues->size() : 0;
}
inline Steinberg_Vst_IParamValueQueue* SMTG_STDMETHODCALLTYPE opcGetData(void* self,
                                                                         Steinberg_int32 index) {
  auto* pc = static_cast<OutParameterChanges*>(self);
  if (!pc->queues || index < 0 || index >= (Steinberg_int32) pc->queues->size()) return nullptr;
  return (Steinberg_Vst_IParamValueQueue*) &(*pc->queues)[(size_t) index];
}
inline Steinberg_Vst_IParamValueQueue* SMTG_STDMETHODCALLTYPE
opcAddData(void* self, const Steinberg_Vst_ParamID* id, Steinberg_int32* index) {
  auto* pc = static_cast<OutParameterChanges*>(self);
  if (!pc->queues || !id) return nullptr;
  std::vector<OutParamQueue>& queues = *pc->queues;
  // One queue per parameter per block, as the interface promises: asking
  // again for the same id is handed the queue it already has.
  const int listed = pc->index ? pc->index->find((uint32_t) *id) : -1;
  const bool known = listed >= 0 && (size_t) listed < pc->listedCount && pc->slotOf;
  if (known) {
    const int32_t at = pc->slotOf[listed];
    if (at >= 0) {
      if (index) *index = at;
      return (Steinberg_Vst_IParamValueQueue*) &queues[(size_t) at];
    }
  } else {
    for (int k = 0; k < pc->unlistedCount; ++k) {
      const int32_t at = pc->unlisted[k];
      if (queues[(size_t) at].id != *id) continue;
      if (index) *index = at;
      return (Steinberg_Vst_IParamValueQueue*) &queues[(size_t) at];
    }
    if (pc->unlistedCount >= OutParameterChanges::kMaxUnlisted) return nullptr;
  }
  if (queues.size() >= queues.capacity()) return nullptr;
  OutParamQueue q;
  q.lpVtbl = outParamQueueVtbl();
  q.id = *id;
  q.listed = known ? listed : -1;
  queues.push_back(q);
  const auto at = (int32_t) (queues.size() - 1);
  if (known) pc->slotOf[listed] = at;
  else pc->unlisted[pc->unlistedCount++] = at;
  if (index) *index = at;
  return (Steinberg_Vst_IParamValueQueue*) &queues.back();
}

inline Steinberg_Vst_IParameterChangesVtbl* outParamChangesVtbl() {
  static Steinberg_Vst_IParameterChangesVtbl v = {
      hostQueryNoInterface, hostAddRefStatic, hostReleaseStatic,
      opcCount,             opcGetData,       opcAddData,
  };
  return &v;
}

struct ComponentHandler;
/** IComponentHandler2, the same object's second face: setDirty is how a
 *  VST3 plugin says its state changed where no parameter shows it. */
struct ComponentHandler2Face {
  Steinberg_Vst_IComponentHandler2Vtbl* lpVtbl = nullptr;
  ComponentHandler* self = nullptr;
};


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
  /** What a plugin reports through this object waits here for the owner's
   *  drainParameterEdits(). Bounded, and full refuses: see `edits`. */
  static constexpr size_t kMaxEdits = 4096;

  Steinberg_Vst_IComponentHandlerVtbl* lpVtbl = nullptr;
  ComponentHandler2Face second;
  // The format calls every method here from the UI thread, and plugins call
  // them from process() too -- an edit made by MIDI learn, a restart when
  // the latency moves -- so everything they write is safe from any thread,
  // as the CLAP side's callbacks are.
  /** setDirty(true) since the owner last asked. */
  std::atomic<bool> stateDirty{false};
  /** Set when the plugin reports kParamValuesChanged; the owner re-reads. */
  std::atomic<bool> valuesStale{false};
  /** Set when it reports kLatencyChanged or kIoChanged: it must be set up
   *  and activated again before the change applies. */
  std::atomic<bool> restartRequested{false};
  /** Gestures and values, for the owner's drain. A lock-free queue rather
   *  than the vector it was: an edit from the audio thread met the main
   *  thread copying and clearing that vector, and grew it, allocating, on
   *  the audio thread. */
  MpscQueue<ParamEdit, kMaxEdits>* edits = nullptr;
  /** id -> index, because the edits a caller sees are indexed by position. */
  const ParamIdIndex* params = nullptr;
  /** Where an edit goes on its way to the PROCESSOR: the owner's queue for
   *  the next process() call. Null until the plugin is fully loaded. */
  void (*forward)(void* owner, Steinberg_Vst_ParamID id, double normalized) = nullptr;
  void* owner = nullptr;
  /** Plain values, not normalised: the owner converts, since only it holds the
   *  controller needed to do it. Null until the plugin is fully loaded. */
  Steinberg_Vst_IEditController* controller = nullptr;
};

inline int chIndexOf(const ComponentHandler* h, Steinberg_Vst_ParamID id) {
  return h->params ? h->params->find((uint32_t) id) : -1;
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
  auto* h = static_cast<ComponentHandler*>(self);
  if (std::memcmp(iid, Steinberg_Vst_IComponentHandler2_iid, sizeof(Steinberg_TUID)) == 0 &&
      h->second.lpVtbl) {
    *obj = &h->second;
    return Steinberg_kResultOk;
  }
  *obj = nullptr;
  return Steinberg_kNoInterface;
}

inline Steinberg_tresult SMTG_STDMETHODCALLTYPE ch2Query(void* self, const Steinberg_TUID iid,
                                                         void** obj) {
  return chQuery(static_cast<ComponentHandler2Face*>(self)->self, iid, obj);
}
/** The plugin's state moved where no parameter shows it -- a file loaded
 *  through its own editor. Without this face the SDK's own VST3 wrapper,
 *  which says so here, was heard by every DAW and not by this host: a
 *  session closed without asking, and the file was lost. */
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE ch2SetDirty(void* self, Steinberg_TBool state) {
  if (state)
    static_cast<ComponentHandler2Face*>(self)->self->stateDirty.store(true, std::memory_order_release);
  return Steinberg_kResultOk;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE ch2RequestOpenEditor(void*, Steinberg_FIDString) {
  return Steinberg_kResultFalse; // the caller opens editors, not the plugin
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE ch2GroupEdit(void*) {
  return Steinberg_kResultOk; // every edit is already reported as it comes
}

inline Steinberg_Vst_IComponentHandler2Vtbl* componentHandler2Vtbl() {
  static Steinberg_Vst_IComponentHandler2Vtbl v = {
      ch2Query,     hostAddRefStatic, hostReleaseStatic, ch2SetDirty, ch2RequestOpenEditor,
      ch2GroupEdit, ch2GroupEdit,
  };
  return &v;
}

inline Steinberg_tresult SMTG_STDMETHODCALLTYPE chBeginEdit(void* self,
                                                            Steinberg_Vst_ParamID id) {
  auto* h = static_cast<ComponentHandler*>(self);
  if (!h->edits) return Steinberg_kResultFalse;
  const int index = chIndexOf(h, id);
  if (index < 0) return Steinberg_kResultFalse;
  // Refused when full, never grown: this may be the audio thread.
  return h->edits->push({ParamEdit::Kind::kGestureBegin, index, 0.0}) ? Steinberg_kResultOk
                                                                     : Steinberg_kResultFalse;
}

inline Steinberg_tresult SMTG_STDMETHODCALLTYPE chPerformEdit(void* self, Steinberg_Vst_ParamID id,
                                                              Steinberg_Vst_ParamValue value) {
  auto* h = static_cast<ComponentHandler*>(self);
  // To the PROCESSOR first, which is what performEdit is for: the controller
  // already has the value, and the DSP hears it only through the parameter
  // changes of a process() call the host makes. This recorded the edit for
  // automation and sent it nowhere, so a plugin whose controller is its own
  // object -- most of them -- never heard its own editor. Every parameter,
  // hidden ones included: those are the plugin's business, not the list's.
  if (h->forward) h->forward(h->owner, id, value);
  if (!h->edits) return h->forward ? Steinberg_kResultOk : Steinberg_kResultFalse;
  const int index = chIndexOf(h, id);
  if (index < 0) return h->forward ? Steinberg_kResultOk : Steinberg_kResultFalse;
  // VST3 reports normalised; everything a caller sees here is plain, so that
  // one hosting API means the same thing whichever format is underneath.
  double plain = value;
  if (h->controller)
    plain = h->controller->lpVtbl->normalizedParamToPlain(h->controller, id, value);
  h->edits->push({ParamEdit::Kind::kValue, index, plain});
  return Steinberg_kResultOk; // the processor has it, whether or not the caller's queue had room
}

inline Steinberg_tresult SMTG_STDMETHODCALLTYPE chEndEdit(void* self, Steinberg_Vst_ParamID id) {
  auto* h = static_cast<ComponentHandler*>(self);
  if (!h->edits) return Steinberg_kResultFalse;
  const int index = chIndexOf(h, id);
  if (index < 0) return Steinberg_kResultFalse;
  return h->edits->push({ParamEdit::Kind::kGestureEnd, index, 0.0}) ? Steinberg_kResultOk
                                                                   : Steinberg_kResultFalse;
}

inline Steinberg_tresult SMTG_STDMETHODCALLTYPE chRestartComponent(void* self,
                                                                   Steinberg_int32 flags) {
  auto* h = static_cast<ComponentHandler*>(self);
  // Recorded, not answered on the spot: the plugin is inside its own call and
  // reading every parameter back out of it from here would re-enter it.
  if (flags & Steinberg_Vst_RestartFlags_kParamValuesChanged)
    h->valuesStale.store(true, std::memory_order_release);
  // The latency moving was ignored like everything else here, so a graph
  // went on compensating for the old one -- a comb filter on every parallel
  // branch -- until something re-prepared it for another reason.
  if (flags & (Steinberg_Vst_RestartFlags_kLatencyChanged | Steinberg_Vst_RestartFlags_kIoChanged))
    h->restartRequested.store(true, std::memory_order_release);
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
 *  This one declined every request, "for the same reason the CLAP side does:
 *  the window belongs to the caller", and recorded it where no caller could
 *  read it. A view that waits for onSize() -- Steinberg's own do -- kept its
 *  size, editorSize() never moved, and a caller following it had nothing to
 *  follow. The request is granted now as Steinberg's host grants it: the
 *  view is given the size through onSize() before resizeView returns, and
 *  the caller's window follows through editorSize(). */
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
                                                             struct Steinberg_IPlugView* view,
                                                             struct Steinberg_ViewRect* rect) {
  auto* frame = static_cast<PlugFrame*>(self);
  if (!rect || !view) return Steinberg_kInvalidArgument;
  const int64_t width = (int64_t) rect->right - rect->left, height = (int64_t) rect->bottom - rect->top;
  // No size a screen has: refused, as a size of nothing is.
  if (width <= 0 || height <= 0 || width > 32768 || height > 32768) return Steinberg_kResultFalse;
  frame->requestedWidth = (uint32_t) width;
  frame->requestedHeight = (uint32_t) height;
  frame->resizeRequested = true;
  // onSize() inside resizeView, as Steinberg's own host calls it: that is
  // where a view takes the size it asked for, and kResultTrue says it has.
  Steinberg_ViewRect granted{0, 0, (Steinberg_int32) width, (Steinberg_int32) height};
  return view->lpVtbl->onSize(view, &granted) == Steinberg_kResultOk ? Steinberg_kResultTrue
                                                                    : Steinberg_kResultFalse;
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
  /** A stream over `buffer`, which the caller owns and keeps alive, with
   *  the cursor at its first byte. A write lands at the cursor, overwriting
   *  and growing the vector past its end. */
  explicit MemoryStream(std::vector<uint8_t>* buffer) : buffer_(buffer) { lpVtbl = vtbl(); }
  /** The cursor back to the first byte, for a second reader of the same
   *  bytes. */
  void rewind() { pos_ = 0; }

  Steinberg_IBStreamVtbl* lpVtbl = nullptr;

private:
  static MemoryStream* self(void* p) { return static_cast<MemoryStream*>(p); }

  static Steinberg_tresult SMTG_STDMETHODCALLTYPE msRead(void* p, void* dest,
                                                         Steinberg_int32 bytes,
                                                         Steinberg_int32* read) {
    MemoryStream* s = self(p);
    if (!dest || bytes < 0) return Steinberg_kInvalidArgument;
    const size_t left = s->pos_ < s->buffer_->size() ? s->buffer_->size() - s->pos_ : 0;
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
    // AT THE CURSOR, overwriting what is there and growing past the end. It
    // used to append whatever the cursor said, so a plugin that writes a
    // placeholder size, writes its data and seeks back to fill the size in --
    // an ordinary way to write a chunk -- produced a state with the size
    // tacked on at the end and the placeholder left in place.
    const auto* data = static_cast<const uint8_t*>(src);
    const size_t end = s->pos_ + (size_t) bytes;
    if (end > s->buffer_->size()) s->buffer_->resize(end, 0);
    if (bytes > 0) std::memcpy(s->buffer_->data() + s->pos_, data, (size_t) bytes);
    s->pos_ = end;
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
    // Past the end is allowed, as for a file: the next write fills the gap
    // with zeros, and a read there reads nothing.
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

/** IEventList: a block's notes and MIDI, in either direction -- the events
 *  handed to process(), and the ones the plugin sends back out. The vector
 *  behind it is reserved before the block. */
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
  // Controllers, channel pressure, pitch bend and program changes arrive as
  // kLegacyMIDICCOutEvent, poly pressure as its own event, SysEx as a data
  // event: all of them MIDI again here. They were refused, so a controller a
  // plugin emitted never reached the next plugin in a graph.
  if (e->type == Steinberg_Vst_Event_EventTypes_kLegacyMIDICCOutEvent) {
    const auto& cc = e->Steinberg_Vst_Event_midiCCOut;
    const int channel = cc.channel < 0 ? 0 : (cc.channel & 0x0F);
    const int value = cc.value & 0x7F, value2 = cc.value2 & 0x7F;
    if (cc.controlNumber < 128)
      el->produced->addEvent(MidiMessage(0xB0 | channel, cc.controlNumber, value), offset);
    else if (cc.controlNumber == Steinberg_Vst_ControllerNumbers_kAfterTouch)
      el->produced->addEvent(MidiMessage(0xD0 | channel, value, 0), offset);
    else if (cc.controlNumber == Steinberg_Vst_ControllerNumbers_kPitchBend)
      el->produced->addEvent(MidiMessage(0xE0 | channel, value, value2), offset);
    else if (cc.controlNumber == Steinberg_Vst_ControllerNumbers_kCtrlProgramChange)
      el->produced->addEvent(MidiMessage(0xC0 | channel, value, 0), offset);
    else
      return Steinberg_kNotImplemented;
    return Steinberg_kResultOk;
  }
  if (e->type == Steinberg_Vst_Event_EventTypes_kPolyPressureEvent) {
    const auto& pp = e->Steinberg_Vst_Event_polyPressure;
    int pressure = (int) (pp.pressure * 127.0f + 0.5f);
    pressure = pressure < 0 ? 0 : (pressure > 127 ? 127 : pressure);
    el->produced->addEvent(
        MidiMessage(0xA0 | (pp.channel < 0 ? 0 : (pp.channel & 0x0F)), pp.pitch & 0x7F, pressure),
        offset);
    return Steinberg_kResultOk;
  }
  if (e->type == Steinberg_Vst_Event_EventTypes_kDataEvent &&
      e->Steinberg_Vst_Event_data.type == Steinberg_Vst_DataEvent_DataTypes_kMidiSysEx) {
    const auto& d = e->Steinberg_Vst_Event_data;
    return el->produced->addSysex(d.bytes, d.size, offset) ? Steinberg_kResultOk
                                                           : Steinberg_kResultFalse;
  }
  // Anything else has no MidiBuffer shape here. Refused rather than
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

// ── The host context: a name, messages, and the line between the halves ─────
//
// initialize() takes a context, and this host passed null. The context is
// where a plugin learns the host's name, and the only place it can get an
// IMessage: IHostApplication::createInstance is the factory for the messages
// and attribute lists a processor and its controller talk in. A plugin built
// on Steinberg's SDK -- most commercial VST3s -- makes one with
// allocateMessage(), which answers null without a context: its halves were
// connected and silent, an editor that never heard its own processor, and
// some refused to initialise at all.
//
// Unlike the objects above these are COUNTED, honestly: a plugin may keep a
// message, its attribute list or the context itself past the call that
// handed it over, and each lives until the last reference is given back.

namespace detail {

/** IAttributeList: what a message carries, by name. Every value is COPIED
 *  in on set -- the sender's buffer is its own, often on its stack and
 *  reused at once -- and what a get hands back stays valid until that name
 *  is set again or the list goes. */
struct Vst3AttributeList {
  Steinberg_Vst_IAttributeListVtbl* lpVtbl = nullptr;
  std::atomic<uint32_t> refs{1};
  struct Value {
    enum class Kind { kInt, kFloat, kString, kBinary } kind = Kind::kInt;
    int64_t integer = 0;
    double number = 0.0;
    std::u16string text;
    std::vector<uint8_t> bytes;
  };
  std::map<std::string, Value> values;

  const Value* find(const char* id, Value::Kind kind) const {
    if (!id) return nullptr;
    const auto it = values.find(id);
    return it != values.end() && it->second.kind == kind ? &it->second : nullptr;
  }
  /** A fresh value under `id`, whatever was there before. */
  Value& reset(const char* id, Value::Kind kind) {
    Value& v = values[id];
    v = Value();
    v.kind = kind;
    return v;
  }
};

inline Vst3AttributeList* attributeList(void* self) {
  return static_cast<Vst3AttributeList*>(self);
}

inline Steinberg_uint32 SMTG_STDMETHODCALLTYPE alAddRef(void* self) {
  return attributeList(self)->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}
inline Steinberg_uint32 SMTG_STDMETHODCALLTYPE alRelease(void* self) {
  Vst3AttributeList* list = attributeList(self);
  const uint32_t left = list->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (left == 0) delete list;
  return left;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE alQuery(void* self, const Steinberg_TUID iid,
                                                        void** obj) {
  if (!obj || !iid) return Steinberg_kInvalidArgument;
  if (sameTuid(iid, Steinberg_FUnknown_iid) || sameTuid(iid, Steinberg_Vst_IAttributeList_iid)) {
    alAddRef(self);
    *obj = self;
    return Steinberg_kResultOk;
  }
  *obj = nullptr;
  return Steinberg_kNoInterface;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE alSetInt(void* self,
                                                         Steinberg_Vst_IAttributeList_AttrID id,
                                                         Steinberg_int64 value) {
  if (!id) return Steinberg_kInvalidArgument;
  attributeList(self)->reset(id, Vst3AttributeList::Value::Kind::kInt).integer = value;
  return Steinberg_kResultOk;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE alGetInt(void* self,
                                                         Steinberg_Vst_IAttributeList_AttrID id,
                                                         Steinberg_int64* value) {
  if (!id || !value) return Steinberg_kInvalidArgument;
  const auto* v = attributeList(self)->find(id, Vst3AttributeList::Value::Kind::kInt);
  if (!v) return Steinberg_kResultFalse;
  *value = v->integer;
  return Steinberg_kResultOk;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE alSetFloat(void* self,
                                                           Steinberg_Vst_IAttributeList_AttrID id,
                                                           double value) {
  if (!id) return Steinberg_kInvalidArgument;
  attributeList(self)->reset(id, Vst3AttributeList::Value::Kind::kFloat).number = value;
  return Steinberg_kResultOk;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE alGetFloat(void* self,
                                                           Steinberg_Vst_IAttributeList_AttrID id,
                                                           double* value) {
  if (!id || !value) return Steinberg_kInvalidArgument;
  const auto* v = attributeList(self)->find(id, Vst3AttributeList::Value::Kind::kFloat);
  if (!v) return Steinberg_kResultFalse;
  *value = v->number;
  return Steinberg_kResultOk;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE alSetString(void* self,
                                                            Steinberg_Vst_IAttributeList_AttrID id,
                                                            const Steinberg_Vst_TChar* text) {
  if (!id || !text) return Steinberg_kInvalidArgument;
  size_t length = 0;
  while (text[length]) ++length;
  attributeList(self)->reset(id, Vst3AttributeList::Value::Kind::kString).text.assign(
      reinterpret_cast<const char16_t*>(text), length);
  return Steinberg_kResultOk;
}
/** Into a buffer of `sizeInBytes`, cut to fit and terminated whatever the
 *  length: a reader that sized its buffer for a short name gets the start
 *  of a long one, never an unterminated array. */
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE alGetString(void* self,
                                                            Steinberg_Vst_IAttributeList_AttrID id,
                                                            Steinberg_Vst_TChar* text,
                                                            Steinberg_uint32 sizeInBytes) {
  if (!id || !text) return Steinberg_kInvalidArgument;
  const auto* v = attributeList(self)->find(id, Vst3AttributeList::Value::Kind::kString);
  const size_t room = sizeInBytes / sizeof(Steinberg_Vst_TChar);
  if (!v || room == 0) return Steinberg_kResultFalse;
  const size_t n = (std::min)(v->text.size(), room - 1);
  if (n > 0) std::memcpy(text, v->text.data(), n * sizeof(Steinberg_Vst_TChar));
  text[n] = 0;
  return Steinberg_kResultOk;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE alSetBinary(void* self,
                                                            Steinberg_Vst_IAttributeList_AttrID id,
                                                            const void* data,
                                                            Steinberg_uint32 sizeInBytes) {
  if (!id || (!data && sizeInBytes > 0)) return Steinberg_kInvalidArgument;
  const auto* bytes = static_cast<const uint8_t*>(data);
  attributeList(self)->reset(id, Vst3AttributeList::Value::Kind::kBinary).bytes.assign(
      bytes, bytes + sizeInBytes);
  return Steinberg_kResultOk;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE alGetBinary(void* self,
                                                            Steinberg_Vst_IAttributeList_AttrID id,
                                                            const void** data,
                                                            Steinberg_uint32* sizeInBytes) {
  if (!id || !data || !sizeInBytes) return Steinberg_kInvalidArgument;
  const auto* v = attributeList(self)->find(id, Vst3AttributeList::Value::Kind::kBinary);
  if (!v) return Steinberg_kResultFalse;
  *data = v->bytes.data();
  *sizeInBytes = (Steinberg_uint32) v->bytes.size();
  return Steinberg_kResultOk;
}

inline Vst3AttributeList* makeVst3AttributeList() {
  static Steinberg_Vst_IAttributeListVtbl v = {
      alQuery,    alAddRef,   alRelease,   alSetInt,    alGetInt,    alSetFloat,
      alGetFloat, alSetString, alGetString, alSetBinary, alGetBinary,
  };
  auto* list = new Vst3AttributeList();
  list->lpVtbl = &v;
  return list;
}

/** IMessage: an id and an attribute list, which the message owns. */
struct Vst3Message {
  Steinberg_Vst_IMessageVtbl* lpVtbl = nullptr;
  std::atomic<uint32_t> refs{1};
  std::string id;
  bool hasId = false;
  Vst3AttributeList* attributes = nullptr;
};

inline Vst3Message* message(void* self) { return static_cast<Vst3Message*>(self); }

inline Steinberg_uint32 SMTG_STDMETHODCALLTYPE msgAddRef(void* self) {
  return message(self)->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}
inline Steinberg_uint32 SMTG_STDMETHODCALLTYPE msgRelease(void* self) {
  Vst3Message* m = message(self);
  const uint32_t left = m->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (left == 0) {
    if (m->attributes) alRelease(m->attributes);
    delete m;
  }
  return left;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE msgQuery(void* self, const Steinberg_TUID iid,
                                                         void** obj) {
  if (!obj || !iid) return Steinberg_kInvalidArgument;
  if (sameTuid(iid, Steinberg_FUnknown_iid) || sameTuid(iid, Steinberg_Vst_IMessage_iid)) {
    msgAddRef(self);
    *obj = self;
    return Steinberg_kResultOk;
  }
  *obj = nullptr;
  return Steinberg_kNoInterface;
}
inline Steinberg_FIDString SMTG_STDMETHODCALLTYPE msgGetId(void* self) {
  return message(self)->hasId ? message(self)->id.c_str() : nullptr;
}
inline void SMTG_STDMETHODCALLTYPE msgSetId(void* self, Steinberg_FIDString id) {
  Vst3Message* m = message(self);
  m->hasId = id != nullptr;
  m->id = id ? id : "";
}
/** The message's own list, with no reference added: the interface hands it
 *  out the way Steinberg's host does, and a caller keeping it past the
 *  message adds its own. */
inline struct Steinberg_Vst_IAttributeList* SMTG_STDMETHODCALLTYPE msgAttributes(void* self) {
  return reinterpret_cast<Steinberg_Vst_IAttributeList*>(message(self)->attributes);
}

inline Vst3Message* makeVst3Message() {
  static Steinberg_Vst_IMessageVtbl v = {msgQuery, msgAddRef,     msgRelease,
                                         msgGetId, msgSetId, msgAttributes};
  auto* m = new Vst3Message();
  m->lpVtbl = &v;
  m->attributes = makeVst3AttributeList();
  return m;
}

/** IHostApplication, and IPlugInterfaceSupport as its second face: what
 *  initialize() is handed. One per loaded plugin, counted like the rest, so
 *  a plugin that keeps its context until terminate() -- Steinberg's
 *  ComponentBase does -- holds a live object, whatever order things go in. */
struct Vst3HostContext;
struct Vst3InterfaceSupportFace {
  Steinberg_Vst_IPlugInterfaceSupportVtbl* lpVtbl = nullptr;
  Vst3HostContext* self = nullptr;
};
struct Vst3HostContext {
  Steinberg_Vst_IHostApplicationVtbl* lpVtbl = nullptr;
  Vst3InterfaceSupportFace support;
  std::atomic<uint32_t> refs{1};
  /** UTF-16, terminated, at most 127 units: a String128. */
  Steinberg_Vst_String128 name{};
};

inline Steinberg_uint32 SMTG_STDMETHODCALLTYPE ctxAddRef(void* self) {
  return static_cast<Vst3HostContext*>(self)->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}
inline Steinberg_uint32 SMTG_STDMETHODCALLTYPE ctxRelease(void* self) {
  auto* ctx = static_cast<Vst3HostContext*>(self);
  const uint32_t left = ctx->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (left == 0) delete ctx;
  return left;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE ctxQuery(void* self, const Steinberg_TUID iid,
                                                         void** obj) {
  if (!obj || !iid) return Steinberg_kInvalidArgument;
  auto* ctx = static_cast<Vst3HostContext*>(self);
  if (sameTuid(iid, Steinberg_FUnknown_iid) || sameTuid(iid, Steinberg_Vst_IHostApplication_iid))
    *obj = ctx;
  else if (sameTuid(iid, Steinberg_Vst_IPlugInterfaceSupport_iid))
    *obj = &ctx->support;
  else {
    *obj = nullptr;
    return Steinberg_kNoInterface;
  }
  ctxAddRef(ctx);
  return Steinberg_kResultOk;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE ctxGetName(void* self,
                                                           Steinberg_Vst_String128 name) {
  if (!name) return Steinberg_kInvalidArgument;
  std::memcpy(name, static_cast<Vst3HostContext*>(self)->name, sizeof(Steinberg_Vst_String128));
  return Steinberg_kResultOk;
}
/** The factory a plugin's allocateMessage() calls: IMessage's id as the
 *  class, and IAttributeList's for a bare list. A new object, with the one
 *  reference the caller owns. */
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE ctxCreateInstance(void*, Steinberg_TUID cid,
                                                                  Steinberg_TUID iid, void** obj) {
  if (!obj) return Steinberg_kInvalidArgument;
  *obj = nullptr;
  if (!cid || !iid) return Steinberg_kInvalidArgument;
  if (sameTuid(cid, Steinberg_Vst_IMessage_iid)) {
    Vst3Message* m = makeVst3Message();
    const Steinberg_tresult result = msgQuery(m, iid, obj);
    msgRelease(m); // the query's reference is the caller's; this was ours
    return result;
  }
  if (sameTuid(cid, Steinberg_Vst_IAttributeList_iid)) {
    Vst3AttributeList* list = makeVst3AttributeList();
    const Steinberg_tresult result = alQuery(list, iid, obj);
    alRelease(list);
    return result;
  }
  return Steinberg_kResultFalse;
}

inline Steinberg_tresult SMTG_STDMETHODCALLTYPE supportQuery(void* self, const Steinberg_TUID iid,
                                                             void** obj) {
  return ctxQuery(static_cast<Vst3InterfaceSupportFace*>(self)->self, iid, obj);
}
inline Steinberg_uint32 SMTG_STDMETHODCALLTYPE supportAddRef(void* self) {
  return ctxAddRef(static_cast<Vst3InterfaceSupportFace*>(self)->self);
}
inline Steinberg_uint32 SMTG_STDMETHODCALLTYPE supportRelease(void* self) {
  return ctxRelease(static_cast<Vst3InterfaceSupportFace*>(self)->self);
}
/** Yes for exactly the plugin interfaces this host asks a plugin for and
 *  uses, so a plugin that adapts to what its host can do adapts to this
 *  one; no for everything else. */
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE supportIsSupported(void*,
                                                                   const Steinberg_TUID iid) {
  if (!iid) return Steinberg_kInvalidArgument;
  const char* const used[] = {
      Steinberg_Vst_IComponent_iid,      Steinberg_Vst_IAudioProcessor_iid,
      Steinberg_Vst_IEditController_iid, Steinberg_Vst_IConnectionPoint_iid,
      Steinberg_Vst_IUnitInfo_iid,       Steinberg_Vst_IMidiMapping_iid,
      Steinberg_IPlugViewContentScaleSupport_iid,
  };
  for (const char* known : used)
    if (std::memcmp(iid, known, sizeof(Steinberg_TUID)) == 0) return Steinberg_kResultOk;
  return Steinberg_kResultFalse;
}

/** [main-thread] A context going by `utf8Name`, which is cut to the 127
 *  UTF-16 units a String128 holds -- never mid-pair. */
inline Vst3HostContext* makeVst3HostContext(const std::string& utf8Name) {
  static Steinberg_Vst_IHostApplicationVtbl app = {ctxQuery, ctxAddRef, ctxRelease, ctxGetName,
                                                   ctxCreateInstance};
  static Steinberg_Vst_IPlugInterfaceSupportVtbl support = {supportQuery, supportAddRef,
                                                            supportRelease, supportIsSupported};
  auto* ctx = new Vst3HostContext();
  ctx->lpVtbl = &app;
  ctx->support.lpVtbl = &support;
  ctx->support.self = ctx;
  // UTF-8 to UTF-16 by hand: the one place this file goes that way. A byte
  // that starts no character, or a sequence cut short, is U+FFFD.
  size_t n = 0;
  for (size_t i = 0; i < utf8Name.size();) {
    const auto lead = (unsigned char) utf8Name[i];
    const int extra = lead < 0x80                    ? 0
                      : lead >= 0xF0 && lead < 0xF5 ? 3
                      : lead >= 0xE0 && lead < 0xF0 ? 2
                      : lead >= 0xC2 && lead < 0xE0 ? 1
                                                    : -1;
    uint32_t cp = extra == 0   ? lead
                  : extra == 1 ? (lead & 0x1Fu)
                  : extra == 2 ? (lead & 0x0Fu)
                               : (lead & 0x07u);
    bool good = extra >= 0 && i + (size_t) extra < utf8Name.size();
    for (int k = 1; good && k <= extra; ++k) {
      const auto next = (unsigned char) utf8Name[i + (size_t) k];
      if ((next & 0xC0u) != 0x80u) good = false;
      cp = (cp << 6) | (next & 0x3Fu);
    }
    if (!good || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    i += good ? (size_t) extra + 1 : 1;
    const size_t units = cp >= 0x10000 ? 2 : 1;
    if (n + units > 127) break; // a String128 and its terminator, never half a pair
    if (units == 2) {
      ctx->name[n++] = (Steinberg_Vst_TChar) (0xD800 + ((cp - 0x10000) >> 10));
      ctx->name[n++] = (Steinberg_Vst_TChar) (0xDC00 + ((cp - 0x10000) & 0x3FF));
    } else {
      ctx->name[n++] = (Steinberg_Vst_TChar) cp;
    }
  }
  ctx->name[n] = 0;
  return ctx;
}

/**
 * What each half is connected TO: a proxy for the other half, as Steinberg's
 * own hosting code connects them, rather than the other half itself.
 *
 * Two reasons. A connection carries notify(), which the format confines to
 * the main thread and which plugins call from process() anyway -- a
 * processor publishing a meter, a waveform -- and connected directly, the
 * controller's notify(), UI code that allocates and locks and redraws, ran
 * on the audio thread. The proxy delivers only on the thread that loaded the
 * plugin, and never from inside its process(), so a block called on that
 * same thread -- an offline render -- is covered too; anything else is
 * refused, as Steinberg's proxy refuses it. And the host can cut a proxy:
 * a plugin that keeps the pointer it was given after disconnect() reaches a
 * line that answers false, not a half that has been terminated.
 */
struct Vst3ConnectionProxy {
  Steinberg_Vst_IConnectionPointVtbl* lpVtbl = nullptr;
  std::atomic<uint32_t> refs{1};
  /** The half messages go to, until the host cuts the line. */
  std::atomic<Steinberg_Vst_IConnectionPoint*> peer{nullptr};
  std::thread::id mainThread;
  /** The thread inside the plugin's process() right now, if any, which the
   *  plugin object publishes; null once the line is cut. */
  const std::atomic<std::thread::id>* audioThread = nullptr;
};

inline Steinberg_uint32 SMTG_STDMETHODCALLTYPE cpxAddRef(void* self) {
  return static_cast<Vst3ConnectionProxy*>(self)->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}
inline Steinberg_uint32 SMTG_STDMETHODCALLTYPE cpxRelease(void* self) {
  auto* proxy = static_cast<Vst3ConnectionProxy*>(self);
  const uint32_t left = proxy->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
  if (left == 0) delete proxy;
  return left;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE cpxQuery(void* self, const Steinberg_TUID iid,
                                                         void** obj) {
  if (!obj || !iid) return Steinberg_kInvalidArgument;
  if (sameTuid(iid, Steinberg_FUnknown_iid) || sameTuid(iid, Steinberg_Vst_IConnectionPoint_iid)) {
    cpxAddRef(self);
    *obj = self;
    return Steinberg_kResultOk;
  }
  *obj = nullptr;
  return Steinberg_kNoInterface;
}
/** The host makes and breaks the connection; a plugin asking the proxy to is
 *  told no. */
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE cpxConnect(void*,
                                                           struct Steinberg_Vst_IConnectionPoint*) {
  return Steinberg_kResultFalse;
}
inline Steinberg_tresult SMTG_STDMETHODCALLTYPE cpxNotify(void* self,
                                                          struct Steinberg_Vst_IMessage* msg) {
  auto* proxy = static_cast<Vst3ConnectionProxy*>(self);
  if (!msg) return Steinberg_kInvalidArgument;
  // The thread first, which touches nothing but the proxy: a call from the
  // audio thread or any other is refused before it can read anything the
  // main thread may be tearing down.
  const std::thread::id caller = std::this_thread::get_id();
  if (caller != proxy->mainThread) return Steinberg_kResultFalse;
  Steinberg_Vst_IConnectionPoint* to = proxy->peer.load(std::memory_order_acquire);
  if (!to) return Steinberg_kResultFalse;
  if (proxy->audioThread && proxy->audioThread->load(std::memory_order_acquire) == caller)
    return Steinberg_kResultFalse;
  return to->lpVtbl->notify(to, msg);
}

/** [main-thread] A line to `peer`, delivering on this thread only. */
inline Vst3ConnectionProxy* makeVst3ConnectionProxy(
    Steinberg_Vst_IConnectionPoint* peer, const std::atomic<std::thread::id>* audioThread) {
  static Steinberg_Vst_IConnectionPointVtbl v = {cpxQuery,   cpxAddRef,  cpxRelease,
                                                 cpxConnect, cpxConnect, cpxNotify};
  auto* proxy = new Vst3ConnectionProxy();
  proxy->lpVtbl = &v;
  proxy->peer.store(peer, std::memory_order_release);
  proxy->mainThread = std::this_thread::get_id();
  proxy->audioThread = audioThread;
  return proxy;
}

/** [main-thread] Cut the line and let go of it: whatever the plugin still
 *  holds of it answers false from now on. */
inline void cutVst3ConnectionProxy(Vst3ConnectionProxy*& proxy) {
  if (!proxy) return;
  proxy->peer.store(nullptr, std::memory_order_release);
  proxy->audioThread = nullptr;
  cpxRelease(proxy);
  proxy = nullptr;
}

} // namespace detail

class Vst3Plugin;
/** A VST3 string -- UTF-16, up to 128 units -- as UTF-8, which is what every
 *  string a caller of this host sees is. Narrowed one unit at a time, as it
 *  was, "µs" and "Größe" came out as Latin-1 bytes: invalid UTF-8 in a
 *  parameter list, and anything outside Latin-1 cut to garbage. */
inline std::string utf8FromVst3(const Steinberg_char16* text, int capacity = 128) {
  std::string out;
  if (!text) return out;
  for (int i = 0; i < capacity && text[i]; ++i) {
    uint32_t cp = (uint32_t) text[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < capacity && text[i + 1] >= 0xDC00 &&
        text[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t) text[i + 1] - 0xDC00);
      ++i;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    if (cp < 0x80) {
      out.push_back((char) cp);
    } else if (cp < 0x800) {
      out.push_back((char) (0xC0 | (cp >> 6)));
      out.push_back((char) (0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back((char) (0xE0 | (cp >> 12)));
      out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char) (0x80 | (cp & 0x3F)));
    } else {
      out.push_back((char) (0xF0 | (cp >> 18)));
      out.push_back((char) (0x80 | ((cp >> 12) & 0x3F)));
      out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char) (0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

inline std::unique_ptr<HostedPlugin> loadVst3(const PluginDescription&);

/** The VST3 side of it: a module's component and its controller, connected
 *  to each other and driven as HostedPlugin describes. Made by loadVst3();
 *  the COM objects above are what it hands the plugin. */
class Vst3Plugin final : public HostedPlugin {
public:
  /** [main-thread, audio stopped] Release, close the editor, disconnect and
   *  terminate both halves, and close the module. */
  ~Vst3Plugin() override { unload(); }

  const PluginDescription& description() const override { return desc_; }
  bool isValid() const override { return processor_ != nullptr; }

  bool prepare(double sampleRate, uint32_t maxBlockSize, uint32_t numChannels = 2) override {
    if (!processor_ || !component_) return false;
    if (active_) release();

    maxBlock_ = maxBlockSize > 0 ? maxBlockSize : 1;
    channels_ = numChannels > 0 ? (numChannels > 8 ? 8 : numChannels) : 1;
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

    negotiateBuses();

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

    // A buffer for every bus at its agreed width -- the plugin reads its
    // sidechain and its second channel from these whatever the caller has.
    layoutBuses(inWidth_, inStorage_, inPtrs_, inBuses_);
    layoutBuses(outWidth_, outStorage_, outPtrs_, outBuses_);
    // Room for a full MidiBuffer of notes, for a plugin with an event input
    // to hear them on. process() lists none for any other, and the 48 KB was
    // held by every effect for nothing.
    if (acceptsMidi_) events_.reserve(MidiBuffer::kMaxEvents);
    // queues_ carries BOTH the caller's queued parameter changes and the
    // controller messages translated out of this block's MIDI, so it is sized
    // for both. Sizing it for one would silently drop the other's tail.
    queues_.reserve(kMaxQueuedParams + (size_t) MidiBuffer::kMaxEvents);
    // Room for every parameter the controller has, hidden ones included, to
    // report a change in the same block -- a preset recalled on the audio
    // thread does exactly that. A LISTED parameter's newest value always
    // reaches drainParameterEdits() (its own mailbox). The ones the host does
    // not list -- hidden, the controller's business only -- are found by a
    // walk capped at 64 a block, and travel by a bounded queue that refuses
    // the newest when it is full: a limit, stated, rather than a promise.
    {
      const Steinberg_int32 n = controller_ ? controller_->lpVtbl->getParameterCount(controller_)
                                            : (Steinberg_int32) params_.size();
      const size_t room = (size_t) (n > 0 ? n : 0) + 16;
      outQueues_.clear();
      outQueues_.reserve(room < kMaxOutQueues ? room : kMaxOutQueues);
      std::fill(outSlotOf_.begin(), outSlotOf_.end(), -1);
    }

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
  bool takeRestartRequest() override {
    return handler_.restartRequested.exchange(false, std::memory_order_acq_rel);
  }
  bool takeStateDirty() override {
    return handler_.stateDirty.exchange(false, std::memory_order_acq_rel);
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
    const auto id = (Steinberg_Vst_ParamID) params_[(size_t) index].id;
    double norm = controller_->lpVtbl->plainParamToNormalized(controller_, id, value);
    norm = norm < 0.0 ? 0.0 : (norm > 1.0 ? 1.0 : norm);
    // The PROCESSOR is told through the queue -- writing into a plugin that
    // is processing is the race the event list exists to avoid -- and the
    // controller immediately, so a read-back is coherent. Posted first, so a
    // change that cannot be delivered is refused on both sides rather than
    // one; a listed parameter always can be, through its mailbox when the
    // queue is full. Lock-free, as on the CLAP side, and for the same
    // reason: it was a vector cleared at the end of process(), losing what
    // arrived during the block.
    if (!postToProcessor(id, norm)) return false;
    controller_->lpVtbl->setParamNormalized(controller_, id, norm);
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
    return utf8FromVst3(text);
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

  /** VST3's way: a view that canResize() is asked to constrain the size
   *  (checkSizeConstraint), then given it with onSize(), as Steinberg's
   *  hosts do when a plugin window's frame is dragged. */
  bool setEditorSize(uint32_t width, uint32_t height) override {
    if (!view_ || width == 0 || height == 0) return false;
    if (view_->lpVtbl->canResize(view_) != Steinberg_kResultTrue) return false;
    Steinberg_ViewRect rect{0, 0, (Steinberg_int32) width, (Steinberg_int32) height};
    view_->lpVtbl->checkSizeConstraint(view_, &rect);
    if (rect.right - rect.left <= 0 || rect.bottom - rect.top <= 0) return false;
    return view_->lpVtbl->onSize(view_, &rect) == Steinberg_kResultOk;
  }

  size_t drainParameterEdits(std::vector<ParamEdit>& out) override {
    const size_t before = out.size();
    // What the plugin said through the component handler, in order, from
    // whichever thread it said it on.
    ParamEdit edit;
    while (edits_.pop(edit)) out.push_back(edit);
    // What the processor reported from its blocks. The controller is told
    // here, on its own thread, or a plugin whose DSP moves a value -- or
    // publishes a meter as a read-only parameter -- shows the old one in its
    // own editor for ever; and the caller hears it as an edit like any other.
    //
    // The newest value of each listed parameter, from its own mailbox. They
    // came through one queue of 1,024 before, and the NEWEST were the ones a
    // full queue refused: a meter reporting every block filled it between
    // two drains, and anything else the DSP moved in that time was lost --
    // the editor and the caller kept the old value for good.
    if (outAny_.exchange(false, std::memory_order_acq_rel) && controller_) {
      for (size_t i = 0; i < outBoxCount_; ++i) {
        Mailbox& box = outBoxes_[i];
        if (!box.dirty.exchange(false, std::memory_order_acq_rel)) continue;
        const double value = box.value.load(std::memory_order_acquire);
        const auto id = (Steinberg_Vst_ParamID) params_[i].id;
        controller_->lpVtbl->setParamNormalized(controller_, id, value);
        out.push_back({ParamEdit::Kind::kValue, (int) i,
                       controller_->lpVtbl->normalizedParamToPlain(controller_, id, value)});
      }
    }
    // The ones the host does not list -- hidden parameters, the controller's
    // business only -- by queue.
    QueuedParam change;
    while (outChanges_.pop(change))
      if (controller_) controller_->lpVtbl->setParamNormalized(controller_, change.id, change.value);
    // A plugin that reported kParamValuesChanged is owed a re-read. Doing it
    // here rather than inside restartComponent keeps the host off the
    // plugin's own stack, and this is a plain main-thread call.
    if (handler_.valuesStale.exchange(false, std::memory_order_acq_rel) && controller_) {
      for (size_t i = 0; i < params_.size(); ++i) {
        const auto id = (Steinberg_Vst_ParamID) params_[i].id;
        const double norm = controller_->lpVtbl->getParamNormalized(controller_, id);
        out.push_back({ParamEdit::Kind::kValue, (int) i,
                       controller_->lpVtbl->normalizedParamToPlain(controller_, id, norm)});
      }
    }
    return out.size() - before;
  }

  int numPresets() const override { return (int) presetNames_.size(); }
  std::string presetName(int index) const override {
    if (index < 0 || index >= numPresets()) return std::string();
    return presetNames_[(size_t) index];
  }
  bool loadPreset(int index) override {
    if (index < 0 || index >= numPresets() || programParam_ < 0 || !controller_) return false;
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
    if (!postToProcessor(id, norm)) return false;
    controller_->lpVtbl->setParamNormalized(controller_, id, norm);
    return true;
  }

  bool saveState(std::vector<uint8_t>& out) const override;
  bool loadState(const uint8_t* data, size_t size) override;

private:
  friend std::unique_ptr<HostedPlugin> loadVst3(const PluginDescription&);
  Vst3Plugin() = default;

  static constexpr size_t kMaxQueuedParams = 64;
  /** Output changes to parameters the host does not list, on their way to
   *  the main thread, and the most parameters one block may report. */
  static constexpr size_t kMaxOutChanges = 1024;
  static constexpr size_t kMaxOutQueues = 4096;

  void unload();
  void readCapabilities();

  /** One parameter value on its way across a thread boundary, normalised as
   *  VST3 speaks it: a host write waiting to become a point in the next
   *  block's IParameterChanges, or a change the plugin reported in process()
   *  waiting for the main thread to hand it to the controller. */
  struct QueuedParam {
    Steinberg_Vst_ParamID id;
    double value;
  };

  LibHandle lib_ = nullptr;
  bool moduleEntered_ = false; // its exit is owed only after this
  Steinberg_IPluginFactory* factory_ = nullptr;
  /** What both halves' initialize() is given; this object's reference. */
  detail::Vst3HostContext* context_ = nullptr;
  Steinberg_Vst_IComponent* component_ = nullptr;
  /** terminate() is owed only for an initialize() that succeeded. */
  bool componentInitialized_ = false;
  Steinberg_Vst_IAudioProcessor* processor_ = nullptr;
  Steinberg_Vst_IEditController* controller_ = nullptr;
  /** Looked up once, because CC and pitch bend have no event form in this
   *  format and every block that carries one needs the mapping. */
  Steinberg_Vst_IMidiMapping* midiMapping_ = nullptr;
  bool controllerIsSeparate_ = false;
  /** The two halves' IConnectionPoints, joined at load and parted at unload,
   *  for a plugin whose controller is a separate object. */
  Steinberg_Vst_IConnectionPoint* componentPoint_ = nullptr;
  Steinberg_Vst_IConnectionPoint* controllerPoint_ = nullptr;
  /** What each half is connected to: the line to the other one. */
  detail::Vst3ConnectionProxy* toController_ = nullptr;
  detail::Vst3ConnectionProxy* toComponent_ = nullptr;
  /** The thread inside process() while it is there, else none: a message a
   *  plugin sends from there is not delivered. */
  std::atomic<std::thread::id> processThread_{std::thread::id()};

  /** The component handler's forward: an edit from the controller, queued
   *  for the processor's next block like any parameter change. */
  static void forwardEdit(void* owner, Steinberg_Vst_ParamID id, double normalized) {
    static_cast<Vst3Plugin*>(owner)->postToProcessor(id, normalized);
  }

  /** One listed parameter's newest value, for when the queue is full. */
  struct Mailbox {
    std::atomic<double> value{0.0};
    std::atomic<bool> dirty{false};
  };
  std::unique_ptr<Mailbox[]> mailboxes_;
  size_t mailboxCount_ = 0;
  std::atomic<bool> mailAny_{false};
  /** The other direction: the newest value the processor reported for each
   *  listed parameter, until drainParameterEdits() takes it. */
  std::unique_ptr<Mailbox[]> outBoxes_;
  size_t outBoxCount_ = 0;
  std::atomic<bool> outAny_{false};
  /** Each listed parameter's queue in the block under way, or -1. */
  std::vector<int32_t> outSlotOf_;

  /**
   * A value on its way to the processor's next block, from any thread.
   *
   * The queue, normally. When it is full -- edits made while no audio runs,
   * or a burst faster than the blocks -- a listed parameter's value goes to
   * its mailbox instead, which keeps only the newest. A full queue used to
   * drop the NEWEST value, leaving the processor at some step of a knob drag
   * while the controller showed where it ended. Once a parameter has mail,
   * its later values go to the mail too, so the queue can never deliver an
   * older value after a newer one. False only for an unlisted id with the
   * queue full.
   */
  bool postToProcessor(Steinberg_Vst_ParamID id, double normalized) {
    const int index = paramIndex_.find((uint32_t) id);
    Mailbox* box = index >= 0 && (size_t) index < mailboxCount_ ? &mailboxes_[(size_t) index]
                                                                 : nullptr;
    if (box && box->dirty.load(std::memory_order_acquire)) {
      box->value.store(normalized, std::memory_order_release);
      mailAny_.store(true, std::memory_order_release);
      return true;
    }
    if (queued_.push({id, normalized})) return true;
    if (!box) return false;
    box->value.store(normalized, std::memory_order_release);
    box->dirty.store(true, std::memory_order_release);
    mailAny_.store(true, std::memory_order_release);
    return true;
  }
  PluginDescription desc_;
  std::vector<HostedParam> params_;
  ParamIdIndex paramIndex_;

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
  /** The handler's edits, filled from any thread, drained on the main one. */
  MpscQueue<ParamEdit, ComponentHandler::kMaxEdits> edits_;
  double editorScale_ = 1.0;
  PlugFrame frame_;
  Steinberg_IPlugView* view_ = nullptr;
  bool hasEditor_ = false;
  int64_t projectTime_ = 0;
  double sampleRate_ = 48000.0;

  std::vector<float> inStorage_, outStorage_;
  /** Every bus's channel pointers, bus after bus; the bus buffers index in. */
  std::vector<float*> inPtrs_, outPtrs_;
  std::vector<Steinberg_Vst_AudioBusBuffers> inBuses_, outBuses_;
  /** Each audio bus's agreed width, in the plugin's own bus order. */
  std::vector<uint32_t> inWidth_, outWidth_;
  /** Bounded, so a plugin declaring absurd numbers runs out of buses, not
   *  memory. */
  static constexpr Steinberg_int32 kMaxBuses = 32;
  static constexpr uint32_t kMaxBusChannels = 64;

  /** The arrangement a caller's channel count means: kMono for one -- a
   *  single CENTRE speaker; kSpeakerL, which this asked for, is one left
   *  speaker and a plugin taking mono refuses it -- then stereo, and the
   *  usual layouts up to 7.1. */
  static Steinberg_Vst_SpeakerArrangement arrangementFor(uint32_t channels) {
    switch (channels) {
      case 1: return Steinberg_Vst_SpeakerArr_kMono;
      case 3: return Steinberg_Vst_SpeakerArr_k30Cine;
      case 4: return Steinberg_Vst_SpeakerArr_k40Music;
      case 5: return Steinberg_Vst_SpeakerArr_k50;
      case 6: return Steinberg_Vst_SpeakerArr_k51;
      case 7: return Steinberg_Vst_SpeakerArr_k61Music;
      case 8: return Steinberg_Vst_SpeakerArr_k71Cine;
      default: return Steinberg_Vst_SpeakerArr_kStereo;
    }
  }

  static uint32_t channelsIn(Steinberg_Vst_SpeakerArrangement arrangement) {
    uint32_t n = 0;
    for (; arrangement; arrangement &= arrangement - 1) ++n;
    return n;
  }

  /**
   * Agree an arrangement for EVERY audio bus, then read back what was agreed.
   *
   * The request asks for the caller's width on the main buses -- the first
   * each way -- and leaves the others as the plugin has them. All of them go
   * in one call because the specification says so, and a plugin given one
   * bus of two may refuse the lot. Refused or not, each bus is then asked
   * what it is: a refusal leaves the plugin at an arrangement of its own
   * choosing, and the buffers process() hands over must be that wide. This
   * asked for one bus each way, ignored the answer, and passed the caller's
   * width regardless.
   */
  void negotiateBuses() {
    const auto busCount = [&](Steinberg_Vst_BusDirection dir) {
      Steinberg_int32 n =
          component_->lpVtbl->getBusCount(component_, Steinberg_Vst_MediaTypes_kAudio, dir);
      return n < 0 ? 0 : (n > kMaxBuses ? kMaxBuses : n);
    };
    const Steinberg_int32 nIn = busCount(Steinberg_Vst_BusDirections_kInput);
    const Steinberg_int32 nOut = busCount(Steinberg_Vst_BusDirections_kOutput);
    const auto current = [&](Steinberg_Vst_BusDirection dir, Steinberg_int32 index) {
      Steinberg_Vst_SpeakerArrangement arr = 0;
      if (processor_->lpVtbl->getBusArrangement(processor_, dir, index, &arr) ==
          Steinberg_kResultOk)
        return arr;
      // A plugin that will not say: its bus info's channel count, as the
      // nearest arrangement.
      Steinberg_Vst_BusInfo info{};
      if (component_->lpVtbl->getBusInfo(component_, Steinberg_Vst_MediaTypes_kAudio, dir, index,
                                         &info) == Steinberg_kResultOk &&
          info.channelCount > 0)
        return arrangementFor((uint32_t) info.channelCount);
      return (Steinberg_Vst_SpeakerArrangement) 0;
    };
    std::vector<Steinberg_Vst_SpeakerArrangement> ins((size_t) nIn), outs((size_t) nOut);
    for (Steinberg_int32 i = 0; i < nIn; ++i) ins[(size_t) i] = current(Steinberg_Vst_BusDirections_kInput, i);
    for (Steinberg_int32 i = 0; i < nOut; ++i) outs[(size_t) i] = current(Steinberg_Vst_BusDirections_kOutput, i);
    const Steinberg_Vst_SpeakerArrangement wanted = arrangementFor(channels_);
    if (nIn > 0) ins[0] = wanted;
    if (nOut > 0) outs[0] = wanted;
    processor_->lpVtbl->setBusArrangements(processor_, nIn > 0 ? ins.data() : nullptr, nIn,
                                           nOut > 0 ? outs.data() : nullptr, nOut);
    inWidth_.assign((size_t) nIn, 0u);
    outWidth_.assign((size_t) nOut, 0u);
    for (Steinberg_int32 i = 0; i < nIn; ++i) {
      const uint32_t w = channelsIn(current(Steinberg_Vst_BusDirections_kInput, i));
      inWidth_[(size_t) i] = w > kMaxBusChannels ? kMaxBusChannels : w;
    }
    for (Steinberg_int32 i = 0; i < nOut; ++i) {
      const uint32_t w = channelsIn(current(Steinberg_Vst_BusDirections_kOutput, i));
      outWidth_[(size_t) i] = w > kMaxBusChannels ? kMaxBusChannels : w;
    }
  }

  /** Storage, pointers and bus buffers for buses of these widths. */
  void layoutBuses(const std::vector<uint32_t>& widths, std::vector<float>& storage,
                   std::vector<float*>& ptrs, std::vector<Steinberg_Vst_AudioBusBuffers>& buses) {
    size_t total = 0;
    for (uint32_t w : widths) total += w;
    storage.assign(total * maxBlock_, 0.0f);
    ptrs.assign(total, nullptr);
    for (size_t c = 0; c < total; ++c) ptrs[c] = storage.data() + c * maxBlock_;
    buses.assign(widths.size(), Steinberg_Vst_AudioBusBuffers{});
    size_t at = 0;
    for (size_t bus = 0; bus < widths.size(); ++bus) {
      buses[bus].numChannels = (Steinberg_int32) widths[bus];
      buses[bus].silenceFlags = 0;
      buses[bus].Steinberg_Vst_AudioBusBuffers_channelBuffers32 = ptrs.data() + at;
      at += widths[bus];
    }
  }
  std::vector<Steinberg_Vst_Event> events_;
  MpscQueue<QueuedParam, kMaxQueuedParams> queued_;
  std::vector<ParamQueue> queues_;
  /** This block's output parameter changes, and the way they travel to
   *  drainParameterEdits(): the controller belongs to the main thread. */
  std::vector<OutParamQueue> outQueues_;
  MpscQueue<QueuedParam, kMaxOutChanges> outChanges_;
  MidiBuffer produced_;
};

inline void Vst3Plugin::unload() {
  release();
  // Released in the reverse order they were made. terminate() is owed once
  // per object and release() once per reference, which for a component that
  // is its own controller are not the same count.
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
    handler_.forward = nullptr;
    handler_.owner = nullptr;
  }
  // Disconnected each from the other before either is terminated, the
  // reverse of how they were joined, and the lines cut: a half that kept
  // its pointer reaches nothing.
  if (componentPoint_ && controllerPoint_) {
    componentPoint_->lpVtbl->disconnect(
        componentPoint_, reinterpret_cast<Steinberg_Vst_IConnectionPoint*>(toController_));
    controllerPoint_->lpVtbl->disconnect(
        controllerPoint_, reinterpret_cast<Steinberg_Vst_IConnectionPoint*>(toComponent_));
  }
  detail::cutVst3ConnectionProxy(toController_);
  detail::cutVst3ConnectionProxy(toComponent_);
  if (componentPoint_) componentPoint_->lpVtbl->release(componentPoint_);
  if (controllerPoint_) controllerPoint_->lpVtbl->release(controllerPoint_);
  componentPoint_ = controllerPoint_ = nullptr;
  // Every queryInterface that succeeded handed over a reference, and each is
  // owed back. The first version released the controller only when it was a
  // separate object and the MIDI mapping never, so a plugin whose component
  // is its own controller -- every SDK plugin, and most single-class ones --
  // was left two references up and never destroyed: its destructor never ran,
  // and ExitDll and the unload below took its code away from under it.
  if (midiMapping_) midiMapping_->lpVtbl->release(midiMapping_);
  midiMapping_ = nullptr;
  if (controller_) {
    // terminate() pairs with the initialize() this host called, which it only
    // did for a separate controller; the component's own runs further down.
    if (controllerIsSeparate_) controller_->lpVtbl->terminate(controller_);
    controller_->lpVtbl->release(controller_);
  }
  controller_ = nullptr;
  if (processor_) processor_->lpVtbl->release(processor_);
  processor_ = nullptr;
  if (component_) {
    // Owed for an initialize() that succeeded, and only then: one that
    // refused was told it was being shut down without ever having started.
    if (componentInitialized_) component_->lpVtbl->terminate(component_);
    component_->lpVtbl->release(component_);
  }
  component_ = nullptr;
  componentInitialized_ = false;
  // After both halves are terminated, which is where a plugin lets go of
  // the context it kept; one that keeps it longer keeps it alive.
  if (context_) detail::ctxRelease(context_);
  context_ = nullptr;
  if (factory_) factory_->lpVtbl->release(factory_);
  factory_ = nullptr;
  if (lib_) {
    if (moduleEntered_) vst3ModuleExit(lib_);
    closeLib(lib_);
  }
  moduleEntered_ = false;
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
          const Steinberg_int32 programs =
              (Steinberg_int32) detail::heldCount(listInfo.programCount, detail::kMaxPresets);
          for (Steinberg_int32 i = 0; i < programs; ++i) {
            Steinberg_Vst_String128 name{};
            if (units->lpVtbl->getProgramName(units, listInfo.id, i, name) !=
                Steinberg_kResultOk)
              continue;
            presetNames_.push_back(utf8FromVst3(name));
          }
        }
      }
      units->lpVtbl->release(units);
    }
  }

  const Steinberg_int32 n = (Steinberg_int32) detail::heldCount(
      controller_->lpVtbl->getParameterCount(controller_), detail::kMaxParameters);
  params_.reserve((size_t) (n < 4096 ? n : 4096)); // past that, grown as the plugin proves it
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
    p.name = utf8FromVst3(info.title);
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
  // The reserve above counted the hidden parameters too, and an instrument's
  // 2080 MIDI controller parameters left 217 KB of room behind the handful
  // it lists -- held for as long as the plugin is, by every instance. Down
  // to what was listed: one move of the list, at load.
  params_.shrink_to_fit();
  paramIndex_.build(params_);
  // One mailbox per listed parameter, made here at load because a full
  // queue is met on the audio and UI threads, where nothing may allocate.
  mailboxes_.reset(new Mailbox[params_.size() > 0 ? params_.size() : 1]);
  mailboxCount_ = params_.size();
  outBoxes_.reset(new Mailbox[params_.size() > 0 ? params_.size() : 1]);
  outBoxCount_ = params_.size();
  outSlotOf_.assign(params_.size(), -1);
}

inline void Vst3Plugin::process(AudioBlock<float>& io, const MidiBuffer* midi) {
  produced_.clear(); // this block's emissions only, whether or not it is processed
  if (!processor_ || !processing_) return;
  const uint32_t frames = (uint32_t) io.getNumSamples();
  if (frames == 0 || frames > maxBlock_) return;

  // The caller's channels meet the MAIN input bus, the first; every other
  // input is silent. A mono caller is heard on every channel of a wider bus.
  const uint32_t chans = (uint32_t) io.getNumChannels();
  for (size_t bus = 0; bus < inBuses_.size(); ++bus) {
    const Steinberg_Vst_AudioBusBuffers& b = inBuses_[bus];
    for (Steinberg_int32 c = 0; c < b.numChannels; ++c) {
      const uint32_t from = chans == 1 ? 0u : (uint32_t) c;
      const float* src = bus == 0 && from < chans ? io.getChannelPointer(from) : nullptr;
      float* dst = b.Steinberg_Vst_AudioBusBuffers_channelBuffers32[c];
      if (src) std::memcpy(dst, src, frames * sizeof(float));
      else std::memset(dst, 0, frames * sizeof(float));
    }
  }
  for (float* out : outPtrs_) std::memset(out, 0, frames * sizeof(float));

  events_.clear();
  if (midi && acceptsMidi_) {
    // Inside the block, as the CLAP side already keeps them: a MidiBuffer
    // does not know how long its block is, and one filled by a caller can
    // hold a position at or past the end, which went to the plugin as a
    // sampleOffset past numSamples. The buffer is sorted, and clamping keeps
    // it so.
    const int last = (int) frames - 1;
    for (const auto& entry : *midi) {
      const MidiMessage& m = entry.getMessage();
      Steinberg_Vst_Event e{};
      e.busIndex = 0;
      const int at = entry.samplePosition;
      e.sampleOffset = at < 0 ? 0 : (at > last ? last : at);
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
  QueuedParam q;
  while (queues_.size() < queues_.capacity() && queued_.pop(q)) {
    ParamQueue pq;
    pq.lpVtbl = paramQueueVtbl();
    pq.id = q.id;
    pq.value = q.value;
    queues_.push_back(pq);
  }
  // Then the mail, which is newer than anything the queue held for the same
  // parameter. Skipped in one load when there is none; what does not fit in
  // this block stays for the next rather than being dropped.
  if (mailAny_.exchange(false, std::memory_order_acq_rel)) {
    for (size_t i = 0; i < mailboxCount_; ++i) {
      Mailbox& box = mailboxes_[i];
      if (!box.dirty.load(std::memory_order_acquire)) continue;
      if (queues_.size() >= queues_.capacity()) {
        mailAny_.store(true, std::memory_order_release);
        break;
      }
      box.dirty.store(false, std::memory_order_release);
      ParamQueue pq;
      pq.lpVtbl = paramQueueVtbl();
      pq.id = (Steinberg_Vst_ParamID) params_[i].id;
      pq.value = box.value.load(std::memory_order_acquire);
      queues_.push_back(pq);
    }
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
      } else if (m.isProgramChange()) {
        controller = Steinberg_Vst_ControllerNumbers_kCtrlProgramChange;
        normalised = m.getProgramChangeNumber() / 127.0;
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
  EventList outEvents{};
  outEvents.lpVtbl = eventListVtbl();
  outEvents.produced = &produced_;


  Steinberg_Vst_ProcessContext context{};
  context.state = Steinberg_Vst_ProcessContext_StatesAndFlags_kPlaying |
                  Steinberg_Vst_ProcessContext_StatesAndFlags_kTempoValid |
                  Steinberg_Vst_ProcessContext_StatesAndFlags_kTimeSigValid |
                  Steinberg_Vst_ProcessContext_StatesAndFlags_kProjectTimeMusicValid;
  // The session's rate, and a musical position that MOVES. The context said
  // 48 kHz whatever the plugin was prepared at, and flagged a project time in
  // beats as valid while leaving it at zero -- so a tempo-synced LFO or an
  // arpeggiator hosted here saw a song frozen on its first beat. Still the
  // fixed 120 BPM in 4/4 this host has always reported: it has no transport
  // of its own to follow.
  context.sampleRate = sampleRate_;
  context.projectTimeSamples = projectTime_;
  context.tempo = 120.0;
  context.timeSigNumerator = 4;
  context.timeSigDenominator = 4;
  context.projectTimeMusic = (double) projectTime_ / sampleRate_ * (context.tempo / 60.0);
  context.barPositionMusic = std::floor(context.projectTimeMusic / 4.0) * 4.0;
  context.state |= Steinberg_Vst_ProcessContext_StatesAndFlags_kBarPositionValid;

  Steinberg_Vst_ProcessData data{};
  data.processMode = Steinberg_Vst_ProcessModes_kRealtime;
  data.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
  data.numSamples = (Steinberg_int32) frames;
  data.numInputs = (Steinberg_int32) inBuses_.size();
  data.numOutputs = (Steinberg_int32) outBuses_.size();
  data.inputs = inBuses_.empty() ? nullptr : inBuses_.data();
  data.outputs = outBuses_.empty() ? nullptr : outBuses_.data();
  data.inputParameterChanges = (Steinberg_Vst_IParameterChanges*) &changes;
  // Room for what the plugin changed itself. Left null, a processor had no
  // way to say so: a meter published as a read-only parameter never moved,
  // and a value its DSP set stayed old in its editor.
  outQueues_.clear();
  OutParameterChanges outChanges{};
  outChanges.lpVtbl = outParamChangesVtbl();
  outChanges.queues = &outQueues_;
  outChanges.index = &paramIndex_;
  outChanges.slotOf = outSlotOf_.empty() ? nullptr : outSlotOf_.data();
  outChanges.listedCount = outSlotOf_.size();
  data.outputParameterChanges = (Steinberg_Vst_IParameterChanges*) &outChanges;
  data.inputEvents = events_.empty() ? nullptr : (Steinberg_Vst_IEventList*) &inEvents;
  data.outputEvents = (Steinberg_Vst_IEventList*) &outEvents;
  data.processContext = &context;

  processThread_.store(std::this_thread::get_id(), std::memory_order_release);
  processor_->lpVtbl->process(processor_, &data);
  processThread_.store(std::thread::id(), std::memory_order_release);
  projectTime_ += frames;
  bool reported = false;
  for (const OutParamQueue& queue : outQueues_) {
    if (queue.listed >= 0) outSlotOf_[(size_t) queue.listed] = -1; // ready for the next block
    if (queue.points <= 0) continue;
    if (queue.listed >= 0 && (size_t) queue.listed < outBoxCount_) {
      Mailbox& box = outBoxes_[(size_t) queue.listed];
      box.value.store(queue.value, std::memory_order_relaxed);
      box.dirty.store(true, std::memory_order_release);
      reported = true;
    } else {
      outChanges_.push({queue.id, queue.value}); // false when full: bounded, never grown
    }
  }
  if (reported) outAny_.store(true, std::memory_order_release);

  // Back from the MAIN output bus only; aux outputs are the plugin's own. A
  // caller wider than the bus gets its last channel repeated, so a mono
  // output fills every channel, and no output bus at all leaves silence.
  const Steinberg_Vst_AudioBusBuffers* mainOut =
      !outBuses_.empty() && outBuses_[0].numChannels > 0 ? &outBuses_[0] : nullptr;
  for (uint32_t c = 0; c < chans; ++c) {
    float* dst = io.getChannelPointer(c);
    if (mainOut) {
      const Steinberg_int32 from =
          (Steinberg_int32) c < mainOut->numChannels ? (Steinberg_int32) c : mainOut->numChannels - 1;
      std::memcpy(dst, mainOut->Steinberg_Vst_AudioBusBuffers_channelBuffers32[from],
                  frames * sizeof(float));
    } else {
      std::memset(dst, 0, frames * sizeof(float));
    }
  }
}

/** A VST3 session blob that carries the controller's own state as well as
 *  the component's: this, then the component's length and bytes, then the
 *  controller's, lengths as 64-bit little-endian. A blob without it is the
 *  component's state alone -- what a plugin with no controller state saves,
 *  and every session saved before the controller's was kept. */
constexpr uint8_t kVst3StateFrame[8] = {'S', 'o', 'n', 'V', 'S', 'T', '3', 1};

inline bool Vst3Plugin::saveState(std::vector<uint8_t>& out) const {
  if (!component_) return false;
  out.clear();
  std::vector<uint8_t> component;
  MemoryStream stream(&component);
  if (component_->lpVtbl->getState(component_, (Steinberg_IBStream*) &stream) != Steinberg_kResultOk)
    return false;
  // And the controller's own: an editor's open tab, a zoom, anything a
  // plugin keeps in its controller alone. Saved without it, a session
  // reopened with the sound restored and the editor at its defaults.
  std::vector<uint8_t> controller;
  if (controller_) {
    MemoryStream controllerStream(&controller);
    if (controller_->lpVtbl->getState(controller_, (Steinberg_IBStream*) &controllerStream) !=
        Steinberg_kResultOk)
      controller.clear();
  }
  if (controller.empty()) {
    out.swap(component); // nothing to frame: the blob every host reads
    return true;
  }
  auto putLength = [&out](uint64_t n) {
    for (int i = 0; i < 8; ++i) out.push_back((uint8_t) (n >> (8 * i)));
  };
  out.insert(out.end(), kVst3StateFrame, kVst3StateFrame + sizeof(kVst3StateFrame));
  putLength(component.size());
  out.insert(out.end(), component.begin(), component.end());
  putLength(controller.size());
  out.insert(out.end(), controller.begin(), controller.end());
  return true;
}

inline bool Vst3Plugin::loadState(const uint8_t* data, size_t size) {
  if (!component_ || (!data && size > 0)) return false;
  // Framed, the two states; otherwise all of it is the component's. A frame
  // whose lengths do not add up is read as a component's state that happens
  // to start with the same bytes.
  std::vector<uint8_t> component, controller;
  bool framed = false;
  if (size >= sizeof(kVst3StateFrame) + 16 &&
      std::memcmp(data, kVst3StateFrame, sizeof(kVst3StateFrame)) == 0) {
    auto getLength = [data](size_t at) {
      uint64_t n = 0;
      for (int i = 0; i < 8; ++i) n |= (uint64_t) data[at + (size_t) i] << (8 * i);
      return n;
    };
    const size_t head = sizeof(kVst3StateFrame);
    const uint64_t componentSize = getLength(head);
    if (componentSize <= size - head - 16) {
      const size_t controllerAt = head + 8 + (size_t) componentSize;
      const uint64_t controllerSize = getLength(controllerAt);
      if (controllerSize == size - controllerAt - 8) {
        component.assign(data + head + 8, data + controllerAt);
        controller.assign(data + controllerAt + 8, data + size);
        framed = true;
      }
    }
  }
  if (!framed) component.assign(data, data + size);
  MemoryStream stream(&component);
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
  // Then its own state, after the component's, as the specification orders
  // them.
  if (controller_ && framed && !controller.empty()) {
    MemoryStream controllerStream(&controller);
    controller_->lpVtbl->setState(controller_, (Steinberg_IBStream*) &controllerStream);
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
#define SONORE_HOST_LV2_UI__resize SONORE_HOST_LV2_UI_URI "#resize"

/** LV2_Feature: a URI and whatever the extension it names hands over. */
struct HostLv2Feature {
  const char* URI;
  void* data;
};

// -- options:options and buf-size, from the host's side ----------------------
//
// Spelled here for the same reason: neither header is in the core package
// this SDK carries. A plugin built with the DPF framework -- a large share of
// the LV2 plugins there are -- will not instantiate without options:options,
// and reads its block size and rate from it.

#define SONORE_HOST_LV2_OPTIONS__options "http://lv2plug.in/ns/ext/options#options"
#define SONORE_HOST_LV2_BUF_SIZE_URI "http://lv2plug.in/ns/ext/buf-size"
#define SONORE_HOST_LV2_BUF_SIZE__boundedBlockLength SONORE_HOST_LV2_BUF_SIZE_URI "#boundedBlockLength"
#define SONORE_HOST_LV2_BUF_SIZE__maxBlockLength SONORE_HOST_LV2_BUF_SIZE_URI "#maxBlockLength"
#define SONORE_HOST_LV2_BUF_SIZE__minBlockLength SONORE_HOST_LV2_BUF_SIZE_URI "#minBlockLength"
#define SONORE_HOST_LV2_BUF_SIZE__sequenceSize SONORE_HOST_LV2_BUF_SIZE_URI "#sequenceSize"
#define SONORE_HOST_LV2_PARAMETERS__sampleRate "http://lv2plug.in/ns/ext/parameters#sampleRate"

/** LV2_Options_Option: one value the host states, keyed and typed by URID.
 *  A list ends with an entry whose key and value are both zero. */
struct HostLv2Option {
  uint32_t context; // LV2_OPTIONS_INSTANCE, 0
  uint32_t subject;
  LV2_URID key;
  uint32_t size;
  LV2_URID type;
  const void* value;
};

// -- worker:schedule, from the host's side -----------------------------------
//
// A plugin's slow work -- loading a sample, building an impulse response --
// cannot happen in run(), and a plugin owns no thread to do it on. It asks
// the host's: run() schedules a job, the host does it elsewhere through the
// plugin's work(), and the answer comes back through work_response() in the
// audio thread, after a run() and before the next. Samplers and convolvers
// will not instantiate without it.

#define SONORE_HOST_LV2_WORKER__schedule "http://lv2plug.in/ns/ext/worker#schedule"
#define SONORE_HOST_LV2_WORKER__interface "http://lv2plug.in/ns/ext/worker#interface"

/** LV2_Worker_Status is 0 for success and 2 for "no space". */
using HostLv2WorkerRespond = uint32_t (*)(void* handle, uint32_t size, const void* data);

/** LV2_Worker_Schedule: the host's half, given to the plugin. */
struct HostLv2WorkerSchedule {
  void* handle;
  uint32_t (*schedule_work)(void* handle, uint32_t size, const void* data);
};

/** LV2_Worker_Interface: the plugin's half, from extension_data(). */
struct HostLv2WorkerInterface {
  uint32_t (*work)(LV2_Handle, HostLv2WorkerRespond respond, void* respondHandle, uint32_t size,
                   const void* data);
  uint32_t (*work_response)(LV2_Handle, uint32_t size, const void* body);
  uint32_t (*end_run)(LV2_Handle);
};

/**
 * One direction of the worker's traffic: whole messages, each a 32-bit
 * length and its bytes, in a ring one thread writes and one reads. Lock-free
 * and allocation-free once made, because one end is the audio thread. A
 * message that does not fit is refused: LV2 has "no space" for exactly that.
 */
class HostLv2WorkRing {
public:
  void allocate(size_t bytes) {
    buffer_.assign(bytes, 0);
    write_.store(0, std::memory_order_relaxed);
    read_.store(0, std::memory_order_relaxed);
  }
  size_t capacity() const { return buffer_.size(); }

  bool write(uint32_t size, const void* data) {
    const size_t cap = buffer_.size();
    const size_t w = write_.load(std::memory_order_relaxed);
    const size_t used = w - read_.load(std::memory_order_acquire);
    const size_t need = sizeof(uint32_t) + (size_t) size;
    if (cap == 0 || (size && !data) || need > cap - used) return false;
    copyIn(w, &size, sizeof(uint32_t));
    copyIn(w + sizeof(uint32_t), data, size);
    write_.store(w + need, std::memory_order_release);
    return true;
  }

  /** The next message into `out`, which holds capacity() bytes. */
  bool read(uint8_t* out, uint32_t* size) {
    const size_t r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire)) return false;
    uint32_t n = 0;
    copyOut(r, &n, sizeof(uint32_t));
    copyOut(r + sizeof(uint32_t), out, n);
    read_.store(r + sizeof(uint32_t) + n, std::memory_order_release);
    *size = n;
    return true;
  }

private:
  // The positions only grow; the index into the buffer is their remainder.
  void copyIn(size_t at, const void* from, size_t n) {
    const size_t cap = buffer_.size(), i = at % cap, first = n < cap - i ? n : cap - i;
    if (first) std::memcpy(buffer_.data() + i, from, first);
    if (n > first) std::memcpy(buffer_.data(), static_cast<const uint8_t*>(from) + first, n - first);
  }
  void copyOut(size_t at, void* to, size_t n) const {
    const size_t cap = buffer_.size(), i = at % cap, first = n < cap - i ? n : cap - i;
    if (first) std::memcpy(to, buffer_.data() + i, first);
    if (n > first) std::memcpy(static_cast<uint8_t*>(to) + first, buffer_.data(), n - first);
  }

  std::vector<uint8_t> buffer_;
  std::atomic<size_t> write_{0}, read_{0};
};

/** How an interface tells the host a control moved. Format 0 means the buffer
 *  is one float and the port is an ordinary control port. */
using HostLv2UiWrite = void (*)(void* controller, uint32_t portIndex, uint32_t bufferSize,
                                uint32_t format, const void* buffer);

/** LV2UI_Descriptor: what a UI binary exports, one per UI it contains. */
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

/** LV2UI_Idle_Interface: called on the host's UI tick. A non-zero answer
 *  means the UI has closed itself. */
struct HostLv2UiIdle {
  int (*idle)(void* ui);
};

/** LV2UI_Resize, both ways round: as a feature the host offers, an
 *  interface asks for a size through it; as an interface's extension data,
 *  the host tells the interface its new size (the handle field then unused,
 *  the interface's own handle passed instead). Zero is success. */
struct HostLv2UiResize {
  void* handle;
  int (*ui_resize)(void* handle, int width, int height);
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

/** One port of an LV2 plugin, as the bundle's Turtle describes it: kind and
 *  direction, range and default, and what its values are called. */
struct Lv2Port {
  int index = -1;
  std::string symbol;
  bool isControl = false, isAudio = false, isAtom = false, isCv = false;
  bool isInput = false, isOutput = false;
  float defaultValue = 0.0f, minValue = 0.0f, maxValue = 1.0f;
  std::string name;
  // What a value of this control is called: lv2:integer, lv2:enumeration or
  // lv2:toggled make it stepped; scale points name values; the unit's symbol
  // follows a number.
  bool stepped = false, toggled = false;
  std::vector<std::pair<float, std::string>> scalePoints;
  std::string unit;
  // An atom port's atom:supports: whether it names midi:MidiEvent, and
  // whether it names anything at all -- a port that says it carries patch
  // messages is not where MIDI goes.
  bool supportsMidi = false, declaresSupport = false;
  // What the port is FOR, when it is the host's rather than the user's: its
  // lv2:designation (lv2:latency, lv2:enabled, lv2:freeWheeling), and whether
  // it carries lv2:reportsLatency, the older way to say the first.
  std::string designation;
  bool reportsLatency = false;
};

namespace detail {

/**
 * One parsed document's triples by subject, for a loader that asks it
 * hundreds of questions.
 *
 * turtle::Document reads every triple to answer one object() or objects(),
 * which is right for a scan asking a bundle for its name and its binary.
 * Loading an LV2 plugin asks a dozen questions of every port and two of
 * every port of every preset: a plugin with 256 parameters and 16 presets
 * made thirteen thousand such reads of documents of up to twelve thousand
 * triples, 180 ms per instance, and nine seconds for a session with fifty.
 * Here a subject's triples are found by a binary search instead.
 *
 * The answers are the document's own, in its order: the triples of one
 * subject are kept in the order the file wrote them, so object() is the
 * first one Document::object() finds and objects() the list it returns.
 * The document must outlive the index and not change while it is used.
 */
class TripleIndex {
public:
  explicit TripleIndex(const turtle::Document& doc) : doc_(&doc) {
    order_.resize(doc.triples.size());
    for (size_t i = 0; i < order_.size(); ++i) order_[i] = i;
    std::stable_sort(order_.begin(), order_.end(), [&doc](size_t a, size_t b) {
      return doc.triples[a].subject < doc.triples[b].subject;
    });
  }

  /** Positions in the document of the triples about one subject. */
  struct Range {
    std::vector<size_t>::const_iterator first, last;
    std::vector<size_t>::const_iterator begin() const { return first; }
    std::vector<size_t>::const_iterator end() const { return last; }
  };

  Range about(const std::string& subject) const {
    const auto& triples = doc_->triples;
    const auto first = std::lower_bound(order_.begin(), order_.end(), subject,
                                        [&triples](size_t i, const std::string& s) { return triples[i].subject < s; });
    const auto last = std::upper_bound(first, order_.end(), subject,
                                       [&triples](const std::string& s, size_t i) { return s < triples[i].subject; });
    return Range{first, last};
  }

  const turtle::Triple& at(size_t position) const { return doc_->triples[position]; }

  /** As Document::object(). */
  std::string object(const std::string& subject, const std::string& predicate) const {
    for (size_t i : about(subject))
      if (doc_->triples[i].predicate == predicate) return doc_->triples[i].object;
    return std::string();
  }

  /** As Document::objects(). */
  std::vector<std::string> objects(const std::string& subject, const std::string& predicate) const {
    std::vector<std::string> found;
    for (size_t i : about(subject))
      if (doc_->triples[i].predicate == predicate) found.push_back(doc_->triples[i].object);
    return found;
  }

private:
  const turtle::Document* doc_;
  std::vector<size_t> order_; // positions, by subject, each subject's in document order
};

} // namespace detail

/** The symbol of an LV2 units:unit -- one of the extension's own, or a unit
 *  the bundle declares with units:symbol (else rdfs:label). Empty for none,
 *  and for the units whose number is the whole story (coef, midiNote).
 *  `doc` is the bundle's turtle::Document, or a detail::TripleIndex of it. */
template <typename Triples>
inline std::string lv2UnitSymbol(const Triples& doc, const std::string& unit) {
  static const std::string kUnits = "http://lv2plug.in/ns/extensions/units#";
  if (unit.empty()) return std::string();
  if (unit.compare(0, kUnits.size(), kUnits) == 0) {
    static const char* const kSymbols[][2] = {
        {"db", "dB"},       {"hz", "Hz"},     {"khz", "kHz"},  {"mhz", "MHz"},
        {"ms", "ms"},       {"s", "s"},       {"min", "min"},  {"pc", "%"},
        {"cent", "ct"},     {"semitone12TET", "semi"},         {"bpm", "bpm"},
        {"oct", "oct"},     {"degree", "deg"},                 {"frame", "frames"},
        {"beat", "beats"},  {"bar", "bars"},  {"m", "m"},      {"cm", "cm"},
        {"mm", "mm"},       {"km", "km"},     {"inch", "in"},  {"mile", "mi"},
    };
    const std::string name = unit.substr(kUnits.size());
    for (const auto& u : kSymbols)
      if (name == u[0]) return u[1];
    return std::string();
  }
  std::string symbol = doc.object(unit, kUnits + "symbol");
  if (symbol.empty()) symbol = doc.object(unit, "http://www.w3.org/2000/01/rdf-schema#label");
  return symbol;
}

/** One pset:Preset, reduced to what applying it costs: port indices and the
 *  values they take.
 *
 *  A preset that says nothing about a port leaves it alone -- that is LV2's
 *  rule, not a shortcut here, and it is why this is a sparse list rather than
 *  a full control vector. State properties (state:state) are not read: our
 *  own plugins put everything in ports, and silently applying half a preset
 *  would be worse than declining a kind we cannot honour. */
struct Lv2Preset {
  std::string uri;  ///< The preset's own URI.
  std::string name; ///< Its rdfs:label, the name a menu shows.
  std::vector<std::pair<int, float>> values; ///< (port index, value) for each port it sets.
};

namespace detail {

/**
 * Atoms between an interface's thread and the audio thread, one direction
 * each: a single-producer, single-consumer ring of records, each a port
 * index and an atom -- header and body -- of any size the ring can hold.
 *
 * Variable-sized, which MpscQueue's fixed slots are not: what an interface
 * writes may be a file path, and what a plugin sends back its whole state as
 * text, and a slot big enough for those would make a queue enormous to carry
 * three bytes of MIDI. Single-producer is the format's own rule: an
 * interface calls write_function from its one thread, and only process()
 * writes the other way. Nothing allocates once it is made, and a producer
 * never waits -- a record that does not fit now is refused, and the caller
 * counts it.
 */
class AtomRing {
public:
  /** [main-thread, nothing running] `capacity`: a power of two, at least 8. */
  void allocate(size_t capacity) {
    storage_.assign(capacity, 0);
    mask_ = capacity - 1;
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
  }

  /** [producer] One record. False, and nothing written, when it does not
   *  fit in what the consumer has left free. */
  bool push(uint32_t port, const void* atom, uint32_t size) {
    if (storage_.empty() || (!atom && size > 0)) return false;
    const size_t need = kHeader + padded(size);
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t used = head - tail_.load(std::memory_order_acquire);
    if (need > storage_.size() - used) return false;
    const uint32_t header[2] = {port, size};
    copyIn(head, header, kHeader);
    copyIn(head + kHeader, atom, size);
    head_.store(head + need, std::memory_order_release);
    return true;
  }

  /** [consumer] The next record's port and size, left where it is. */
  bool peek(uint32_t& port, uint32_t& size) const {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (storage_.empty() || head_.load(std::memory_order_acquire) == tail) return false;
    uint32_t header[2];
    copyOut(tail, header, kHeader);
    port = header[0];
    size = header[1];
    return true;
  }
  /** [consumer] The next record's atom into `out`, which has room for the
   *  size peek() gave, and the record taken. */
  void take(void* out) {
    uint32_t port = 0, size = 0;
    if (!peek(port, size)) return;
    const size_t tail = tail_.load(std::memory_order_relaxed);
    copyOut(tail + kHeader, out, size);
    tail_.store(tail + kHeader + padded(size), std::memory_order_release);
  }
  /** [consumer] The next record, dropped. */
  void skip() {
    uint32_t port = 0, size = 0;
    if (!peek(port, size)) return;
    tail_.store(tail_.load(std::memory_order_relaxed) + kHeader + padded(size),
                std::memory_order_release);
  }
  /** [consumer] Everything written so far, dropped. */
  void clear() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

private:
  /** Port and size, ahead of every record; with every record padded to 8,
   *  a header never straddles the end of the storage. */
  static constexpr size_t kHeader = 8;
  static size_t padded(size_t n) { return (n + 7u) & ~(size_t) 7u; }

  void copyIn(size_t at, const void* from, size_t n) {
    const size_t start = at & mask_, first = (std::min)(n, storage_.size() - start);
    if (first > 0) std::memcpy(storage_.data() + start, from, first);
    if (n > first) std::memcpy(storage_.data(), static_cast<const uint8_t*>(from) + first, n - first);
  }
  void copyOut(size_t at, void* to, size_t n) const {
    const size_t start = at & mask_, first = (std::min)(n, storage_.size() - start);
    if (first > 0) std::memcpy(to, storage_.data() + start, first);
    if (n > first) std::memcpy(static_cast<uint8_t*>(to) + first, storage_.data(), n - first);
  }

  std::vector<uint8_t> storage_;
  size_t mask_ = 0;
  /** Bytes ever written and ever taken: each grows, and only its own side
   *  writes it. */
  std::atomic<size_t> head_{0}, tail_{0};
};

} // namespace detail

class Lv2Plugin;
inline std::unique_ptr<HostedPlugin> loadLv2(const PluginDescription&);

/** The LV2 side of it: a plugin loaded from its bundle -- the Turtle read for
 *  its ports and presets, the binary for its code -- and driven as
 *  HostedPlugin describes. Made by loadLv2(). Control ports are floats the
 *  host owns; a value's text comes from the bundle, since LV2 has no call
 *  that asks the plugin. */
class Lv2Plugin final : public HostedPlugin {
public:
  /** [main-thread, audio stopped] Release, close the interface, stop the
   *  worker, clean the instance up and close the binary. */
  ~Lv2Plugin() override { unload(); }

  const PluginDescription& description() const override { return desc_; }
  bool isValid() const override { return descriptor_ != nullptr; }

  bool prepare(double sampleRate, uint32_t maxBlockSize, uint32_t numChannels = 2) override;
  /** deactivate() only after activate(): the specification pairs them, and a
   *  release before any prepare, or a second release, deactivated an instance
   *  that was not active -- which a plugin may answer by freeing what it never
   *  allocated. */
  void release() override {
    if (active_ && handle_ && descriptor_ && descriptor_->deactivate) {
      // deactivate() is in the instantiation class too: not beside a work()
      // the worker is still doing. This is the main thread, which may wait.
      std::lock_guard<std::mutex> alone(workRunning_);
      descriptor_->deactivate(handle_);
    }
    active_ = false;
    resetPending_.store(false, std::memory_order_relaxed);
  }
  void reset() override {
    // LV2 has no reset entry point. Deactivating and reactivating is what the
    // specification says clears a plugin's state, and it is what a host does.
    // But both are in the instantiation class -- nothing else on the
    // instance beside them -- and a reset straight from here ran beside a
    // state restore on the main thread and the worker's work(). So it is
    // applied at the start of the next block, under the gate the block takes,
    // when no work() is under way (see process()), as CLAP's reset is.
    if (!handle_ || !descriptor_ || !active_) return;
    resetPending_.store(true, std::memory_order_release);
  }

  void process(AudioBlock<float>& io, const MidiBuffer* midi = nullptr) override;

  /** The lv2:latency output, as the last block left it, and only if it is a
   *  number of samples: below a billion, as the LADSPA side has it. The port
   *  is a float the plugin writes, and 1e10 converted to uint32 is undefined
   *  -- it came back as 1,410,065,408 -- and it was read from the port
   *  itself, which the audio thread writes during run(). */
  uint32_t latencySamples() const override {
    if (latencyPort_ < 0 || (size_t) latencyPort_ >= controls_.size()) return 0;
    const size_t at = (size_t) latencyPort_;
    const float value = at < outputControl_.size() && outputControl_[at]
                            ? reported_[at].load(std::memory_order_relaxed)
                            : controls_[at];
    return value > 0.0f && value < 1.0e9f ? (uint32_t) (value + 0.5f) : 0u;
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

  /** What the bundle lets a value be called: a scale point's label, On or
   *  Off for a toggle, else the number -- whole for a stepped control -- and
   *  the unit's symbol. LV2 has no call into the plugin for its own text, so
   *  this is everything there is to say. */
  std::string parameterText(int index, double value) const override {
    const Lv2Port* port = nullptr;
    if (index >= 0 && index < (int) params_.size())
      for (const Lv2Port& p : ports_)
        if (p.index == (int) params_[(size_t) index].id) port = &p;
    char buffer[96];
    if (!port) {
      std::snprintf(buffer, sizeof(buffer), "%g", value);
      return std::string(buffer);
    }
    const double tolerance = 1e-6 * (std::fabs(value) > 1.0 ? std::fabs(value) : 1.0);
    for (const auto& point : port->scalePoints)
      if (std::fabs((double) point.first - value) <= tolerance) return point.second;
    if (port->toggled) return value >= 0.5 ? "On" : "Off";
    if (port->stepped) std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    else std::snprintf(buffer, sizeof(buffer), "%g", value);
    std::string text(buffer);
    if (!port->unit.empty()) text += " " + port->unit;
    return text;
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

    // urid:map and unmap beside the parent: an interface that talks to its
    // plugin in atoms names everything it says -- atom:eventTransfer, its
    // own message types -- through the map, and was given none, so it could
    // say nothing. The same map the plugin has, so both ends agree on ids.
    map_.handle = this;
    map_.map = mapUri;
    unmap_.handle = this;
    unmap_.unmap = unmapUri;
    HostLv2Feature parentFeature{SONORE_HOST_LV2_UI__parent, parent};
    HostLv2Feature mapFeature{LV2_URID__map, &map_};
    HostLv2Feature unmapFeature{LV2_URID__unmap, &unmap_};
    // The plugin's own options, the rate among them: an interface built with
    // the DPF framework will not instantiate without them either. Before the
    // first prepare() the list is empty -- its first entry is the end.
    HostLv2Feature optionsFeature{SONORE_HOST_LV2_OPTIONS__options, options_};
    // ui:resize, so the interface can say the size it wants: heard here and
    // answered by editorSize(), where the widget's own size cannot be read.
    uiRequestedWidth_ = uiRequestedHeight_ = 0;
    uiResizeRequest_.handle = this;
    uiResizeRequest_.ui_resize = uiAsksForSize;
    HostLv2Feature resizeFeature{SONORE_HOST_LV2_UI__resize, &uiResizeRequest_};
    const HostLv2Feature* features[] = {&parentFeature, &mapFeature,    &unmapFeature,
                                        &optionsFeature, &resizeFeature, nullptr};

    // The queues the interface and the audio thread pass atoms through, made
    // at the first open and kept until the plugin goes, never remade, so
    // neither end allocates. 64 KB toward the plugin holds dozens of
    // messages between two blocks; 128 KB toward the interface holds five
    // full output buffers, a few blocks' worth of a busy notify port between
    // two of the caller's idle ticks. Past that, dropped and counted. They
    // were made at load, and a session of LV2 plugins whose editors were
    // never opened held 192 KB per plugin for them. Before instantiate(): an
    // interface may write as it starts. Published to the audio thread by
    // uiRings_, which deliverFromUi() reads before it touches fromUi_.
    if (uridEventTransfer_ != 0 && !uiRings_.load(std::memory_order_relaxed)) {
      fromUi_.allocate((size_t) 1 << 16);
      toUi_.allocate((size_t) 1 << 17);
      uiRings_.store(true, std::memory_order_release);
    }

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
    // control is what a host owes a UI it just opened -- the outputs too.
    uiEchoed_.assign(controls_.size(), 0.0f);
    for (const Lv2Port& p : ports_)
      if (p.isControl && (p.isInput || p.isOutput)) sendPortToUi(p.index, true);
    // What the plugin sent while no interface listened is a closed one's
    // news; from here on its atom outputs are copied for this one.
    toUi_.clear();
    uiListening_.store(true, std::memory_order_release);
    return true;
  }

  /** The interface's own ui:resize: the widget is sized first, as a host
   *  does when its window is dragged, and the interface is then told. One
   *  without the interface cannot be resized from here. */
  bool setEditorSize(uint32_t width, uint32_t height) override {
    if (!uiHandle_ || !uiDesc_ || !uiDesc_->extension_data || width == 0 || height == 0) return false;
    const auto* resize =
        static_cast<const HostLv2UiResize*>(uiDesc_->extension_data(SONORE_HOST_LV2_UI__resize));
    if (!resize || !resize->ui_resize) return false;
#if defined(_WIN32)
    if (uiWidget_)
      SetWindowPos((HWND) uiWidget_, nullptr, 0, 0, (int) width, (int) height,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
    if (resize->ui_resize(uiHandle_, (int) width, (int) height) != 0) return false;
    uiRequestedWidth_ = width;
    uiRequestedHeight_ = height;
    return true;
  }

  void closeEditor() override {
    uiListening_.store(false, std::memory_order_release);
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
    // Non-zero is the interface saying it has closed itself -- a close box
    // of its own, a renderer that died -- and the host is to stop calling
    // it. This went on calling idle() and port_event() on it every tick,
    // and kept it "open", so the caller could not open it again: an editor
    // gone from the screen and impossible to bring back. It is an embedded
    // one, with no showInterface to show it again by, so it is cleaned up
    // as if the caller had closed it -- editorSize() says no more, and
    // openEditor() makes a new one.
    if (uiIdle_ && uiIdle_->idle && uiHandle_ && uiIdle_->idle(uiHandle_) != 0) {
      closeEditor();
      return;
    }
    // The other direction, on the same beat: a control that moved for any
    // reason -- automation, a preset, another editor -- has to reach the
    // interface, or it shows numbers that stopped being true. And every
    // OUTPUT control, which is how a meter, a gain reduction or anything
    // else the plugin reports reaches an interface that shares no memory
    // with it: only the inputs were sent, and every meter in an LV2 editor
    // stood still, the SDK's own among them.
    for (const Lv2Port& p : ports_)
      if (p.isControl && (p.isInput || p.isOutput)) sendPortToUi(p.index, false);
    deliverToUi();
  }

  /** Atoms that went no further between the plugin and its interface, each
   *  way: a queue that was full drops what does not fit and counts it here,
   *  rather than make the audio thread or the interface wait. */
  uint64_t droppedUiEvents() const {
    return droppedFromUi_.load(std::memory_order_relaxed) +
           droppedToUi_.load(std::memory_order_relaxed);
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
    // linking the toolkit it belongs to. What the interface asked for, or was
    // given, through ui:resize is the size there is to answer.
    if (uiRequestedWidth_ == 0 || uiRequestedHeight_ == 0) return false;
    width = uiRequestedWidth_;
    height = uiRequestedHeight_;
    return true;
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
    if (self && format != 0) {
      self->uiWriteAtom(portIndex, bufferSize, format, buffer);
      return;
    }
    if (!self || bufferSize != sizeof(float) || !buffer) return;
    if (portIndex >= self->controls_.size()) return;
    const float value = *static_cast<const float*>(buffer);
    self->controls_[portIndex] = value;
    if (portIndex < self->uiEchoed_.size()) self->uiEchoed_[portIndex] = value;

    const int index = self->paramIndex_.find(portIndex);
    if (index >= 0 && self->edits_.size() < 4096)
      self->edits_.push_back({ParamEdit::Kind::kValue, index, (double) value});
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
    // An output is the audio thread's to write: read as the last block left
    // it, from its copy, not from the port the plugin may be writing now.
    const size_t at = (size_t) index;
    const float value = at < outputControl_.size() && outputControl_[at]
                            ? reported_[at].load(std::memory_order_relaxed)
                            : controls_[at];
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
  /** ui:resize as a feature: the size the interface last asked for. */
  HostLv2UiResize uiResizeRequest_{};
  uint32_t uiRequestedWidth_ = 0, uiRequestedHeight_ = 0;
  static int uiAsksForSize(void* handle, int width, int height) {
    auto* self = static_cast<Lv2Plugin*>(handle);
    if (!self || width <= 0 || height <= 0) return 1;
    self->uiRequestedWidth_ = (uint32_t) width;
    self->uiRequestedHeight_ = (uint32_t) height;
#if defined(_WIN32)
    if (self->uiWidget_)
      SetWindowPos((HWND) self->uiWidget_, nullptr, 0, 0, width, height,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
#endif
    return 0;
  }
  void* uiHandle_ = nullptr;
  void* uiWidget_ = nullptr;
  std::vector<float> uiEchoed_;
  /** Which control ports are outputs, and each one's value as the last
   *  block left it -- copied after run() on the audio thread, read by the
   *  interface's main thread. Sized at load, so process() allocates nothing. */
  std::vector<char> outputControl_;
  std::unique_ptr<std::atomic<float>[]> reported_;
  /** [audio-thread] After run(): every output control into its copy. */
  void publishOutputs() {
    for (size_t i = 0; i < outputControl_.size(); ++i)
      if (outputControl_[i]) reported_[i].store(controls_[i], std::memory_order_relaxed);
  }

  // ── Atoms between the plugin and its interface ──────────────────────────
  //
  // An interface that talks in atoms writes them through write_function as
  // atom:eventTransfer -- one atom, which the host delivers to the plugin as
  // one event on the atom input it names -- and hears the plugin's atom
  // outputs through port_event the same way, an atom per event. It is the
  // only channel there is for anything that is not one float: which file a
  // sampler holds, a plugin's state, which keys are down. This host dropped
  // the writes and forwarded nothing, so every such interface sat blank --
  // the SDK's own LV2 editor among them. Each direction is a bounded ring,
  // the audio thread's end lock-free and allocation-free.

  /** [ui-thread] An atom the interface wrote, queued for the next block.
   *  eventTransfer only, to an atom input, and only whole: an atom claiming
   *  more than the buffer that carries it is refused. */
  void uiWriteAtom(uint32_t port, uint32_t bufferSize, uint32_t format, const void* buffer) {
    if (!buffer || uridEventTransfer_ == 0 || format != uridEventTransfer_) return;
    if (bufferSize < sizeof(LV2_Atom)) return;
    const auto* atom = static_cast<const LV2_Atom*>(buffer);
    if (atom->size > bufferSize - sizeof(LV2_Atom)) return;
    bool atomInput = false;
    for (const Lv2Port& p : ports_)
      if (p.index == (int) port && p.isAtom && p.isInput) atomInput = true;
    if (!atomInput) return;
    if (!fromUi_.push(port, buffer, (uint32_t) (sizeof(LV2_Atom) + atom->size)))
      droppedFromUi_.fetch_add(1, std::memory_order_relaxed);
  }

  /** [audio-thread] What the interface wrote since the last block, each as
   *  an event at the block's first frame on the atom input it named, ahead
   *  of any MIDI, so every sequence stays in time order. One that does not
   *  fit in this block's buffer waits for the next, and everything after it
   *  with it, so nothing overtakes; one no buffer could hold is dropped and
   *  counted. */
  void deliverFromUi() {
    if (!uiRings_.load(std::memory_order_acquire)) return; // no interface opened yet
    uint32_t port = 0, size = 0;
    while (fromUi_.peek(port, size)) {
      uint8_t* buffer = nullptr;
      for (const AtomSlot& slot : atomSlots_)
        if (slot.input && slot.port == (int) port) buffer = atomStorage_.data() + slot.offset;
      // The frame number, then the atom as written, padded to 8.
      const uint64_t need = sizeof(int64_t) + (((uint64_t) size + 7u) & ~(uint64_t) 7u);
      if (!buffer || sizeof(LV2_Atom_Sequence) + need > atomBytes_) {
        fromUi_.skip();
        droppedFromUi_.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(buffer);
      if (sizeof(LV2_Atom) + seq->atom.size + need > atomBytes_) break;
      auto* ev = reinterpret_cast<LV2_Atom_Event*>(reinterpret_cast<uint8_t*>(&seq->body) +
                                                   seq->atom.size);
      ev->time.frames = 0;
      fromUi_.take(&ev->body);
      seq->atom.size += (uint32_t) need;
    }
  }

  /** [audio-thread] Every event the plugin wrote to one atom output, as far
   *  as it wrote and no further. The sequence is the PLUGIN's: an output it
   *  left alone is still the atom:Chunk primeAtomPorts() put there, its
   *  bytes a block gone by, which were read again as new; and a size or an
   *  event running past the buffer the host gave was walked as given, off
   *  the end of it. Counted in 64 bits, so no event size can wrap the walk
   *  back to its start. */
  template <typename Fn>
  void forEachOutputEvent(const uint8_t* buffer, Fn&& fn) const {
    const auto* seq = reinterpret_cast<const LV2_Atom_Sequence*>(buffer);
    const uint64_t capacity = atomBytes_ - sizeof(LV2_Atom);
    if (seq->atom.type != uridSequence_ || seq->atom.size > capacity) return;
    const uint64_t size = seq->atom.size;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(&seq->body);
    uint64_t offset = sizeof(LV2_Atom_Sequence_Body);
    while (offset + sizeof(LV2_Atom_Event) <= size) {
      const auto* ev = reinterpret_cast<const LV2_Atom_Event*>(base + offset);
      const uint64_t body = ev->body.size;
      if (offset + sizeof(LV2_Atom_Event) + body > size) break;
      fn(ev);
      offset += sizeof(LV2_Atom_Event) + ((body + 7u) & ~(uint64_t) 7u);
    }
  }

  /** [audio-thread] After run(), while an interface listens: every event
   *  on every atom output, copied out of the block for it -- except the
   *  MIDI on the MIDI output, which is producedMidi()'s. A full queue drops
   *  and counts rather than wait. */
  void forwardToUi() {
    if (!uiListening_.load(std::memory_order_acquire)) return;
    for (const AtomSlot& slot : atomSlots_) {
      if (slot.input) continue;
      const bool midiPort = slot.port == atomOutPort_;
      forEachOutputEvent(atomStorage_.data() + slot.offset, [&](const LV2_Atom_Event* ev) {
        if (midiPort && ev->body.type == uridMidiEvent_) return;
        if (!toUi_.push((uint32_t) slot.port, &ev->body,
                        (uint32_t) (sizeof(LV2_Atom) + ev->body.size)))
          droppedToUi_.fetch_add(1, std::memory_order_relaxed);
      });
    }
  }

  /** [main-thread] What the plugin sent since the last tick, to the open
   *  interface's port_event, an atom per event as eventTransfer has it. */
  void deliverToUi() {
    uint32_t port = 0, size = 0;
    while (toUi_.peek(port, size)) {
      if (uiScratch_.size() < size) uiScratch_.resize(size);
      toUi_.take(uiScratch_.data());
      if (uiHandle_ && uiDesc_ && uiDesc_->port_event)
        uiDesc_->port_event(uiHandle_, port, size, uridEventTransfer_, uiScratch_.data());
    }
  }

  detail::AtomRing fromUi_, toUi_;
  /** The two above are made: raised once, by the first openEditor(), and
   *  never lowered. */
  std::atomic<bool> uiRings_{false};
  /** True while an interface is open: the audio thread copies for it. */
  std::atomic<bool> uiListening_{false};
  std::atomic<uint64_t> droppedFromUi_{0}, droppedToUi_{0};
  /** Where one atom on its way to the interface is put together. */
  std::vector<uint8_t> uiScratch_;
  LV2_URID uridEventTransfer_ = 0;
  LV2_URID_Unmap unmap_{};

  void unload() {
    release();
    // The interface reads the ports this object owns, so it goes first.
    closeEditor();
    stopWorker(); // it calls into the instance
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
    std::lock_guard<std::mutex> lock(uridLock_);
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
    // Locked: a plugin may map from its worker or its UI while another
    // thread maps too, as jalv allows for. Four threads mapping at once
    // got over a thousand inconsistent ids from the bare map, and sometimes
    // a crash. Not real-time safe, which the spec says it need not be:
    // plugins cache the ids they need on the audio thread.
    std::lock_guard<std::mutex> lock(self->uridLock_);
    auto found = self->urids_.find(uri);
    if (found != self->urids_.end()) return found->second;
    const LV2_URID id = (LV2_URID) (self->urids_.size() + 1);
    self->urids_[uri] = id;
    return id;
  }
  /** The other way, for an interface that asks for urid:unmap. The string
   *  is the map's own key, which stays where it is for the object's life. */
  static const char* unmapUri(LV2_URID_Unmap_Handle handle, LV2_URID id) {
    auto* self = static_cast<Lv2Plugin*>(handle);
    std::lock_guard<std::mutex> lock(self->uridLock_);
    for (const auto& entry : self->urids_)
      if (entry.second == id) return entry.first.c_str();
    return nullptr;
  }

  LibHandle lib_ = nullptr;
  const LV2_Descriptor* descriptor_ = nullptr;
  LV2_Handle handle_ = nullptr;
  PluginDescription desc_;
  std::vector<HostedParam> params_;
  ParamIdIndex paramIndex_;
  std::vector<Lv2Port> ports_;
  std::map<std::string, LV2_URID> urids_;
  mutable std::mutex uridLock_;

  std::vector<float> controls_;
  std::vector<int> audioIn_, audioOut_;
  /** Audio ports beyond the main channels, which this host does not route. */
  std::vector<float> spareIn_, spareOut_;
  std::vector<float> cvIn_, cvOut_, unknownPort_;
  int latencyPort_ = -1, atomInPort_ = -1, atomOutPort_ = -1, enabledPort_ = -1;

  std::vector<float> storage_;
  std::vector<float*> inPtrs_, outPtrs_;
  /** A buffer for EVERY atom port, each its own, side by side in
   *  atomStorage_. They all shared one buffer each way, so a plugin with a
   *  MIDI output and a UI notification output -- every LV2 plugin this SDK
   *  builds that makes MIDI -- wrote both into one place: the notification
   *  port, claimed after the MIDI was written, reset the sequence, and the
   *  host read back no MIDI at all. Atom inputs other than the MIDI one get
   *  an empty sequence rather than the MIDI meant for it. */
  struct AtomSlot {
    int port;
    bool input;
    size_t offset;
  };
  std::vector<AtomSlot> atomSlots_;
  std::vector<uint8_t> atomStorage_;
  size_t atomBytes_ = 0;
  /** The MIDI input's and output's buffers inside atomStorage_, or null. */
  uint8_t* midiIn_ = nullptr;
  uint8_t* midiOut_ = nullptr;
  /** [audio-thread] Every atom input an empty sequence, and every output an
   *  atom:Chunk as long as its buffer, as LV2 has a host leave them before
   *  run(): the plugin writes its own atom over the chunk, and one still
   *  there afterwards is an output it did not write. */
  void primeAtomPorts() {
    for (const AtomSlot& slot : atomSlots_) {
      auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(atomStorage_.data() + slot.offset);
      seq->atom.type = slot.input ? uridSequence_ : uridChunk_;
      seq->atom.size = slot.input ? (uint32_t) sizeof(LV2_Atom_Sequence_Body)
                                  : (uint32_t) (atomBytes_ - sizeof(LV2_Atom));
      seq->body.unit = 0;
      seq->body.pad = 0;
    }
  }
  MidiBuffer produced_;
  /** The URIDs process() needs, looked up ONCE.
   *
   *  Calling mapUri() per block cost four allocations a block: the map is
   *  keyed by std::string and a const char* lookup builds a temporary, and
   *  these URIs are far too long for the small-string optimisation. The
   *  allocation audit reported 800 across 200 blocks, which is exactly that. */
  LV2_URID uridSequence_ = 0, uridMidiEvent_ = 0, uridChunk_ = 0;
  /** State properties with no instance to hold them: what loadState() was
   *  given before the first prepare(). An instance is made only there, and
   *  the restore into nothing was dropped. */
  std::vector<StateProperty> heldState_;
  LV2_URID_Map map_{};
  LV2_Feature mapFeature_{};
  LV2_Feature unmapFeature_{}, optionsFeature_{}, boundedFeature_{};
  /** What options:options states -- the rate, the block lengths process()
   *  keeps to, the atom buffers' size -- and the values it points at. Set by
   *  prepare() before each instance, and kept for its life. */
  HostLv2Option options_[5]{};
  float optionRate_ = 0.0f;
  int32_t optionMaxBlock_ = 0, optionMinBlock_ = 1, optionSequenceSize_ = 0;
  std::vector<const LV2_Feature*> features_;

  /** The worker (see HostLv2WorkerSchedule): a thread doing what run()
   *  scheduled, the rings between it and the audio thread, and the audio
   *  thread's copy of an answer. The thread exists only for an instance
   *  with a worker interface, and stops before that instance goes. */
  HostLv2WorkerSchedule workSchedule_{};
  LV2_Feature workFeature_{};
  const HostLv2WorkerInterface* workIface_ = nullptr;
  HostLv2WorkRing workRequests_, workResponses_;
  /** Where the audio thread reads an answer, and the worker a job: both
   *  sized by startWorker(), on the thread that prepares. */
  std::vector<uint8_t> workScratch_, workJob_;
  std::thread workThread_;
  std::mutex workMutex_;
  std::condition_variable workWake_;
  bool workStop_ = false;
  /** Raised by stopWorker(): the jobs still queued are dropped, not done. */
  std::atomic<bool> workDropping_{false};
  static constexpr size_t kWorkRingBytes = 65536;

  /** [audio thread] The plugin's schedule_work(): queued for the worker. */
  static uint32_t scheduleWork(void* handle, uint32_t size, const void* data) {
    auto* self = static_cast<Lv2Plugin*>(handle);
    if (!self->workRequests_.write(size, data)) return 2; // LV2_WORKER_ERR_NO_SPACE
    // Not under the mutex: this is the audio thread. The worker also looks
    // every few milliseconds, so a wake-up lost between its look and its
    // wait costs that and no more.
    self->workWake_.notify_one();
    return 0;
  }
  /** [worker thread] work()'s answer, queued for the audio thread. */
  static uint32_t respondWork(void* handle, uint32_t size, const void* data) {
    return static_cast<Lv2Plugin*>(handle)->workResponses_.write(size, data) ? 0 : 2;
  }
  void startWorker() {
    workIface_ = descriptor_->extension_data
                     ? static_cast<const HostLv2WorkerInterface*>(
                           descriptor_->extension_data(SONORE_HOST_LV2_WORKER__interface))
                     : nullptr;
    if (!workIface_ || !workIface_->work) {
      workIface_ = nullptr;
      return;
    }
    workScratch_.assign(workResponses_.capacity(), 0);
    // Here, not on the worker as it starts: a thread that allocates at some
    // moment after prepare() returns allocates while the audio already runs,
    // which is how host_test's count of allocations inside process() came
    // out at 1 instead of 0 on one run in four.
    workJob_.assign(workRequests_.capacity(), 0);
    workStop_ = false;
    workDropping_.store(false, std::memory_order_relaxed);
    workThread_ = std::thread([this] { workLoop(); });
  }
  void workLoop() {
    std::unique_lock<std::mutex> lock(workMutex_);
    while (!workStop_) {
      lock.unlock();
      uint32_t size = 0;
      // Until stopWorker() says to drop the rest. It said so, and the loop
      // went on through every job still queued -- thousands, for a plugin
      // scheduling one a block -- so an unload or a prepare waited for all
      // of them: two minutes in lv2_ports_test.
      while (!workDropping_.load(std::memory_order_acquire) &&
             workRequests_.read(workJob_.data(), &size)) {
        // Never beside a restore(), which takes this (see restoreAlone) --
        // and one waiting goes first: the lock is not fair, and a worker
        // taking it straight back after every job held a restore off for
        // as long as jobs kept coming.
        while (restoreWanted_.load(std::memory_order_acquire))
          std::this_thread::sleep_for(std::chrono::microseconds(200));
        std::lock_guard<std::mutex> alone(workRunning_);
        workIface_->work(handle_, respondWork, this, size, workJob_.data());
      }
      lock.lock();
      if (!workStop_) workWake_.wait_for(lock, std::chrono::milliseconds(5));
    }
  }
  /** Before the instance it works for goes: what is still queued is dropped
   *  with it. */
  void stopWorker() {
    if (workThread_.joinable()) {
      workDropping_.store(true, std::memory_order_release);
      {
        std::lock_guard<std::mutex> lock(workMutex_);
        workStop_ = true;
      }
      workWake_.notify_one();
      workThread_.join();
    }
    workIface_ = nullptr;
  }
  /**
   * [main-thread] The state extension's restore(), with the instance to
   * itself.
   *
   * restore() "MUST NOT be called concurrently with any other function on
   * the same plugin instance" -- LV2 is not CLAP or VST3 here, whose state
   * load a plugin must take beside its process() -- and loadState() called it
   * straight into a plugin the audio thread was running and its worker was
   * working: a sampler swapping its sample in restore() swapped it under its
   * own run(). The worker is held off by workRunning_, and the audio thread
   * by the gate: a block that starts while a restore holds the instance is
   * not processed -- left as it came in, as HostedPlugin has it for a block
   * the plugin cannot take -- rather than waiting on the audio thread for
   * however long a plugin's restore() takes. This waits for at most the
   * block or the job under way.
   */
  void restoreAlone(const LV2_State_Interface* state, std::vector<StateProperty>* properties) {
    // Announced first, so neither the worker nor the audio thread takes
    // its turn again: each had only to come straight back to win every
    // race, and an audio thread that never pauses -- an offline render --
    // with a worker always busy kept a restore waiting for seconds a time.
    restoreWanted_.store(true, std::memory_order_seq_cst);
    std::lock_guard<std::mutex> alone(workRunning_);
    int expected = kGateFree;
    while (!gate_.compare_exchange_weak(expected, kGateRestoring, std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
      expected = kGateFree;
      std::this_thread::yield();
    }
    const LV2_Feature* const none[] = {nullptr};
    state->restore(handle_, retrieveProperty, properties, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE,
                   none);
    gate_.store(kGateFree, std::memory_order_release);
    restoreWanted_.store(false, std::memory_order_release);
  }
  /** Who has the instance: nobody, run() on the audio thread, or a
   *  restore() on the main one. */
  static constexpr int kGateFree = 0, kGateRunning = 1, kGateRestoring = 2;
  std::atomic<int> gate_{kGateFree};
  /** reset() was asked for: the next block that has the instance, with no
   *  work() under way, deactivates and activates it first. */
  std::atomic<bool> resetPending_{false};
  /** A restore is waiting for the instance, or has it. */
  std::atomic<bool> restoreWanted_{false};
  /** Held by the worker around each work(), and by a restore(). */
  std::mutex workRunning_;

  /** [audio thread] After each run(): the answers that are ready, in the
   *  order they were given, then end_run(). */
  void deliverWorkResponses() {
    if (!workIface_) return;
    uint32_t size = 0;
    while (workResponses_.read(workScratch_.data(), &size))
      if (workIface_->work_response) workIface_->work_response(handle_, size, workScratch_.data());
    if (workIface_->end_run) workIface_->end_run(handle_);
  }

  uint32_t maxBlock_ = 0, channels_ = 2;
  bool active_ = false;
};

inline bool Lv2Plugin::prepare(double sampleRate, uint32_t maxBlockSize, uint32_t numChannels) {
  if (!descriptor_) return false;
  // What the new instance must start from: the old one's state, taken before
  // it goes, or what loadState() held for an instance that did not exist yet.
  // Every prepare makes a new instance in this format, and both were lost: a
  // session's restore-then-prepare dropped a sampler's file while returning
  // true, and any later prepare -- a block-size change -- wiped it again.
  std::vector<StateProperty> carried;
  const LV2_State_Interface* state = stateInterface();
  if (handle_) {
    if (state && state->save) {
      const LV2_Feature* const none[] = {nullptr};
      state->save(handle_, storeProperty, &carried, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, none);
    }
    release();
    stopWorker();
    if (descriptor_->cleanup) descriptor_->cleanup(handle_);
    handle_ = nullptr;
  } else {
    carried.swap(heldState_);
  }
  maxBlock_ = maxBlockSize > 0 ? maxBlockSize : 1;
  channels_ = numChannels > 0 ? (numChannels > 8 ? 8 : numChannels) : 1;

  // The rate is fixed at INSTANTIATE in this format, so re-preparing means a
  // whole new instance. Nothing else here works that way.
  map_.handle = this;
  map_.map = mapUri;
  mapFeature_.URI = LV2_URID__map;
  mapFeature_.data = &map_;
  // A full MidiBuffer's worth of events each way: 24 bytes each (the event
  // header and a three-byte message padded to eight). 8 KB held 341 and the
  // rest of a dense block was dropped without a word, note-offs included.
  // Sized here, before the instance: options:options states it.
  const size_t atomBytes = sizeof(LV2_Atom_Sequence) +
                           (size_t) MidiBuffer::kMaxEvents * (sizeof(LV2_Atom_Event) + 8);
  atomBytes_ = atomBytes > 8192 ? atomBytes : 8192; // a multiple of 8: each slot stays aligned

  // The rest of what a plugin may need before it exists. A plugin built with
  // the DPF framework refused to instantiate without options:options, and
  // urid:unmap was given only to the interface. The block lengths are the
  // ones process() keeps to -- it refuses an empty or oversized block -- so
  // boundedBlockLength is a promise kept, and the minimum is one frame.
  unmap_.handle = this;
  unmap_.unmap = unmapUri;
  unmapFeature_.URI = LV2_URID__unmap;
  unmapFeature_.data = &unmap_;
  optionRate_ = (float) sampleRate;
  optionMaxBlock_ = (int32_t) maxBlock_;
  optionMinBlock_ = 1;
  optionSequenceSize_ = (int32_t) atomBytes_;
  const LV2_URID atomInt = mapUri(this, LV2_ATOM__Int), atomFloat = mapUri(this, LV2_ATOM__Float);
  options_[0] = {0, 0, mapUri(this, SONORE_HOST_LV2_PARAMETERS__sampleRate), (uint32_t) sizeof(float),
                 atomFloat, &optionRate_};
  options_[1] = {0, 0, mapUri(this, SONORE_HOST_LV2_BUF_SIZE__maxBlockLength), (uint32_t) sizeof(int32_t),
                 atomInt, &optionMaxBlock_};
  options_[2] = {0, 0, mapUri(this, SONORE_HOST_LV2_BUF_SIZE__minBlockLength), (uint32_t) sizeof(int32_t),
                 atomInt, &optionMinBlock_};
  options_[3] = {0, 0, mapUri(this, SONORE_HOST_LV2_BUF_SIZE__sequenceSize), (uint32_t) sizeof(int32_t),
                 atomInt, &optionSequenceSize_};
  options_[4] = HostLv2Option{};
  optionsFeature_.URI = SONORE_HOST_LV2_OPTIONS__options;
  optionsFeature_.data = options_;
  boundedFeature_.URI = SONORE_HOST_LV2_BUF_SIZE__boundedBlockLength;
  boundedFeature_.data = nullptr;
  features_.clear();
  features_.push_back(&mapFeature_);
  features_.push_back(&unmapFeature_);
  features_.push_back(&optionsFeature_);
  features_.push_back(&boundedFeature_);
  // And the worker, for a plugin whose slow work cannot happen in run().
  // The rings exist before the instance, which may schedule from activate().
  workRequests_.allocate(kWorkRingBytes);
  workResponses_.allocate(kWorkRingBytes);
  workSchedule_.handle = this;
  workSchedule_.schedule_work = scheduleWork;
  workFeature_.URI = SONORE_HOST_LV2_WORKER__schedule;
  workFeature_.data = &workSchedule_;
  features_.push_back(&workFeature_);
  features_.push_back(nullptr);

  // "It MUST include the trailing directory separator so that simply
  // appending a filename will yield the path to that file in the bundle."
  // It was passed bare, so a plugin appending "presets.ttl" to it looked for
  // "Foo.lv2presets.ttl" next to the bundle. The UI was already given one.
  std::string bundle = desc_.path;
  if (!bundle.empty() && bundle.back() != '/' && bundle.back() != '\\') bundle += '/';
  handle_ = descriptor_->instantiate(descriptor_, sampleRate, bundle.c_str(), features_.data());
  if (!handle_) {
    // Held again for the next prepare(), as loadState() holds it: the old
    // instance is gone, and this local was the only copy of what it had.
    heldState_.swap(carried);
    return false;
  }
  startWorker();

  // Resolved here, where allocating is what prepare() is for.
  uridSequence_ = mapUri(this, LV2_ATOM__Sequence);
  uridChunk_ = mapUri(this, LV2_ATOM__Chunk);
  uridMidiEvent_ = mapUri(this, LV2_MIDI__MidiEvent);

  storage_.assign((size_t) channels_ * maxBlock_ * 2, 0.0f);
  inPtrs_.assign(channels_, nullptr);
  outPtrs_.assign(channels_, nullptr);
  for (uint32_t c = 0; c < channels_; ++c) {
    inPtrs_[c] = storage_.data() + (size_t) c * maxBlock_;
    outPtrs_[c] = storage_.data() + (size_t) (channels_ + c) * maxBlock_;
  }
  // atomBytes_ was sized before the instance, for options:options.
  atomSlots_.clear();
  for (const Lv2Port& p : ports_)
    if (p.isAtom && (p.isInput || p.isOutput))
      atomSlots_.push_back({p.index, p.isInput, atomSlots_.size() * atomBytes_});
  atomStorage_.assign(atomSlots_.size() * atomBytes_, 0);
  auto atomBuffer = [&](int port) -> uint8_t* {
    for (const AtomSlot& slot : atomSlots_)
      if (slot.port == port) return atomStorage_.data() + slot.offset;
    return nullptr;
  };
  midiIn_ = atomInPort_ >= 0 ? atomBuffer(atomInPort_) : nullptr;
  midiOut_ = atomOutPort_ >= 0 ? atomBuffer(atomOutPort_) : nullptr;
  // Audio ports past the main channels -- a splitter's aux outputs, a
  // ducker's key input -- each get a buffer of their own: silence in, and
  // somewhere harmless out. They were all connected to the FIRST main
  // channel, so every aux output wrote over the main left channel and a key
  // input heard the signal it was meant to duck.
  int extraIns = 0, extraOuts = 0, seenIns = 0, seenOuts = 0;
  for (const Lv2Port& p : ports_) {
    if (!p.isAudio) continue;
    if (p.isInput && seenIns++ >= (int) channels_) ++extraIns;
    if (p.isOutput && seenOuts++ >= (int) channels_) ++extraOuts;
  }
  spareIn_.assign((size_t) extraIns * maxBlock_, 0.0f);
  spareOut_.assign((size_t) extraOuts * maxBlock_, 0.0f);
  // CV ports are audio-rate control signals: a block of floats each, silent
  // going in. They, and any port of a class this host does not know, were
  // left unconnected -- null, which the spec forbids for any port not
  // declared lv2:connectionOptional and which a plugin dereferences.
  int cvIns = 0, cvOuts = 0;
  for (const Lv2Port& p : ports_)
    if (p.isCv) (p.isOutput ? cvOuts : cvIns)++;
  cvIn_.assign((size_t) cvIns * maxBlock_, 0.0f);
  cvOut_.assign((size_t) cvOuts * maxBlock_, 0.0f);
  unknownPort_.assign((size_t) maxBlock_ < 2048 ? 2048 : (size_t) maxBlock_, 0.0f);

  // Every port gets a pointer, including the ones this host does not use. A
  // plugin is entitled to dereference anything it declared, and connecting
  // only what we care about is how a host crashes a plugin that did nothing
  // wrong.
  int audioInSeen = 0, audioOutSeen = 0, cvInSeen = 0, cvOutSeen = 0;
  for (const Lv2Port& p : ports_) {
    if (p.isControl) {
      descriptor_->connect_port(handle_, (uint32_t) p.index, &controls_[(size_t) p.index]);
    } else if (p.isAudio && p.isInput) {
      const int k = audioInSeen++;
      descriptor_->connect_port(handle_, (uint32_t) p.index,
                                k < (int) channels_
                                    ? inPtrs_[(size_t) k]
                                    : spareIn_.data() + (size_t) (k - (int) channels_) * maxBlock_);
    } else if (p.isAudio && p.isOutput) {
      const int k = audioOutSeen++;
      descriptor_->connect_port(handle_, (uint32_t) p.index,
                                k < (int) channels_
                                    ? outPtrs_[(size_t) k]
                                    : spareOut_.data() + (size_t) (k - (int) channels_) * maxBlock_);
    } else if (p.isAtom && (p.isInput || p.isOutput)) {
      descriptor_->connect_port(handle_, (uint32_t) p.index, atomBuffer(p.index));
    } else if (p.isCv) {
      float* block = p.isOutput ? cvOut_.data() + (size_t) cvOutSeen++ * maxBlock_
                                : cvIn_.data() + (size_t) cvInSeen++ * maxBlock_;
      descriptor_->connect_port(handle_, (uint32_t) p.index, block);
    } else {
      // A class this host does not speak: silence it can read, room it can
      // write, rather than a null pointer.
      descriptor_->connect_port(handle_, (uint32_t) p.index, unknownPort_.data());
    }
  }

  // Restored before activate(): restore() is in the instantiation class, and
  // a plugin may size what it activates from what it holds.
  if (!carried.empty() && state && state->restore) restoreAlone(state, &carried);

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
    primeAtomPorts();
    descriptor_->run(handle_, 1);
    deliverWorkResponses();
    publishOutputs();
    // A block like any other for what it sends an open interface: a plugin
    // that tells its interface its state on the first run after activate()
    // counts it told.
    forwardToUi();
  }
  return true;
}

inline void Lv2Plugin::process(AudioBlock<float>& io, const MidiBuffer* midi) {
  produced_.clear(); // this block's emissions only, whether or not it is processed
  if (!handle_ || !descriptor_ || !active_) return;
  const uint32_t frames = (uint32_t) io.getNumSamples();
  if (frames == 0 || frames > maxBlock_) return;
  // Not while a state restore has the instance, or waits for it
  // (restoreAlone): this block is left as it came in, and the plugin's own
  // queues wait for the next.
  int expected = kGateFree;
  if (restoreWanted_.load(std::memory_order_acquire) ||
      !gate_.compare_exchange_strong(expected, kGateRunning, std::memory_order_acquire,
                                     std::memory_order_relaxed))
    return;
  // A reset asked for (reset()), now that the block has the instance: with
  // the worker held off too, or at the next block if a work() is under way.
  // Tried, never waited for, on this thread.
  if (resetPending_.load(std::memory_order_acquire)) {
    std::unique_lock<std::mutex> alone(workRunning_, std::try_to_lock);
    if (alone.owns_lock()) {
      resetPending_.store(false, std::memory_order_relaxed);
      if (descriptor_->deactivate) descriptor_->deactivate(handle_);
      if (descriptor_->activate) descriptor_->activate(handle_);
    }
  }

  const uint32_t chans = (uint32_t) io.getNumChannels();
  for (uint32_t c = 0; c < channels_; ++c) {
    const float* src = c < chans ? io.getChannelPointer(c) : nullptr;
    if (src) std::memcpy(inPtrs_[c], src, frames * sizeof(float));
    else std::memset(inPtrs_[c], 0, frames * sizeof(float));
    std::memset(outPtrs_[c], 0, frames * sizeof(float));
  }

  // MIDI in, as an atom sequence. Built by hand because the atom-forge header
  // is not vendored and the sequence layout for 3-byte events is small and
  // fixed -- a header, then one aligned event per message. What the
  // interface wrote goes first, at frame 0, so the sequence stays in order.
  primeAtomPorts();
  deliverFromUi();
  if (midiIn_) {
    auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(midiIn_);
    if (midi) {
      const LV2_URID midiEvent = uridMidiEvent_;
      uint8_t* base = reinterpret_cast<uint8_t*>(&seq->body);
      // Inside the frames run() is given, as the CLAP side keeps them: a
      // caller's MidiBuffer can hold a position at or past the end. Sorted
      // before, so still in time order after, as a sequence must be.
      const int last = (int) frames - 1;
      for (const auto& entry : *midi) {
        const uint32_t need = (uint32_t) (sizeof(LV2_Atom_Event) + 8u);
        // Summed on the left, so an atom buffer shorter than a header can
        // never underflow the subtraction into a bound that admits anything.
        if (sizeof(LV2_Atom) + seq->atom.size + need > atomBytes_) break;
        auto* ev = reinterpret_cast<LV2_Atom_Event*>(base + seq->atom.size);
        const int at = entry.samplePosition;
        ev->time.frames = at < 0 ? 0 : (at > last ? last : at);
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

  descriptor_->run(handle_, frames);
  deliverWorkResponses();
  publishOutputs();

  // What the plugin wrote this block, and nothing else (forEachOutputEvent).
  if (midiOut_) {
    const LV2_URID midiEvent = uridMidiEvent_;
    forEachOutputEvent(midiOut_, [&](const LV2_Atom_Event* ev) {
      if (ev->body.type != midiEvent || ev->body.size < 3) return;
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(ev) + sizeof(LV2_Atom_Event);
      if (bytes[0] >= 0x80 && bytes[0] < 0xf0)
        produced_.addEvent(MidiMessage(bytes[0], bytes[1], bytes[2]), (int) ev->time.frames);
    });
  }
  forwardToUi();

  // Back from the plugin's outputs, and a channel past its last one repeats
  // that one -- a MONO effect on a stereo track is heard on both sides, as
  // the CLAP, VST3 and LADSPA sides have it. Only the outputs it declared are
  // connected, so the right channel of a mono plugin read a buffer nothing
  // wrote, and came back silent. A plugin with no audio output leaves
  // silence, as before.
  const size_t outs = (std::min)(audioOut_.size(), (size_t) channels_);
  const size_t last = outs > 0 ? outs - 1 : 0;
  for (uint32_t c = 0; c < chans; ++c)
    std::memcpy(io.getChannelPointer(c), outPtrs_[c < last ? c : last], frames * sizeof(float));
  gate_.store(kGateFree, std::memory_order_release);
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

/** One file's triples added to a bundle-wide document, its blank nodes
 *  renamed apart from every other file's.
 *
 *  A blank node belongs to the file it is written in, and the parser names
 *  every file's _:sonore1, _:sonore2 and on. Merged as they were, the
 *  second port of plugin.ttl and the second blank node of a MOD modgui.ttl
 *  -- which describes the ports again, an lv2:index and lv2:symbol each --
 *  were one node with two indices. Listed first, the GUI's file decided
 *  which: a port was taken for another, and the one it really was never
 *  connected. */
inline void appendScoped(turtle::Document& into, const turtle::Document& from, int scope) {
  const std::string tag = "_:f" + std::to_string(scope) + ".";
  auto scoped = [&](const std::string& node) {
    return node.compare(0, 2, "_:") == 0 ? tag + node.substr(2) : node;
  };
  for (turtle::Triple t : from.triples) {
    t.subject = scoped(t.subject);
    if (!t.objectIsLiteral) t.object = scoped(t.object);
    into.triples.push_back(std::move(t));
  }
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
  } else if (!handle_) {
    properties = heldState_; // loaded, not yet given to an instance
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
  // Read whole, and only then applied. A session file is not this host's
  // to trust: each value went into its port as it was read, so a NaN or
  // 1e30 reached the plugin as a control value -- setParameterValue()
  // clamps, this did not -- and a blob found broken further on returned
  // false with its controls already changed, half a restore reported as
  // none. A NaN is not a value and refuses the blob, as LADSPA's does; a
  // number outside a port's range is held to it, as a host write is.
  size_t pos = 0;
  uint32_t count = 0;
  if (!detail::readU32(data, size, &pos, &count)) return false;
  if (count != params_.size()) return false; // a blob for a different plugin
  std::vector<float> values(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (pos + sizeof(float) > size) return false;
    std::memcpy(&values[i], data + pos, sizeof(float));
    pos += sizeof(float);
    if (!(values[i] == values[i])) return false;
  }

  struct Property {
    std::string key, type;
    size_t at, length;
  };
  std::vector<Property> read;
  uint32_t properties = 0;
  // A blob written before this host understood the state extension simply
  // ends after the controls, and that is a restore rather than a failure.
  const bool hasProperties = detail::readU32(data, size, &pos, &properties);
  for (uint32_t i = 0; hasProperties && i < properties; ++i) {
    uint32_t keyLength = 0, typeLength = 0, valueLength = 0;
    if (!detail::readU32(data, size, &pos, &keyLength)) return false;
    if (pos + keyLength > size) return false;
    Property p;
    p.key.assign((const char*) data + pos, keyLength);
    pos += keyLength;
    if (!detail::readU32(data, size, &pos, &typeLength)) return false;
    if (pos + typeLength > size) return false;
    p.type.assign((const char*) data + pos, typeLength);
    pos += typeLength;
    if (!detail::readU32(data, size, &pos, &valueLength)) return false;
    if (pos + valueLength > size) return false;
    p.at = pos;
    p.length = valueLength;
    pos += valueLength;
    read.push_back(std::move(p));
  }

  for (uint32_t i = 0; i < count; ++i) {
    const HostedParam& p = params_[i];
    const double v = values[i];
    const double held = v < p.minValue ? p.minValue : (v > p.maxValue ? p.maxValue : v);
    controls_[(size_t) p.id] = (float) held;
  }
  if (!hasProperties) return true;

  std::vector<StateProperty> restored;
  for (const Property& p : read) {
    StateProperty sp;
    sp.key = mapUri(this, p.key.c_str());
    sp.type = p.type.empty() ? 0 : mapUri(this, p.type.c_str());
    sp.value.assign(data + p.at, data + p.at + p.length);
    restored.push_back(std::move(sp));
  }

  const LV2_State_Interface* state = stateInterface();
  if (!handle_) {
    // No instance until prepare(), which restores this into the one it makes.
    heldState_ = std::move(restored);
    return true;
  }
  heldState_.clear();
  // With the instance to itself: the audio thread may be running it.
  if (state && state->restore) restoreAlone(state, &restored);
  return true;
}

/** A plugin's author the way lilv finds it: the foaf:name of a doap:maintainer
 *  on the plugin, else of one on the plugin's lv2:project. Empty when the
 *  bundle names nobody. */
inline std::string lv2MaintainerName(const turtle::Document& doc, const std::string& plugin) {
  const std::string doap = "http://usefulinc.com/ns/doap#";
  std::string who = doc.object(plugin, doap + "maintainer");
  if (who.empty()) {
    const std::string project = doc.object(plugin, "http://lv2plug.in/ns/lv2core#project");
    if (!project.empty()) who = doc.object(project, doap + "maintainer");
  }
  return who.empty() ? std::string() : doc.object(who, "http://xmlns.com/foaf/0.1/name");
}

/** Read a bundle's Turtle and describe what is in it, without loading code. */
inline void describeLv2Bundle(const std::string& bundlePath,
                              std::vector<PluginDescription>* found) {
  const std::string manifestPath = bundlePath + "/manifest.ttl";
  std::FILE* file = ::sonore::pathio::open(manifestPath.c_str(), "rb");
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
    d.vendor = lv2MaintainerName(manifest, subject);

    // The real description usually lives in a second file. Read it for the
    // name, the author and the category, because a manifest is deliberately
    // thin.
    for (const std::string& also : manifest.objects(subject, rdfs + "seeAlso")) {
      std::FILE* extra = ::sonore::pathio::open(also.c_str(), "rb");
      if (!extra) continue;
      std::string more;
      while ((got = std::fread(buffer, 1, sizeof(buffer), extra)) > 0) more.append(buffer, got);
      std::fclose(extra);
      turtle::Document doc;
      if (!turtle::parse(more, bundlePath, &doc)) continue;
      if (d.name.empty()) d.name = doc.object(subject, doap + "name");
      if (d.vendor.empty()) d.vendor = lv2MaintainerName(doc, subject);
      for (const auto& t : doc.triples) {
        if (t.subject != subject) continue;
        if (t.predicate != "http://www.w3.org/1999/02/22-rdf-syntax-ns#type") continue;
        if (t.object == lv2 + "Plugin") continue;
        // The categories are lv2core's classes; a doap:Project, which a
        // plugin often is as well, says nothing about what it does.
        if (t.object.compare(0, lv2.size(), lv2) != 0) continue;
        if (!d.features.empty()) d.features += ";";
        d.features += t.object;
        if (t.object.find("Instrument") != std::string::npos) d.isInstrument = true;
      }
    }
    if (d.name.empty()) d.name = subject;
    found->push_back(d);
  }
}

/** [main-thread] Load the LV2 plugin a scan described: `description.path`
 *  is the bundle folder and `description.id` the plugin's URI. Reads its
 *  Turtle for ports, presets and interface, and opens its binary; the
 *  instance itself is made in prepare(), at the host's rate. Null when the
 *  bundle cannot be read or declares its ports wrongly, or its binary does
 *  not load or has no plugin by that URI. loadPlugin() calls this for a
 *  format of "LV2". */
inline std::unique_ptr<HostedPlugin> loadLv2(const PluginDescription& description) {
  std::unique_ptr<Lv2Plugin> hosted(new Lv2Plugin());
  hosted->desc_ = description;

  const std::string manifestPath = description.path + "/manifest.ttl";
  std::FILE* file = ::sonore::pathio::open(manifestPath.c_str(), "rb");
  if (!file) return nullptr;
  std::string text;
  char buffer[8192];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, got);
  std::fclose(file);

  turtle::Document manifest;
  if (!turtle::parse(text, description.path, &manifest)) return nullptr;
  // Every question below goes through an index of its document: asked of
  // the documents themselves, one per triple read, they were 95% of the
  // load (see detail::TripleIndex).
  const detail::TripleIndex manifestIndex(manifest);

  const std::string lv2 = "http://lv2plug.in/ns/lv2core#";
  const std::string rdfs = "http://www.w3.org/2000/01/rdf-schema#";
  const std::string atom = "http://lv2plug.in/ns/ext/atom#";

  const std::string binary = manifestIndex.object(description.id, lv2 + "binary");
  if (binary.empty()) return nullptr;

  // Ports come from the seeAlso document; gather every one of them.
  turtle::Document all;
  int files = 0;
  detail::appendScoped(all, manifest, files++);
  // Each file once: read twice, its blank nodes would be two sets of ports.
  std::vector<std::string> merged{manifestPath};
  for (const std::string& also : manifestIndex.objects(description.id, rdfs + "seeAlso")) {
    if (std::find(merged.begin(), merged.end(), also) != merged.end()) continue;
    merged.push_back(also);
    std::FILE* extra = ::sonore::pathio::open(also.c_str(), "rb");
    if (!extra) continue;
    std::string more;
    while ((got = std::fread(buffer, 1, sizeof(buffer), extra)) > 0) more.append(buffer, got);
    std::fclose(extra);
    turtle::Document doc;
    if (!turtle::parse(more, description.path, &doc)) continue;
    detail::appendScoped(all, doc, files++);
  }
  // `all` is whole now, and not added to again.
  const detail::TripleIndex allIndex(all);

  // Every port's lv2:index is a whole number, and together they are 0 to N-1
  // for the N ports the plugin declares, each once -- the core spec's rule,
  // and lilv's. The host sizes its control array by the highest and writes
  // each control at its own: an index of -1, read with atoi, wrote four
  // bytes before that array, in the heap's bookkeeping, and two billion
  // asked for 8 GB. A port with no index, or two ports on one, leaves a port
  // the plugin expects never connected. Such a description is refused, as a
  // missing binary is.
  // A port stated twice -- a port with a URI of its own, listed in two
  // files -- is still one port.
  std::vector<std::string> portNodes;
  for (const std::string& node : allIndex.objects(description.id, lv2 + "port"))
    if (std::find(portNodes.begin(), portNodes.end(), node) == portNodes.end())
      portNodes.push_back(node);
  std::vector<char> indexTaken(portNodes.size(), 0);
  int highest = -1;
  for (const std::string& portNode : portNodes) {
    Lv2Port p;
    const std::string index = allIndex.object(portNode, lv2 + "index");
    char* end = nullptr;
    const long number = std::strtol(index.c_str(), &end, 10);
    if (index.empty() || *end != '\0' || number < 0 || number >= (long) portNodes.size() ||
        indexTaken[(size_t) number])
      return nullptr;
    indexTaken[(size_t) number] = 1;
    p.index = (int) number;
    p.symbol = allIndex.object(portNode, lv2 + "symbol");
    p.name = allIndex.object(portNode, lv2 + "name");
    for (size_t at : allIndex.about(portNode)) {
      const turtle::Triple& t = allIndex.at(at);
      if (t.predicate != "http://www.w3.org/1999/02/22-rdf-syntax-ns#type") continue;
      if (t.object == lv2 + "ControlPort") p.isControl = true;
      if (t.object == lv2 + "AudioPort") p.isAudio = true;
      if (t.object == atom + "AtomPort") p.isAtom = true;
      if (t.object == lv2 + "CVPort") p.isCv = true;
      if (t.object == lv2 + "InputPort") p.isInput = true;
      if (t.object == lv2 + "OutputPort") p.isOutput = true;
    }
    const std::string def = allIndex.object(portNode, lv2 + "default");
    const std::string lo = allIndex.object(portNode, lv2 + "minimum");
    const std::string hi = allIndex.object(portNode, lv2 + "maximum");
    if (!def.empty()) p.defaultValue = (float) std::atof(def.c_str());
    if (!lo.empty()) p.minValue = (float) std::atof(lo.c_str());
    if (!hi.empty()) p.maxValue = (float) std::atof(hi.c_str());
    for (const std::string& property : allIndex.objects(portNode, lv2 + "portProperty")) {
      if (property == lv2 + "integer" || property == lv2 + "enumeration") p.stepped = true;
      if (property == lv2 + "toggled") p.stepped = p.toggled = true;
      if (property == lv2 + "reportsLatency") p.reportsLatency = true;
    }
    p.designation = allIndex.object(portNode, lv2 + "designation");
    for (const std::string& point : allIndex.objects(portNode, lv2 + "scalePoint")) {
      const std::string label = allIndex.object(point, rdfs + "label");
      const std::string value = allIndex.object(point, "http://www.w3.org/1999/02/22-rdf-syntax-ns#value");
      if (!label.empty() && !value.empty()) p.scalePoints.emplace_back((float) std::atof(value.c_str()), label);
    }
    p.unit = lv2UnitSymbol(allIndex, allIndex.object(portNode, "http://lv2plug.in/ns/extensions/units#unit"));
    for (const std::string& kind : allIndex.objects(portNode, atom + "supports")) {
      p.declaresSupport = true;
      if (kind == LV2_MIDI__MidiEvent) p.supportsMidi = true;
    }
    if (p.index > highest) highest = p.index;
    hosted->ports_.push_back(p);
  }
  if (highest < 0) return nullptr; // a plugin with no ports is not one

  hosted->controls_.assign((size_t) highest + 1, 0.0f);
  hosted->outputControl_.assign(hosted->controls_.size(), 0);
  hosted->reported_.reset(new std::atomic<float>[hosted->controls_.size()]);
  for (size_t i = 0; i < hosted->controls_.size(); ++i) hosted->reported_[i].store(0.0f);
  for (const Lv2Port& p : hosted->ports_)
    if (p.isControl && p.isOutput && !p.isInput) hosted->outputControl_[(size_t) p.index] = 1;

  // The ports the host drives rather than the user -- the latency it reads,
  // the switch that is its bypass, freewheeling -- known by what the bundle
  // DESIGNATES them, as the specification and lilv know them, and only then
  // by symbol. A symbol is the bundle's own choice: the DPF framework writes
  // "lv2_latency" and "lv2_enabled", and matched by symbol alone such a
  // plugin reported no latency -- a graph went on summing it uncompensated
  // with its dry branch -- had no bypass, and listed the host's switches
  // among the user's parameters. A bundle that designates nothing is read
  // by symbol, as it always was.
  int latencyPort = -1, enabledPort = -1, freewheelPort = -1;
  for (const Lv2Port& p : hosted->ports_) {
    if (!p.isControl) continue;
    if (p.isOutput && latencyPort < 0 && (p.designation == lv2 + "latency" || p.reportsLatency))
      latencyPort = p.index;
    if (p.isInput && enabledPort < 0 && p.designation == lv2 + "enabled") enabledPort = p.index;
    if (p.isInput && freewheelPort < 0 && p.designation == lv2 + "freeWheeling") freewheelPort = p.index;
  }
  for (const Lv2Port& p : hosted->ports_) {
    if (!p.isControl) continue;
    if (p.isOutput && latencyPort < 0 && p.symbol == "latency") latencyPort = p.index;
    if (p.isInput && enabledPort < 0 && p.symbol == "enabled") enabledPort = p.index;
    if (p.isInput && freewheelPort < 0 && p.symbol == "freewheel") freewheelPort = p.index;
  }
  hosted->latencyPort_ = latencyPort;
  hosted->enabledPort_ = enabledPort;

  for (const Lv2Port& p : hosted->ports_) {
    if (p.isControl) hosted->controls_[(size_t) p.index] = p.defaultValue;
    if (p.isAudio && p.isInput) hosted->audioIn_.push_back(p.index);
    if (p.isAudio && p.isOutput) hosted->audioOut_.push_back(p.index);
    // Input control ports are the plugin's PARAMETERS. The host's own --
    // enabled, freewheel -- are its machinery and belong in a user's
    // parameter list no more than a bypass button does.
    if (p.isControl && p.isInput && p.index != enabledPort && p.index != freewheelPort) {
      HostedParam hp;
      hp.id = (uint32_t) p.index;
      hp.name = p.name.empty() ? p.symbol : p.name;
      hp.minValue = p.minValue;
      hp.maxValue = p.maxValue;
      hp.defaultValue = p.defaultValue;
      hp.stepped = p.stepped;
      hosted->params_.push_back(hp);
    }
  }
  hosted->paramIndex_.build(hosted->params_);

  // Which atom port MIDI goes in and comes out of: the one that says it
  // carries midi:MidiEvent, else the first that says nothing of what it
  // carries. It was simply the LAST atom port each way, which was the MIDI
  // one only because of the order a TTL happened to list them in; a port
  // that names what it supports, and MIDI is not it, is never chosen.
  auto midiPort = [&](bool input) {
    int unspoken = -1;
    for (const Lv2Port& p : hosted->ports_) {
      if (!p.isAtom || (input ? !p.isInput : (!p.isOutput || p.isInput))) continue;
      if (p.supportsMidi) return p.index;
      if (!p.declaresSupport && unspoken < 0) unspoken = p.index;
    }
    return unspoken;
  };
  hosted->atomInPort_ = midiPort(true);
  hosted->atomOutPort_ = midiPort(false);

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
      for (const std::string& to : manifestIndex.objects(t.subject, lv2 + "appliesTo"))
        if (to == description.id) applies = true;
      if (!applies) continue;

      Lv2Preset preset;
      preset.uri = t.subject;
      preset.name = manifestIndex.object(t.subject, rdfs + "label");
      if (preset.name.empty()) preset.name = t.subject;
      hosted->presets_.push_back(preset);
    }

    // The values, from whichever document actually carries them. A preset
    // defined inline in the manifest needs no second read; the usual case
    // points at presets.ttl through its own seeAlso. Each document parsed is
    // indexed beside it; a map's elements stay where they are, so the index
    // keeps pointing at its document.
    std::map<std::string, turtle::Document> parsed;
    std::map<std::string, detail::TripleIndex> parsedIndex;
    for (Lv2Preset& preset : hosted->presets_) {
      std::vector<const detail::TripleIndex*> sources;
      sources.push_back(&manifestIndex);
      for (const std::string& also : manifestIndex.objects(preset.uri, rdfs + "seeAlso")) {
        auto it = parsedIndex.find(also);
        if (it == parsedIndex.end()) {
          std::FILE* extra = ::sonore::pathio::open(also.c_str(), "rb");
          if (!extra) continue;
          std::string more;
          while ((got = std::fread(buffer, 1, sizeof(buffer), extra)) > 0) more.append(buffer, got);
          std::fclose(extra);
          turtle::Document doc;
          if (!turtle::parse(more, description.path, &doc)) continue;
          const auto stored = parsed.insert(std::make_pair(also, std::move(doc))).first;
          it = parsedIndex.insert(std::make_pair(also, detail::TripleIndex(stored->second))).first;
        }
        sources.push_back(&it->second);
      }

      for (const detail::TripleIndex* doc : sources) {
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
    for (const std::string& uiNode : allIndex.objects(description.id, ui + "ui")) {
      bool rightKind = false;
      for (size_t at : allIndex.about(uiNode)) {
        const turtle::Triple& t = allIndex.at(at);
        if (t.predicate == rdfType && t.object == nativeLv2UiType()) rightKind = true;
      }
      if (!rightKind) continue;
      const std::string uiBinary = allIndex.object(uiNode, ui + "binary");
      if (uiBinary.empty()) continue;
      hosted->uiUri_ = uiNode;
      // The same treatment lv2:binary gets: the parser resolved it against
      // the bundle, and the leaf goes back on this side of the separator.
      hosted->uiBinary_ = description.path + "/" + binaryLeaf(uiBinary);
      break;
    }
  }
  // An interface and a plugin with atom ports talk in atoms, through two
  // queues openEditor() makes the first time (see there). Decided here, by
  // mapping the URI they travel under: nonzero means the queues are wanted.
  if (hosted->hasEditor()) {
    bool atoms = false;
    for (const Lv2Port& p : hosted->ports_)
      if (p.isAtom) atoms = true;
    if (atoms)
      hosted->uridEventTransfer_ = Lv2Plugin::mapUri(hosted.get(), LV2_ATOM__eventTransfer);
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

// ── The LADSPA side ──────────────────────────────────────────────────────────
//
// LV2's predecessor, and still what a good number of Linux effects ship as.
// Always compiled in, unlike VST3 and LV2: its ABI is a few structs and one
// symbol, declared in ladspa_abi.h from the specification, so there is no SDK
// to vendor and nothing to gate on.
//
// What the format cannot say, this host does not invent. LADSPA has no MIDI,
// no state beyond its control ports, no presets, no bypass, no editor and no
// notion of a vendor's version: each of those answers "none" here, which is
// the truth about the plugin rather than a gap in the host.

class LadspaPlugin;
inline std::unique_ptr<HostedPlugin> loadLadspa(const PluginDescription& description);

/** One LADSPA plugin, driven through the same interface as the other three. */
class LadspaPlugin final : public HostedPlugin {
public:
  /** [main-thread, audio stopped] Deactivate and clean up the instance,
   *  then close the library. */
  ~LadspaPlugin() override { unload(); }

  const PluginDescription& description() const override { return desc_; }
  bool isValid() const override { return descriptor_ != nullptr; }

  /**
   * Instantiate at this rate, size every buffer, connect every port, activate.
   *
   * The rate is an argument to instantiate() in LADSPA -- an instance is for
   * one rate for life -- so every prepare makes a new instance, as LV2 does.
   * The control values survive it: they are the host's, held here, and the
   * new instance is connected to the same storage.
   */
  bool prepare(double sampleRate, uint32_t maxBlockSize, uint32_t numChannels = 2) override {
    if (!descriptor_ || !descriptor_->instantiate || !descriptor_->connect_port ||
        !descriptor_->run)
      return false;
    destroyInstance();
    (void) numChannels; // every audio port gets a buffer of its own; see process()
    maxBlock_ = maxBlockSize > 0 ? maxBlockSize : 1;
    const unsigned long rate = sampleRate > 0.0 ? (unsigned long) (sampleRate + 0.5) : 48000ul;
    // Bounds given as fractions of the rate are only numbers once there IS a
    // rate. They were worked out against a provisional one at load so a host
    // could list the parameters before preparing; now they are made true.
    if (rate != rateForBounds_) {
      rateForBounds_ = rate;
      buildParameters(/*keepValues=*/true);
    }

    handle_ = descriptor_->instantiate(descriptor_, rate);
    if (!handle_) return false;

    audio_.assign((size_t) (audioIn_.size() + audioOut_.size()) * maxBlock_, 0.0f);
    for (size_t k = 0; k < audioIn_.size(); ++k)
      descriptor_->connect_port(handle_, audioIn_[k], audio_.data() + k * maxBlock_);
    for (size_t k = 0; k < audioOut_.size(); ++k)
      descriptor_->connect_port(handle_, audioOut_[k],
                                audio_.data() + (audioIn_.size() + k) * maxBlock_);
    // EVERY control port, outputs included. A plugin may write to any port it
    // declared and is entitled to a valid pointer for each; leaving the
    // outputs unconnected is how a host crashes a plugin that did nothing
    // wrong.
    for (unsigned long port = 0; port < descriptor_->portCount; ++port)
      if (descriptor_->portDescriptors[port] & ladspa::kPortControl)
        descriptor_->connect_port(handle_, port, &controls_[(size_t) port]);
    copyControlsIn();

    if (descriptor_->activate) descriptor_->activate(handle_);
    active_ = true;

    // One silent sample, so a plugin that reports latency has reported it by
    // the time a host asks -- which is straight after prepare, to lay the
    // track out. The same reason, and the same cost, as the LV2 side.
    if (latencyPort_ >= 0) {
      std::fill(audio_.begin(), audio_.end(), 0.0f);
      descriptor_->run(handle_, 1);
      publishLatency();
    }
    return true;
  }

  /** deactivate() only after activate(), as the specification pairs them. */
  void release() override {
    if (active_ && handle_ && descriptor_ && descriptor_->deactivate)
      descriptor_->deactivate(handle_);
    active_ = false;
  }

  /** LADSPA's reset is the activate pair: "activate ... resets the plugin's
   *  state", and deactivate must come first. */
  void reset() override {
    if (!handle_ || !descriptor_ || !active_) return;
    if (descriptor_->deactivate) descriptor_->deactivate(handle_);
    if (descriptor_->activate) descriptor_->activate(handle_);
  }

  /**
   * [audio-thread] Copy in, run, copy out. Nothing allocates: every buffer was
   * sized in prepare().
   *
   * Channel c feeds audio input port c and silence feeds the rest. Output
   * channel c comes from output port c, and a channel past the last port
   * repeats the last one -- so a MONO effect on a stereo track is heard on
   * both sides rather than leaving the right channel silent, which is what a
   * user means by putting it there. A plugin with no audio output at all (an
   * analyser) leaves the signal as it found it.
   */
  void process(AudioBlock<float>& io, const MidiBuffer* midi = nullptr) override {
    (void) midi; // LADSPA has no event input of any kind
    if (!handle_ || !active_) return;
    const size_t frames = io.getNumSamples();
    if (frames == 0 || frames > maxBlock_) return;
    const size_t chans = io.getNumChannels();

    copyControlsIn();
    for (size_t k = 0; k < audioIn_.size(); ++k) {
      float* dst = audio_.data() + k * maxBlock_;
      if (k < chans) std::memcpy(dst, io.getChannelPointer(k), frames * sizeof(float));
      else std::memset(dst, 0, frames * sizeof(float));
    }
    descriptor_->run(handle_, (unsigned long) frames);
    publishLatency();

    if (audioOut_.empty()) return;
    for (size_t c = 0; c < chans; ++c) {
      const size_t k = c < audioOut_.size() ? c : audioOut_.size() - 1;
      std::memcpy(io.getChannelPointer(c), audio_.data() + (audioIn_.size() + k) * maxBlock_,
                  frames * sizeof(float));
    }
  }

  /** The output control port called "latency", by the convention every
   *  Linux host follows: LADSPA itself has no latency field. */
  uint32_t latencySamples() const override {
    const float v = latency_.load(std::memory_order_relaxed);
    return v > 0.0f && v < 1.0e9f ? (uint32_t) (v + 0.5f) : 0u;
  }
  uint32_t tailSamples() const override { return 0; }
  bool acceptsMidi() const override { return false; }
  bool hasAudioInput() const override { return !audioIn_.empty(); }

  int numParameters() const override { return (int) params_.size(); }
  const HostedParam& parameter(int index) const override { return params_[(size_t) index]; }

  double parameterValue(int index) const override {
    if (index < 0 || index >= numParameters()) return 0.0;
    return (double) values_[(size_t) index].load(std::memory_order_relaxed);
  }

  /** [main-thread] Held in an atomic and copied into the port before the next
   *  run(), rather than written into the port itself: the port is a float the
   *  plugin reads during run() on the audio thread, and a store into it from
   *  here is a data race even when it happens to work. */
  bool setParameterValue(int index, double value) override {
    if (index < 0 || index >= numParameters()) return false;
    values_[(size_t) index].store((float) clampParam(params_[(size_t) index], value),
                                  std::memory_order_relaxed);
    return true;
  }

  std::string parameterText(int index, double value) const override {
    if (index < 0 || index >= numParameters()) return std::string();
    const HintFlags& f = flags_[(size_t) index];
    if (f.toggled) return value > 0.0 ? "On" : "Off";
    char buffer[48];
    if (f.integer) {
      std::snprintf(buffer, sizeof(buffer), "%lld", (long long) std::llround(value));
      return buffer;
    }
    numtext::format(buffer, sizeof(buffer), value, "%.4g");
    return buffer;
  }

  /**
   * The control inputs, keyed by PORT INDEX rather than by position.
   *
   * That is the whole of a LADSPA plugin's state -- there is no other channel
   * for it -- and a port index is the one thing about a control that the
   * specification fixes for good. A plugin that gains a control in an update
   * keeps every old port's index, so a session saved before restores what it
   * had and leaves the new one at its default.
   */
  bool saveState(std::vector<uint8_t>& out) const override {
    out.clear();
    const char magic[4] = {'L', 'D', 'S', 'P'};
    out.insert(out.end(), magic, magic + 4);
    putU32(out, 1); // version
    putU32(out, (uint32_t) params_.size());
    for (size_t i = 0; i < params_.size(); ++i) {
      putU32(out, params_[i].id);
      const float v = values_[i].load(std::memory_order_relaxed);
      uint32_t bits = 0;
      std::memcpy(&bits, &v, sizeof(bits));
      putU32(out, bits);
    }
    return true;
  }

  bool loadState(const uint8_t* data, size_t size) override {
    std::vector<std::pair<uint32_t, float>> decoded;
    if (!decodeState(data, size, &decoded)) return false;
    // Everything not in the blob goes to its default, which is what the
    // other formats do with a session that predates a control.
    for (size_t i = 0; i < params_.size(); ++i) {
      double v = params_[i].defaultValue;
      for (const auto& entry : decoded)
        if (entry.first == params_[i].id) v = (double) entry.second;
      values_[i].store((float) clampParam(params_[i], v), std::memory_order_relaxed);
    }
    return true;
  }

  /**
   * Read a saved blob without applying it. Separate, and static, so the
   * parser of something that arrives from a session FILE can be fed every
   * malformed input there is without a plugin binary to hand.
   *
   * False for anything that is not exactly one well-formed blob: a refused
   * restore is reported, where half of one would be applied in silence.
   */
  static bool decodeState(const uint8_t* data, size_t size,
                          std::vector<std::pair<uint32_t, float>>* out) {
    if (!out) return false;
    out->clear();
    if (!data || size < 12 || std::memcmp(data, "LDSP", 4) != 0) return false;
    size_t pos = 4;
    uint32_t version = 0, count = 0;
    if (!getU32(data, size, &pos, &version) || version != 1) return false;
    if (!getU32(data, size, &pos, &count)) return false;
    // Compared against what is LEFT, so a count of four billion is refused
    // before a single entry is reserved for it.
    if (count > (size - pos) / 8) return false;
    out->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t port = 0, bits = 0;
      if (!getU32(data, size, &pos, &port) || !getU32(data, size, &pos, &bits)) return false;
      float v = 0.0f;
      std::memcpy(&v, &bits, sizeof(v));
      // A NaN is not a value; the loader would clamp it into the range and
      // call that a restore.
      if (!(v == v)) return false;
      out->push_back({port, v});
    }
    return pos == size;
  }

  const MidiBuffer& producedMidi() const override { return produced_; }

  // None of these exist in the format. See the section comment.
  bool setBypassed(bool) override { return false; }
  bool isBypassed() const override { return false; }
  bool hasBypass() const override { return false; }
  int numPresets() const override { return 0; }
  std::string presetName(int) const override { return std::string(); }
  bool loadPreset(int) override { return false; }
  size_t drainParameterEdits(std::vector<ParamEdit>&) override { return 0; }
  bool hasEditor() const override { return false; }
  bool openEditor(void*) override { return false; }
  void closeEditor() override {}
  void idleEditor() override {}
  bool editorSize(uint32_t&, uint32_t&) const override { return false; }
  bool setEditorScale(double) override { return false; }

  /** The plugin's own description of itself: its properties, for a host that
   *  wants to honour kPropertyInplaceBroken or check kPropertyHardRtCapable. */
  const ladspa::Descriptor* ladspaDescriptor() const { return descriptor_; }

  /**
   * One control port's range and default, as the specification defines them.
   * Public and static so the rules -- the "low" default is a quarter of the
   * way up, geometrically when the hint says logarithmic; bounds given as a
   * fraction of the sample rate are multiplied by it -- are a test rather than
   * something checked by loading a plugin that happens to use each one.
   */
  static HostedParam describePort(const ladspa::PortRangeHint& hint, double sampleRate) {
    HostedParam p;
    const int h = hint.hintDescriptor;
    const bool toggled = (h & ladspa::kHintToggled) != 0;
    const bool log = (h & ladspa::kHintLogarithmic) != 0;
    const double scale = (h & ladspa::kHintSampleRate) ? sampleRate : 1.0;
    // A bound that is not a usable number is no bound. The struct is read out
    // of somebody else's binary, and a NaN taken as a bound turns every clamp
    // after it into a pass-through. Past a billion is not usable either: no
    // control means it, and at 1e30 the range a one-sided bound is given
    // below, lo + 1, IS lo -- the fuzzer found that empty range.
    auto finite = [](double v) { return v == v && std::fabs(v) < 1.0e9; };
    const bool below = (h & ladspa::kHintBoundedBelow) != 0 && finite((double) hint.lowerBound * scale);
    const bool above = (h & ladspa::kHintBoundedAbove) != 0 && finite((double) hint.upperBound * scale);
    double lo = below ? (double) hint.lowerBound * scale : 0.0;
    double hi = above ? (double) hint.upperBound * scale : 1.0;
    if (toggled) {
      lo = 0.0;
      hi = 1.0;
    } else {
      // A range the plugin did not bound on one side gets one that contains
      // the side it did -- a host has to draw SOMETHING, and a slider whose
      // top is below its bottom is not something.
      if (!below && above) lo = (std::min)(0.0, hi - 1.0);
      if (below && !above) hi = (std::max)(lo + 1.0, 1.0);
      if (!(hi > lo)) hi = lo + 1.0;
    }
    p.minValue = lo;
    p.maxValue = hi;
    p.stepped = toggled || (h & ladspa::kHintInteger) != 0;

    // A geometric mean needs two positive ends. Where the plugin asked for a
    // logarithmic default over a range touching zero, the arithmetic one is
    // the nearest thing that is still a number.
    const bool geometric = log && lo > 0.0 && hi > 0.0;
    auto between = [&](double towardHigh) {
      return geometric ? std::exp(std::log(lo) * (1.0 - towardHigh) + std::log(hi) * towardHigh)
                       : lo * (1.0 - towardHigh) + hi * towardHigh;
    };
    double def = lo;
    switch (h & ladspa::kHintDefaultMask) {
      case ladspa::kHintDefaultMinimum: def = lo; break;
      case ladspa::kHintDefaultLow: def = between(0.25); break;
      case ladspa::kHintDefaultMiddle: def = between(0.5); break;
      case ladspa::kHintDefaultHigh: def = between(0.75); break;
      case ladspa::kHintDefaultMaximum: def = hi; break;
      case ladspa::kHintDefault0: def = 0.0; break;
      case ladspa::kHintDefault1: def = 1.0; break;
      case ladspa::kHintDefault100: def = 100.0; break;
      case ladspa::kHintDefault440: def = 440.0; break;
      default:
        // No default named. The bottom of the range, or zero where zero is
        // inside it -- a gain or a mix control left at its minimum is silence.
        def = (lo <= 0.0 && hi >= 0.0) ? 0.0 : lo;
        break;
    }
    if (p.stepped) def = std::floor(def + 0.5);
    p.defaultValue = def < lo ? lo : (def > hi ? hi : def);
    return p;
  }

private:
  friend std::unique_ptr<HostedPlugin> loadLadspa(const PluginDescription&);

  /** The LADSPA range hints that change how a control is shown: a toggle is
   *  a switch, an integer control steps. */
  struct HintFlags {
    bool toggled = false;
    bool integer = false;
  };

  static double clampParam(const HostedParam& p, double v) {
    if (!(v == v)) return p.defaultValue;
    v = v < p.minValue ? p.minValue : (v > p.maxValue ? p.maxValue : v);
    return p.stepped ? std::floor(v + 0.5) : v;
  }

  /** The parameter list: every CONTROL INPUT, in port order. Rebuilt when
   *  the rate changes because a rate-relative bound is a different number at
   *  each rate; values already set are kept, clamped into the new range. */
  void buildParameters(bool keepValues) {
    std::vector<float> previous;
    if (keepValues)
      for (size_t i = 0; i < params_.size(); ++i)
        previous.push_back(values_[i].load(std::memory_order_relaxed));
    params_.clear();
    flags_.clear();
    for (unsigned long port = 0; port < descriptor_->portCount; ++port) {
      const int pd = descriptor_->portDescriptors[port];
      if (!(pd & ladspa::kPortControl) || !(pd & ladspa::kPortInput)) continue;
      ladspa::PortRangeHint hint{0, 0.0f, 0.0f};
      if (descriptor_->portRangeHints) hint = descriptor_->portRangeHints[port];
      HostedParam p = describePort(hint, (double) rateForBounds_);
      p.id = (uint32_t) port;
      p.name = descriptor_->portNames && descriptor_->portNames[port] ? descriptor_->portNames[port]
                                                                      : std::string();
      params_.push_back(p);
      HintFlags f;
      f.toggled = (hint.hintDescriptor & ladspa::kHintToggled) != 0;
      f.integer = (hint.hintDescriptor & ladspa::kHintInteger) != 0;
      flags_.push_back(f);
    }
    values_.reset(new std::atomic<float>[params_.size() > 0 ? params_.size() : 1]);
    for (size_t i = 0; i < params_.size(); ++i) {
      const double v = keepValues && i < previous.size() ? (double) previous[i] : params_[i].defaultValue;
      values_[i].store((float) clampParam(params_[i], v), std::memory_order_relaxed);
    }
  }

  /** [audio-thread] The host's values into the ports the plugin reads. */
  void copyControlsIn() {
    for (size_t i = 0; i < params_.size(); ++i)
      controls_[(size_t) params_[i].id] = values_[i].load(std::memory_order_relaxed);
  }

  /** [audio-thread] What the plugin wrote to its latency port, for the main
   *  thread to read without racing the next run(). */
  void publishLatency() {
    if (latencyPort_ >= 0)
      latency_.store(controls_[(size_t) latencyPort_], std::memory_order_relaxed);
  }

  void destroyInstance() {
    release();
    if (handle_ && descriptor_ && descriptor_->cleanup) descriptor_->cleanup(handle_);
    handle_ = nullptr;
  }

  void unload() {
    destroyInstance();
    descriptor_ = nullptr;
    if (lib_) closeLib(lib_);
    lib_ = nullptr;
  }

  static void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t) ((v >> (8 * i)) & 0xffu));
  }
  static bool getU32(const uint8_t* data, size_t size, size_t* pos, uint32_t* out) {
    if (*pos > size || size - *pos < 4) return false;
    *out = (uint32_t) data[*pos] | ((uint32_t) data[*pos + 1] << 8) |
           ((uint32_t) data[*pos + 2] << 16) | ((uint32_t) data[*pos + 3] << 24);
    *pos += 4;
    return true;
  }

  PluginDescription desc_;
  LibHandle lib_ = nullptr;
  const ladspa::Descriptor* descriptor_ = nullptr;
  ladspa::Handle handle_ = nullptr;
  bool active_ = false;
  uint32_t maxBlock_ = 0;
  unsigned long rateForBounds_ = 48000ul;

  std::vector<HostedParam> params_;
  std::vector<HintFlags> flags_;
  std::unique_ptr<std::atomic<float>[]> values_;
  /** One float per PORT, indexed by port: what control ports are connected
   *  to. Outputs land here too. */
  std::vector<float> controls_;
  std::vector<unsigned long> audioIn_, audioOut_;
  std::vector<float> audio_;
  int latencyPort_ = -1;
  std::atomic<float> latency_{0.0f};
  MidiBuffer produced_;
};

/**
 * The format, as the reference arranges it: where its plugins live, which files
 * might be one, what a file holds, and loading one.
 *
 * Static throughout, because there is nothing per-object to keep -- the list
 * of what was found belongs in a PluginCache, and a loaded plugin owns its
 * library.
 */
class LADSPAPluginFormat {
public:
  /** "LADSPA": the PluginDescription::format of what describe() finds. */
  static const char* formatName() { return "LADSPA"; }

  /** By extension alone, which is all a LADSPA library has: a .so on Linux,
   *  a .so or .dylib on macOS, a .dll on Windows. Nothing is opened. */
  static bool fileMightContainThisPluginType(const std::string& path) {
    auto endsWith = [&](const char* suffix) {
      const size_t n = std::strlen(suffix);
      if (path.size() <= n) return false;
      for (size_t i = 0; i < n; ++i) {
        char c = path[path.size() - n + i];
        if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
        if (c != suffix[i]) return false;
      }
      return true;
    };
#if defined(_WIN32)
    return endsWith(".dll");
#elif defined(__APPLE__)
    return endsWith(".so") || endsWith(".dylib");
#else
    return endsWith(".so");
#endif
  }

  /**
   * Where to look: LADSPA_PATH when it is set, which is what every LADSPA
   * host honours, and otherwise the folders the distributions install to.
   *
   * Windows has no convention at all, so there it is LADSPA_PATH or nothing;
   * a host that keeps LADSPA plugins in a folder of its own passes that
   * folder in.
   */
  static std::vector<std::string> defaultLocationsToSearch() {
    std::vector<std::string> out;
    const std::string env = pathio::environment("LADSPA_PATH");
#if defined(_WIN32)
    const char separator = ';';
#else
    const char separator = ':';
#endif
    if (!env.empty()) {
      size_t pos = 0;
      while (pos <= env.size()) {
        size_t end = env.find(separator, pos);
        if (end == std::string::npos) end = env.size();
        if (end > pos) out.push_back(env.substr(pos, end - pos));
        pos = end + 1;
      }
      return out;
    }
#if defined(__APPLE__)
    const std::string home = pathio::environment("HOME");
    if (!home.empty()) out.push_back(home + "/Library/Audio/Plug-Ins/LADSPA");
    out.push_back("/Library/Audio/Plug-Ins/LADSPA");
#elif !defined(_WIN32)
    const std::string home = pathio::environment("HOME");
    if (!home.empty()) out.push_back(home + "/.ladspa");
    out.push_back("/usr/local/lib/ladspa");
    out.push_back("/usr/lib/ladspa");
    out.push_back("/usr/lib64/ladspa");
#endif
    return out;
  }

  /** The candidate files in one folder, sorted, nothing opened. Not
   *  recursive, for the reason pluginFilesIn gives. */
  static std::vector<std::string> filesIn(const std::string& directory) {
    std::vector<std::string> out;
#if defined(_WIN32)
    WIN32_FIND_DATAW fd{};
    const std::string pattern = directory + "\\*";
    HANDLE h = FindFirstFileW(pathio::widen(pattern.c_str()).c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string name = pathio::narrow(fd.cFileName);
        if (fileMightContainThisPluginType(name)) out.push_back(directory + "\\" + name);
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
#else
    if (DIR* dir = opendir(directory.c_str())) {
      while (struct dirent* e = readdir(dir)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (fileMightContainThisPluginType(name)) out.push_back(directory + "/" + name);
      }
      closedir(dir);
    }
#endif
    std::sort(out.begin(), out.end());
    return out;
  }

  /** Open one library and describe every plugin in it. Runs the library's
   *  code -- its static constructors and the entry point -- which is why a
   *  caller that scans should go through scanFilesCached and its dead man's
   *  switch. Empty for a file that is not a LADSPA library. */
  static std::vector<PluginDescription> describe(const std::string& path) {
    std::vector<PluginDescription> found;
    LibHandle lib = openLib(path.c_str());
    if (!lib) return found;
    auto entry = (ladspa::DescriptorFunction) libSymbol(lib, ladspa::kEntryPointName);
    // A library that exports a thousand plugins exists; one that claims
    // more than this is a corrupt entry point looping for ever.
    for (unsigned long i = 0; entry && i < 4096; ++i) {
      const ladspa::Descriptor* d = entry(i);
      if (!d) break;
      PluginDescription info;
      info.path = path;
      info.format = formatName();
      info.id = std::to_string(d->uniqueId);
      info.name = d->name ? d->name : (d->label ? d->label : "");
      info.vendor = d->maker ? d->maker : "";
      // The label is the plugin's machine name and the only other identity
      // the format has; carried so a caller can tell two plugins apart even
      // where a careless library gave both the same unique id.
      info.features = std::string("ladspa;label=") + (d->label ? d->label : "");
      found.push_back(info);
    }
    closeLib(lib);
    return found;
  }

  /** [main-thread] Instantiate what describe() found; loadLadspa() says how.
   *  Null when the library or the plugin in it has gone. */
  static std::unique_ptr<HostedPlugin> load(const PluginDescription& description) {
    return loadLadspa(description);
  }
};

/** Instantiate a LADSPA plugin a scan described: by unique id, or by label
 *  when the stored id is not a number. Null if the file or the plugin has
 *  gone. The instance itself is made in prepare(), at the host's rate. */
inline std::unique_ptr<HostedPlugin> loadLadspa(const PluginDescription& description) {
  std::unique_ptr<LadspaPlugin> hosted(new LadspaPlugin());
  hosted->desc_ = description;
  hosted->lib_ = openLib(description.path.c_str());
  if (!hosted->lib_) return nullptr;
  auto entry = (ladspa::DescriptorFunction) libSymbol(hosted->lib_, ladspa::kEntryPointName);
  if (!entry) return nullptr;
  for (unsigned long i = 0; i < 4096; ++i) {
    const ladspa::Descriptor* d = entry(i);
    if (!d) break;
    if (std::to_string(d->uniqueId) == description.id || (d->label && description.id == d->label)) {
      hosted->descriptor_ = d;
      break;
    }
  }
  const ladspa::Descriptor* d = hosted->descriptor_;
  if (!d || !d->portDescriptors || d->portCount > 65536) return nullptr;

  hosted->controls_.assign((size_t) d->portCount, 0.0f);
  for (unsigned long port = 0; port < d->portCount; ++port) {
    const int pd = d->portDescriptors[port];
    // Exactly one kind and one direction, as the specification requires. A
    // port that claimed to be audio AND control was filed as audio here and
    // then connected to a single control float by prepare(), and the plugin
    // wrote a whole block into it: past the end of controls_, in the host's
    // heap. One that is neither is never connected at all. There is no
    // connection such a plugin agrees with, so it is not loaded.
    if (((pd & ladspa::kPortAudio) != 0) == ((pd & ladspa::kPortControl) != 0) ||
        ((pd & ladspa::kPortInput) != 0) == ((pd & ladspa::kPortOutput) != 0))
      return nullptr;
    if (pd & ladspa::kPortAudio) {
      if (pd & ladspa::kPortInput) hosted->audioIn_.push_back(port);
      else if (pd & ladspa::kPortOutput) hosted->audioOut_.push_back(port);
    } else if ((pd & ladspa::kPortControl) && (pd & ladspa::kPortOutput) && d->portNames &&
               d->portNames[port]) {
      std::string name = d->portNames[port];
      for (char& c : name)
        if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
      if (name == "latency" || name == "_latency") hosted->latencyPort_ = (int) port;
    }
  }
  hosted->buildParameters(/*keepValues=*/false);
  return std::unique_ptr<HostedPlugin>(hosted.release());
}
// ── Audio Units, declared here and defined in host_au.h ──────────────────────
//
// The scan and the loader below reach them, and an AU host is a file of its
// own -- included at the bottom of this one, so a caller of host.h gets it
// without asking. Both exist on every platform and off macOS say why there
// is nothing.

/** The units a .component bundle at `path` declares, read from its
 *  Info.plist without running its code. A unit the system registry does not
 *  know is listed with the feature word "unregistered". Off macOS: empty,
 *  and `error`, when given, says why. */
inline std::vector<PluginDescription> describeAudioUnitBundle(const std::string& path,
                                                              std::string* error = nullptr);
/** [main-thread] Instantiate the Audio Unit `description.id` names
 *  ("type:subtype:manufacturer"), from the system registry; the path is not
 *  used. Null on failure, with the reason in `error` when it is given; on
 *  success `error` holds anything noted on the way, or is empty. An AUv3 is
 *  created asynchronously, the main run loop running while it waits. Off
 *  macOS always null. loadPlugin() calls this for a format of "AU". */
inline std::unique_ptr<HostedPlugin> loadAudioUnit(const PluginDescription& description,
                                                   std::string* error = nullptr);

// ── Scanning ─────────────────────────────────────────────────────────────────

/** Where a .clap's real binary lives.
 *
 *  On macOS a .clap is a BUNDLE -- a directory whose binary sits at
 *  `Contents/MacOS/<name>` -- exactly like a .vst3. On Windows and Linux the
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

/** Every file in a folder that MIGHT be a plugin, by name alone.
 *
 *  Nothing is opened here. That separation is what lets a caller decide, per
 *  file, whether to load it -- which is the difference between a scan that
 *  can skip a plugin known to crash and one that cannot. */
inline std::vector<std::string> pluginFilesIn(const std::string& directory) {
  std::vector<std::string> candidates;

#if defined(_WIN32)
  // Wide, then UTF-8: the narrow API returns names in the ANSI code page,
  // which then went on to be opened as if they were UTF-8.
  WIN32_FIND_DATAW fd{};
  const std::string pattern = directory + "\\*";
  HANDLE h = FindFirstFileW(pathio::widen(pattern.c_str()).c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      const std::string name = pathio::narrow(fd.cFileName);
      if (name == "." || name == "..") continue;
      if ((name.size() > 5 && name.compare(name.size() - 5, 5, ".clap") == 0) ||
          (name.size() > 5 && name.compare(name.size() - 5, 5, ".vst3") == 0) ||
          (name.size() > 4 && name.compare(name.size() - 4, 4, ".lv2") == 0))
        candidates.push_back(directory + "\\" + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
#else
  if (DIR* dir = opendir(directory.c_str())) {
    while (struct dirent* e = readdir(dir)) {
      const std::string name = e->d_name;
      if (name == "." || name == "..") continue;
      if ((name.size() > 5 && name.compare(name.size() - 5, 5, ".clap") == 0) ||
          (name.size() > 5 && name.compare(name.size() - 5, 5, ".vst3") == 0) ||
          (name.size() > 4 && name.compare(name.size() - 4, 4, ".lv2") == 0)
#if defined(__APPLE__)
          // An Audio Unit bundle, described from its Info.plist (host_au.h).
          || (name.size() > 10 && name.compare(name.size() - 10, 10, ".component") == 0)
#endif
      )
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
  const bool isComponent =
      path.size() > 10 && path.compare(path.size() - 10, 10, ".component") == 0;

  // Read, not run: an AU's bundle declares its units in Info.plist, and the
  // registry, not this file, is what loads one.
  if (isComponent) return describeAudioUnitBundle(path);

  if (isClap) {
    LibHandle lib = openLib(clapBinaryPath(path).c_str());
    if (!lib) return found;
    const auto* entry = static_cast<const clap_plugin_entry_t*>(libSymbol(lib, "clap_entry"));
    if (entry && detail::clapEntryAcquire(entry, path.c_str())) {
      const auto* factory =
          static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
      if (factory) {
        const uint32_t count =
            detail::heldCount(factory->get_plugin_count(factory), detail::kMaxPluginsPerFile);
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
      detail::clapEntryRelease(entry);
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
      // The class id as text, because that is what identifies a plugin inside
      // a module and it has to survive being written into a session file: the
      // text FUID::toString makes (vst3_uid.h), the same on every platform and
      // the one a .vstpreset another host wrote carries.
      d.id = vst3uid::toText(info.cid);
      d.name = detail::fixedString(info.name);
      // PClassInfo has no vendor or version; the richer PClassInfo2 does, and
      // asking for it is optional. Left empty rather than guessed.
      Steinberg_PFactoryInfo fi{};
      if (factory->lpVtbl->getFactoryInfo(factory, &fi) == Steinberg_kResultOk)
        d.vendor = detail::fixedString(fi.vendor);
      d.isInstrument = false; // decided below, from the subcategories
      Steinberg_IPluginFactory2* f2 = nullptr;
      if (factory->lpVtbl->queryInterface(factory, Steinberg_IPluginFactory2_iid, (void**) &f2) ==
              Steinberg_kResultOk &&
          f2) {
        const Steinberg_int32 n = (Steinberg_int32) detail::heldCount(
            f2->lpVtbl->countClasses(f2), detail::kMaxPluginsPerFile);
        for (Steinberg_int32 i = 0; i < n; ++i) {
          Steinberg_PClassInfo2 info2{};
          if (f2->lpVtbl->getClassInfo2(f2, i, &info2) != Steinberg_kResultOk) continue;
          if (std::memcmp(info2.cid, info.cid, sizeof(Steinberg_TUID)) != 0) continue;
          d.features = detail::fixedString(info2.subCategories);
          d.version = detail::fixedString(info2.version);
          if (d.vendor.empty()) d.vendor = detail::fixedString(info2.vendor);
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
  // Last, and by extension: a .dll or .so that is none of the above may be a
  // LADSPA library. pluginFilesIn never offers one -- a plugin folder full of
  // other people's DLLs is not a LADSPA folder -- so this is reached only for
  // a file somebody chose, or one LADSPAPluginFormat::filesIn listed.
  if (LADSPAPluginFormat::fileMightContainThisPluginType(path))
    return LADSPAPluginFormat::describe(path);
  (void) isVst3;
  (void) isLv2;
  return found;
}

/** Everything in a folder, loaded and described. One implementation, shared
 *  with the cached scan: the walk and the per-file load are the same code.
 *
 *  Not recursive, and that is on purpose: a scan that walks a whole drive is
 *  how a host ends up loading code it was never pointed at. A caller that
 *  wants a tree can walk it and call this per folder, which keeps the decision
 *  where it belongs. */
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
  if (!hosted->lib_) return nullptr;
  hosted->moduleEntered_ = vst3ModuleInit(hosted->lib_);
  if (!hosted->moduleEntered_) return nullptr;

  auto getFactory = (Steinberg_IPluginFactory * (*) ()) libSymbol(hosted->lib_,
                                                                  "GetPluginFactory");
  if (!getFactory) return nullptr;
  hosted->factory_ = getFactory();
  if (!hosted->factory_) return nullptr;

  // The description's id is the class id, printed as hex; find the class that
  // matches rather than trusting an index, which changes between versions.
  const Steinberg_int32 count = (Steinberg_int32) detail::heldCount(
      hosted->factory_->lpVtbl->countClasses(hosted->factory_), detail::kMaxPluginsPerFile);
  bool found = false;
  Steinberg_PClassInfo chosen{};
  for (Steinberg_int32 i = 0; i < count; ++i) {
    Steinberg_PClassInfo info{};
    if (hosted->factory_->lpVtbl->getClassInfo(hosted->factory_, i, &info) != Steinberg_kResultOk)
      continue;
    if (std::strcmp(info.category, "Audio Module Class") != 0) continue;
    if (description.id == vst3uid::toText(info.cid)) {
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
  // A context, not null: the host's name, and the factory for the messages
  // its halves talk in. Made here because this is where the plugin's life
  // starts; the name is the one setHostName() last gave.
  hosted->context_ = detail::makeVst3HostContext(hostName());
  auto* context = reinterpret_cast<Steinberg_FUnknown*>(hosted->context_);
  if (hosted->component_->lpVtbl->initialize(hosted->component_, context) != Steinberg_kResultOk)
    return nullptr;
  hosted->componentInitialized_ = true;
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
      // The same context. Its answer was ignored and a controller that
      // refused went on being used, uninitialised; a plugin whose controller
      // will not start is a plugin that does not load, as Steinberg's own
      // host has it -- released, and not terminated, since it never began.
      if (hosted->controller_->lpVtbl->initialize(hosted->controller_, context) !=
          Steinberg_kResultOk) {
        hosted->controller_->lpVtbl->release(hosted->controller_);
        hosted->controller_ = nullptr;
        return nullptr;
      }
      hosted->controllerIsSeparate_ = true;
      // Joined each to the other: the channel the processor sends meters and
      // the controller sends anything else down. Without it, a plugin built
      // on that channel shows an editor that never moves. Each is connected
      // to a line to the other rather than to the other itself, so nothing
      // it sends from the audio thread arrives (Vst3ConnectionProxy).
      Steinberg_Vst_IConnectionPoint* a = nullptr;
      Steinberg_Vst_IConnectionPoint* b = nullptr;
      hosted->component_->lpVtbl->queryInterface(hosted->component_,
                                                 Steinberg_Vst_IConnectionPoint_iid, (void**) &a);
      hosted->controller_->lpVtbl->queryInterface(hosted->controller_,
                                                  Steinberg_Vst_IConnectionPoint_iid, (void**) &b);
      if (a && b) {
        hosted->toController_ = detail::makeVst3ConnectionProxy(b, &hosted->processThread_);
        hosted->toComponent_ = detail::makeVst3ConnectionProxy(a, &hosted->processThread_);
        hosted->componentPoint_ = a;
        hosted->controllerPoint_ = b;
        a->lpVtbl->connect(
            a, reinterpret_cast<Steinberg_Vst_IConnectionPoint*>(hosted->toController_));
        b->lpVtbl->connect(
            b, reinterpret_cast<Steinberg_Vst_IConnectionPoint*>(hosted->toComponent_));
      } else {
        if (a) a->lpVtbl->release(a);
        if (b) b->lpVtbl->release(b);
      }
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
    hosted->handler_.second.lpVtbl = componentHandler2Vtbl();
    hosted->handler_.second.self = &hosted->handler_;
    hosted->handler_.edits = &hosted->edits_;
    hosted->handler_.params = &hosted->paramIndex_;
    hosted->handler_.controller = hosted->controller_;
    hosted->handler_.forward = &Vst3Plugin::forwardEdit;
    hosted->handler_.owner = hosted.get();
    hosted->controller_->lpVtbl->setComponentHandler(
        hosted->controller_, (Steinberg_Vst_IComponentHandler*) &hosted->handler_);
  }

  hosted->readCapabilities();
  return std::unique_ptr<HostedPlugin>(hosted.release());
}
#endif

/** Instantiate what a scan described. Returns null if the file moved, the id
 *  is gone, or the plugin refused to initialise, all of which happen to a
 *  real session opened on a different machine. */
inline std::unique_ptr<HostedPlugin> loadPlugin(const PluginDescription& description) {
#if defined(SONORE_HOST_VST3)
  if (description.format == "VST3") return loadVst3(description);
#endif
#if defined(SONORE_HOST_LV2)
  if (description.format == "LV2") return loadLv2(description);
#endif
  if (description.format == LADSPAPluginFormat::formatName()) return loadLadspa(description);
  if (description.format == "AU") return loadAudioUnit(description);
  if (description.format != "CLAP") return nullptr;

  std::unique_ptr<ClapPlugin> hosted(new ClapPlugin());
  hosted->desc_ = description;
  hosted->wireCallbacks();

  hosted->lib_ = openLib(clapBinaryPath(description.path).c_str());
  if (!hosted->lib_) return nullptr;

  hosted->entry_ =
      static_cast<const clap_plugin_entry_t*>(libSymbol(hosted->lib_, "clap_entry"));
  if (!hosted->entry_ || !detail::clapEntryAcquire(hosted->entry_, description.path.c_str())) {
    hosted->entry_ = nullptr; // never release an entry that failed to init
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
  // Nothing has packed an event yet: a flush asked for from init() is made
  // on the caller's next drain, after this returns.
  hosted->reserveEvents();
  hosted->crawlPresets(description.id.c_str());
  return std::unique_ptr<HostedPlugin>(hosted.release());
}

} // namespace host
} // namespace sonore

// Audio Units: AuPlugin, AudioUnitPluginFormat, and the two functions declared
// above. After everything, because it builds on all of it.
#include "host_au.h"
