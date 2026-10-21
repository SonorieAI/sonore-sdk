// SPDX-License-Identifier: Apache-2.0
// Sonore SDK. Audio CDs: reading them (AudioCDReader) and burning them
// (AudioCDBurner). Windows only; elsewhere both report that nothing is there.
//
// ── Reading ─────────────────────────────────────────────────────────────────
//
// An audio CD has no files. Explorer shows "Track01.cda", but those are
// 44-byte stubs the file system driver invents; the audio is raw sectors of
// 2352 bytes -- 588 stereo frames of 16-bit little-endian PCM at 44.1 kHz,
// with no header and no checksum the drive will let us see. So the reader
// talks to the drive directly: IOCTL_CDROM_READ_TOC_EX for the table of
// contents (where each track starts, in sectors, and which are data), and
// IOCTL_CDROM_RAW_READ in CDDA mode for the sectors themselves.
//
// The TOC is parsed from the bytes the drive returns (CDToc::parse), not
// through the SDK's C structs, so it can be tested without a drive and read
// with suspicion: a drive's answer is input like any other, and a TOC whose
// tracks go backwards or past the lead-out is refused, not trusted.
//
// What this does NOT do is what secure rippers do: re-read every sector until
// two reads agree, correct for the drive's read offset, and check the result
// against AccurateRip. A scratched disc reads here as whatever the drive's own
// error concealment produced. That is the reference's behaviour too.
//
// ── Burning ─────────────────────────────────────────────────────────────────
//
// Through IMAPI2, Windows' own burning service: IDiscFormat2TrackAtOnce writes
// each track as 16-bit stereo 44.1 kHz PCM from a stream, then closes the disc
// so any CD player reads it. Tracks are padded with silence to a whole sector
// and to the four-second minimum the Red Book sets, because a drive refuses
// (or a player skips) a shorter one.
//
// IMAPI2 reports progress through a COM connection point, and those events
// arrive on IMAPI's own thread -- and deadlock if the burning thread is a
// single-threaded apartment that is busy inside the blocking AddAudioTrack. So
// burn() does the COM work on a thread of its own, in the multithreaded
// apartment, and the caller's thread only polls: the progress callback runs on
// the CALLER's thread, as the reference's does, and returning true from it
// cancels.
//
// ── What has been run ───────────────────────────────────────────────────────
//
// The machine this was written on has no optical drive. The TOC parser, the
// sector arithmetic, the CDDB id, the track preparation and every "no drive"
// path are tested (audio_io_test); the DeviceIoControl and IMAPI2 calls
// compile and are written to Microsoft's documentation, and have not read or
// burned a real disc.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "wav.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <imapi2.h>
#include <imapi2error.h>
#include "path_io.h"
#include "win_com.h"
// BSTR and SAFEARRAY, which every IMAPI2 property speaks.
#if defined(_MSC_VER)
#pragma comment(lib, "oleaut32.lib")
#endif
#endif

namespace sonore {

/**
 * A CD's table of contents: where each track starts, and where the disc's
 * program area ends (the lead-out). Addresses are LBAs -- sector numbers
 * counted from the start of the first track's index 1, which is the "00:02:00"
 * of MSF time; kPregapSectors is that two seconds.
 */
struct CDToc {
  static constexpr int kSamplesPerSector = 588;  ///< Stereo frames in one sector.
  static constexpr int kBytesPerSector = 2352;   ///< 588 frames of two 16-bit samples.
  static constexpr int kSectorsPerSecond = 75;   ///< Sectors, or MSF frames, per second.
  static constexpr int kPregapSectors = 150;     ///< The two seconds before LBA 0.
  static constexpr uint32_t kSampleRate = 44100; ///< Every audio CD's rate, in Hz.

  /** One entry of the table: the track's number as the disc numbers it (not
   *  always from 1), the LBA it starts at, and whether it is audio -- a data
   *  track on an enhanced CD is listed so the numbering stays the disc's, and
   *  is not readable as audio. */
  struct Track {
    int number = 0;          ///< The disc's own number for it.
    int32_t startSector = 0; ///< The LBA it starts at.
    bool isAudio = true;     ///< False for a data track.
  };

  int firstTrack = 0;        ///< The first track number the disc uses.
  int lastTrack = 0;         ///< The last.
  std::vector<Track> tracks; ///< In disc order; an index here is 0-based whatever the numbers.
  int32_t leadOutSector = 0; ///< The LBA where the program area ends.

  /** MSF (minutes, seconds, frames of 1/75 s) to LBA. */
  static int32_t msfToSector(int minutes, int seconds, int frames) {
    return (minutes * 60 + seconds) * kSectorsPerSecond + frames - kPregapSectors;
  }

  /** Where track `index` (0-based) starts, in frames from the start of the
   *  first track: the reference's getPositionOfTrackStart. Index ==
   *  tracks.size() is the end of the disc. -1 out of range. */
  int64_t trackStartSample(size_t index) const {
    if (tracks.empty() || index > tracks.size()) return -1;
    const int32_t sector = index == tracks.size() ? leadOutSector : tracks[index].startSector;
    return (int64_t) (sector - tracks.front().startSector) * kSamplesPerSector;
  }

  /** The whole disc's length in stereo frames, from the first track's start
   *  to the lead-out; -1 with no tracks. */
  int64_t lengthInSamples() const { return trackStartSample(tracks.size()); }

  /**
   * The freedb/CDDB disc id: a digit sum of every track's start time in
   * seconds, the disc's length, and the track count, packed in 32 bits. Not
   * unique -- it was designed in 1993 to be typed -- but it is what every
   * CD database still answers to.
   */
  uint32_t cddbId() const {
    if (tracks.empty()) return 0;
    auto digitSum = [](int32_t n) {
      int32_t sum = 0;
      for (; n > 0; n /= 10) sum += n % 10;
      return sum;
    };
    int32_t n = 0;
    for (const Track& t : tracks) n += digitSum((t.startSector + kPregapSectors) / kSectorsPerSecond);
    const int32_t length = (leadOutSector + kPregapSectors) / kSectorsPerSecond -
                           (tracks.front().startSector + kPregapSectors) / kSectorsPerSecond;
    return ((uint32_t) (n % 0xff) << 24) | ((uint32_t) length << 8) | (uint32_t) tracks.size();
  }

  /**
   * Parse a READ TOC response, format 0 with LBA addresses -- what a drive
   * returns to MMC's READ TOC and Windows to IOCTL_CDROM_READ_TOC_EX: a
   * big-endian length, the first and last track numbers, then 8-byte
   * descriptors ending with the lead-out (track 0xAA).
   *
   * False, leaving `out` alone, for anything that is not a coherent disc:
   * track numbers out of 1..99 or out of order, addresses that go backwards
   * or start past the lead-out, a missing lead-out, a length that does not
   * fit. A drive returning a bad TOC is rare; a reader trusting one walks off
   * the end of the disc.
   */
  static bool parse(const uint8_t* bytes, size_t size, CDToc* out) {
    if (!bytes || !out || size < 4) return false;
    const size_t declared = ((size_t) bytes[0] << 8 | bytes[1]) + 2;
    if (declared > size || declared < 4 + 16) return false; // at least one track + lead-out
    const int first = bytes[2], last = bytes[3];
    if (first < 1 || last > 99 || first > last) return false;
    const size_t descriptors = (declared - 4) / 8;
    const size_t expected = (size_t) (last - first + 1) + 1;
    if (descriptors < expected) return false;

    CDToc toc;
    toc.firstTrack = first;
    toc.lastTrack = last;
    bool sawLeadOut = false;
    for (size_t i = 0; i < expected; ++i) {
      const uint8_t* d = bytes + 4 + i * 8;
      const int control = d[1] & 0x0f;
      const int number = d[2];
      const int32_t address = (int32_t) (((uint32_t) d[4] << 24) | ((uint32_t) d[5] << 16) |
                                         ((uint32_t) d[6] << 8) | (uint32_t) d[7]);
      // A CD's addresses are MSF underneath, and 99:59:74 is the last one MSF
      // can say. Past it is not a disc: a lead-out at 0x7FFFFFFF made a
      // ten-terabyte track for readTrack to allocate, and overflowed the
      // CDDB id's arithmetic.
      if (address < 0 || address > msfToSector(99, 59, 74)) return false;
      if (i + 1 == expected) {
        if (number != 0xAA) return false;
        toc.leadOutSector = address;
        sawLeadOut = true;
      } else {
        if (number != first + (int) i) return false;
        if (!toc.tracks.empty() && address <= toc.tracks.back().startSector) return false;
        Track t;
        t.number = number;
        t.startSector = address;
        t.isAudio = (control & 0x04) == 0; // CONTROL bit 2: data track
        toc.tracks.push_back(t);
      }
    }
    if (!sawLeadOut || toc.leadOutSector <= toc.tracks.back().startSector) return false;
    *out = std::move(toc);
    return true;
  }
};

namespace cddetail {

/** Interleaved float to the 16-bit little-endian stereo a CD holds, padded
 *  with silence to a whole sector and at least the Red Book's four seconds.
 *  Mono is duplicated to both channels. */
inline std::vector<uint8_t> toCdAudio(const float* interleaved, size_t numFrames, int channels) {
  const size_t minimumFrames = (size_t) 4 * CDToc::kSectorsPerSecond * CDToc::kSamplesPerSector;
  size_t frames = numFrames < minimumFrames ? minimumFrames : numFrames;
  const size_t perSector = (size_t) CDToc::kSamplesPerSector;
  frames = (frames + perSector - 1) / perSector * perSector;
  std::vector<uint8_t> out(frames * 4, 0);
  auto quantise = [](float v) -> int16_t {
    if (!(v == v)) return 0;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    const float scaled = v * 32767.0f;
    return (int16_t) (scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
  };
  for (size_t i = 0; i < numFrames; ++i) {
    const float l = interleaved[i * (size_t) channels];
    const float r = channels > 1 ? interleaved[i * (size_t) channels + 1] : l;
    const int16_t ql = quantise(l), qr = quantise(r);
    uint8_t* p = out.data() + i * 4;
    p[0] = (uint8_t) ((uint16_t) ql & 0xff);
    p[1] = (uint8_t) ((uint16_t) ql >> 8);
    p[2] = (uint8_t) ((uint16_t) qr & 0xff);
    p[3] = (uint8_t) ((uint16_t) qr >> 8);
  }
  return out;
}

/** CDDA sector bytes to interleaved stereo float. */
inline void fromCdAudio(const uint8_t* bytes, size_t frames, float* interleaved) {
  for (size_t i = 0; i < frames * 2; ++i) {
    const int16_t v = (int16_t) (uint16_t) (bytes[i * 2] | (bytes[i * 2 + 1] << 8));
    interleaved[i] = (float) v / 32768.0f;
  }
}

#if defined(_WIN32)

inline std::string fromBstr(BSTR s) {
  if (!s) return std::string();
  std::wstring w(s, SysStringLen(s));
  return pathio::narrow(w.c_str());
}

/** A BSTR that frees itself. */
class Bstr {
public:
  explicit Bstr(const wchar_t* s = nullptr) : s_(s ? SysAllocString(s) : nullptr) {}
  ~Bstr() { SysFreeString(s_); }
  Bstr(const Bstr&) = delete;
  Bstr& operator=(const Bstr&) = delete;
  BSTR* put() {
    SysFreeString(s_);
    s_ = nullptr;
    return &s_;
  }
  BSTR get() const { return s_; }

private:
  BSTR s_ = nullptr;
};

/** The drive letters Windows reports as CD/DVD devices, as "D:". */
inline std::vector<std::string> opticalDrives() {
  std::vector<std::string> drives;
  const DWORD mask = GetLogicalDrives();
  for (int i = 0; i < 26; ++i) {
    if (!(mask & (1u << i))) continue;
    const wchar_t root[4] = {(wchar_t) (L'A' + i), L':', L'\\', 0};
    if (GetDriveTypeW(root) == DRIVE_CDROM) drives.push_back(std::string(1, (char) ('A' + i)) + ":");
  }
  return drives;
}

inline comdetail::Ref<IDiscRecorder2> openRecorder(const std::wstring& uniqueId) {
  comdetail::Ref<IDiscRecorder2> recorder;
  if (FAILED(CoCreateInstance(__uuidof(MsftDiscRecorder2), nullptr, CLSCTX_ALL,
                              __uuidof(IDiscRecorder2), recorder.putVoid())) ||
      !recorder)
    return comdetail::Ref<IDiscRecorder2>();
  Bstr id(uniqueId.c_str());
  if (FAILED(recorder->InitializeDiscRecorder(id.get()))) return comdetail::Ref<IDiscRecorder2>();
  return recorder;
}

inline comdetail::Ref<IDiscFormat2TrackAtOnce> trackAtOnceFor(IDiscRecorder2* recorder) {
  comdetail::Ref<IDiscFormat2TrackAtOnce> tao;
  if (FAILED(CoCreateInstance(__uuidof(MsftDiscFormat2TrackAtOnce), nullptr, CLSCTX_ALL,
                              __uuidof(IDiscFormat2TrackAtOnce), tao.putVoid())) ||
      !tao)
    return comdetail::Ref<IDiscFormat2TrackAtOnce>();
  if (FAILED(tao->put_Recorder(recorder))) return comdetail::Ref<IDiscFormat2TrackAtOnce>();
  return tao;
}

/** Each element of a SAFEARRAY of VARIANTs, handed to `fn`. */
template <class Fn>
void forEachVariant(SAFEARRAY* array, Fn&& fn) {
  if (!array) return;
  LONG lo = 0, hi = -1;
  if (SUCCEEDED(SafeArrayGetLBound(array, 1, &lo)) && SUCCEEDED(SafeArrayGetUBound(array, 1, &hi))) {
    for (LONG i = lo; i <= hi; ++i) {
      VARIANT v;
      VariantInit(&v);
      if (SUCCEEDED(SafeArrayGetElement(array, &i, &v))) fn(v);
      VariantClear(&v);
    }
  }
  SafeArrayDestroy(array);
}

/**
 * The connection-point sink IMAPI2 reports burning progress to. It only
 * writes numbers into atomics the burning call polls; when the caller has
 * asked to cancel, it calls CancelAddTrack from inside the event, which is
 * where IMAPI2 documents that call as safe.
 */
class TaoProgressSink final : public DDiscFormat2TrackAtOnceEvents {
public:
  TaoProgressSink(std::atomic<double>& overall, std::atomic<bool>& cancel, double totalSectors)
      : overall_(overall), cancel_(cancel), total_(totalSectors > 0.0 ? totalSectors : 1.0) {}

  /** Set by the burning thread before each track: the sectors already
   *  written and this track's own, so an event can say how far the WHOLE
   *  burn has got. */
  std::atomic<double> sectorsBefore{0.0};
  std::atomic<double> trackSectors{0.0};

  // IUnknown. The sink lives on the burning call's stack and outlives the
  // connection, so the count is kept for COM's sake and never deletes.
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDispatch)) ||
        IsEqualIID(riid, __uuidof(DDiscFormat2TrackAtOnceEvents))) {
      *out = static_cast<DDiscFormat2TrackAtOnceEvents*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG) ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override { return (ULONG) --refs_; }

  // IDispatch: events arrive through Invoke, by DISPID.
  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
    if (count) *count = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD, DISPPARAMS* params, VARIANT*,
                                   EXCEPINFO*, UINT*) override {
    if (id != DISPID_DDISCFORMAT2TAOEVENTS_UPDATE || !params || params->cArgs != 2)
      return S_OK;
    // Arguments arrive in reverse: rgvarg[1] is the first, the format object.
    const VARIANT& object = params->rgvarg[1];
    const VARIANT& progress = params->rgvarg[0];
    return Update(object.vt == VT_DISPATCH ? object.pdispVal : nullptr,
                  progress.vt == VT_DISPATCH ? progress.pdispVal : nullptr);
  }

  HRESULT STDMETHODCALLTYPE Update(IDispatch* object, IDispatch* progress) override {
    if (progress) {
      comdetail::Ref<IWriteEngine2EventArgs> args;
      if (SUCCEEDED(progress->QueryInterface(__uuidof(IWriteEngine2EventArgs), args.putVoid())) &&
          args) {
        LONG start = 0, count = 0, written = 0;
        if (SUCCEEDED(args->get_StartLba(&start)) && SUCCEEDED(args->get_SectorCount(&count)) &&
            SUCCEEDED(args->get_LastWrittenLba(&written)) && count > 0) {
          double f = (double) (written - start) / (double) count;
          f = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
          overall_.store((sectorsBefore.load() + f * trackSectors.load()) / total_);
        }
      }
    }
    if (cancel_.load() && object) {
      comdetail::Ref<IDiscFormat2TrackAtOnce> tao;
      if (SUCCEEDED(object->QueryInterface(__uuidof(IDiscFormat2TrackAtOnce), tao.putVoid())) && tao)
        tao->CancelAddTrack();
    }
    return S_OK;
  }

private:
  std::atomic<double>& overall_;
  std::atomic<bool>& cancel_;
  const double total_;
  std::atomic<long> refs_{1};
};

#endif

} // namespace cddetail

/**
 * Reading an audio CD in a drive: its table of contents, and its audio as
 * 44.1 kHz stereo float.
 */
class AudioCDReader {
public:
  static constexpr uint32_t sampleRate = CDToc::kSampleRate; ///< 44100 Hz, every audio CD's.
  static constexpr int numChannels = 2;                       ///< Always stereo.

  /** The machine's CD/DVD drives, by drive letter ("D:"). Empty on any
   *  platform but Windows, and on a machine with no optical drive. */
  static std::vector<std::string> getAvailableCDNames() {
#if defined(_WIN32)
    return cddetail::opticalDrives();
#else
    return {};
#endif
  }

  /** Open the drive at `index` in getAvailableCDNames() and read its TOC.
   *  Null, with `error` saying why, when there is no such drive, no disc, or
   *  a disc with no TOC the drive will give. */
  static std::unique_ptr<AudioCDReader> createReaderForCD(int index, std::string* error = nullptr) {
    auto fail = [&](const char* why) {
      if (error) *error = why;
      return std::unique_ptr<AudioCDReader>();
    };
#if defined(_WIN32)
    const std::vector<std::string> drives = getAvailableCDNames();
    if (index < 0 || (size_t) index >= drives.size())
      return fail(drives.empty() ? "this machine has no CD drive" : "there is no CD drive at that index");
    std::unique_ptr<AudioCDReader> reader(new AudioCDReader());
    const std::wstring device = L"\\\\.\\" + pathio::widen(drives[(size_t) index].c_str());
    reader->handle_ = CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
    if (reader->handle_ == INVALID_HANDLE_VALUE) return fail("the drive could not be opened");
    reader->name_ = drives[(size_t) index];
    if (!reader->refreshTrackLengths()) return fail(reader->error_.c_str());
    if (error) error->clear();
    return reader;
#else
    (void) index;
    return fail("reading audio CDs is implemented on Windows only");
#endif
  }

  /** Closes the drive. */
  ~AudioCDReader() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#endif
  }
  AudioCDReader(const AudioCDReader&) = delete;            ///< Not copyable: it owns the drive's handle.
  AudioCDReader& operator=(const AudioCDReader&) = delete; ///< Not copyable.

  /** The drive, as getAvailableCDNames() lists it ("D:"). */
  const std::string& name() const { return name_; }
  /** The table of contents as last read. */
  const CDToc& toc() const { return toc_; }
  /** Tracks on the disc, data tracks included. */
  int getNumTracks() const { return (int) toc_.tracks.size(); }
  /** Where track `track` (0-based) starts, in stereo frames from the first
   *  track; getNumTracks() gives the end of the disc, and anything else -1. */
  int64_t getPositionOfTrackStart(int track) const {
    return track < 0 ? -1 : toc_.trackStartSample((size_t) track);
  }
  /** Every track's start in samples, then the end of the disc. */
  std::vector<int64_t> getTrackOffsets() const {
    std::vector<int64_t> offsets;
    for (size_t i = 0; i <= toc_.tracks.size(); ++i) offsets.push_back(toc_.trackStartSample(i));
    return offsets;
  }
  /** True for an audio track; false for a data track or an index out of
   *  range. */
  bool isTrackAudio(int track) const {
    return track >= 0 && (size_t) track < toc_.tracks.size() && toc_.tracks[(size_t) track].isAudio;
  }
  /** The disc's length in stereo frames. */
  int64_t lengthInSamples() const { return toc_.lengthInSamples(); }
  /** The freedb/CDDB disc id: CDToc::cddbId(). */
  uint32_t getCDDBId() const { return toc_.cddbId(); }
  /** Why the last call that failed failed. */
  const std::string& lastError() const { return error_; }

  /** A disc is still in the drive (and it has not been swapped for another
   *  since the TOC was read -- the drive answers "changed" then too). */
  bool isCDStillPresent() const {
#if defined(_WIN32)
    DWORD bytes = 0;
    return DeviceIoControl(handle_, IOCTL_STORAGE_CHECK_VERIFY, nullptr, 0, nullptr, 0, &bytes,
                           nullptr) != FALSE;
#else
    return false;
#endif
  }

  /** Read the TOC again -- after a disc change. */
  bool refreshTrackLengths() {
#if defined(_WIN32)
    CDROM_READ_TOC_EX request;
    std::memset(&request, 0, sizeof(request));
    request.Format = CDROM_READ_TOC_EX_FORMAT_TOC;
    request.Msf = 0; // LBA addresses
    request.SessionTrack = 1;
    CDROM_TOC raw;
    std::memset(&raw, 0, sizeof(raw));
    DWORD got = 0;
    if (!DeviceIoControl(handle_, IOCTL_CDROM_READ_TOC_EX, &request, sizeof(request), &raw,
                         sizeof(raw), &got, nullptr)) {
      error_ = "the drive returned no table of contents (is there a disc in it?)";
      return false;
    }
    CDToc parsed;
    if (!CDToc::parse(reinterpret_cast<const uint8_t*>(&raw), got, &parsed)) {
      error_ = "the drive returned a table of contents that does not describe a disc";
      return false;
    }
    toc_ = std::move(parsed);
    error_.clear();
    return true;
#else
    error_ = "reading audio CDs is implemented on Windows only";
    return false;
#endif
  }

  /**
   * Read `numFrames` stereo frames starting `startSample` frames into the
   * disc (0 = the start of the first track) into `interleaved` (2 floats a
   * frame). The range must lie inside the disc's audio.
   */
  bool readSamples(int64_t startSample, size_t numFrames, float* interleaved) {
    if (!interleaved || startSample < 0 || toc_.tracks.empty()) return failWith("nothing to read");
    if (startSample + (int64_t) numFrames > lengthInSamples())
      return failWith("the range runs past the end of the disc");
#if defined(_WIN32)
    const int64_t perSector = CDToc::kSamplesPerSector;
    int64_t sector = toc_.tracks.front().startSector + startSample / perSector;
    size_t skip = (size_t) (startSample % perSector);
    size_t done = 0;
    // 20 sectors a request: under the 64 KiB a drive transfers in one command
    // on every controller Windows has shipped a driver for.
    const DWORD chunk = 20;
    std::vector<uint8_t> buffer((size_t) chunk * CDToc::kBytesPerSector);
    while (done < numFrames) {
      const size_t wantFrames = skip + (numFrames - done);
      DWORD sectors = (DWORD) ((wantFrames + (size_t) perSector - 1) / (size_t) perSector);
      if (sectors > chunk) sectors = chunk;
      if (!readSectors(sector, sectors, buffer.data())) return false;
      const size_t available = (size_t) sectors * (size_t) perSector - skip;
      const size_t take = available < numFrames - done ? available : numFrames - done;
      cddetail::fromCdAudio(buffer.data() + skip * 4, take, interleaved + done * 2);
      done += take;
      skip = 0;
      sector += sectors;
    }
    return true;
#else
    (void) numFrames;
    return failWith("reading audio CDs is implemented on Windows only");
#endif
  }

  /** One whole track as a 44.1 kHz stereo WavData. */
  bool readTrack(int track, WavData* out) {
    if (!out || !isTrackAudio(track)) return failWith("there is no audio track at that index");
    const int64_t start = getPositionOfTrackStart(track);
    const int64_t end = getPositionOfTrackStart(track + 1);
    if (start < 0 || end <= start) return failWith("the track has no length");
    // The ceiling every reader here keeps (audiofile.h): a 99-minute track is
    // 529M samples, and a table of contents is the disc's word for its length.
    if ((uint64_t) (end - start) * 2u > (uint64_t) kMaxDecodedSamples)
      return failWith("the track is longer than one decoded file may be (kMaxDecodedSamples)");
    out->sampleRate = sampleRate;
    out->numChannels = (uint16_t) numChannels;
    out->samples.assign((size_t) (end - start) * 2, 0.0f);
    if (!readSamples(start, (size_t) (end - start), out->samples.data())) {
      out->samples.clear();
      return false;
    }
    return true;
  }

  /** Ask the drive to eject the disc. Nothing is reported if it will not.
   *  Does nothing off Windows. */
  void ejectDisk() {
#if defined(_WIN32)
    DWORD bytes = 0;
    DeviceIoControl(handle_, IOCTL_STORAGE_EJECT_MEDIA, nullptr, 0, nullptr, 0, &bytes, nullptr);
#endif
  }

private:
  AudioCDReader() = default;

  bool failWith(const char* why) {
    error_ = why;
    return false;
  }

#if defined(_WIN32)
  /** Raw CDDA sectors. A drive reports a scratch as a failed read; each
   *  request is retried, then split into single sectors so one bad sector
   *  does not take its nineteen neighbours with it. */
  bool readSectors(int64_t sector, DWORD count, uint8_t* dest) {
    auto attempt = [&](int64_t at, DWORD n, uint8_t* to) {
      RAW_READ_INFO info;
      std::memset(&info, 0, sizeof(info));
      // The offset is in 2048-byte units whatever the sector really holds:
      // the driver divides by 2048 to recover the sector number.
      info.DiskOffset.QuadPart = at * 2048;
      info.SectorCount = n;
      info.TrackMode = CDDA;
      DWORD got = 0;
      return DeviceIoControl(handle_, IOCTL_CDROM_RAW_READ, &info, sizeof(info), to,
                             n * (DWORD) CDToc::kBytesPerSector, &got, nullptr) != FALSE &&
             got == n * (DWORD) CDToc::kBytesPerSector;
    };
    for (int tries = 0; tries < 3; ++tries)
      if (attempt(sector, count, dest)) return true;
    for (DWORD i = 0; i < count; ++i) {
      bool ok = false;
      for (int tries = 0; tries < 3 && !ok; ++tries)
        ok = attempt(sector + i, 1, dest + (size_t) i * CDToc::kBytesPerSector);
      if (!ok) return failWith("a sector could not be read (a scratch, or the disc was removed)");
    }
    return true;
  }

  HANDLE handle_ = INVALID_HANDLE_VALUE;
#endif
  std::string name_;
  std::string error_;
  CDToc toc_;
};

/**
 * Burning an audio CD: add tracks, then burn() writes them track-at-once and
 * closes the disc.
 */
class AudioCDBurner {
public:
  /** What is in the recorder. */
  enum class DiskState {
    unknown,             ///< The recorder could not be asked, or did not say.
    noDisc,              ///< Empty.
    writableDiskPresent, ///< A blank disc it can write.
    readOnlyDiskPresent  ///< A disc it cannot write: written already, or the wrong kind.
  };

  /** Called on burn()'s calling thread, with 0..1. Return true to cancel. */
  using ProgressCallback = std::function<bool(float proportionComplete)>;

  /** The machine's recorders, as "vendor product (D:)". Empty on any
   *  platform but Windows and on a machine with no recorder. */
  static std::vector<std::string> findAvailableDevices() {
    std::vector<std::string> names;
#if defined(_WIN32)
    for (const Device& d : devices()) names.push_back(d.name);
#endif
    return names;
  }

  /** The recorder at `index` in findAvailableDevices(). Null, with `error`
   *  saying why, when there is none there, or off Windows. Nothing is opened
   *  yet: each call reaches the drive itself. */
  static std::unique_ptr<AudioCDBurner> openDevice(int index, std::string* error = nullptr) {
    auto fail = [&](const char* why) {
      if (error) *error = why;
      return std::unique_ptr<AudioCDBurner>();
    };
#if defined(_WIN32)
    const std::vector<Device> all = devices();
    if (index < 0 || (size_t) index >= all.size())
      return fail(all.empty() ? "this machine has no CD recorder" : "there is no recorder at that index");
    std::unique_ptr<AudioCDBurner> burner(new AudioCDBurner());
    burner->uniqueId_ = all[(size_t) index].uniqueId;
    burner->name_ = all[(size_t) index].name;
    if (error) error->clear();
    return burner;
#else
    (void) index;
    return fail("burning audio CDs is implemented on Windows only");
#endif
  }

  AudioCDBurner(const AudioCDBurner&) = delete;            ///< Not copyable.
  AudioCDBurner& operator=(const AudioCDBurner&) = delete; ///< Not copyable.

  /** The recorder, as findAvailableDevices() lists it. */
  const std::string& name() const { return name_; }
  /** Why the last call that failed failed. */
  const std::string& lastError() const { return error_; }

  /** Ask the recorder what disc it holds. Blocks while the drive answers. */
  DiskState getDiskState() const {
#if defined(_WIN32)
    comdetail::Apartment com;
    if (!com.usable()) return DiskState::unknown;
    comdetail::Ref<IDiscRecorder2> recorder = cddetail::openRecorder(uniqueId_);
    if (!recorder) return DiskState::unknown;
    comdetail::Ref<IDiscFormat2TrackAtOnce> tao = cddetail::trackAtOnceFor(recorder.get());
    if (!tao) return DiskState::unknown;
    IMAPI_MEDIA_PHYSICAL_TYPE type = IMAPI_MEDIA_TYPE_UNKNOWN;
    const HRESULT hr = tao->get_CurrentPhysicalMediaType(&type);
    if (hr == E_IMAPI_RECORDER_MEDIA_NO_MEDIA) return DiskState::noDisc;
    if (FAILED(hr)) return DiskState::unknown;
    VARIANT_BOOL supported = VARIANT_FALSE, blank = VARIANT_FALSE;
    if (SUCCEEDED(tao->IsCurrentMediaSupported(recorder.get(), &supported)) &&
        supported != VARIANT_FALSE && SUCCEEDED(tao->get_MediaHeuristicallyBlank(&blank)) &&
        blank != VARIANT_FALSE)
      return DiskState::writableDiskPresent;
    return DiskState::readOnlyDiskPresent;
#else
    return DiskState::unknown;
#endif
  }

  /** True for any disc, writable or not. */
  bool isDiskPresent() const {
    const DiskState s = getDiskState();
    return s == DiskState::writableDiskPresent || s == DiskState::readOnlyDiskPresent;
  }

  /** Eject the tray. False, with lastError() saying so, when the drive will
   *  not, and always off Windows. */
  bool openTray() {
#if defined(_WIN32)
    comdetail::Apartment com;
    comdetail::Ref<IDiscRecorder2> recorder;
    if (com.usable()) recorder = cddetail::openRecorder(uniqueId_);
    if (!recorder || FAILED(recorder->EjectMedia())) return failWith("the tray could not be opened");
    return true;
#else
    return failWith("burning audio CDs is implemented on Windows only");
#endif
  }

  /** Write speeds the drive offers for the disc in it, as multiples of
   *  single speed (75 sectors a second). Empty without a writable disc. */
  std::vector<int> getAvailableWriteSpeeds() const {
    std::vector<int> speeds;
#if defined(_WIN32)
    comdetail::Apartment com;
    if (!com.usable()) return speeds;
    comdetail::Ref<IDiscRecorder2> recorder = cddetail::openRecorder(uniqueId_);
    comdetail::Ref<IDiscFormat2TrackAtOnce> tao;
    if (recorder) tao = cddetail::trackAtOnceFor(recorder.get());
    SAFEARRAY* array = nullptr;
    if (tao && SUCCEEDED(tao->get_SupportedWriteSpeeds(&array)))
      cddetail::forEachVariant(array, [&](const VARIANT& v) {
        LONG sectorsPerSecond = 0;
        if (v.vt == VT_I4) sectorsPerSecond = v.lVal;
        else if (v.vt == VT_UI4) sectorsPerSecond = (LONG) v.ulVal;
        if (sectorsPerSecond >= CDToc::kSectorsPerSecond)
          speeds.push_back((int) (sectorsPerSecond / CDToc::kSectorsPerSecond));
      });
#endif
    return speeds;
  }

  /** Buffer-underrun protection (on by default): the drive pauses rather
   *  than ruin the disc when data does not arrive in time. */
  void setBufferUnderrunProtection(bool shouldBeEnabled) { underrunProtection_ = shouldBeEnabled; }

  /** Free sectors on the disc in the drive -- each holds 588 stereo frames.
   *  -1 when there is no writable disc to ask. */
  int getNumAvailableAudioBlocks() const {
#if defined(_WIN32)
    comdetail::Apartment com;
    if (!com.usable()) return -1;
    comdetail::Ref<IDiscRecorder2> recorder = cddetail::openRecorder(uniqueId_);
    comdetail::Ref<IDiscFormat2TrackAtOnce> tao;
    if (recorder) tao = cddetail::trackAtOnceFor(recorder.get());
    LONG freeSectors = 0;
    if (!tao || FAILED(tao->get_FreeSectorsOnMedia(&freeSectors))) return -1;
    return (int) freeSectors;
#else
    return -1;
#endif
  }

  /** Queue a track: 44.1 kHz audio, mono or stereo, interleaved. Converted
   *  to the disc's 16-bit stereo now, so the caller's buffer is free again
   *  when this returns. */
  bool addAudioTrack(const float* interleaved, size_t numFrames, int channels = 2) {
    if (!interleaved || numFrames == 0) return failWith("an empty track");
    if (channels < 1 || channels > 2) return failWith("an audio CD carries mono or stereo only");
    if (tracks_.size() >= 99) return failWith("an audio CD holds at most 99 tracks");
    tracks_.push_back(cddetail::toCdAudio(interleaved, numFrames, channels));
    return true;
  }

  /** A decoded file as a track. It must already be at 44.1 kHz: resampling
   *  is a quality decision this does not make for the caller. */
  bool addAudioTrack(const WavData& audio) {
    if (audio.sampleRate != CDToc::kSampleRate)
      return failWith("audio for a CD must be at 44.1 kHz");
    return addAudioTrack(audio.samples.data(), audio.numFrames(), audio.numChannels);
  }

  /** Tracks queued for the next burn(). */
  size_t getNumTracks() const { return tracks_.size(); }
  /** Forget the queued tracks. */
  void clearTracks() { tracks_.clear(); }

  /** Sectors the queued tracks will occupy, before the gaps the drive adds. */
  int64_t sectorsNeeded() const {
    int64_t total = 0;
    for (const auto& t : tracks_) total += (int64_t) (t.size() / CDToc::kBytesPerSector);
    return total;
  }

  /**
   * Burn the queued tracks and close the disc. Blocks until done. Returns an
   * empty string on success, otherwise what went wrong -- as the reference
   * does. `writeSpeed` is a multiple of single speed; 0 lets the drive choose.
   */
  std::string burn(const ProgressCallback& progress, bool ejectDiscAfterwards, int writeSpeed = 0) {
    if (tracks_.empty()) return error_ = "there are no tracks to burn";
#if defined(_WIN32)
    std::atomic<bool> finished{false};
    std::atomic<double> overall{0.0};
    std::atomic<bool> cancel{false};
    std::string result;
    std::thread worker([&] {
      result = burnInMultithreadedApartment(overall, cancel, ejectDiscAfterwards, writeSpeed);
      finished.store(true);
    });
    while (!finished.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (progress && !cancel.load() && progress((float) overall.load())) cancel.store(true);
    }
    worker.join();
    if (result.empty() && progress) progress(1.0f);
    error_ = result;
    return result;
#else
    (void) progress;
    (void) ejectDiscAfterwards;
    (void) writeSpeed;
    return error_ = "burning audio CDs is implemented on Windows only";
#endif
  }

private:
  AudioCDBurner() = default;

  bool failWith(const char* why) {
    error_ = why;
    return false;
  }

#if defined(_WIN32)
  /** One recorder IMAPI2 knows: its unique id, which is what a burn opens it
   *  by, and the "vendor product (D:)" name findAvailableDevices lists. An
   *  index into devices() is only good until the next enumeration. */
  struct Device {
    std::wstring uniqueId; ///< IMAPI2's id for it.
    std::string name;      ///< "vendor product (D:)".
  };

  static std::vector<Device> devices() {
    std::vector<Device> found;
    comdetail::Apartment com;
    if (!com.usable()) return found;
    comdetail::Ref<IDiscMaster2> master;
    if (FAILED(CoCreateInstance(__uuidof(MsftDiscMaster2), nullptr, CLSCTX_ALL,
                                __uuidof(IDiscMaster2), master.putVoid())) ||
        !master)
      return found;
    VARIANT_BOOL supported = VARIANT_FALSE;
    if (FAILED(master->get_IsSupportedEnvironment(&supported)) || supported == VARIANT_FALSE)
      return found;
    LONG count = 0;
    if (FAILED(master->get_Count(&count))) return found;
    for (LONG i = 0; i < count; ++i) {
      cddetail::Bstr id;
      if (FAILED(master->get_Item(i, id.put())) || !id.get()) continue;
      Device d;
      d.uniqueId.assign(id.get(), SysStringLen(id.get()));
      comdetail::Ref<IDiscRecorder2> recorder = cddetail::openRecorder(d.uniqueId);
      if (!recorder) continue;
      cddetail::Bstr vendor, product;
      recorder->get_VendorId(vendor.put());
      recorder->get_ProductId(product.put());
      d.name = cddetail::fromBstr(vendor.get()) + " " + cddetail::fromBstr(product.get());
      SAFEARRAY* paths = nullptr;
      std::string letters;
      if (SUCCEEDED(recorder->get_VolumePathNames(&paths)))
        cddetail::forEachVariant(paths, [&](const VARIANT& v) {
          if (v.vt == VT_BSTR && v.bstrVal) {
            std::string p = cddetail::fromBstr(v.bstrVal);
            while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
            letters += (letters.empty() ? "" : ", ") + p;
          }
        });
      if (!letters.empty()) d.name += " (" + letters + ")";
      found.push_back(std::move(d));
    }
    return found;
  }

  /** IStream over one track's bytes. SetSize gives the stream its exact
   *  length: an HGLOBAL handed in directly reports GlobalSize, which the
   *  allocator may round up -- and IMAPI2 refuses a stream that is not a
   *  whole number of sectors. */
  static comdetail::Ref<IStream> streamFor(const std::vector<uint8_t>& bytes) {
    comdetail::Ref<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put())) || !stream)
      return comdetail::Ref<IStream>();
    ULARGE_INTEGER size;
    size.QuadPart = bytes.size();
    ULONG written = 0;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (FAILED(stream->SetSize(size)) ||
        FAILED(stream->Write(bytes.data(), (ULONG) bytes.size(), &written)) ||
        written != bytes.size() || FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)))
      return comdetail::Ref<IStream>();
    return stream;
  }

  std::string burnInMultithreadedApartment(std::atomic<double>& overall, std::atomic<bool>& cancel,
                                           bool eject, int writeSpeed) {
    // A thread of our own, so the apartment is always ours to choose.
    comdetail::Apartment com;
    if (!com.usable()) return "COM could not be initialised";
    comdetail::Ref<IDiscRecorder2> recorder = cddetail::openRecorder(uniqueId_);
    if (!recorder) return "the recorder could not be opened";
    comdetail::Ref<IDiscFormat2TrackAtOnce> tao = cddetail::trackAtOnceFor(recorder.get());
    if (!tao) return "the recorder does not support track-at-once audio";
    VARIANT_BOOL supported = VARIANT_FALSE;
    if (FAILED(tao->IsCurrentMediaSupported(recorder.get(), &supported)) ||
        supported == VARIANT_FALSE)
      return "the disc in the drive cannot be written as an audio CD";
    cddetail::Bstr client(L"Sonore");
    tao->put_ClientName(client.get());
    tao->put_BufferUnderrunFreeDisabled(underrunProtection_ ? VARIANT_FALSE : VARIANT_TRUE);
    if (writeSpeed > 0) tao->SetWriteSpeed((LONG) writeSpeed * CDToc::kSectorsPerSecond, VARIANT_FALSE);

    if (FAILED(tao->PrepareMedia())) return "the disc could not be prepared for writing";
    LONG freeSectors = 0;
    if (SUCCEEDED(tao->get_FreeSectorsOnMedia(&freeSectors)) &&
        (int64_t) freeSectors < sectorsNeeded()) {
      tao->ReleaseMedia();
      return "the tracks do not fit on the disc";
    }

    cddetail::TaoProgressSink sink(overall, cancel, (double) sectorsNeeded());
    comdetail::Ref<IConnectionPointContainer> container;
    comdetail::Ref<IConnectionPoint> point;
    DWORD cookie = 0;
    if (SUCCEEDED(tao->QueryInterface(__uuidof(IConnectionPointContainer), container.putVoid())) &&
        container &&
        SUCCEEDED(container->FindConnectionPoint(__uuidof(DDiscFormat2TrackAtOnceEvents),
                                                 point.put())) &&
        point)
      point->Advise(static_cast<DDiscFormat2TrackAtOnceEvents*>(&sink), &cookie);

    std::string result;
    const double total = (double) sectorsNeeded();
    double before = 0.0;
    for (const std::vector<uint8_t>& track : tracks_) {
      if (cancel.load()) {
        result = "the burn was cancelled";
        break;
      }
      comdetail::Ref<IStream> stream = streamFor(track);
      if (!stream) {
        result = "a track could not be staged for writing";
        break;
      }
      const double sectors = (double) (track.size() / CDToc::kBytesPerSector);
      sink.sectorsBefore.store(before);
      sink.trackSectors.store(sectors);
      // Blocks for the whole track; the sink keeps `overall` current from
      // IMAPI's thread meanwhile, and burn()'s loop reports it.
      const HRESULT hr = tao->AddAudioTrack(stream.get());
      if (cancel.load() || hr == E_IMAPI_REQUEST_CANCELLED) {
        result = "the burn was cancelled";
        break;
      }
      if (FAILED(hr)) {
        result = "the drive failed while writing a track";
        break;
      }
      before += sectors;
      overall.store(before / total);
    }
    if (point) point->Unadvise(cookie);
    // Closes the session, so the disc plays in any CD player -- also after a
    // failure or a cancel, which leaves the tracks already written readable.
    if (FAILED(tao->ReleaseMedia()) && result.empty()) result = "the disc could not be closed";
    if (eject) recorder->EjectMedia();
    return result;
  }

  std::wstring uniqueId_;
#endif
  std::string name_;
  std::string error_;
  std::vector<std::vector<uint8_t>> tracks_;
  bool underrunProtection_ = true;
};

} // namespace sonore
