// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the operating system's own audio decoders.
//
// WindowsMediaAudioFormat decodes through Media Foundation (a source reader
// asked for float PCM: MP3, AAC in M4A/MP4 or ADTS, WMA, ALAC, AC-3 -- whatever
// decoders the machine has installed). CoreAudioFormat does the same on macOS
// through Audio Toolbox's ExtAudioFile. Both land in the WavData every other
// reader here fills.
//
// ── Why this is opt-in ──────────────────────────────────────────────────────
//
// The decoders compiled into audiofile.h open a file identically on every
// machine, and that is a property worth keeping as the DEFAULT: a format that
// opens on the user's Mac and silently fails on their collaborator's PC is a
// project that loads with an empty sampler and nothing saying why. So
// readAudioFile() still answers from its fixed set, and these decoders sit
// behind it only once an application asks -- enableSystemAudioFormats() --
// for the formats it has decided are worth that trade (an M4A a user dragged
// out of their music library is the usual one).
//
// Even then the built-in decoders go FIRST: a WAV, FLAC or MP3 opens through
// the SDK's own code whatever the OS has, and the OS only sees what they could
// not open -- a container they do not know, or a codec inside one they refuse.
//
// ── Why Media Foundation is loaded at runtime ───────────────────────────────
//
// The N and KN editions of Windows ship without Media Foundation until the
// user installs the Media Feature Pack. A plugin LINKED against mfplat.dll
// does not fail to decode there -- it fails to LOAD, and the host reports a
// broken plugin with no reason. LoadLibrary at the first decode, from
// System32 only, turns that into isAvailable() == false. The handful of GUIDs
// needed are spelled out here for the same reason (they would otherwise come
// from mfuuid.lib); audio_io_test checks each against the SDK's own.
//
// ── Paths, not URLs ─────────────────────────────────────────────────────────
//
// MFCreateSourceReaderFromURL takes http:// as happily as a path, and a
// plugin that opens a socket because a string in a project file said so is
// doing something nobody asked for. The file is opened as a byte stream
// (MFCreateFile), which only knows the file system.
//
// Offline-only, like every reader here: these allocate, block on disk and run
// a decoder, so they belong on a loader thread, never in process().
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "audiofile.h"
#include "path_io.h"
#include "wav.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include "win_com.h"
#elif defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace sonore {

namespace sysaudiodetail {

inline bool fail(WavData* out, std::string* error, const char* why) {
  if (out) {
    out->samples.clear();
    out->sampleRate = 0;
    out->numChannels = 0;
  }
  if (error) *error = why;
  return false;
}

/** Grow `dst` by `count` samples, or refuse past the ceiling every decoder
 *  here is held to: a system decoder expands a small file into gigabytes as
 *  readily as ours would. */
inline bool roomFor(const std::vector<float>& dst, size_t count, size_t maxSamples) {
  return count <= maxSamples && dst.size() <= maxSamples - count;
}

#if defined(_WIN32)

// The GUIDs, as mfapi.h spells them. `inline constexpr` so there is ONE
// object of each in the program: a reference is passed to SetGUID, and a
// per-TU copy reached through an inline function would be an ODR violation.
inline constexpr GUID kMtMajorType = {
    0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
inline constexpr GUID kMtSubtype = {
    0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
inline constexpr GUID kMediaTypeAudio = {
    0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kAudioFormatPcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kAudioFormatFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kMtAudioNumChannels = {
    0x37e48bf5, 0x645e, 0x4c5b, {0x89, 0xde, 0xad, 0xa9, 0xe2, 0x9b, 0x69, 0x6a}};
inline constexpr GUID kMtAudioSamplesPerSecond = {
    0x5faeeae7, 0x0290, 0x4c31, {0x9e, 0x8a, 0xc5, 0x34, 0xf6, 0x8d, 0x9d, 0xba}};
inline constexpr GUID kMtAudioBitsPerSample = {
    0xf2deb57f, 0x40fa, 0x4764, {0xaa, 0x33, 0xed, 0x4f, 0x2d, 0x1f, 0xf6, 0x69}};

/** MF_VERSION as Windows 7 and later define it. Spelled out because mfapi.h
 *  derives it from _WIN32_WINNT, and a project that pins an older target
 *  would otherwise ask for the Vista API and be refused. */
constexpr ULONG kMfVersion = (0x0002u << 16) | 0x0070u;

/** The five entry points, resolved once. Never freed: a system DLL loaded
 *  into a host process stays there, and unloading it under another plugin
 *  that also uses it is how hosts crash at exit. */
struct MfApi {
  using StartupFn = HRESULT(WINAPI*)(ULONG, DWORD);
  using ShutdownFn = HRESULT(WINAPI*)();
  using MediaTypeFn = HRESULT(WINAPI*)(IMFMediaType**);
  using FileStreamFn = HRESULT(WINAPI*)(MF_FILE_ACCESSMODE, MF_FILE_OPENMODE, MF_FILE_FLAGS,
                                        LPCWSTR, IMFByteStream**);
  using ReaderFn = HRESULT(WINAPI*)(IMFByteStream*, IMFAttributes*, IMFSourceReader**);

  StartupFn startup = nullptr;
  ShutdownFn shutdown = nullptr;
  MediaTypeFn createMediaType = nullptr;
  FileStreamFn createFileStream = nullptr;
  ReaderFn createReader = nullptr;

  bool loaded() const {
    return startup && shutdown && createMediaType && createFileStream && createReader;
  }
};

inline const MfApi& mfApi() {
  static const MfApi api = [] {
    MfApi a;
    // System32 only: a bare name would also search the host's own folder,
    // and a planted mfplat.dll beside a DAW would run inside it.
    HMODULE plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE rw = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!plat || !rw) return a;
    a.startup = (MfApi::StartupFn) (void*) GetProcAddress(plat, "MFStartup");
    a.shutdown = (MfApi::ShutdownFn) (void*) GetProcAddress(plat, "MFShutdown");
    a.createMediaType = (MfApi::MediaTypeFn) (void*) GetProcAddress(plat, "MFCreateMediaType");
    a.createFileStream = (MfApi::FileStreamFn) (void*) GetProcAddress(plat, "MFCreateFile");
    a.createReader =
        (MfApi::ReaderFn) (void*) GetProcAddress(rw, "MFCreateSourceReaderFromByteStream");
    return a;
  }();
  return api;
}

/** MFStartup and MFShutdown are reference counted, so a scope per decode is
 *  correct even while another thread -- or another plugin -- is mid-decode. */
class MfSession {
public:
  explicit MfSession(const MfApi& api) : api_(api) {
    started_ = SUCCEEDED(api_.startup(kMfVersion, MFSTARTUP_NOSOCKET));
  }
  ~MfSession() {
    if (started_) api_.shutdown();
  }
  MfSession(const MfSession&) = delete;
  MfSession& operator=(const MfSession&) = delete;
  bool started() const { return started_; }

private:
  const MfApi& api_;
  bool started_ = false;
};

struct MfFormat {
  bool isFloat = false;
  UINT32 bits = 0;
  UINT32 channels = 0;
  UINT32 rate = 0;
};

inline bool currentFormat(IMFSourceReader* reader, MfFormat* fmt) {
  comdetail::Ref<IMFMediaType> type;
  if (FAILED(reader->GetCurrentMediaType((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                         type.put())) ||
      !type)
    return false;
  GUID subtype{};
  if (FAILED(type->GetGUID(kMtSubtype, &subtype))) return false;
  MfFormat f;
  f.isFloat = IsEqualGUID(subtype, kAudioFormatFloat) != 0;
  if (!f.isFloat && !IsEqualGUID(subtype, kAudioFormatPcm)) return false;
  if (FAILED(type->GetUINT32(kMtAudioNumChannels, &f.channels)) ||
      FAILED(type->GetUINT32(kMtAudioSamplesPerSecond, &f.rate)) ||
      FAILED(type->GetUINT32(kMtAudioBitsPerSample, &f.bits)))
    return false;
  if (f.channels == 0 || f.channels > 32 || f.rate == 0) return false;
  if (f.isFloat ? f.bits != 32 : (f.bits != 8 && f.bits != 16 && f.bits != 24 && f.bits != 32))
    return false;
  *fmt = f;
  return true;
}

/** One decoded buffer onto the end of `dst`, as floats. Only whole frames:
 *  a decoder that hands over a ragged tail would otherwise shift every
 *  channel after it by one. */
inline bool appendDecoded(const BYTE* data, DWORD bytes, const MfFormat& fmt,
                          std::vector<float>& dst, size_t maxSamples) {
  const size_t width = fmt.bits / 8u;
  const size_t frameBytes = width * fmt.channels;
  const size_t count = (bytes / frameBytes) * fmt.channels;
  if (!roomFor(dst, count, maxSamples)) return false;
  const size_t at = dst.size();
  dst.resize(at + count);
  float* to = dst.data() + at;
  const BYTE* p = data;
  for (size_t i = 0; i < count; ++i, p += width) {
    if (fmt.isFloat) {
      float v;
      std::memcpy(&v, p, sizeof(v));
      to[i] = v;
    } else if (fmt.bits == 8) {
      to[i] = ((float) p[0] - 128.0f) / 128.0f; // 8-bit WAV is unsigned
    } else if (fmt.bits == 16) {
      to[i] = (float) (int16_t) (uint16_t) (p[0] | (p[1] << 8)) / 32768.0f;
    } else if (fmt.bits == 24) {
      const int32_t v = (int32_t) (((uint32_t) p[0] << 8) | ((uint32_t) p[1] << 16) |
                                   ((uint32_t) p[2] << 24)) >> 8;
      to[i] = (float) v / 8388608.0f;
    } else {
      int32_t v;
      std::memcpy(&v, p, sizeof(v));
      to[i] = (float) ((double) v / 2147483648.0);
    }
  }
  return true;
}

inline bool readMediaFoundation(const char* path, WavData* out, std::string* error,
                                size_t maxSamples) {
  if (!path || !out) return fail(out, error, "no path or no destination");
  fail(out, nullptr, "");
  const MfApi& api = mfApi();
  if (!api.loaded())
    return fail(out, error,
                "Media Foundation is not on this machine (an N edition of Windows without "
                "the Media Feature Pack?)");
  // Declared in this order so they are destroyed in the reverse one: every
  // interface released, then MFShutdown, then the apartment.
  comdetail::Apartment com;
  if (!com.usable()) return fail(out, error, "COM could not be initialised on this thread");
  MfSession session(api);
  if (!session.started()) return fail(out, error, "MFStartup failed");

  const std::wstring wide = pathio::widenPath(path);
  comdetail::Ref<IMFByteStream> stream;
  if (FAILED(api.createFileStream(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST,
                                  MF_FILEFLAGS_NONE, wide.c_str(), stream.put())) ||
      !stream)
    return fail(out, error, "the file could not be opened");
  comdetail::Ref<IMFSourceReader> reader;
  if (FAILED(api.createReader(stream.get(), nullptr, reader.put())) || !reader)
    return fail(out, error, "no installed media source understands this file");

  reader->SetStreamSelection((DWORD) MF_SOURCE_READER_ALL_STREAMS, FALSE);
  if (FAILED(reader->SetStreamSelection((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE)))
    return fail(out, error, "the file has no audio stream");

  // Float first, which every decoder Windows ships can produce. A partial
  // type -- major type and subtype only -- leaves the channel count and rate
  // to the stream, which is the point: this reads the file as it is.
  comdetail::Ref<IMFMediaType> want;
  if (FAILED(api.createMediaType(want.put())) || !want)
    return fail(out, error, "MFCreateMediaType failed");
  want->SetGUID(kMtMajorType, kMediaTypeAudio);
  want->SetGUID(kMtSubtype, kAudioFormatFloat);
  if (FAILED(reader->SetCurrentMediaType((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr,
                                         want.get()))) {
    // Uncompressed sources offer only what the file holds and a reader with
    // no converter in the chain cannot turn 16-bit into float, so ask for
    // integer PCM and convert here.
    want->SetGUID(kMtSubtype, kAudioFormatPcm);
    if (FAILED(reader->SetCurrentMediaType((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr,
                                           want.get())))
      return fail(out, error, "no installed decoder handles this stream");
  }
  MfFormat fmt;
  if (!currentFormat(reader.get(), &fmt))
    return fail(out, error, "the decoder offered a sample format this reader does not take");

  std::vector<float>& dst = out->samples;
  for (;;) {
    DWORD streamIndex = 0, flags = 0;
    LONGLONG time = 0;
    comdetail::Ref<IMFSample> sample;
    if (FAILED(reader->ReadSample((DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &streamIndex,
                                  &flags, &time, sample.put())))
      return fail(out, error, "the decoder failed partway through the file");
    if (flags & MF_SOURCE_READERF_ERROR) return fail(out, error, "the stream reported an error");
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      // A stream that changes shape mid-file would silently corrupt the
      // interleaving -- the same refusal readMp3 makes.
      MfFormat now;
      if (!currentFormat(reader.get(), &now) || now.channels != fmt.channels ||
          now.rate != fmt.rate)
        return fail(out, error, "the stream changed channel count or rate partway through");
      fmt = now;
    }
    if (sample) {
      comdetail::Ref<IMFMediaBuffer> buffer;
      if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())) || !buffer)
        return fail(out, error, "a decoded buffer could not be read");
      BYTE* data = nullptr;
      DWORD length = 0;
      if (FAILED(buffer->Lock(&data, nullptr, &length)))
        return fail(out, error, "a decoded buffer could not be locked");
      const bool fits = appendDecoded(data, length, fmt, dst, maxSamples);
      buffer->Unlock();
      if (!fits) return fail(out, error, "the decoded audio is past the size ceiling");
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
  }
  if (dst.empty()) return fail(out, error, "the file decoded to no audio");
  out->numChannels = (uint16_t) fmt.channels;
  out->sampleRate = fmt.rate;
  if (error) error->clear();
  return true;
}

#elif defined(__APPLE__)

inline bool readExtAudioFile(const char* path, WavData* out, std::string* error,
                             size_t maxSamples) {
  if (!path || !out) return fail(out, error, "no path or no destination");
  fail(out, nullptr, "");
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      kCFAllocatorDefault, (const UInt8*) path, (CFIndex) std::strlen(path), false);
  if (!url) return fail(out, error, "the path could not be made into a URL");
  ExtAudioFileRef file = nullptr;
  const OSStatus opened = ExtAudioFileOpenURL(url, &file);
  CFRelease(url);
  if (opened != noErr || !file) return fail(out, error, "Core Audio cannot open this file");

  // ExtAudioFile is a C handle with no destructor: every exit below goes
  // through here or leaks the decoder.
  struct Closer {
    ExtAudioFileRef f;
    ~Closer() { ExtAudioFileDispose(f); }
  } closer{file};

  AudioStreamBasicDescription fileFormat;
  std::memset(&fileFormat, 0, sizeof(fileFormat));
  UInt32 size = (UInt32) sizeof(fileFormat);
  if (ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &size, &fileFormat) !=
      noErr)
    return fail(out, error, "the file's format could not be read");
  const UInt32 channels = fileFormat.mChannelsPerFrame;
  if (channels == 0 || channels > 32 || !(fileFormat.mSampleRate > 0.0) ||
      fileFormat.mSampleRate > 4.0e6)
    return fail(out, error, "the file declares an impossible channel count or rate");

  // The client format is what ExtAudioFile converts TO: interleaved float at
  // the file's own rate and channel count, so there is no resampling and no
  // remix -- the file as it is, in the one layout WavData holds. ExtAudioFile
  // also applies the container's priming and remainder, so an M4A comes out
  // the length it was encoded at.
  AudioStreamBasicDescription client;
  std::memset(&client, 0, sizeof(client));
  client.mSampleRate = fileFormat.mSampleRate;
  client.mFormatID = kAudioFormatLinearPCM;
  client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  client.mBitsPerChannel = 32;
  client.mChannelsPerFrame = channels;
  client.mFramesPerPacket = 1;
  client.mBytesPerFrame = 4u * channels;
  client.mBytesPerPacket = 4u * channels;
  if (ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat,
                              (UInt32) sizeof(client), &client) != noErr)
    return fail(out, error, "no decoder converts this file to float");

  const UInt32 chunkFrames = 4096;
  std::vector<float> chunk((size_t) chunkFrames * channels);
  std::vector<float>& dst = out->samples;
  for (;;) {
    AudioBufferList list;
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = channels;
    list.mBuffers[0].mDataByteSize = (UInt32) (chunk.size() * sizeof(float));
    list.mBuffers[0].mData = chunk.data();
    UInt32 frames = chunkFrames;
    if (ExtAudioFileRead(file, &frames, &list) != noErr)
      return fail(out, error, "the decoder failed partway through the file");
    if (frames == 0) break;
    const size_t count = (size_t) frames * channels;
    if (!roomFor(dst, count, maxSamples))
      return fail(out, error, "the decoded audio is past the size ceiling");
    dst.insert(dst.end(), chunk.begin(), chunk.begin() + (std::ptrdiff_t) count);
  }
  if (dst.empty()) return fail(out, error, "the file decoded to no audio");
  out->numChannels = (uint16_t) channels;
  out->sampleRate = (uint32_t) (fileFormat.mSampleRate + 0.5);
  if (error) error->clear();
  return true;
}

#endif

} // namespace sysaudiodetail

/**
 * Decoding through Windows Media Foundation.
 *
 * Exists on every platform so code that names it compiles everywhere;
 * isAvailable() is false off Windows, and on a Windows without Media
 * Foundation installed.
 */
class WindowsMediaAudioFormat {
public:
  /** True on a Windows where Media Foundation could be loaded. */
  static bool isAvailable() {
#if defined(_WIN32)
    return sysaudiodetail::mfApi().loaded();
#else
    return false;
#endif
  }

  /** Decode the file at a UTF-8 path to interleaved float. False, with `out`
   *  empty and `error` (if given) saying why, when no installed decoder takes
   *  it. `maxSamples` is the ceiling every reader here keeps; the parameter
   *  exists for tests. */
  static bool read(const char* path, WavData* out, std::string* error = nullptr,
                   size_t maxSamples = kMaxDecodedSamples) {
#if defined(_WIN32)
    return sysaudiodetail::readMediaFoundation(path, out, error, maxSamples);
#else
    (void) maxSamples;
    (void) path;
    return sysaudiodetail::fail(out, error, "Media Foundation exists only on Windows");
#endif
  }

  /** What a stock Windows 10/11 decodes. A file picker's filter, not a
   *  promise: codec packs add to it, and N editions take all of it away. */
  static std::vector<std::string> fileExtensions() {
    return {".mp3", ".m4a", ".mp4", ".aac", ".adts", ".wma", ".asf", ".3gp",
            ".3g2", ".ac3", ".ec3", ".amr", ".flac", ".wav"};
  }
};

/**
 * Decoding through Audio Toolbox's ExtAudioFile on macOS: AAC and ALAC in
 * M4A/CAF, MP3, AIFF-C codecs, and whatever else the system can read.
 */
class CoreAudioFormat {
public:
  /** True on macOS, false elsewhere. */
  static bool isAvailable() {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
  }

  /** Decode the file at a UTF-8 path to interleaved float. False, with `out`
   *  empty and `error` (if given) saying why, when the system cannot read
   *  it; always false off macOS. `maxSamples` is the ceiling every reader
   *  here keeps. */
  static bool read(const char* path, WavData* out, std::string* error = nullptr,
                   size_t maxSamples = kMaxDecodedSamples) {
#if defined(__APPLE__)
    return sysaudiodetail::readExtAudioFile(path, out, error, maxSamples);
#else
    (void) maxSamples;
    (void) path;
    return sysaudiodetail::fail(out, error, "Core Audio exists only on macOS");
#endif
  }

  /** What a stock macOS decodes, for a file picker's filter. */
  static std::vector<std::string> fileExtensions() {
    return {".m4a", ".mp4", ".aac", ".adts", ".caf", ".mp3", ".aif", ".aiff", ".aifc",
            ".wav", ".3gp", ".3g2", ".ac3", ".amr", ".snd", ".au", ".flac"};
  }
};

/** True when this platform has a system decoder this SDK can reach. Linux has
 *  none: there is no system codec API there, only libraries a user may or may
 *  not have, and the built-in set is the answer. */
inline bool systemAudioFormatsAvailable() {
  return WindowsMediaAudioFormat::isAvailable() || CoreAudioFormat::isAvailable();
}

/** Decode through this platform's system decoder, whatever the file is --
 *  bypassing the built-in ones. readAudioFile is the entry point that prefers
 *  the built-in decoders; this is for a caller that has decided otherwise. */
inline bool readAudioFileWithSystemCodecs(const char* path, WavData* out,
                                          std::string* error = nullptr) {
#if defined(_WIN32)
  return WindowsMediaAudioFormat::read(path, out, error);
#elif defined(__APPLE__)
  return CoreAudioFormat::read(path, out, error);
#else
  (void) path;
  return sysaudiodetail::fail(out, error, "this platform has no system audio decoder");
#endif
}

namespace sysaudiodetail {
inline bool fallbackDecoder(const char* path, WavData* out) {
  return readAudioFileWithSystemCodecs(path, out, nullptr);
}
} // namespace sysaudiodetail

/** Put the system decoders behind readAudioFile -- after the built-in ones --
 *  or, with false, take them away again. Per module (a plugin's choice is its
 *  own), and safe to call while other threads are loading files. */
inline void enableSystemAudioFormats(bool enable = true) {
  setAudioFileFallback(enable ? &sysaudiodetail::fallbackDecoder : nullptr);
}

} // namespace sonore
