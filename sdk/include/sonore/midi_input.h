// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: live MIDI input for the standalone build.
//
// A standalone instrument that cannot be played by a keyboard is a demo, not
// an application. This is the smallest honest implementation of "the notes a
// device sends reach the DSP": one platform backend each, everything loaded at
// runtime so a machine without MIDI still builds and runs.
//
// The device thread and the audio thread never share anything but a
// lock-free ring, because the audio callback must not wait for a driver.

#pragma once

#include <atomic>
#include <thread>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "audio.h"
#include "sysex.h"

#if defined(_WIN32) && !defined(SONORE_APPLE_SYNTAX_CHECK)
// windows.h defines min/max as MACROS, which breaks std::min/std::max in any
// translation unit that includes this header after <algorithm>.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// WIN32_LEAN_AND_MEAN excludes mmsystem.h, which is where HMIDIIN lives.
#include <mmsystem.h>
#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
#include <CoreMIDI/CoreMIDI.h>
#include <dlfcn.h>
#elif defined(__linux__) && !defined(SONORE_APPLE_SYNTAX_CHECK)
#include <dlfcn.h>
#include <poll.h> // the reader thread waits in poll(), not a blocking read
#include <time.h>
#endif

namespace sonore {
namespace midiin {

/** Single-producer/single-consumer ring of 3-byte messages. The device thread
 *  writes, the audio thread drains; neither ever blocks the other. */
class MessageQueue {
public:
  static constexpr int kCapacity = 512;

  void push(uint8_t status, uint8_t d1, uint8_t d2) {
    const uint32_t w = write_.load(std::memory_order_relaxed);
    const uint32_t next = (w + 1) % kCapacity;
    if (next == read_.load(std::memory_order_acquire)) return; // full: drop, never block
    slots_[w] = {status, d1, d2};
    write_.store(next, std::memory_order_release);
  }

  /**
   * A complete SysEx message, from whichever backend assembled it.
   *
   * Its own ring, because the messages are variable length and the three-byte
   * slots above cannot hold one. Same discipline: the producer is a driver
   * callback or a reader thread, the consumer is the audio thread, and a full
   * ring drops rather than blocks.
   *
   * Copied on the way in. Every backend hands over a pointer it owns -- a
   * winmm MIDIHDR that is about to be recycled, a CoreMIDI packet that lives
   * only for the callback -- and keeping any of them would be reading freed
   * memory a block later.
   */
  bool pushSysex(const uint8_t* data, size_t bytes) {
    if (!data || bytes < 2 || bytes > kSysexBytes) return false;
    const uint32_t w = sysexWrite_.load(std::memory_order_relaxed);
    const uint32_t next = (w + 1) % kSysexSlots;
    if (next == sysexRead_.load(std::memory_order_acquire)) {
      droppedSysex_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    SysexSlot& slot = sysex_[w];
    for (size_t i = 0; i < bytes; ++i) slot.bytes[i] = data[i];
    slot.length = (uint16_t) bytes;
    sysexWrite_.store(next, std::memory_order_release);
    return true;
  }

  /** How many SysEx messages were dropped for want of room. A plugin that
   *  quietly stops answering MIDI-CI should be able to say why. */
  uint32_t droppedSysex() const { return droppedSysex_.load(std::memory_order_relaxed); }

  /** Drain everything pending into a block's MIDI buffer. Events land at
   *  offset 0: a USB keyboard's jitter is already larger than one block, so
   *  pretending to know the sample offset would be false precision. */
  void drain(MidiBuffer& out) {
    uint32_t r = read_.load(std::memory_order_relaxed);
    const uint32_t w = write_.load(std::memory_order_acquire);
    while (r != w) {
      const Slot& s = slots_[r];
      if (deliverableToDsp(s.status)) out.addEvent(MidiMessage(s.status, s.d1, s.d2), 0);
      r = (r + 1) % kCapacity;
    }
    read_.store(r, std::memory_order_release);

    uint32_t sr = sysexRead_.load(std::memory_order_relaxed);
    const uint32_t sw = sysexWrite_.load(std::memory_order_acquire);
    while (sr != sw) {
      const SysexSlot& slot = sysex_[sr];
      out.addSysex(slot.bytes, slot.length, 0);
      sr = (sr + 1) % kSysexSlots;
    }
    sysexRead_.store(sr, std::memory_order_release);
  }

private:
  struct Slot {
    uint8_t status = 0, d1 = 0, d2 = 0;
  };
  // Four in flight is generous for what actually arrives: MIDI-CI is a
  // negotiation of a few messages, not a stream. Sized to match what a block
  // can carry, so the ring never holds something MidiBuffer would refuse.
  static constexpr int kSysexSlots = 4;
  static constexpr int kSysexBytes = MidiBuffer::kSysexCapacity;
  struct SysexSlot {
    uint8_t bytes[kSysexBytes] = {0};
    uint16_t length = 0;
  };
  Slot slots_[kCapacity];
  std::atomic<uint32_t> write_{0};
  std::atomic<uint32_t> read_{0};
  SysexSlot sysex_[kSysexSlots];
  std::atomic<uint32_t> sysexWrite_{0};
  std::atomic<uint32_t> sysexRead_{0};
  std::atomic<uint32_t> droppedSysex_{0};
};

// ── Windows: winmm ───────────────────────────────────────────────────────────
#if defined(_WIN32) && !defined(SONORE_APPLE_SYNTAX_CHECK)

class Device {
public:
  /** Every MIDI input the machine has, in the order --midi-input numbers
   *  them. Empty is a normal answer, not a failure: most machines have none. */
  static std::vector<std::string> listDevices() {
    std::vector<std::string> names;
    HMODULE lib = LoadLibraryA("winmm.dll");
    if (!lib) return names;
    auto numDevs = (UINT(WINAPI*)()) GetProcAddress(lib, "midiInGetNumDevs");
    auto getCaps = (MMRESULT(WINAPI*)(UINT_PTR, MIDIINCAPSA*, UINT))
        GetProcAddress(lib, "midiInGetDevCapsA");
    if (numDevs && getCaps) {
      const UINT count = numDevs();
      for (UINT i = 0; i < count; ++i) {
        MIDIINCAPSA caps{};
        if (getCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) names.push_back(caps.szPname);
        else names.push_back("MIDI input " + std::to_string(i));
      }
    }
    FreeLibrary(lib);
    return names;
  }

  const std::string& deviceName() const { return deviceName_; }

  bool open(MessageQueue* queue, int index = -1) {
    // Idempotent, because a second open() used to OVERWRITE the handle and
    // leave the first one still delivering -- the same note arriving twice,
    // then three times, with no sign of where the copies came from. Closing
    // first costs nothing when nothing is open.
    close();
    queue_ = queue;
    lib_ = LoadLibraryA("winmm.dll");
    if (!lib_) {
      error_ = "winmm.dll is not available";
      return false;
    }
    auto getNumDevs = (UINT(WINAPI*)()) GetProcAddress(lib_, "midiInGetNumDevs");
    midiInOpen_ = (MMRESULT(WINAPI*)(HMIDIIN*, UINT, DWORD_PTR, DWORD_PTR, DWORD))
        GetProcAddress(lib_, "midiInOpen");
    midiInStart_ = (MMRESULT(WINAPI*)(HMIDIIN)) GetProcAddress(lib_, "midiInStart");
    midiInPrepareHeader_ = (MMRESULT(WINAPI*)(HMIDIIN, MIDIHDR*, UINT))
        GetProcAddress(lib_, "midiInPrepareHeader");
    midiInUnprepareHeader_ = (MMRESULT(WINAPI*)(HMIDIIN, MIDIHDR*, UINT))
        GetProcAddress(lib_, "midiInUnprepareHeader");
    midiInAddBuffer_ = (MMRESULT(WINAPI*)(HMIDIIN, MIDIHDR*, UINT))
        GetProcAddress(lib_, "midiInAddBuffer");
    midiInReset_ = (MMRESULT(WINAPI*)(HMIDIIN)) GetProcAddress(lib_, "midiInReset");
    midiInStop_ = (MMRESULT(WINAPI*)(HMIDIIN)) GetProcAddress(lib_, "midiInStop");
    midiInClose_ = (MMRESULT(WINAPI*)(HMIDIIN)) GetProcAddress(lib_, "midiInClose");
    if (!getNumDevs || !midiInOpen_ || !midiInStart_) {
      error_ = "winmm is missing its MIDI entry points";
      return false;
    }
    if (getNumDevs() == 0) {
      error_ = "no MIDI input device is connected";
      return false;
    }
    // The device the caller asked for, or the first one.
    //
    // This used to be hard-coded to 0, with a comment saying the standalone
    // had no UI to ask the question with. It has had one since the audio
    // device picker was built, so the reason expired and the code did not --
    // which is a thing to look for wherever a decision is written down
    // alongside its justification.
    const UINT count = getNumDevs();
    UINT chosen = (index >= 0 && (UINT) index < count) ? (UINT) index : 0;
    deviceName_ = nameOf(chosen);
    if (midiInOpen_(&handle_, chosen, (DWORD_PTR) &callback, (DWORD_PTR) this,
                    CALLBACK_FUNCTION) !=
        MMSYSERR_NOERROR) {
      error_ = "the MIDI input device refused to open";
      return false;
    }
    // SysEx buffers, lent to the driver. Without these winmm delivers short
    // messages and silently discards every SysEx -- there is no error and no
    // callback, which is why this was missing for as long as it was.
    //
    // Best effort: a MIDI keyboard that never sends SysEx works exactly the
    // same without them, so a driver that refuses the buffers must not cost
    // the user their notes.
    if (midiInPrepareHeader_ && midiInAddBuffer_) {
      for (int i = 0; i < kSysexBuffers; ++i) {
        std::memset(&headers_[i], 0, sizeof(MIDIHDR));
        headers_[i].lpData = (LPSTR) sysexBuffers_[i];
        headers_[i].dwBufferLength = (DWORD) sizeof(sysexBuffers_[i]);
        if (midiInPrepareHeader_(handle_, &headers_[i], sizeof(MIDIHDR)) == MMSYSERR_NOERROR)
          midiInAddBuffer_(handle_, &headers_[i], sizeof(MIDIHDR));
      }
    }

    midiInStart_(handle_);
    return true;
  }

  /** [driver-thread] Hand a returned buffer straight back to winmm. Without
   *  this a driver gets one SysEx buffer's worth and then goes quiet, which
   *  looks exactly like a controller that stopped sending. */
  void requeue(MIDIHDR* header) {
    if (!handle_ || !midiInAddBuffer_ || closing_) return;
    midiInAddBuffer_(handle_, header, sizeof(MIDIHDR));
  }

  // RAII: a Device dropped without an explicit close() must still stop the
  // driver and return its buffers, or it leaks the open device and the pinned
  // SysEx headers.
  ~Device() { close(); }

  void close() {
    closing_ = true;
    if (handle_) {
      if (midiInStop_) midiInStop_(handle_);
      // RESET before unpreparing: it returns every buffer still lent out, and
      // midiInUnprepareHeader refuses a header the driver still holds. Getting
      // this order wrong leaks the buffers and leaves the device open.
      if (midiInReset_) midiInReset_(handle_);
      if (midiInUnprepareHeader_)
        for (int i = 0; i < kSysexBuffers; ++i)
          midiInUnprepareHeader_(handle_, &headers_[i], sizeof(MIDIHDR));
      if (midiInClose_) midiInClose_(handle_);
      handle_ = nullptr;
    }
    closing_ = false;
    if (lib_) {
      FreeLibrary(lib_);
      lib_ = nullptr;
    }
  }

  const std::string& error() const { return error_; }

private:
  static void CALLBACK callback(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR) {
    auto* self = (Device*) instance;
    if (msg == MIM_LONGDATA) {
      // A SysEx buffer coming back. winmm fills the MIDIHDR we lent it and
      // returns it here; dwBytesRecorded says how much of it is real, and a
      // message longer than one buffer arrives as several of these.
      //
      // Zero bytes means the buffer is being returned at CLOSE, not filled.
      // Requeuing it there is how a close hangs for ever.
      if (self && self->queue_) {
        auto* header = (MIDIHDR*) p1;
        if (header && header->dwBytesRecorded > 0) {
          self->assembler_.pushBlock((const uint8_t*) header->lpData,
                                     (size_t) header->dwBytesRecorded,
                                     [self](const uint8_t* bytes, size_t n) {
                                       self->queue_->pushSysex(bytes, n);
                                     });
          self->requeue(header);
        }
      }
      return;
    }
    if (msg != MIM_DATA) return;
    if (self && self->queue_)
      self->queue_->push((uint8_t) (p1 & 0xff), (uint8_t) ((p1 >> 8) & 0x7f),
                         (uint8_t) ((p1 >> 16) & 0x7f));
  }

  HMODULE lib_ = nullptr;
  HMIDIIN handle_ = nullptr;
  MessageQueue* queue_ = nullptr;
  std::string error_;
  MMRESULT(WINAPI* midiInOpen_)(HMIDIIN*, UINT, DWORD_PTR, DWORD_PTR, DWORD) = nullptr;
  MMRESULT(WINAPI* midiInStart_)(HMIDIIN) = nullptr;
  MMRESULT(WINAPI* midiInPrepareHeader_)(HMIDIIN, MIDIHDR*, UINT) = nullptr;
  MMRESULT(WINAPI* midiInUnprepareHeader_)(HMIDIIN, MIDIHDR*, UINT) = nullptr;
  MMRESULT(WINAPI* midiInAddBuffer_)(HMIDIIN, MIDIHDR*, UINT) = nullptr;
  MMRESULT(WINAPI* midiInReset_)(HMIDIIN) = nullptr;
  /** Two, so the driver always holds one while the other is being emptied. */
  static constexpr int kSysexBuffers = 2;
  MIDIHDR headers_[kSysexBuffers]{};
  uint8_t sysexBuffers_[kSysexBuffers][1024]{};
  SysexAssembler assembler_;
  bool closing_ = false;
  std::string deviceName_;

  /** One device's name, for the log and the picker. */
  std::string nameOf(UINT index) const {
    if (!lib_) return std::string();
    auto getCaps = (MMRESULT(WINAPI*)(UINT_PTR, MIDIINCAPSA*, UINT))
        GetProcAddress(lib_, "midiInGetDevCapsA");
    MIDIINCAPSA caps{};
    if (getCaps && getCaps(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR) return caps.szPname;
    return "MIDI input " + std::to_string(index);
  }
  MMRESULT(WINAPI* midiInStop_)(HMIDIIN) = nullptr;
  MMRESULT(WINAPI* midiInClose_)(HMIDIIN) = nullptr;
};

// ── Linux: ALSA sequencer, dlopened ─────────────────────────────────────────
#elif defined(__linux__) && !defined(SONORE_APPLE_SYNTAX_CHECK)

class Device {
public:
  /** ALSA rawmidi through a VIRTUAL port, which is one endpoint whatever is
   *  plugged in: anything connects to it with aconnect or a patchbay. So
   *  there is one entry rather than a hardware list, and saying so is more
   *  honest than enumerating cards the sequencer does not route through
   *  this. */
  static std::vector<std::string> listDevices() {
    return {std::string("ALSA virtual rawmidi port")};
  }

  const std::string& deviceName() const { return deviceName_; }

  bool open(MessageQueue* queue, int index = -1) {
    (void) index; // one virtual port; an index would be a choice with no options
    // Idempotent, because a second open() used to OVERWRITE the handle and
    // leave the first one still delivering -- the same note arriving twice,
    // then three times, with no sign of where the copies came from. Closing
    // first costs nothing when nothing is open.
    close();
    queue_ = queue;
    deviceName_ = "ALSA virtual rawmidi port";
    lib_ = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib_) {
      error_ = "libasound.so.2 is not installed";
      return false;
    }
    // snd_rawmidi_open(inputp, outputp, name, mode): TWO handle-out pointers,
    // input first. Passing one is how this silently opened nothing.
    open_ = (int (*)(void**, void**, const char*, int)) dlsym(lib_, "snd_rawmidi_open");
    close_ = (int (*)(void*)) dlsym(lib_, "snd_rawmidi_close");
    read_ = (long (*)(void*, void*, size_t)) dlsym(lib_, "snd_rawmidi_read");
    // Poll descriptors so the wait lives in poll() with a timeout rather than
    // parked inside snd_rawmidi_read -- see pump(). Universally present since
    // ALSA 1.0; pump() degrades to a short sleep if somehow absent.
    pollCount_ = (int (*)(void*)) dlsym(lib_, "snd_rawmidi_poll_descriptors_count");
    pollDescr_ = (int (*)(void*, struct pollfd*, unsigned)) dlsym(
        lib_, "snd_rawmidi_poll_descriptors");
    if (!open_ || !read_) {
      error_ = "libasound is missing its rawmidi entry points";
      return false;
    }
    // "virtual" is a port any tool can connect to (aconnect, a DAW, a
    // keyboard through the sequencer), which beats guessing a hardware id.
    // NONBLOCK (0x0002): the read must return rather than park, so poll() owns
    // the waiting and the thread can always notice running_ went false.
    if (open_(&handle_, nullptr, "virtual", 0x0002) < 0) { // input only, no output
      error_ = "no ALSA rawmidi input could be opened";
      return false;
    }
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { pump(); });
    return true;
  }

  // Stops and joins the reader before the object goes away. Without it a
  // joinable std::thread is destroyed -> std::terminate takes the whole app
  // down, and the fix above (poll instead of a blocking read) is what makes the
  // join actually return rather than hang.
  ~Device() { close(); }

  void close() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    if (handle_ && close_) close_(handle_);
    handle_ = nullptr;
    if (lib_) {
      dlclose(lib_);
      lib_ = nullptr;
    }
  }

  const std::string& error() const { return error_; }

private:
  void pump() {
    // The wait happens in poll(), not in snd_rawmidi_read. A blocking read with
    // no MIDI arriving would never come back to check running_, so close()'s
    // join() would hang for ever -- and quitting the app while not playing is
    // the common case, not an edge one. poll() wakes on incoming data OR on its
    // timeout; the timeout is the only thing that lets the thread see it should
    // stop. The handle is opened NONBLOCK so the drain below ends at EAGAIN.
    const int nfds = (pollCount_ && handle_) ? pollCount_(handle_) : 0;
    std::vector<struct pollfd> fds(nfds > 0 ? (size_t) nfds : 0);
    if (nfds > 0 && pollDescr_) pollDescr_(handle_, fds.data(), (unsigned) nfds);
    const bool canPoll = nfds > 0 && !fds.empty();

    uint8_t byte = 0;
    uint8_t message[3] = {0, 0, 0};
    int wanted = 0, have = 0;
    while (running_.load(std::memory_order_acquire)) {
      if (canPoll) {
        if (::poll(fds.data(), (nfds_t) nfds, 100 /* ms */) <= 0) continue;
      } else {
        // No poll descriptors: sleep briefly so the loop still bounds shutdown
        // and does not spin a core on EAGAIN.
        struct timespec ts{0, 5 * 1000 * 1000}; // 5 ms
        nanosleep(&ts, nullptr);
      }
      // Drain every byte the nonblocking handle has; read returns < 1 at EAGAIN.
      while (running_.load(std::memory_order_acquire) && read_(handle_, &byte, 1) == 1) {
      if (byte >= 0xf8) {
        // A realtime byte may appear BETWEEN the bytes of another message --
        // that is legal MIDI, and it is why they are handled here rather than
        // as part of whatever is half-assembled. Passed through now, and the
        // partial message below is left exactly as it was.
        queue_->push(byte, 0, 0);
        continue;
      }
      // SysEx first: while one is being collected every byte belongs to it,
      // including status bytes, which is what ends it. Letting the
      // running-status parser below see those bytes would rebuild a note-on
      // out of the middle of a manufacturer's payload.
      if (isSysexStart(byte) || assembler_.inProgress()) {
        if (assembler_.push(byte)) queue_->pushSysex(assembler_.data(), assembler_.size());
        // A status byte that TERMINATED a SysEx without 0xF7 is still a status
        // byte, and the message it starts must not be lost.
        if (isDataByte(byte) || isSysexStart(byte) || assembler_.inProgress()) continue;
      }
      if (byte >= 0x80) {
        message[0] = byte;
        have = 1;
        // Program change and channel pressure are two bytes, the rest three.
        const uint8_t high = byte & 0xf0;
        wanted = (high == 0xc0 || high == 0xd0) ? 2 : 3;
        continue;
      }
      if (have == 0) continue; // data before any status: running status we
                               // never saw the start of
      message[have++] = byte;
      if (have == wanted) {
        queue_->push(message[0], message[1], wanted == 3 ? message[2] : 0);
        have = 1; // running status: the same status may repeat
      }
      } // drain loop: back to poll() when the handle reports EAGAIN
    }
  }

  void* lib_ = nullptr;
  void* handle_ = nullptr;
  MessageQueue* queue_ = nullptr;
  std::string error_;
  std::thread thread_;
  SysexAssembler assembler_;
  std::string deviceName_;
  std::atomic<bool> running_{false};
  int (*open_)(void**, void**, const char*, int) = nullptr;
  int (*close_)(void*) = nullptr;
  long (*read_)(void*, void*, size_t) = nullptr;
  int (*pollCount_)(void*) = nullptr;
  int (*pollDescr_)(void*, struct pollfd*, unsigned) = nullptr;
};

// ── macOS: CoreMIDI ─────────────────────────────────────────────────────────
#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)

class Device {
public:
  /** Every CoreMIDI source. Compiled on macOS; unrun against a device, because
   *  the CI runner has none. */
  static std::vector<std::string> listDevices() {
    std::vector<std::string> names;
    const ItemCount count = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < count; ++i) names.push_back(sourceName(MIDIGetSource(i)));
    return names;
  }

  const std::string& deviceName() const { return deviceName_; }

  bool open(MessageQueue* queue, int index = -1) {
    // Idempotent, because a second open() used to OVERWRITE the handle and
    // leave the first one still delivering -- the same note arriving twice,
    // then three times, with no sign of where the copies came from. Closing
    // first costs nothing when nothing is open.
    close();
    queue_ = queue;
    sourceIndex_ = index;
    if (MIDIGetNumberOfSources() == 0) {
      error_ = "no CoreMIDI source is connected";
      return false;
    }
    CFStringRef name = CFSTR("Sonore");
    if (MIDIClientCreate(name, nullptr, nullptr, &client_) != noErr) {
      error_ = "CoreMIDI refused to create a client";
      return false;
    }
    if (MIDIInputPortCreate(client_, CFSTR("In"), readProc, this, &port_) != noErr) {
      error_ = "CoreMIDI refused to create an input port";
      return false;
    }
    // Every source at once: the standalone has no UI to choose with, and a
    // player expects whatever they plugged in to just work.
    const ItemCount sources = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < sources; ++i) MIDIPortConnectSource(port_, MIDIGetSource(i), nullptr);
    return true;
  }

  // RAII: dropping a Device without close() would leak the CoreMIDI port and
  // client, which keep delivering into a queue that is about to disappear.
  ~Device() { close(); }

  void close() {
    if (port_) MIDIPortDispose(port_);
    if (client_) MIDIClientDispose(client_);
    port_ = 0;
    client_ = 0;
  }

  const std::string& error() const { return error_; }

private:
  static void readProc(const MIDIPacketList* list, void* refCon, void*) {
    auto* self = (Device*) refCon;
    if (!self || !self->queue_ || !list) return;
    const MIDIPacket* packet = &list->packet[0];
    for (UInt32 p = 0; p < list->numPackets; ++p) {
      // A packet may carry several messages back to back; walk them by the
      // length their status byte implies rather than assuming one each.
      for (UInt16 i = 0; i < packet->length;) {
        const uint8_t status = packet->data[i];
        // Realtime first: one byte, no data, and it may sit between two
        // halves of anything else.
        if (isSystemRealtime(status)) {
          self->queue_->push(status, 0, 0);
          ++i;
          continue;
        }
        if (isSysexStart(status) || self->assembler_.inProgress()) {
          // A packet may carry a fragment: CoreMIDI splits a long SysEx
          // across packets, so the assembler holds state between them rather
          // than each packet being a whole message.
          const UInt16 remaining = (UInt16) (packet->length - i);
          const UInt16 used = (UInt16) self->assembler_.pushBlock(
              packet->data + i, remaining,
              [self](const uint8_t* bytes, size_t n) { self->queue_->pushSysex(bytes, n); });
          (void) used;
          // pushBlock consumes the rest of the packet. Anything after the
          // terminator was pushed through the assembler too, which ignores
          // bytes outside a message -- so nothing is lost and nothing is
          // parsed twice.
          break;
        }
        if (!deliverableToDsp(status)) {
          // A data byte with no status in front of it, or the start of a
          // SysEx this queue cannot carry. The SAME policy the wrappers use,
          // rather than a second range test that could drift from it.
          ++i;
          continue;
        }
        const uint8_t high = status & 0xf0;
        // System common: 0xF1 and 0xF3 carry one data byte, 0xF2 carries two,
        // and the rest carry none. Sharing the channel-message table here
        // would read a byte that is not there.
        UInt16 len = (high == 0xc0 || high == 0xd0) ? 2 : 3;
        if (status >= 0xF1) len = (status == 0xF2) ? 3 : ((status == 0xF1 || status == 0xF3) ? 2 : 1);
        if (i + len > packet->length) break;
        self->queue_->push(status, packet->data[i + 1], len == 3 ? packet->data[i + 2] : 0);
        i += len;
      }
      packet = MIDIPacketNext(packet);
    }
  }

  MIDIClientRef client_ = 0;
  MIDIPortRef port_ = 0;
  MessageQueue* queue_ = nullptr;
  std::string error_, deviceName_;
  int sourceIndex_ = -1;
  /** Held BETWEEN packets: CoreMIDI splits a long SysEx across several, so a
   *  per-packet assembler would emit nothing at all for exactly the messages
   *  worth having. */
  SysexAssembler assembler_;

  static std::string sourceName(MIDIEndpointRef endpoint) {
    CFStringRef name = nullptr;
    if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name) != noErr || !name)
      return std::string("MIDI source");
    const CFIndex length = CFStringGetLength(name);
    const CFIndex bytes = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer((size_t) bytes, 0);
    const bool ok = CFStringGetCString(name, buffer.data(), bytes, kCFStringEncodingUTF8);
    CFRelease(name);
    return ok ? std::string(buffer.data()) : std::string("MIDI source");
  }
};

#else

class Device {
public:
  static std::vector<std::string> listDevices() { return {}; }
  const std::string& deviceName() const { return deviceName_; }
  bool open(MessageQueue*, int = -1) {
    error_ = "no MIDI backend for this platform";
    return false;
  }
  void close() {}
  const std::string& error() const { return error_; }

private:
  std::string error_, deviceName_;
};

#endif

} // namespace midiin
} // namespace sonore
