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
#include "path_io.h"
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
  static constexpr int kCapacity = 512; ///< Ring slots; one stays empty, so 511 messages wait at most.

  /** [driver-thread] One short message, as the device sent it. Dropped
   *  without a word when the ring is full: the device thread never waits. */
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

  /** Drain what fits into a block's MIDI buffer. Events land at offset 0: a
   *  USB keyboard's jitter is already larger than one block, so pretending
   *  to know the sample offset would be false precision.
   *
   *  What does NOT fit stays in the ring for the next block. It used to be
   *  taken out regardless and dropped by the full buffer -- note-offs
   *  included, which is a stuck note -- when waiting one block costs a few
   *  milliseconds of a latency the comment above already calls jitter. */
  void drain(MidiBuffer& out) {
    uint32_t r = read_.load(std::memory_order_relaxed);
    const uint32_t w = write_.load(std::memory_order_acquire);
    while (r != w) {
      const Slot& s = slots_[r];
      if (deliverableToDsp(s.status)) {
        if (out.isFull()) break;
        out.addEvent(MidiMessage(s.status, s.d1, s.d2), 0);
      }
      r = (r + 1) % kCapacity;
    }
    read_.store(r, std::memory_order_release);

    // The same rule for SysEx: a message the block cannot take waits for the
    // next one. The slot was released whether addSysex took it or not, so
    // two 1500-byte messages in one block lost the second -- uncounted, since
    // droppedSysex() only sees a full RING.
    uint32_t sr = sysexRead_.load(std::memory_order_relaxed);
    const uint32_t sw = sysexWrite_.load(std::memory_order_acquire);
    while (sr != sw) {
      const SysexSlot& slot = sysex_[sr];
      if (!out.addSysex(slot.bytes, slot.length, 0)) break;
      sr = (sr + 1) % kSysexSlots;
    }
    sysexRead_.store(sr, std::memory_order_release);
  }

private:
  /** One short message as the driver gave it, unfiltered: drain() is where
   *  a status a DSP does not take is skipped. */
  struct Slot {
    uint8_t status = 0, ///< The status byte.
        d1 = 0,         ///< The first data byte, or 0.
        d2 = 0;         ///< The second, or 0.
  };
  // Four in flight is generous for what actually arrives: MIDI-CI is a
  // negotiation of a few messages, not a stream. FIVE slots for four: a ring
  // keeps one empty to tell full from empty, and four held three. Each slot
  // holds what one empty block can carry, so a message that waits always
  // fits in the next block.
  static constexpr int kSysexSlots = 5;
  static constexpr int kSysexBytes = MidiBuffer::kSysexCapacity;
  /** One complete SysEx, copied in, F0 to F7. Sized to a whole block's SysEx
   *  arena, so a message that waits always fits the next block. */
  struct SysexSlot {
    uint8_t bytes[kSysexBytes] = {0}; ///< The message, F0 to F7.
    uint16_t length = 0;              ///< Its length in bytes.
  };
  Slot slots_[kCapacity];
  std::atomic<uint32_t> write_{0};
  std::atomic<uint32_t> read_{0};
  SysexSlot sysex_[kSysexSlots];
  std::atomic<uint32_t> sysexWrite_{0};
  std::atomic<uint32_t> sysexRead_{0};
  std::atomic<uint32_t> droppedSysex_{0};
};

/**
 * Bytes in, messages out, for ONE source.
 *
 * The byte-stream backends -- ALSA rawmidi, and CoreMIDI, whose packets are
 * byte runs that may split a SysEx -- each had a parser of their own, and
 * each got different things wrong:
 *
 *   * Linux treated every 0xF1..0xF7 status as a three-byte message that set
 *     running status. MTC quarter frames, song select and tune request never
 *     came out, and the bytes after a SysEx came out as [F7 xx yy] events.
 *   * macOS shared one SysEx assembler between every source, so a note from
 *     one keyboard in the middle of another's SysEx was lost with the SysEx;
 *     it dropped a clock byte inside a SysEx packet and anything after the
 *     F7 in the same packet; and it read a data byte past the packet's end.
 *
 * One parser, then, with the rules written once: realtime bytes pass
 * through wherever they fall, SysEx is collected until its F7 or until a
 * status byte ends it (which then starts its own message), system common
 * messages are sized by shortMessageLength and cancel running status, and
 * channel messages keep it. Platform-free, so it is tested on any machine.
 */
class StreamParser {
public:
  /** `onShort(status, d1, d2)` for each complete short message,
   *  `onSysex(bytes, length)` for each complete SysEx. */
  template <typename OnShort, typename OnSysex>
  void feed(uint8_t byte, OnShort&& onShort, OnSysex&& onSysex) {
    if (byte >= 0xF8) { // realtime: anywhere, even inside a SysEx
      onShort(byte, 0, 0);
      return;
    }
    if (isSysexStart(byte) || sysex_.inProgress()) {
      if (sysex_.push(byte)) onSysex(sysex_.data(), sysex_.size());
      if (byte >= 0x80) have_ = 0; // a SysEx cancels running status
      if (byte < 0x80 || isSysexStart(byte) || byte == 0xF7) return;
      // Any other status byte ended the SysEx unfinished, and is still the
      // start of a message of its own.
    }
    if (byte >= 0x80) {
      const int length = shortMessageLength(byte);
      have_ = 0;
      if (length == 0) return; // 0xF4, 0xF5, a stray 0xF7: not messages
      if (length == 1) {       // tune request
        onShort(byte, 0, 0);
        return;
      }
      message_[0] = byte;
      have_ = 1;
      wanted_ = length;
      return;
    }
    if (have_ == 0) return; // a data byte with no status in front of it
    message_[have_++] = byte;
    if (have_ == wanted_) {
      onShort(message_[0], message_[1], wanted_ == 3 ? message_[2] : 0);
      // Running status is for channel messages; a system common message
      // leaves none behind.
      have_ = message_[0] < 0xF0 ? 1 : 0;
    }
  }

  /** Into a queue, as every backend wants. */
  void feed(uint8_t byte, MessageQueue& queue) {
    feed(
        byte, [&queue](uint8_t s, uint8_t d1, uint8_t d2) { queue.push(s, d1, d2); },
        [&queue](const uint8_t* bytes, size_t n) { queue.pushSysex(bytes, n); });
  }

  /** Forget a message part way through, SysEx included, and running
   *  status: for a source that was disconnected and reconnected. */
  void reset() {
    sysex_.reset();
    have_ = 0;
    wanted_ = 0;
  }

private:
  SysexAssembler sysex_;
  uint8_t message_[3] = {0, 0, 0};
  int have_ = 0, wanted_ = 0;
};

// ── Windows: winmm ───────────────────────────────────────────────────────────
#if defined(_WIN32) && !defined(SONORE_APPLE_SYNTAX_CHECK)

/**
 * One open MIDI input, feeding a MessageQueue: winmm here, ALSA rawmidi on
 * Linux, CoreMIDI on macOS, the same five calls on each.
 *
 * open() and close() belong to the main thread; the bytes arrive on the
 * driver's thread (a winmm callback, a CoreMIDI read proc, a reader thread of
 * this class's own on Linux), which pushes into the queue, and the audio
 * thread drains the queue. The queue is the caller's: it must outlive the
 * device's open life, and only one Device may feed it, since it has one
 * producer side. open() closes whatever was open first; the destructor
 * closes too. It owns OS handles: do not copy one.
 *
 * On failure open() returns false and error() says why in a sentence fit for
 * a user; an open() that succeeds leaves it empty. No device at all is the
 * common case and an ordinary answer.
 */
class Device {
public:
  /** Every MIDI input the machine has, in UTF-8 and in the order
   *  --midi-input numbers them. Empty is a normal answer, not a failure: most
   *  machines have none. */
  static std::vector<std::string> listDevices() {
    std::vector<std::string> names;
    HMODULE lib = LoadLibraryExW(L"winmm.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return names;
    auto numDevs = (UINT(WINAPI*)()) (void*) GetProcAddress(lib, "midiInGetNumDevs");
    // The wide call: the A one answers in the ANSI code page, not UTF-8, so
    // an accented name came out as bytes no UTF-8 reader takes and one
    // outside the code page as question marks.
    auto getCaps = (MMRESULT(WINAPI*)(UINT_PTR, MIDIINCAPSW*, UINT))
        (void*) GetProcAddress(lib, "midiInGetDevCapsW");
    if (numDevs && getCaps) {
      const UINT count = numDevs();
      for (UINT i = 0; i < count; ++i) {
        MIDIINCAPSW caps{};
        if (getCaps(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
          names.push_back(::sonore::pathio::narrow(caps.szPname, MAXPNAMELEN));
        else names.push_back("MIDI input " + std::to_string(i));
      }
    }
    FreeLibrary(lib);
    return names;
  }

  /** The name of the input open() chose, for a picker or a status line;
   *  empty before the first open(). */
  const std::string& deviceName() const { return deviceName_; }

  /** [main-thread] Close whatever was open, then open input `index`, as
   *  listDevices() numbers them, and start delivering into `queue`. -1, or
   *  an index that is not there, takes the first input on Windows and every
   *  source at once on macOS; Linux has one virtual port and ignores it.
   *  False, with error() saying why, when there is no device or it refused. */
  bool open(MessageQueue* queue, int index = -1) {
    // Idempotent, because a second open() used to OVERWRITE the handle and
    // leave the first one still delivering -- the same note arriving twice,
    // then three times, with no sign of where the copies came from. Closing
    // first costs nothing when nothing is open.
    close();
    // And forget why the LAST open failed: error() is empty while healthy,
    // and a device that opened after a refusal -- plugged in since, say --
    // went on reporting the refusal. midiout's Device did the same.
    error_.clear();
    queue_ = queue;
    lib_ = LoadLibraryExW(L"winmm.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib_) {
      error_ = "winmm.dll is not available";
      return false;
    }
    auto getNumDevs = (UINT(WINAPI*)()) (void*) GetProcAddress(lib_, "midiInGetNumDevs");
    midiInOpen_ = (MMRESULT(WINAPI*)(HMIDIIN*, UINT, DWORD_PTR, DWORD_PTR, DWORD))
        (void*) GetProcAddress(lib_, "midiInOpen");
    midiInStart_ = (MMRESULT(WINAPI*)(HMIDIIN)) (void*) GetProcAddress(lib_, "midiInStart");
    midiInPrepareHeader_ = (MMRESULT(WINAPI*)(HMIDIIN, MIDIHDR*, UINT))
        (void*) GetProcAddress(lib_, "midiInPrepareHeader");
    midiInUnprepareHeader_ = (MMRESULT(WINAPI*)(HMIDIIN, MIDIHDR*, UINT))
        (void*) GetProcAddress(lib_, "midiInUnprepareHeader");
    midiInAddBuffer_ = (MMRESULT(WINAPI*)(HMIDIIN, MIDIHDR*, UINT))
        (void*) GetProcAddress(lib_, "midiInAddBuffer");
    midiInReset_ = (MMRESULT(WINAPI*)(HMIDIIN)) (void*) GetProcAddress(lib_, "midiInReset");
    midiInStop_ = (MMRESULT(WINAPI*)(HMIDIIN)) (void*) GetProcAddress(lib_, "midiInStop");
    midiInClose_ = (MMRESULT(WINAPI*)(HMIDIIN)) (void*) GetProcAddress(lib_, "midiInClose");
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
  /** close(). */
  ~Device() { close(); }

  /** [main-thread] Stop the device and give its buffers back; nothing
   *  reaches the queue afterwards. Harmless when nothing is open. */
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

  /** Why the last open() failed, in a sentence fit for a user; empty after
   *  one that succeeded. */
  const std::string& error() const { return error_; }

private:
  friend struct DeviceTest; // midi_stack_test drives the callback with a fake winmm

  static void CALLBACK callback(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR) {
    auto* self = (Device*) instance;
    if (msg == MIM_LONGDATA || msg == MIM_LONGERROR) {
      // A SysEx buffer coming back. winmm fills the MIDIHDR we lent it and
      // returns it here; dwBytesRecorded says how much of it is real, and a
      // message longer than one buffer arrives as several of these.
      //
      // MIM_LONGERROR is the same buffer holding a SysEx the driver saw cut
      // short -- a status byte before its F7. Neither that nor what the
      // assembler holds of it is a message. But the buffer is ours again
      // all the same, and only lending it back keeps SysEx arriving: winmm
      // throws SysEx away, unannounced, while it holds no buffer. This
      // answered MIM_LONGDATA alone, so two cut-short messages took both
      // buffers and the input was deaf to SysEx -- MIDI-CI included --
      // until it was reopened.
      //
      // Zero bytes means the buffer is being returned at CLOSE, not filled.
      // Requeuing it there is how a close hangs for ever.
      if (self && self->queue_) {
        auto* header = (MIDIHDR*) p1;
        if (header && header->dwBytesRecorded > 0) {
          if (msg == MIM_LONGDATA)
            self->assembler_.pushBlock((const uint8_t*) header->lpData,
                                       (size_t) header->dwBytesRecorded,
                                       [self](const uint8_t* bytes, size_t n) {
                                         self->queue_->pushSysex(bytes, n);
                                       });
          else
            self->assembler_.reset();
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
    auto getCaps = (MMRESULT(WINAPI*)(UINT_PTR, MIDIINCAPSW*, UINT))
        (void*) GetProcAddress(lib_, "midiInGetDevCapsW");
    MIDIINCAPSW caps{};
    if (getCaps && getCaps(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
      return ::sonore::pathio::narrow(caps.szPname, MAXPNAMELEN);
    return "MIDI input " + std::to_string(index);
  }
  MMRESULT(WINAPI* midiInStop_)(HMIDIIN) = nullptr;
  MMRESULT(WINAPI* midiInClose_)(HMIDIIN) = nullptr;
};

// ── Linux: ALSA sequencer, dlopened ─────────────────────────────────────────
#elif defined(__linux__) && !defined(SONORE_APPLE_SYNTAX_CHECK)

/** The Linux Device: the contract of the winmm one above, over one ALSA
 *  virtual rawmidi port read by a thread of its own, which close() joins. */
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
    error_.clear(); // the last failure is not this open's, as on Windows
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
    parser_.reset();
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
      // Through the shared parser: realtime bytes between the bytes of
      // another message, SysEx framing, system common sizes, running status.
      while (running_.load(std::memory_order_acquire) && read_(handle_, &byte, 1) == 1)
        parser_.feed(byte, *queue_);
      // back to poll() when the handle reports EAGAIN
    }
  }

  void* lib_ = nullptr;
  void* handle_ = nullptr;
  MessageQueue* queue_ = nullptr;
  std::string error_;
  std::thread thread_;
  StreamParser parser_;
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

/** The macOS Device: the contract of the winmm one above, over CoreMIDI --
 *  one source, or every source at once when none is chosen, each with a
 *  StreamParser of its own. */
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
    error_.clear(); // the last failure is not this open's, as on Windows
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
    // The source the picker chose, as on every other platform. This said the
    // standalone had no UI to choose with; it has one, and here the choice
    // was ignored -- every source connected, and no name for the picker to
    // show. With no choice (-1, or one that has gone), every source at once:
    // whatever was plugged in should just work.
    //
    // A parser PER SOURCE, handed to CoreMIDI as the connection's refCon. One
    // shared parser meant one shared SysEx in progress: a note from a second
    // keyboard in the middle of the first one's SysEx ended it, and both were
    // lost. Sized before anything connects, so the pointers never move.
    const ItemCount sources = MIDIGetNumberOfSources();
    const bool one = index >= 0 && (ItemCount) index < sources;
    parsers_.assign(one ? 1 : (size_t) sources, StreamParser());
    if (one) {
      const MIDIEndpointRef source = MIDIGetSource((ItemCount) index);
      MIDIPortConnectSource(port_, source, &parsers_[0]);
      deviceName_ = sourceName(source);
    } else {
      for (ItemCount i = 0; i < sources; ++i)
        MIDIPortConnectSource(port_, MIDIGetSource(i), &parsers_[(size_t) i]);
      deviceName_ = sources == 1 ? sourceName(MIDIGetSource(0)) : std::string("every MIDI source");
    }
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
  static void readProc(const MIDIPacketList* list, void* refCon, void* connRefCon) {
    auto* self = (Device*) refCon;
    auto* parser = (StreamParser*) connRefCon;
    if (!self || !self->queue_ || !list || !parser) return;
    // Every byte of every packet through this source's own parser. A packet
    // is a byte run: it may hold several messages, a fragment of a SysEx
    // that continues in the next, a clock byte inside either, and a message
    // after a SysEx's F7. The parser keeps its state between packets, and
    // never reads past the bytes a packet actually holds.
    const MIDIPacket* packet = &list->packet[0];
    for (UInt32 p = 0; p < list->numPackets; ++p) {
      for (UInt16 i = 0; i < packet->length; ++i) parser->feed(packet->data[i], *self->queue_);
      packet = MIDIPacketNext(packet);
    }
  }

  MIDIClientRef client_ = 0;
  MIDIPortRef port_ = 0;
  MessageQueue* queue_ = nullptr;
  std::string error_, deviceName_;
  int sourceIndex_ = -1;
  /** One per connected source, held BETWEEN packets: CoreMIDI splits a long
   *  SysEx across several, and two sources must not share one in progress. */
  std::vector<StreamParser> parsers_;

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

/** No MIDI backend for this platform: the same calls, an empty list, and an
 *  open() that says so. */
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
