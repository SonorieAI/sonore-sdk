// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: where a plugin's own MIDI goes.
//
// The standalone could be PLAYED and could not play anything back. Its
// comment said so plainly: "No host to route emitted MIDI to: the buffer
// exists so the DSP can write safely, and is dropped after the call." For an
// effect that is nothing; for the arpeggiator, the note splitter, or anything
// else built on `producesMidi`, it means the entire output of the plugin was
// written into a buffer and thrown away. A MIDI plugin you cannot hear is not
// a plugin.
//
// Two sinks, one interface, because the two useful answers to "where does it
// go" are a port and a file:
//
//   Device:  a hardware or virtual MIDI port, one backend per platform, every
//            symbol loaded at runtime so a machine with no MIDI still builds
//            and runs.
//   File:    a Standard MIDI File, written when the sink closes. Offline
//            rendering, and the only one of the two that a test can read back
//            and check, which is why it exists rather than being a nicety.
//
// Timestamps are seconds from the start of the run. A port ignores them and
// sends now, because that is what a port is; a file keeps them, because that
// is what a file is for.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "midi_file.h"

#if defined(_WIN32) && !defined(SONORE_APPLE_SYNTAX_CHECK)
// windows.h defines min/max as MACROS, which breaks std::min/std::max in any
// translation unit that includes this header after <algorithm>.
#ifndef NOMINMAX
#define NOMINMAX
#endif
// WIN32_LEAN_AND_MEAN, and then mmsystem.h by name.
//
// Without it windows.h drags in the ORIGINAL winsock.h, which redefines every
// type winsock2.h already declared -- and this header sits in the same build
// as osc.h, which needs winsock2. Leaving it out cost a wall of 'sockaddr:
// struct type redefinition'. Including mmsystem.h explicitly is what puts
// HMIDIOUT back, which is the only thing LEAN_AND_MEAN took away here.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mmsystem.h>
#elif defined(__linux__) && !defined(SONORE_APPLE_SYNTAX_CHECK)
#include <dlfcn.h>
#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
#include <CoreMIDI/CoreMIDI.h>
#endif

namespace sonore {
namespace midiout {

/** Where produced MIDI goes.
 *
 *  Deliberately not a queue: a standalone calls send() from the audio thread
 *  for a port (winmm and ALSA both take a short message without allocating)
 *  and from anywhere for a file. What each sink may do on that thread is the
 *  sink's business and is documented on the sink, rather than being hidden
 *  behind a ring buffer that every caller then pays for. */
class Sink {
public:
  /** A sink is closed by its own destructor; this one does nothing. */
  virtual ~Sink() = default;
  /** `timeSeconds` is measured from the start of the run, never from the
   *  epoch: a file written on Tuesday and one written on Wednesday have to
   *  contain the same ticks. */
  virtual void send(const MidiMessage& message, double timeSeconds) = 0;
  /** Flush and release. Safe to call twice; the second does nothing. */
  virtual bool close() = 0;
  /** Empty while healthy. A caller that prints this gets a reason rather than
   *  silence, which is the whole difference between "no MIDI came out" and
   *  "no MIDI could come out, because winmm has no devices". */
  virtual const std::string& error() const = 0;
};

// ── A Standard MIDI File ─────────────────────────────────────────────────────

/** Everything sent, written out as one track when the sink closes.
 *
 *  As constructed it ALLOCATES on send, which is right for an offline render
 *  that calls it on its own thread. A sink fed from a live audio thread calls
 *  startLive() first: send() then only writes into a ring, a thread of the
 *  sink's own moves the ring into the track, and nothing is lost unless the
 *  ring overflows between two of its passes -- which is counted and reported
 *  on close, never silent. */
class FileSink final : public Sink {
public:
  /** `bpm` fixes the seconds-to-ticks conversion and is written into the file
   *  as a tempo event, so a reader converts back to the same seconds instead
   *  of assuming 120 and being wrong by a factor. */
  FileSink(std::string path, double bpm = 120.0) : path_(std::move(path)), bpm_(bpm > 0 ? bpm : 120.0) {}
  /** close(): the file is written now if it was not already. */
  ~FileSink() override { close(); }

  /** [main thread, before the first live send] Make send() allocation-free.
   *  The ring holds `capacity` events, which a drain every 5 ms makes about
   *  1.6 million events a second -- far past anything MIDI carries. */
  void startLive(size_t capacity = 8192) {
    if (live_ || closed_) return;
    size_t size = 64;
    while (size < capacity) size <<= 1;
    ring_.assign(size, MidiFileEvent{});
    mask_ = size - 1;
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
    live_ = true;
    draining_.store(true, std::memory_order_release);
    drainer_ = std::thread([this] {
      while (draining_.load(std::memory_order_acquire)) {
        drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
  }

  void send(const MidiMessage& message, double timeSeconds) override {
    if (closed_) return;
    if (timeSeconds < 0.0) timeSeconds = 0.0;
    const double quarters = timeSeconds * bpm_ / 60.0;
    MidiFileEvent event;
    event.tick = (uint32_t) (quarters * (double) kTicksPerQuarter + 0.5);
    event.message = message;
    if (live_) {
      // The audio thread's half: one slot written, one index published.
      const size_t head = head_.load(std::memory_order_relaxed);
      if (head - tail_.load(std::memory_order_acquire) > mask_) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      ring_[head & mask_] = event;
      head_.store(head + 1, std::memory_order_release);
      return;
    }
    // Kept in order as they arrive, which they already are: a track's events
    // must be sorted by tick and re-sorting on close would reorder a note-off
    // that shares a tick with the note-on after it.
    events_.push_back(event);
    kept_.store(events_.size(), std::memory_order_release);
  }

  bool close() override {
    if (closed_) return written_;
    closed_ = true;
    if (live_) {
      draining_.store(false, std::memory_order_release);
      if (drainer_.joinable()) drainer_.join();
      drain(); // whatever arrived after the thread's last pass
      live_ = false;
    }

    MidiFileData file;
    file.ticksPerQuarter = kTicksPerQuarter;
    MidiTempoEvent tempo;
    tempo.tick = 0;
    tempo.bpm = bpm_;
    file.tempoChanges.push_back(tempo);
    MidiTrack track;
    track.name = "Sonore";
    track.events = events_;
    file.tracks.push_back(track);

    written_ = writeMidiFile(path_.c_str(), file);
    if (!written_) error_ = "the MIDI file could not be written to " + path_;
    else if (const uint64_t lost = dropped_.load(std::memory_order_relaxed))
      error_ = std::to_string(lost) + " MIDI event(s) arrived faster than they could be kept "
               "and are missing from " + path_;
    return written_;
  }

  /** Events kept so far. Live, those the drain has reached -- and safe to ask
   *  from any thread while it runs. It read events_.size() while the drain
   *  thread was growing the vector: a torn read of its two pointers, and a
   *  count of 18446744073709360294 events mid-run. The count is published
   *  beside the vector instead, once the events it counts are in it. */
  size_t numEvents() const { return kept_.load(std::memory_order_acquire); }
  /** Live events the ring had no room for. */
  uint64_t droppedEvents() const { return dropped_.load(std::memory_order_relaxed); }
  const std::string& error() const override { return error_; }

private:
  static constexpr uint16_t kTicksPerQuarter = 960;

  /** The drain's half: everything published so far, into the track. */
  void drain() {
    const size_t head = head_.load(std::memory_order_acquire);
    size_t tail = tail_.load(std::memory_order_relaxed);
    while (tail != head) {
      events_.push_back(ring_[tail & mask_]);
      ++tail;
    }
    tail_.store(tail, std::memory_order_release);
    kept_.store(events_.size(), std::memory_order_release);
  }

  std::string path_;
  double bpm_ = 120.0;
  std::vector<MidiFileEvent> events_; // the drain's alone while live; see numEvents()
  std::atomic<size_t> kept_{0};       // events_.size(), for any thread to read
  std::string error_;
  bool closed_ = false, written_ = false;
  // Live mode: a single-producer, single-consumer ring and its drain thread.
  bool live_ = false;
  std::vector<MidiFileEvent> ring_;
  size_t mask_ = 0;
  std::atomic<size_t> head_{0}, tail_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<bool> draining_{false};
  std::thread drainer_;
};

namespace detail {

/** [audio-thread] One short message through a NON-blocking writer -- ALSA's
 *  snd_rawmidi_write on a handle opened SND_RAWMIDI_NONBLOCK -- counted in
 *  `dropped` unless all of it went. `write` returns the bytes it took, or a
 *  negative errno: -EAGAIN when the port's buffer is full, the case this is
 *  for. Never retried: a loop on a full buffer is a spin on the audio
 *  thread, the very wait the non-blocking handle exists to avoid. A message
 *  cut short leaves the receiver a fragment, which its next status byte
 *  ends, so the stream recovers by itself. True when the message went. */
inline bool writeShortMessage(long (*write)(void*, const void*, size_t), void* handle,
                              const uint8_t* bytes, size_t n, std::atomic<uint64_t>& dropped) {
  if (write(handle, bytes, n) == (long) n) return true;
  dropped.fetch_add(1, std::memory_order_relaxed);
  return false;
}

/** What a port's error() says after close() when send() dropped messages:
 *  FileSink's rule, that a loss is reported rather than silent. */
inline std::string droppedReport(uint64_t n) {
  return std::to_string(n) + " MIDI message(s) found the port with no room for them and were "
         "not sent";
}

} // namespace detail

// ── Windows: winmm ───────────────────────────────────────────────────────────
#if defined(_WIN32) && !defined(SONORE_APPLE_SYNTAX_CHECK)

/**
 * A MIDI output port as a Sink: winmm here, ALSA's virtual rawmidi port on
 * Linux, a CoreMIDI destination on macOS, with the same calls on each.
 *
 * enumerate(), open() and close() belong to the main thread. send() is the
 * one call made from the audio thread: it sends at once, ignores the
 * timestamp, and takes short messages only (SysEx has no path out here).
 * open(index) takes an index into enumerate()'s list, closes whatever was
 * open first, and says why in error() when it fails (and clears it when it
 * does not: error() is about the latest open). close() -- also run by
 * the destructor -- releases the port; winmm's resets it first, so no note is
 * left sounding on the other end. Owns OS handles: do not copy one.
 *
 * send() never waits for the port. A message the driver will not take now
 * -- winmm busy, ALSA's buffer full, CoreMIDI refusing -- is dropped and
 * counted: droppedEvents() says how many since open(), and error() says so
 * after close(), as FileSink does for its ring.
 */
class Device final : public Sink {
public:
  /** close(). */
  ~Device() override { close(); }

  /** The ports this machine has, in UTF-8 and in the order winmm reports
   *  them, which is the order an index passed to open() means. Empty is a
   *  normal answer on a machine with no MIDI hardware and no virtual ports. */
  static std::vector<std::string> enumerate() {
    std::vector<std::string> names;
    HMODULE lib = LoadLibraryExW(L"winmm.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return names;
    auto getNumDevs = (UINT(WINAPI*)()) GetProcAddress(lib, "midiOutGetNumDevs");
    auto getDevCaps = (MMRESULT(WINAPI*)(UINT_PTR, LPMIDIOUTCAPSW, UINT))
        GetProcAddress(lib, "midiOutGetDevCapsW");
    if (getNumDevs && getDevCaps) {
      const UINT n = getNumDevs();
      for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPSW caps{};
        // A device whose name cannot be read still HAS an index, and open()
        // takes that index: skipping it shifted every later name by one, so
        // the port the list showed was not the port that opened. A
        // placeholder keeps the two in step, as midiin's list does.
        if (getDevCaps(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
          names.push_back("MIDI output " + std::to_string(i));
          continue;
        }
        // UTF-8, as every name in the SDK is. Each UTF-16 unit was cast to
        // char, so an e-acute (U+00E9) came back as the lone byte E9, which
        // is not UTF-8, and a CJK name as one unrelated byte a character.
        names.push_back(::sonore::pathio::narrow(caps.szPname, MAXPNAMELEN));
      }
    }
    FreeLibrary(lib);
    return names;
  }

  /** [main-thread] Close whatever was open, then open port `index`, 0-based
   *  in enumerate()'s order (Linux has one, at 0). False, with error() saying
   *  why, for no device, an index that is not there, or a port that refused. */
  bool open(int index) {
    // Idempotent, as midiin's is: a second open() overwrote the handle and
    // left the first one open -- leaked, and still holding a port that a
    // device with one client then refused to the second open. Closing first
    // costs nothing when nothing is open.
    close();
    // And forget why the LAST open failed. error() is empty while healthy,
    // and a port that opened after a refusal went on reporting the refusal:
    // open(-1) then open(0) on the GS Wavetable Synth succeeded with "there
    // is no MIDI output device at that index" still in error().
    error_.clear();
    dropped_.store(0, std::memory_order_relaxed);
    lib_ = LoadLibraryExW(L"winmm.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib_) {
      error_ = "winmm.dll is not available";
      return false;
    }
    auto getNumDevs = (UINT(WINAPI*)()) GetProcAddress(lib_, "midiOutGetNumDevs");
    midiOutOpen_ = (MMRESULT(WINAPI*)(HMIDIOUT*, UINT, DWORD_PTR, DWORD_PTR, DWORD))
        GetProcAddress(lib_, "midiOutOpen");
    midiOutShortMsg_ =
        (MMRESULT(WINAPI*)(HMIDIOUT, DWORD)) GetProcAddress(lib_, "midiOutShortMsg");
    midiOutReset_ = (MMRESULT(WINAPI*)(HMIDIOUT)) GetProcAddress(lib_, "midiOutReset");
    midiOutClose_ = (MMRESULT(WINAPI*)(HMIDIOUT)) GetProcAddress(lib_, "midiOutClose");
    if (!getNumDevs || !midiOutOpen_ || !midiOutShortMsg_) {
      error_ = "winmm is missing its MIDI output entry points";
      release();
      return false;
    }
    const UINT n = getNumDevs();
    if (n == 0) {
      error_ = "no MIDI output device is available";
      release();
      return false;
    }
    // Checked rather than passed through. winmm takes a UINT, so a negative
    // index becomes an enormous device number and the failure it produces
    // says nothing about what went wrong.
    if (index < 0 || (UINT) index >= n) {
      error_ = "there is no MIDI output device at that index";
      release();
      return false;
    }
    if (midiOutOpen_(&handle_, (UINT) index, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
      error_ = "the MIDI output device refused to open";
      release();
      return false;
    }
    return true;
  }

  /** Sends immediately; the timestamp is ignored, which is what a port means.
   *  No allocation and no lock, so this is safe from the audio thread. */
  void send(const MidiMessage& message, double) override {
    if (!handle_ || !midiOutShortMsg_) return;
    const DWORD packed = (DWORD) ((uint8_t) message.getRawStatus()) |
                         (DWORD) (((uint8_t) message.getRawData1()) << 8) |
                         (DWORD) (((uint8_t) message.getRawData2()) << 16);
    // MIDIERR_NOTREADY is winmm's "busy": not waited out, counted.
    if (midiOutShortMsg_(handle_, packed) != MMSYSERR_NOERROR)
      dropped_.fetch_add(1, std::memory_order_relaxed);
  }

  bool close() override {
    if (handle_) {
      // Reset first: it sends all-notes-off on every channel. Closing a port
      // mid-chord without it leaves the notes sounding on the synth at the
      // other end until something else happens to stop them.
      if (midiOutReset_) midiOutReset_(handle_);
      if (midiOutClose_) midiOutClose_(handle_);
      handle_ = nullptr;
    }
    release();
    if (const uint64_t lost = dropped_.load(std::memory_order_relaxed))
      error_ = detail::droppedReport(lost);
    return true;
  }

  const std::string& error() const override { return error_; }
  /** Messages send() dropped since open(): the driver would not take them. */
  uint64_t droppedEvents() const { return dropped_.load(std::memory_order_relaxed); }

private:
  void release() {
    if (lib_) {
      FreeLibrary(lib_);
      lib_ = nullptr;
    }
  }

  HMODULE lib_ = nullptr;
  HMIDIOUT handle_ = nullptr;
  std::string error_;
  std::atomic<uint64_t> dropped_{0}; // counted on the audio thread, read anywhere
  MMRESULT(WINAPI* midiOutOpen_)(HMIDIOUT*, UINT, DWORD_PTR, DWORD_PTR, DWORD) = nullptr;
  MMRESULT(WINAPI* midiOutShortMsg_)(HMIDIOUT, DWORD) = nullptr;
  MMRESULT(WINAPI* midiOutReset_)(HMIDIOUT) = nullptr;
  MMRESULT(WINAPI* midiOutClose_)(HMIDIOUT) = nullptr;
};

// ── Linux: ALSA rawmidi, dlopened ────────────────────────────────────────────
#elif defined(__linux__) && !defined(SONORE_APPLE_SYNTAX_CHECK)

/** The Linux Device: the winmm one's contract over ALSA's virtual rawmidi
 *  port, the only index being 0. The handle is NON-blocking, so send() on
 *  the audio thread never waits for the receiver; close() turns it back to
 *  blocking and drains what is queued first. */
class Device final : public Sink {
public:
  /** close(). */
  ~Device() override { close(); }

  /** One entry, and it is the truth rather than a list padded to look like
   *  Windows. This backend opens ALSA's "virtual" port, which anything can
   *  connect to with aconnect or from a DAW's own routing; enumerating
   *  hardware ports would mean the sequencer API and a second backend. */
  static std::vector<std::string> enumerate() {
    void* lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return {};
    const bool haveRawmidi = dlsym(lib, "snd_rawmidi_open") != nullptr;
    dlclose(lib);
    if (!haveRawmidi) return {};
    return {std::string("ALSA virtual port")};
  }

  /** [main-thread] Close whatever was open, then open port `index`, 0-based
   *  in enumerate()'s order (Linux has one, at 0). False, with error() saying
   *  why, for no device, an index that is not there, or a port that refused. */
  bool open(int index) {
    // Idempotent, as midiin's is: a second open() overwrote the handle and
    // left the first one open -- leaked, and still holding a port that a
    // device with one client then refused to the second open. Closing first
    // costs nothing when nothing is open.
    close();
    error_.clear(); // the last failure is not this open's, as on Windows
    dropped_.store(0, std::memory_order_relaxed);
    if (index != 0) {
      error_ = "this backend has one port, at index 0";
      return false;
    }
    lib_ = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib_) {
      error_ = "libasound.so.2 is not installed";
      return false;
    }
    // snd_rawmidi_open(inputp, outputp, name, mode): TWO handle-out pointers,
    // input FIRST. This is an output, so the first one is null -- getting
    // that the wrong way round is how the input backend once opened nothing.
    open_ = (int (*)(void**, void**, const char*, int)) dlsym(lib_, "snd_rawmidi_open");
    close_ = (int (*)(void*)) dlsym(lib_, "snd_rawmidi_close");
    write_ = (long (*)(void*, const void*, size_t)) dlsym(lib_, "snd_rawmidi_write");
    drain_ = (int (*)(void*)) dlsym(lib_, "snd_rawmidi_drain");
    nonblock_ = (int (*)(void*, int)) dlsym(lib_, "snd_rawmidi_nonblock");
    if (!open_ || !write_) {
      error_ = "libasound is missing its rawmidi entry points";
      release();
      return false;
    }
    // NONBLOCK (SND_RAWMIDI_NONBLOCK, 0x0002), as the input backend opens its
    // own. send() runs on the audio thread, and on a BLOCKING handle
    // snd_rawmidi_write waits for room when the buffer is full -- a receiver
    // that stopped reading, the sequencer's pool used up -- with the audio
    // callback parked inside it. Mode 0 was that: run against a fake
    // libasound whose buffer filled after four messages, six of ten sends
    // went into the wait. Now a full buffer is -EAGAIN, and the message is
    // dropped and counted (detail::writeShortMessage).
    if (open_(nullptr, &handle_, "virtual", 0x0002) < 0) {
      error_ = "no ALSA rawmidi output could be opened";
      handle_ = nullptr;
      release();
      return false;
    }
    return true;
  }

  void send(const MidiMessage& message, double) override {
    if (!handle_ || !write_) return;
    const uint8_t bytes[3] = {(uint8_t) message.getRawStatus(), (uint8_t) message.getRawData1(),
                              (uint8_t) message.getRawData2()};
    // Exactly as many bytes as the message has. A byte too many is read by
    // the receiver as the start of the next message, and everything after it
    // arrives shifted -- for a clock byte, F8 00 00 was the clock and then
    // one more event.
    const int n = shortMessageLength(bytes[0]);
    if (n > 0) detail::writeShortMessage(write_, handle_, bytes, (size_t) n, dropped_);
  }

  bool close() override {
    if (handle_) {
      // Blocking again for the drain: close() is the main thread's, and
      // waiting there is what makes the queued messages go out rather than
      // stop at the first full buffer.
      if (nonblock_) nonblock_(handle_, 0);
      if (drain_) drain_(handle_);
      if (close_) close_(handle_);
      handle_ = nullptr;
    }
    release();
    if (const uint64_t lost = dropped_.load(std::memory_order_relaxed))
      error_ = detail::droppedReport(lost);
    return true;
  }

  const std::string& error() const override { return error_; }
  /** Messages send() dropped since open(): the port's buffer was full. */
  uint64_t droppedEvents() const { return dropped_.load(std::memory_order_relaxed); }

private:
  void release() {
    if (lib_) {
      dlclose(lib_);
      lib_ = nullptr;
    }
  }

  void* lib_ = nullptr;
  void* handle_ = nullptr;
  std::string error_;
  std::atomic<uint64_t> dropped_{0}; // counted on the audio thread, read anywhere
  int (*open_)(void**, void**, const char*, int) = nullptr;
  int (*close_)(void*) = nullptr;
  long (*write_)(void*, const void*, size_t) = nullptr;
  int (*drain_)(void*) = nullptr;
  int (*nonblock_)(void*, int) = nullptr;
};

// ── macOS: CoreMIDI ──────────────────────────────────────────────────────────
#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)

/** The macOS Device: the winmm one's contract over one CoreMIDI
 *  destination, by its index in enumerate(). */
class Device final : public Sink {
public:
  /** close(). */
  ~Device() override { close(); }

  static std::vector<std::string> enumerate() {
    std::vector<std::string> names;
    const ItemCount n = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < n; ++i) {
      MIDIEndpointRef endpoint = MIDIGetDestination(i);
      if (!endpoint) continue;
      CFStringRef name = nullptr;
      if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) != noErr ||
          !name) {
        names.push_back("MIDI destination");
        continue;
      }
      char buffer[128] = {};
      CFStringGetCString(name, buffer, sizeof(buffer), kCFStringEncodingUTF8);
      CFRelease(name);
      names.push_back(buffer);
    }
    return names;
  }

  /** [main-thread] Close whatever was open, then open port `index`, 0-based
   *  in enumerate()'s order (Linux has one, at 0). False, with error() saying
   *  why, for no device, an index that is not there, or a port that refused. */
  bool open(int index) {
    // Idempotent, as midiin's is: a second open() overwrote the handle and
    // left the first one open -- leaked, and still holding a port that a
    // device with one client then refused to the second open. Closing first
    // costs nothing when nothing is open.
    close();
    error_.clear(); // the last failure is not this open's, as on Windows
    dropped_.store(0, std::memory_order_relaxed);
    const ItemCount n = MIDIGetNumberOfDestinations();
    if (n == 0) {
      error_ = "no CoreMIDI destination is connected";
      return false;
    }
    if (index < 0 || (ItemCount) index >= n) {
      error_ = "there is no MIDI output device at that index";
      return false;
    }
    if (MIDIClientCreate(CFSTR("Sonore"), nullptr, nullptr, &client_) != noErr) {
      error_ = "CoreMIDI refused to create a client";
      return false;
    }
    if (MIDIOutputPortCreate(client_, CFSTR("Out"), &port_) != noErr) {
      error_ = "CoreMIDI refused to create an output port";
      close();
      return false;
    }
    destination_ = MIDIGetDestination((ItemCount) index);
    if (!destination_) {
      error_ = "that CoreMIDI destination could not be opened";
      close();
      return false;
    }
    return true;
  }

  void send(const MidiMessage& message, double) override {
    if (!port_ || !destination_) return;
    const Byte bytes[3] = {(Byte) message.getRawStatus(), (Byte) message.getRawData1(),
                           (Byte) message.getRawData2()};
    const int length = shortMessageLength(bytes[0]); // see the ALSA output
    if (length <= 0) return;
    const ByteCount n = (ByteCount) length;
    Byte storage[256];
    MIDIPacketList* list = (MIDIPacketList*) storage;
    MIDIPacket* packet = MIDIPacketListInit(list);
    packet = MIDIPacketListAdd(list, sizeof(storage), packet, 0, n, bytes);
    if (!packet || MIDISend(port_, destination_, list) != noErr)
      dropped_.fetch_add(1, std::memory_order_relaxed);
  }

  bool close() override {
    if (port_) {
      MIDIPortDispose(port_);
      port_ = 0;
    }
    if (client_) {
      MIDIClientDispose(client_);
      client_ = 0;
    }
    destination_ = 0;
    if (const uint64_t lost = dropped_.load(std::memory_order_relaxed))
      error_ = detail::droppedReport(lost);
    return true;
  }

  const std::string& error() const override { return error_; }
  /** Messages send() dropped since open(): CoreMIDI refused them. */
  uint64_t droppedEvents() const { return dropped_.load(std::memory_order_relaxed); }

private:
  MIDIClientRef client_ = 0;
  MIDIPortRef port_ = 0;
  MIDIEndpointRef destination_ = 0;
  std::string error_;
  std::atomic<uint64_t> dropped_{0}; // counted on the audio thread, read anywhere
};

#else

/** No MIDI backend for this platform: an empty list, an open() that says
 *  so, and a send() that does nothing. */
class Device final : public Sink {
public:
  static std::vector<std::string> enumerate() { return {}; }
  bool open(int) {
    error_ = "no MIDI backend for this platform";
    return false;
  }
  void send(const MidiMessage&, double) override {}
  bool close() override { return true; }
  const std::string& error() const override { return error_; }
  uint64_t droppedEvents() const { return 0; }

private:
  std::string error_;
};

#endif

} // namespace midiout
} // namespace sonore
