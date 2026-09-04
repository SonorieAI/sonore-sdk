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
//   Device: a hardware or virtual MIDI port, one backend per platform, every
//            symbol loaded at runtime so a machine with no MIDI still builds
//            and runs.
//   File: a Standard MIDI File, written when the sink closes. Offline
//            rendering, and the only one of the two that a test can read back
//            and check, which is why it exists rather than being a nicety.
//
// Timestamps are seconds from the start of the run. A port ignores them and
// sends now, because that is what a port is; a file keeps them, because that
// is what a file is for.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "audio.h"
#include "midi_file.h"

#if defined(_WIN32)
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
#elif defined(__linux__)
#include <dlfcn.h>
#elif defined(__APPLE__)
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
 *  ALLOCATES on send, so this is for offline rendering and tests rather than
 *  for a live audio thread. Saying so is better than a lock-free ring that
 *  silently drops the tail of a long render, which is the failure mode of the
 *  clever version. */
class FileSink final : public Sink {
public:
  /** `bpm` fixes the seconds-to-ticks conversion and is written into the file
   *  as a tempo event, so a reader converts back to the same seconds instead
   *  of assuming 120 and being wrong by a factor. */
  FileSink(std::string path, double bpm = 120.0) : path_(std::move(path)), bpm_(bpm > 0 ? bpm : 120.0) {}
  ~FileSink() override { close(); }

  void send(const MidiMessage& message, double timeSeconds) override {
    if (closed_) return;
    if (timeSeconds < 0.0) timeSeconds = 0.0;
    const double quarters = timeSeconds * bpm_ / 60.0;
    MidiFileEvent event;
    event.tick = (uint32_t) (quarters * (double) kTicksPerQuarter + 0.5);
    event.message = message;
    // Kept in order as they arrive, which they already are: a track's events
    // must be sorted by tick and re-sorting on close would reorder a note-off
    // that shares a tick with the note-on after it.
    events_.push_back(event);
  }

  bool close() override {
    if (closed_) return written_;
    closed_ = true;

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
    return written_;
  }

  size_t numEvents() const { return events_.size(); }
  const std::string& error() const override { return error_; }

private:
  static constexpr uint16_t kTicksPerQuarter = 960;

  std::string path_;
  double bpm_ = 120.0;
  std::vector<MidiFileEvent> events_;
  std::string error_;
  bool closed_ = false, written_ = false;
};

// ── Windows: winmm ───────────────────────────────────────────────────────────
#if defined(_WIN32)

class Device final : public Sink {
public:
  ~Device() override { close(); }

  /** The ports this machine has, in the order winmm reports them, which is
   *  the order an index passed to open() means. Empty is a normal answer on a
   *  machine with no MIDI hardware and no virtual ports. */
  static std::vector<std::string> enumerate() {
    std::vector<std::string> names;
    HMODULE lib = LoadLibraryA("winmm.dll");
    if (!lib) return names;
    auto getNumDevs = (UINT(WINAPI*)()) GetProcAddress(lib, "midiOutGetNumDevs");
    auto getDevCaps = (MMRESULT(WINAPI*)(UINT_PTR, LPMIDIOUTCAPSW, UINT))
        GetProcAddress(lib, "midiOutGetDevCapsW");
    if (getNumDevs && getDevCaps) {
      const UINT n = getNumDevs();
      for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPSW caps{};
        if (getDevCaps(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
        std::string narrow;
        for (int k = 0; k < 32 && caps.szPname[k]; ++k) narrow.push_back((char) caps.szPname[k]);
        names.push_back(narrow);
      }
    }
    FreeLibrary(lib);
    return names;
  }

  bool open(int index) {
    lib_ = LoadLibraryA("winmm.dll");
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
    midiOutShortMsg_(handle_, packed);
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
    return true;
  }

  const std::string& error() const override { return error_; }

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
  MMRESULT(WINAPI* midiOutOpen_)(HMIDIOUT*, UINT, DWORD_PTR, DWORD_PTR, DWORD) = nullptr;
  MMRESULT(WINAPI* midiOutShortMsg_)(HMIDIOUT, DWORD) = nullptr;
  MMRESULT(WINAPI* midiOutReset_)(HMIDIOUT) = nullptr;
  MMRESULT(WINAPI* midiOutClose_)(HMIDIOUT) = nullptr;
};

// ── Linux: ALSA rawmidi, dlopened ────────────────────────────────────────────
#elif defined(__linux__)

class Device final : public Sink {
public:
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

  bool open(int index) {
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
    if (!open_ || !write_) {
      error_ = "libasound is missing its rawmidi entry points";
      release();
      return false;
    }
    if (open_(nullptr, &handle_, "virtual", 0) < 0) {
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
    // Two bytes for program change and channel pressure. Sending a third
    // would be sending a byte the receiver reads as the start of the next
    // message, and everything after it arrives shifted.
    const size_t n = twoByteStatus(bytes[0]) ? 2u : 3u;
    write_(handle_, bytes, n);
  }

  bool close() override {
    if (handle_) {
      if (drain_) drain_(handle_);
      if (close_) close_(handle_);
      handle_ = nullptr;
    }
    release();
    return true;
  }

  const std::string& error() const override { return error_; }

private:
  static bool twoByteStatus(uint8_t status) {
    const uint8_t kind = (uint8_t) (status & 0xf0);
    return kind == 0xc0 || kind == 0xd0;
  }

  void release() {
    if (lib_) {
      dlclose(lib_);
      lib_ = nullptr;
    }
  }

  void* lib_ = nullptr;
  void* handle_ = nullptr;
  std::string error_;
  int (*open_)(void**, void**, const char*, int) = nullptr;
  int (*close_)(void*) = nullptr;
  long (*write_)(void*, const void*, size_t) = nullptr;
  int (*drain_)(void*) = nullptr;
};

// ── macOS: CoreMIDI ──────────────────────────────────────────────────────────
#elif defined(__APPLE__)

class Device final : public Sink {
public:
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

  bool open(int index) {
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
    const ByteCount n = twoByteStatus(bytes[0]) ? 2 : 3;
    Byte storage[256];
    MIDIPacketList* list = (MIDIPacketList*) storage;
    MIDIPacket* packet = MIDIPacketListInit(list);
    packet = MIDIPacketListAdd(list, sizeof(storage), packet, 0, n, bytes);
    if (packet) MIDISend(port_, destination_, list);
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
    return true;
  }

  const std::string& error() const override { return error_; }

private:
  static bool twoByteStatus(Byte status) {
    const Byte kind = (Byte) (status & 0xf0);
    return kind == 0xc0 || kind == 0xd0;
  }

  MIDIClientRef client_ = 0;
  MIDIPortRef port_ = 0;
  MIDIEndpointRef destination_ = 0;
  std::string error_;
};

#else

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

private:
  std::string error_;
};

#endif

} // namespace midiout
} // namespace sonore
