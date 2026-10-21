// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: audio through the operating system, and around it.
//
// What is checked, and how:
//
//   * system decoders (system_audio_format.h). On Windows the fixtures are
//     MADE by the system: Media Foundation's own sink writer encodes a 1 kHz
//     sine (left at -6 dBFS, right at -12) to AAC in M4A, AAC in ADTS, MP3 and
//     WMA, and WindowsMediaAudioFormat decodes each back -- rate, channels,
//     length, the tone's frequency and both channels' levels are measured. A
//     float WAV must come back bit-exact; 16- and 24-bit PCM within one step.
//     The MP3 is also decoded by the SDK's own minimp3 and the two decoders
//     must agree sample for sample once aligned. On macOS CoreAudioFormat
//     reads WAV and AIFF written by this SDK and must match them.
//   * the dispatcher: readAudioFile keeps its fixed set by default, and with
//     the system decoders enabled still sends WAV and MP3 to the built-in
//     decoders and only what they refuse to the system (counted).
//   * COM: a decode on a thread the caller made single-threaded leaves it
//     single-threaded, and one on an uninitialised thread leaves it
//     uninitialised.
//   * corrupt input: every fixture truncated and byte-flipped, decoded
//     through both entry points. Nothing may crash; run this under ASan.
//   * LAMEEncoderAudioFormat: there is no LAME on the machine this was
//     written on, so the encoder is THIS PROGRAM -- run with an environment
//     variable set, it behaves as a fake lame (copy input to output, fail,
//     hang, write nothing, or echo its argv). That exercises the launcher for
//     real: CreateProcessW / posix_spawn, the argument quoting (round-tripped
//     through the child's own C runtime), the timeout and the kill, and the
//     temporary files' cleanup on every path.
//   * AudioThumbnailCache: byte budget, least-recently-used eviction,
//     round trips of one thumbnail and of the whole cache, refusal of
//     damaged bytes, FNV-1a known answers, file keys that change with the
//     file, and a two-thread soak.
//   * SystemAudioVolume: read always. Changed ONLY when
//     SONORE_TEST_SYSTEM_VOLUME=1 is set -- CI must never move a machine's
//     volume -- and then restored exactly, mute included, even on failure.
//   * audio CDs: the TOC parser on the bytes a drive returns for a known
//     disc (and on broken ones), sector/sample arithmetic, the CDDB id, the
//     burner's track preparation, and the no-drive paths of both classes.
//     Reading and burning a real disc need a drive this machine does not have.
//
//   audio_io_test                          the normal pass
//   audio_io_test --corrupt-iterations N   a deeper corrupt-input pass (ASan)

#include <sonore/audio_cd.h>
#include <sonore/audiofile.h>
#include <sonore/lame_encoder.h>
#include <sonore/system_audio_format.h>
#include <sonore/system_volume.h>
#include <sonore/thumbnail_cache.h>

#include "temp_path.h"

#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#if defined(_MSC_VER)
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#endif
#else
#include <climits>
#include <cstdlib>
#include <unistd.h>
#endif

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  std::fflush(stdout);
}

void section(const char* name) {
  std::printf("\n── %s ──\n", name);
  std::fflush(stdout);
}

std::string g_self; // this executable, which doubles as the fake encoder

/** A UTF-8 path in the test's temp directory. sonoretest::tempPath joins
 *  through std::filesystem::path(const char*), which on Windows reads the
 *  leaf in the ANSI code page -- right for the ASCII names elsewhere, wrong
 *  for the non-ASCII ones these tests exist to try. */
std::string utf8TempPath(const char* leaf) { return sonoretest::tempDir() + "/" + leaf; }

// ── signals ─────────────────────────────────────────────────────────────────

constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kRate = 44100;
constexpr size_t kFrames = 88200; // two seconds
constexpr double kLeftAmp = 0.5, kRightAmp = 0.25;

std::vector<float> stereoSine(size_t frames, double hz = 1000.0) {
  std::vector<float> s(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    const double v = std::sin(2.0 * kPi * hz * (double) i / kRate);
    s[2 * i] = (float) (kLeftAmp * v);
    s[2 * i + 1] = (float) (kRightAmp * v);
  }
  return s;
}

/** Goertzel power of one channel at `hz`, over frames [from, to). */
double tonePower(const sonore::WavData& d, int channel, double hz, size_t from, size_t to) {
  const double w = 2.0 * kPi * hz / d.sampleRate;
  const double coeff = 2.0 * std::cos(w);
  double s1 = 0.0, s2 = 0.0;
  for (size_t i = from; i < to; ++i) {
    const double s0 = d.samples[i * d.numChannels + (size_t) channel] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

double channelRms(const sonore::WavData& d, int channel, size_t from, size_t to) {
  double e = 0.0;
  for (size_t i = from; i < to; ++i) {
    const double v = d.samples[i * d.numChannels + (size_t) channel];
    e += v * v;
  }
  return std::sqrt(e / (double) (to - from));
}

/** The decoded sine is what was encoded: rate, channels, length (codecs add
 *  priming and pad to a frame, never lose audio), the tone (30 dB above its
 *  neighbours) and both channel levels within half a decibel. */
void checkSine(const sonore::WavData& d, const char* what) {
  char line[256];
  std::snprintf(line, sizeof(line), "%s: 44.1 kHz stereo (got %u Hz, %u ch)", what, d.sampleRate,
                (unsigned) d.numChannels);
  check(d.sampleRate == kRate && d.numChannels == 2, line);
  if (d.sampleRate != kRate || d.numChannels != 2) return;
  const size_t frames = d.numFrames();
  std::snprintf(line, sizeof(line), "%s: length %zu frames, encoded 88200 plus at most 4096",
                what, frames);
  check(frames >= kFrames && frames <= kFrames + 4096, line);
  const size_t from = 22050, to = 66150; // the middle second, clear of priming
  if (frames < to) return;
  const double at = tonePower(d, 0, 1000.0, from, to);
  const double below = tonePower(d, 0, 700.0, from, to);
  const double above = tonePower(d, 0, 1300.0, from, to);
  std::snprintf(line, sizeof(line), "%s: the 1 kHz tone, %.0f dB over 700 Hz and %.0f dB over 1.3 kHz",
                what, 10.0 * std::log10(at / (below + 1e-30)),
                10.0 * std::log10(at / (above + 1e-30)));
  check(at > 1000.0 * below && at > 1000.0 * above, line);
  const double l = channelRms(d, 0, from, to), r = channelRms(d, 1, from, to);
  const double wantL = kLeftAmp / std::sqrt(2.0), wantR = kRightAmp / std::sqrt(2.0);
  std::snprintf(line, sizeof(line), "%s: levels L %.4f (want %.4f), R %.4f (want %.4f)", what, l,
                wantL, r, wantR);
  check(std::fabs(20.0 * std::log10(l / wantL)) < 0.5 &&
            std::fabs(20.0 * std::log10(r / wantR)) < 0.5,
        line);
}

std::vector<uint8_t> readBytes(const std::string& path) {
  std::vector<uint8_t> out;
  std::FILE* f = sonore::pathio::open(path.c_str(), "rb");
  if (!f) return out;
  uint8_t chunk[65536];
  size_t n = 0;
  while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) out.insert(out.end(), chunk, chunk + n);
  std::fclose(f);
  return out;
}

bool writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::FILE* f = sonore::pathio::open(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  return std::fclose(f) == 0 && ok;
}

/** A PCM WAV at 16 or 24 bits -- the SDK writes only float WAV. */
bool writePcmWav(const std::string& path, const std::vector<float>& s, int channels, int bits) {
  std::vector<uint8_t> out;
  auto u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t) (v >> (8 * i)));
  };
  auto u16 = [&](uint32_t v) {
    out.push_back((uint8_t) v);
    out.push_back((uint8_t) (v >> 8));
  };
  const uint32_t width = (uint32_t) bits / 8u;
  const uint32_t data = (uint32_t) s.size() * width;
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  u32(36 + data);
  out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  u32(16);
  u16(1);
  u16((uint32_t) channels);
  u32(kRate);
  u32(kRate * (uint32_t) channels * width);
  u16((uint32_t) channels * width);
  u16((uint32_t) bits);
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  u32(data);
  const double full = bits == 16 ? 32767.0 : 8388607.0;
  for (float v : s) {
    const int32_t q = (int32_t) std::lround(v * full);
    for (uint32_t b = 0; b < width; ++b) out.push_back((uint8_t) ((uint32_t) q >> (8 * b)));
  }
  return writeBytes(path, out);
}

// ── the fake encoder ────────────────────────────────────────────────────────
//
// SONORE_AUDIO_IO_FAKE_LAME selects what it does, SONORE_AUDIO_IO_FAKE_LAME_LOG
// where it writes the argv it was given (one argument per record, as
// "<length>:<bytes>\n", so arguments containing newlines survive).

int fakeLame(const std::vector<std::string>& args) {
  const std::string mode = sonore::pathio::environment("SONORE_AUDIO_IO_FAKE_LAME");
  const std::string log = sonore::pathio::environment("SONORE_AUDIO_IO_FAKE_LAME_LOG");
  if (!log.empty()) {
    std::string text;
    for (size_t i = 1; i < args.size(); ++i)
      text += std::to_string(args[i].size()) + ":" + args[i] + "\n";
    writeBytes(log, std::vector<uint8_t>(text.begin(), text.end()));
  }
  if (mode == "fail") return 3;
  if (mode == "hang") {
    std::this_thread::sleep_for(std::chrono::seconds(60));
    return 0;
  }
  if (mode == "echo" || mode == "empty") return 0;
  // "copy": lame's last two arguments are the input and the output.
  if (args.size() < 3) return 2;
  const std::vector<uint8_t> in = readBytes(args[args.size() - 2]);
  if (in.empty()) return 4;
  return writeBytes(args[args.size() - 1], in) ? 0 : 5;
}

void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
  SetEnvironmentVariableW(sonore::pathio::widen(name).c_str(),
                          value ? sonore::pathio::widen(value).c_str() : nullptr);
#else
  if (value) setenv(name, value, 1);
  else unsetenv(name);
#endif
}

std::vector<std::string> readArgLog(const std::string& path) {
  const std::vector<uint8_t> bytes = readBytes(path);
  std::vector<std::string> args;
  size_t at = 0;
  while (at < bytes.size()) {
    size_t colon = at;
    while (colon < bytes.size() && bytes[colon] != ':') ++colon;
    if (colon >= bytes.size()) break;
    const size_t n = (size_t) std::strtoul(std::string(bytes.begin() + (std::ptrdiff_t) at,
                                                       bytes.begin() + (std::ptrdiff_t) colon)
                                               .c_str(),
                                           nullptr, 10);
    if (colon + 1 + n > bytes.size()) break;
    args.emplace_back(bytes.begin() + (std::ptrdiff_t) colon + 1,
                      bytes.begin() + (std::ptrdiff_t) (colon + 1 + n));
    at = colon + 1 + n + 1;
  }
  return args;
}

size_t lameTempFiles() {
  size_t n = 0;
  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::u8path(sonore::lamedetail::tempDirectory());
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    // u8string: string() converts through the ANSI code page on Windows and
    // throws on a name some other program left in temp that it cannot hold.
    const std::string name = entry.path().filename().u8string();
    if (name.rfind("sonore-lame-", 0) == 0) ++n;
  }
  return n;
}

#if defined(_WIN32)

// ── Media Foundation, the ENCODING side (the test's fixtures) ───────────────

bool encodeWithMediaFoundation(const std::string& path, const GUID& container, const GUID& subtype,
                               UINT32 bytesPerSecond, std::string* why) {
  sonore::comdetail::Apartment com;
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) return (*why = "MFStartup"), false;
  bool ok = false;
  IMFAttributes* attrs = nullptr;
  IMFSinkWriter* writer = nullptr;
  IMFCollection* types = nullptr;
  IMFMediaType* outType = nullptr;
  IMFMediaType* inType = nullptr;
  do {
    if (FAILED(MFCreateAttributes(&attrs, 1))) break;
    attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, container);
    if (FAILED(MFCreateSinkWriterFromURL(sonore::pathio::widen(path.c_str()).c_str(), nullptr,
                                         attrs, &writer))) {
      *why = "no sink writer for this container";
      break;
    }
    if (FAILED(MFTranscodeGetAudioOutputAvailableTypes(subtype, MFT_ENUM_FLAG_ALL, nullptr,
                                                       &types))) {
      *why = "no encoder for this format";
      break;
    }
    DWORD count = 0;
    types->GetElementCount(&count);
    for (DWORD i = 0; i < count && !outType; ++i) {
      IUnknown* unknown = nullptr;
      if (FAILED(types->GetElement(i, &unknown)) || !unknown) continue;
      IMFMediaType* t = nullptr;
      unknown->QueryInterface(IID_PPV_ARGS(&t));
      unknown->Release();
      if (!t) continue;
      UINT32 ch = 0, rate = 0, bps = 0;
      t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
      t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
      t->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &bps);
      if (ch == 2 && rate == kRate && (bytesPerSecond == 0 || bps == bytesPerSecond)) outType = t;
      else t->Release();
    }
    if (!outType) {
      *why = "the encoder offers no 44.1 kHz stereo type";
      break;
    }
    DWORD stream = 0;
    if (FAILED(writer->AddStream(outType, &stream))) break;
    if (FAILED(MFCreateMediaType(&inType))) break;
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 4);
    if (FAILED(writer->SetInputMediaType(stream, inType, nullptr))) {
      *why = "the encoder refused 16-bit PCM input";
      break;
    }
    if (FAILED(writer->BeginWriting())) break;
    const std::vector<float> sine = stereoSine(kFrames);
    const size_t chunk = 4410;
    bool wrote = true;
    for (size_t at = 0; at < kFrames && wrote; at += chunk) {
      IMFMediaBuffer* buffer = nullptr;
      IMFSample* sample = nullptr;
      wrote = SUCCEEDED(MFCreateMemoryBuffer((DWORD) (chunk * 4), &buffer)) &&
              SUCCEEDED(MFCreateSample(&sample));
      if (wrote) {
        BYTE* p = nullptr;
        buffer->Lock(&p, nullptr, nullptr);
        int16_t* s = reinterpret_cast<int16_t*>(p);
        for (size_t i = 0; i < chunk * 2; ++i)
          s[i] = (int16_t) std::lround(sine[at * 2 + i] * 32767.0f);
        buffer->Unlock();
        buffer->SetCurrentLength((DWORD) (chunk * 4));
        sample->AddBuffer(buffer);
        sample->SetSampleTime((LONGLONG) at * 10000000 / kRate);
        sample->SetSampleDuration((LONGLONG) chunk * 10000000 / kRate);
        wrote = SUCCEEDED(writer->WriteSample(stream, sample));
      }
      if (sample) sample->Release();
      if (buffer) buffer->Release();
    }
    ok = wrote && SUCCEEDED(writer->Finalize());
  } while (false);
  if (inType) inType->Release();
  if (outType) outType->Release();
  if (types) types->Release();
  if (writer) writer->Release();
  if (attrs) attrs->Release();
  MFShutdown();
  return ok;
}

struct Fixture {
  const char* name;
  const char* file;
  GUID container;
  GUID subtype;
  UINT32 bytesPerSecond;
  std::string path;
  bool made = false;
};

std::atomic<int> g_fallbackCalls{0};
bool countingFallback(const char* path, sonore::WavData* out) {
  ++g_fallbackCalls;
  return sonore::readAudioFileWithSystemCodecs(path, out);
}

/** Lag (in frames) that best aligns b to a, searched over +-maxLag, by the
 *  smallest squared difference over a window in the middle. Not by
 *  correlation: a 1 kHz sine at 44.1 kHz repeats exactly every 441 samples,
 *  so every tenth period correlates equally well -- the first version of this
 *  check aligned the decoders 2646 frames apart and measured codec noise
 *  instead of decoder disagreement (59 dB, against 90 at the true offset). */
long bestLag(const sonore::WavData& a, const sonore::WavData& b, long maxLag) {
  const size_t from = 30000, len = 8192;
  double best = 1e300;
  long bestAt = 0;
  for (long lag = -maxLag; lag <= maxLag; ++lag) {
    double d = 0.0;
    for (size_t i = from; i < from + len; ++i) {
      const long j = (long) i + lag;
      if (j < 0 || (size_t) j >= b.numFrames()) {
        d = 1e300;
        break;
      }
      const double e = (double) a.samples[i * 2] - b.samples[(size_t) j * 2];
      d += e * e;
    }
    if (d < best) {
      best = d;
      bestAt = lag;
    }
  }
  return bestAt;
}

#endif

// ── sections ────────────────────────────────────────────────────────────────

void testSystemDecoders(int corruptIterations) {
#if defined(_WIN32)
  section("Media Foundation: the GUIDs spelled out match the SDK's");
  {
    using namespace sonore::sysaudiodetail;
    check(IsEqualGUID(kMtMajorType, MF_MT_MAJOR_TYPE) && IsEqualGUID(kMtSubtype, MF_MT_SUBTYPE),
          "MF_MT_MAJOR_TYPE, MF_MT_SUBTYPE");
    check(IsEqualGUID(kMediaTypeAudio, MFMediaType_Audio) &&
              IsEqualGUID(kAudioFormatPcm, MFAudioFormat_PCM) &&
              IsEqualGUID(kAudioFormatFloat, MFAudioFormat_Float),
          "MFMediaType_Audio, MFAudioFormat_PCM, MFAudioFormat_Float");
    check(IsEqualGUID(kMtAudioNumChannels, MF_MT_AUDIO_NUM_CHANNELS) &&
              IsEqualGUID(kMtAudioSamplesPerSecond, MF_MT_AUDIO_SAMPLES_PER_SECOND) &&
              IsEqualGUID(kMtAudioBitsPerSample, MF_MT_AUDIO_BITS_PER_SAMPLE),
          "MF_MT_AUDIO_NUM_CHANNELS, _SAMPLES_PER_SECOND, _BITS_PER_SAMPLE");
    check(kMfVersion == MF_VERSION, "MF_VERSION");
  }

  section("Media Foundation: the system encodes, WindowsMediaAudioFormat decodes");
  check(sonore::WindowsMediaAudioFormat::isAvailable(), "Media Foundation is installed");
  check(sonore::systemAudioFormatsAvailable(), "systemAudioFormatsAvailable()");
  check(!sonore::CoreAudioFormat::isAvailable(), "CoreAudioFormat says it is not here");
  Fixture fixtures[] = {
      {"AAC in M4A", "sine.m4a", MFTranscodeContainerType_MPEG4, MFAudioFormat_AAC, 24000, "", false},
      {"AAC in ADTS", "sine.aac", MFTranscodeContainerType_ADTS, MFAudioFormat_AAC, 24000, "", false},
      {"MP3", "sine.mp3", MFTranscodeContainerType_MP3, MFAudioFormat_MP3, 0, "", false},
      {"WMA", "sine.wma", MFTranscodeContainerType_ASF, MFAudioFormat_WMAudioV8, 0, "", false},
  };
  for (Fixture& f : fixtures) {
    f.path = sonoretest::tempPath(f.file);
    std::string why;
    f.made = encodeWithMediaFoundation(f.path, f.container, f.subtype, f.bytesPerSecond, &why);
    char line[200];
    if (!f.made) {
      std::snprintf(line, sizeof(line), "%s: SKIPPED, this Windows cannot encode it (%s)", f.name,
                    why.c_str());
      std::printf("  ---- %s\n", line);
      continue;
    }
    sonore::WavData d;
    std::string err;
    std::snprintf(line, sizeof(line), "%s: decoded", f.name);
    check(sonore::WindowsMediaAudioFormat::read(f.path.c_str(), &d, &err) && err.empty(), line);
    checkSine(d, f.name);
  }
  // M4A and MP3 are the formats the row promises; an encoder for them ships
  // with every desktop Windows since 8.
  check(fixtures[0].made && fixtures[2].made, "the M4A and MP3 fixtures were made");

  section("Media Foundation: uncompressed WAV, exact");
  {
    const std::vector<float> sine = stereoSine(kFrames);
    const std::string f32 = sonoretest::tempPath("mf-float.wav");
    sonore::writeWav(f32.c_str(), sine.data(), kFrames, 2, kRate);
    sonore::WavData d;
    check(sonore::WindowsMediaAudioFormat::read(f32.c_str(), &d) && d.samples == sine,
          "32-bit float WAV: every sample bit-exact");
    for (int bits : {16, 24}) {
      const std::string p = sonoretest::tempPath(bits == 16 ? "mf-16.wav" : "mf-24.wav");
      writePcmWav(p, sine, 2, bits);
      sonore::WavData pcm;
      double worst = 0.0;
      const bool read = sonore::WindowsMediaAudioFormat::read(p.c_str(), &pcm);
      if (read && pcm.samples.size() == sine.size())
        for (size_t i = 0; i < sine.size(); ++i)
          worst = (std::max)(worst, (double) std::fabs(pcm.samples[i] - sine[i]));
      char line[160];
      std::snprintf(line, sizeof(line), "%d-bit PCM WAV: within one step (worst %.2e)", bits, worst);
      check(read && pcm.samples.size() == sine.size() &&
                worst <= 1.0 / (bits == 16 ? 32767.0 : 8388607.0) + 1e-7,
            line);
    }
  }

  section("Media Foundation: two decoders, one MP3");
  if (fixtures[2].made) {
    sonore::WavData mf, own;
    sonore::WindowsMediaAudioFormat::read(fixtures[2].path.c_str(), &mf);
    const bool ownOk = sonore::readMp3(fixtures[2].path.c_str(), &own);
    check(ownOk, "minimp3 decodes the MP3 Windows wrote");
    if (ownOk && mf.numFrames() > 40000 && own.numFrames() > 40000) {
      const long lag = bestLag(mf, own, 3000);
      double diff = 0.0, energy = 0.0;
      for (size_t i = 22050; i < 66150; ++i) {
        const long j = (long) i + lag;
        if (j < 0 || (size_t) j >= own.numFrames()) continue;
        for (int c = 0; c < 2; ++c) {
          const double a = mf.samples[i * 2 + (size_t) c], b = own.samples[(size_t) j * 2 + (size_t) c];
          diff += (a - b) * (a - b);
          energy += a * a;
        }
      }
      const double snr = 10.0 * std::log10(energy / (diff + 1e-30));
      char line[160];
      std::snprintf(line, sizeof(line),
                    "aligned (offset %ld frames), Media Foundation and minimp3 agree to %.1f dB",
                    lag, snr);
      // Two IEEE-accurate Layer III decoders of one bitstream differ by their
      // arithmetic alone: ~90 dB here. 75 leaves room for a decoder update and
      // none for a dropped granule, a swapped channel or a wrong gain.
      check(snr > 75.0, line);
    }
  }

  section("readAudioFile: built-in decoders first, the system for the rest");
  {
    sonore::WavData d;
    sonore::setAudioFileFallback(nullptr);
    check(!sonore::audioFileFallback(), "no fallback by default");
    if (fixtures[0].made)
      check(!sonore::readAudioFile(fixtures[0].path.c_str(), &d),
            "without it, an M4A is refused: the fixed set is the default");
    sonore::enableSystemAudioFormats(true);
    check(sonore::audioFileFallback() != nullptr, "enableSystemAudioFormats installs one");
    sonore::enableSystemAudioFormats(false);
    check(!sonore::audioFileFallback(), "enableSystemAudioFormats(false) takes it away");

    sonore::setAudioFileFallback(&countingFallback);
    g_fallbackCalls = 0;
    if (fixtures[0].made) {
      check(sonore::readAudioFile(fixtures[0].path.c_str(), &d) && g_fallbackCalls == 1,
            "M4A: no built-in decoder knows it, the system decodes it");
      checkSine(d, "M4A through readAudioFile");
    }
    if (fixtures[1].made) {
      const int before = g_fallbackCalls;
      check(sonore::readAudioFile(fixtures[1].path.c_str(), &d) && g_fallbackCalls == before + 1,
            "ADTS AAC: shares MP3's sync, minimp3 refuses it, the system decodes it");
    }
    if (fixtures[2].made) {
      const int before = g_fallbackCalls;
      sonore::WavData own;
      sonore::readMp3(fixtures[2].path.c_str(), &own);
      check(sonore::readAudioFile(fixtures[2].path.c_str(), &d) && g_fallbackCalls == before &&
                d.samples == own.samples,
            "MP3: decoded by the built-in minimp3, the system never asked");
    }
    {
      const std::vector<float> sine = stereoSine(4410);
      const std::string wav = sonoretest::tempPath("dispatch.wav");
      sonore::writeWav(wav.c_str(), sine.data(), 4410, 2, kRate);
      const int before = g_fallbackCalls;
      check(sonore::readAudioFile(wav.c_str(), &d) && g_fallbackCalls == before &&
                d.samples == sine,
            "WAV: the built-in reader, the system never asked");
    }
    {
      // MP3 frames inside a RIFF (format tag 0x55): the WAV reader knows the
      // container and refuses the codec, so this is the case of a built-in
      // decoder saying no and the system saying yes.
      if (fixtures[2].made) {
        const std::vector<uint8_t> mp3 = readBytes(fixtures[2].path);
        std::vector<uint8_t> w;
        auto u32 = [&](uint32_t v) {
          for (int i = 0; i < 4; ++i) w.push_back((uint8_t) (v >> (8 * i)));
        };
        auto u16 = [&](uint32_t v) {
          w.push_back((uint8_t) v);
          w.push_back((uint8_t) (v >> 8));
        };
        w.insert(w.end(), {'R', 'I', 'F', 'F'});
        u32((uint32_t) (4 + 8 + 30 + 8 + mp3.size()));
        w.insert(w.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
        u32(30);
        u16(0x55);   // WAVE_FORMAT_MPEGLAYER3
        u16(2);
        u32(kRate);
        u32(40000);  // bytes per second, 320 kbps
        u16(1);
        u16(0);
        u16(12);     // cbSize: the MPEGLAYER3WAVEFORMAT tail
        u16(1);      // wID: MPEGLAYER3_ID_MPEG
        u32(2);      // fdwFlags: padding off
        u16(1044);   // nBlockSize
        u16(1);      // nFramesPerBlock
        u16(1393);   // nCodecDelay
        w.insert(w.end(), {'d', 'a', 't', 'a'});
        u32((uint32_t) mp3.size());
        w.insert(w.end(), mp3.begin(), mp3.end());
        const std::string path = sonoretest::tempPath("mp3-in-riff.wav");
        writeBytes(path, w);
        sonore::WavData refused;
        check(!sonore::readWav(path.c_str(), &refused), "MP3-in-RIFF: the built-in WAV reader refuses it");
        const int before = g_fallbackCalls;
        const bool ok = sonore::readAudioFile(path.c_str(), &d);
        check(g_fallbackCalls == before + 1, "...so readAudioFile hands it to the system");
        if (ok) checkSine(d, "MP3-in-RIFF through the system");
        else std::printf("  ---- MP3-in-RIFF: this Windows's WAV source does not take it\n");
      }
    }
    sonore::setAudioFileFallback(nullptr);
  }

  section("Media Foundation: what it must refuse");
  {
    sonore::WavData d;
    std::string err;
    check(!sonore::WindowsMediaAudioFormat::read(sonoretest::tempPath("missing.m4a"), &d, &err) &&
              !err.empty() && d.samples.empty(),
          "a missing file: false, a reason, and nothing decoded");
    const std::string junk = sonoretest::tempPath("junk.m4a");
    std::vector<uint8_t> noise(20000);
    std::mt19937 rng(7);
    for (uint8_t& b : noise) b = (uint8_t) rng();
    writeBytes(junk, noise);
    check(!sonore::WindowsMediaAudioFormat::read(junk.c_str(), &d, &err) && !err.empty(),
          "twenty kilobytes of noise named .m4a: refused");
    check(!sonore::WindowsMediaAudioFormat::read(nullptr, &d, &err), "a null path: refused");
    if (fixtures[0].made)
      check(!sonore::WindowsMediaAudioFormat::read(fixtures[0].path.c_str(), &d, &err, 10000) &&
                d.samples.empty(),
            "past the decoded-size ceiling: refused, not truncated");
    // A path that is a URL is not opened as one.
    check(!sonore::WindowsMediaAudioFormat::read("http://127.0.0.1:9/a.mp3", &d, &err),
          "an http:// \"path\" is a file name that does not exist, not a download");
  }

  section("COM: the caller's apartment is left as it was");
  if (fixtures[0].made) {
    const std::string path = fixtures[0].path;
    bool staReadOk = false, staStillSta = false;
    std::thread sta([&] {
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      sonore::WavData d;
      staReadOk = sonore::WindowsMediaAudioFormat::read(path.c_str(), &d);
      APTTYPE type = APTTYPE_CURRENT;
      APTTYPEQUALIFIER q = APTTYPEQUALIFIER_NONE;
      staStillSta = SUCCEEDED(CoGetApartmentType(&type, &q)) &&
                    (type == APTTYPE_STA || type == APTTYPE_MAINSTA);
      CoUninitialize();
    });
    sta.join();
    check(staReadOk, "a decode on a thread the caller made single-threaded works");
    check(staStillSta, "and the thread is still single-threaded afterwards");

    bool freshReadOk = false, freshUninitialised = false;
    std::thread fresh([&] {
      sonore::WavData d;
      freshReadOk = sonore::WindowsMediaAudioFormat::read(path.c_str(), &d);
      // S_OK means COM was NOT initialised on this thread: the decode
      // balanced what it set up.
      const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
      freshUninitialised = hr == S_OK;
      if (SUCCEEDED(hr)) CoUninitialize();
    });
    fresh.join();
    check(freshReadOk, "a decode on an uninitialised thread works");
    check(freshUninitialised, "and leaves the thread uninitialised");
  }

  section("Media Foundation: corrupt files decode or refuse, never crash");
  {
    std::mt19937 rng(20261013);
    int decoded = 0, refused = 0;
    sonore::enableSystemAudioFormats(true);
    for (const Fixture& f : fixtures) {
      if (!f.made) continue;
      const std::vector<uint8_t> clean = readBytes(f.path);
      const std::string mutant = sonoretest::tempPath((std::string("mutant-") + f.file).c_str());
      for (int i = 0; i < corruptIterations; ++i) {
        std::vector<uint8_t> bytes = clean;
        if (i % 3 == 0) {
          bytes.resize((size_t) rng() % bytes.size());
        } else {
          const int flips = 1 + (int) (rng() % 32);
          for (int k = 0; k < flips; ++k) bytes[(size_t) rng() % bytes.size()] ^= (uint8_t) (1u << (rng() % 8));
          // Half of the flips aimed at the first kilobyte, where the headers are.
          if (i % 2) bytes[(size_t) rng() % (std::min)((size_t) 1024, bytes.size())] = (uint8_t) rng();
        }
        writeBytes(mutant, bytes);
        sonore::WavData d;
        const bool a = sonore::WindowsMediaAudioFormat::read(mutant.c_str(), &d);
        if (a && (d.numChannels == 0 || d.sampleRate == 0 || d.samples.size() % d.numChannels))
          check(false, "a decode that succeeded returned a malformed result");
        const bool b = sonore::readAudioFile(mutant.c_str(), &d);
        (a || b) ? ++decoded : ++refused;
      }
    }
    sonore::enableSystemAudioFormats(false);
    char line[160];
    std::snprintf(line, sizeof(line), "%d mutants: %d decoded, %d refused, none crashed",
                  decoded + refused, decoded, refused);
    check(decoded + refused > 0, line);
  }
#elif defined(__APPLE__)
  section("Core Audio: ExtAudioFile reads what this SDK writes");
  check(sonore::CoreAudioFormat::isAvailable(), "CoreAudioFormat is here");
  const std::vector<float> sine = stereoSine(kFrames);
  const std::string wav = sonoretest::tempPath("ca-float.wav");
  sonore::writeWav(wav.c_str(), sine.data(), kFrames, 2, kRate);
  sonore::WavData d;
  check(sonore::CoreAudioFormat::read(wav.c_str(), &d) && d.samples == sine,
        "32-bit float WAV: every sample bit-exact");
  const std::string aiff = sonoretest::tempPath("ca-24.aiff");
  sonore::writeAiff(aiff.c_str(), sine.data(), kFrames, 2, kRate, 24);
  sonore::WavData a;
  check(sonore::CoreAudioFormat::read(aiff.c_str(), &a), "24-bit AIFF decodes");
  checkSine(a, "24-bit AIFF through Core Audio");
  std::string err;
  check(!sonore::CoreAudioFormat::read(sonoretest::tempPath("missing.m4a"), &d, &err) && !err.empty(),
        "a missing file: false, with a reason");
  (void) corruptIterations;
#else
  section("system decoders: none on this platform");
  sonore::WavData d;
  std::string err;
  check(!sonore::systemAudioFormatsAvailable(), "systemAudioFormatsAvailable() is false");
  check(!sonore::readAudioFileWithSystemCodecs("x.m4a", &d, &err) && !err.empty(),
        "readAudioFileWithSystemCodecs refuses, saying why");
  (void) corruptIterations;
#endif
}

void testLame() {
  section("LAMEEncoderAudioFormat: the arguments");
  using Lame = sonore::LAMEEncoderAudioFormat;
  Lame lame(g_self);
  Lame::Options o;
  {
    const std::vector<std::string> a = lame.arguments(o, "in.wav", "out.mp3");
    check(a == std::vector<std::string>({"--quiet", "-V", "2", "in.wav", "out.mp3"}),
          "default: --quiet -V 2 in.wav out.mp3");
    o.mode = Lame::Options::Mode::cbr;
    o.bitrateKbps = 320;
    o.algorithmQuality = 0;
    o.title = "Song & \"Dance\"";
    o.artist = "O'Brien";
    const std::vector<std::string> b = lame.arguments(o, "in.wav", "out.mp3");
    check(b == std::vector<std::string>({"--quiet", "--cbr", "-b", "320", "-q", "0", "--tt",
                                         "Song & \"Dance\"", "--ta", "O'Brien", "in.wav", "out.mp3"}),
          "CBR 320, -q 0, tags passed as whole arguments");
    o = Lame::Options();
    o.mode = Lame::Options::Mode::abr;
    o.bitrateKbps = 9999;
    o.vbrQuality = -4;
    const std::vector<std::string> c = lame.arguments(o, "i", "o");
    check(c[1] == "--abr" && c[2] == "320", "an out-of-range bitrate is clamped, not passed on");
    check(Lame::cbrBitrates().front() == 32 && Lame::cbrBitrates().back() == 320,
          "the Layer III bitrate table");
  }

  section("LAMEEncoderAudioFormat: launching a real child process (this program, as a fake lame)");
  const size_t tempBefore = lameTempFiles();
  const std::string log = sonoretest::tempPath("fake-lame-args.txt");
  setEnv("SONORE_AUDIO_IO_FAKE_LAME_LOG", log.c_str());
  {
    // Every argument must arrive in the child exactly as it was in the list.
    setEnv("SONORE_AUDIO_IO_FAKE_LAME", "echo");
    const std::vector<std::string> tricky = {
        "plain",           "with space",       "quote\"inside",   "trailing\\",
        "C:\\dir with\\",  "",                 "\\\\server\\share", "x\\\"y",
        "a\\\\\"b c",      "tab\there",        "\"",              "\\",
        "--tt",            "Ñandú 音楽 Олег",  "semi;colon&amp|pipe>out",
        "$HOME `id` %PATH%"};
    const sonore::lamedetail::ProcessResult r = sonore::lamedetail::runProcess(g_self, tricky, 30.0);
    check(r.started && !r.timedOut && r.exitCode == 0, "the child ran and exited 0");
    const std::vector<std::string> got = readArgLog(log);
    bool same = got.size() == tricky.size();
    for (size_t i = 0; same && i < got.size(); ++i) same = got[i] == tricky[i];
    char line[200];
    std::snprintf(line, sizeof(line),
                  "%zu arguments -- spaces, quotes, backslash runs, empty, UTF-8, shell "
                  "metacharacters -- arrive byte for byte",
                  tricky.size());
    check(same, line);
    if (!same)
      for (size_t i = 0; i < got.size() && i < tricky.size(); ++i)
        if (got[i] != tricky[i])
          std::printf("       [%zu] sent <%s> got <%s>\n", i, tricky[i].c_str(), got[i].c_str());
  }
  {
    setEnv("SONORE_AUDIO_IO_FAKE_LAME", "copy");
    const std::vector<float> sine = stereoSine(22050);
    std::vector<uint8_t> mp3;
    check(lame.canEncode(), "canEncode() on an executable that exists");
    const bool ok = lame.encode(sine.data(), 22050, 2, kRate, Lame::Options(), &mp3);
    check(ok && lame.lastError().empty(), "encode: the encoder ran and its output was read back");
    // The fake copies its input, so the "MP3" is the temporary WAV: 16-bit
    // PCM, the right shape, every sample within one step.
    const std::string back = sonoretest::tempPath("fake-output.wav");
    writeBytes(back, mp3);
    sonore::WavData w;
    double worst = 1.0;
    if (sonore::readWav(back.c_str(), &w) && w.samples.size() == sine.size()) {
      worst = 0.0;
      for (size_t i = 0; i < sine.size(); ++i)
        worst = (std::max)(worst, (double) std::fabs(w.samples[i] - sine[i]));
    }
    check(w.sampleRate == kRate && w.numChannels == 2 && worst < 1.0 / 32767.0 + 1e-6,
          "the temporary WAV it was handed is 16-bit PCM of the audio, within one step");
    const std::vector<std::string> args = readArgLog(log);
    check(args.size() == 5 && args[0] == "--quiet" && args[1] == "-V" && args[2] == "2" &&
              args[3].find("sonore-lame-") != std::string::npos &&
              args[4].find("sonore-lame-") != std::string::npos,
          "it was run as: --quiet -V 2 <temp wav> <temp mp3>");
    check(lameTempFiles() == tempBefore, "both temporary files are gone afterwards");

    const std::string dest = utf8TempPath("dest dir é 音/out file.mp3");
    std::filesystem::create_directories(std::filesystem::u8path(dest).parent_path());
    check(lame.write(dest.c_str(), sine.data(), 22050, 2, kRate) && readBytes(dest) == mp3,
          "write(): to a UTF-8 path with spaces, the encoder's output verbatim");
  }
  {
    setEnv("SONORE_AUDIO_IO_FAKE_LAME", "fail");
    const std::vector<float> sine = stereoSine(4410);
    std::vector<uint8_t> mp3;
    check(!lame.encode(sine.data(), 4410, 2, kRate, Lame::Options(), &mp3) &&
              lame.lastError().find("exit code 3") != std::string::npos,
          "an encoder that exits 3: refused, saying so");
    setEnv("SONORE_AUDIO_IO_FAKE_LAME", "empty");
    check(!lame.encode(sine.data(), 4410, 2, kRate, Lame::Options(), &mp3) &&
              lame.lastError().find("wrote nothing") != std::string::npos,
          "an encoder that exits 0 but writes nothing: refused");
    setEnv("SONORE_AUDIO_IO_FAKE_LAME", "hang");
    Lame::Options quick;
    quick.timeoutSeconds = 0.5;
    const auto t0 = std::chrono::steady_clock::now();
    const bool hung = lame.encode(sine.data(), 4410, 2, kRate, quick, &mp3);
    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    char line[160];
    std::snprintf(line, sizeof(line), "an encoder that hangs is stopped at the timeout (%.2f s)", took);
    check(!hung && lame.lastError().find("in time") != std::string::npos && took < 10.0, line);
    check(lameTempFiles() == tempBefore, "no temporary file left behind by failure or timeout");
  }
  {
    std::vector<uint8_t> mp3;
    const std::vector<float> sine = stereoSine(4410);
    Lame missing(sonoretest::tempPath("no-such-lame.exe"));
    check(!missing.canEncode() &&
              !missing.encode(sine.data(), 4410, 2, kRate, Lame::Options(), &mp3) &&
              !missing.lastError().empty(),
          "an executable that is not there: refused before anything is written");
    const sonore::lamedetail::ProcessResult r =
        sonore::lamedetail::runProcess(sonoretest::tempPath("no-such-lame.exe"), {}, 5.0);
    check(!r.started && !r.error.empty(), "runProcess on a missing program: not started, with a reason");
    std::vector<float> six(600);
    check(!lame.encode(six.data(), 100, 6, kRate, Lame::Options(), &mp3),
          "six channels: refused, MP3 carries two");
  }
  setEnv("SONORE_AUDIO_IO_FAKE_LAME", nullptr);
  setEnv("SONORE_AUDIO_IO_FAKE_LAME_LOG", nullptr);
}

sonore::AudioThumbnail thumbOf(double hz, uint32_t buckets, size_t frames = 44100) {
  std::vector<float> l(frames), r(frames);
  for (size_t i = 0; i < frames; ++i) {
    l[i] = (float) (0.8 * std::sin(2.0 * kPi * hz * (double) i / kRate));
    r[i] = (float) (0.3 * std::cos(2.0 * kPi * hz * (double) i / kRate));
  }
  const float* channels[2] = {l.data(), r.data()};
  sonore::AudioThumbnail t;
  t.build(channels, 2, frames, buckets);
  return t;
}

void testThumbnailCache() {
  using sonore::AudioThumbnail;
  using sonore::AudioThumbnailCache;

  section("AudioThumbnail: to bytes and back");
  {
    const AudioThumbnail t = thumbOf(220.0, 300);
    std::vector<uint8_t> bytes;
    t.saveTo(bytes);
    check(bytes.size() == 16 + 2 * 300 * 12, "16 bytes of header, 12 per bucket per channel");
    AudioThumbnail back;
    check(back.loadFrom(bytes.data(), bytes.size()) && back == t &&
              back.numChannels() == 2 && back.numBuckets() == 300,
          "loaded back: every bucket identical");
    const AudioThumbnail other = thumbOf(440.0, 50);
    AudioThumbnail kept = other;
    std::vector<uint8_t> bad = bytes;
    bad.pop_back();
    check(!kept.loadFrom(bad.data(), bad.size()) && kept == other, "one byte short: refused, unchanged");
    bad = bytes;
    bad[0] = 'X';
    check(!kept.loadFrom(bad.data(), bad.size()) && kept == other, "wrong magic: refused");
    bad = bytes;
    bad[12] = 0xff;
    bad[13] = 0xff;
    bad[14] = 0xff;
    bad[15] = 0x7f; // two billion buckets claimed
    check(!kept.loadFrom(bad.data(), bad.size()) && kept == other, "a bucket count the size cannot hold: refused");
    bad = bytes;
    const uint32_t nanBits = 0x7fc00000u;
    std::memcpy(&bad[16], &nanBits, 4);
    check(!kept.loadFrom(bad.data(), bad.size()) && kept == other, "a NaN in a bucket: refused");
    check(!kept.loadFrom(nullptr, 0), "nothing: refused");
  }

  section("AudioThumbnail: JSON under a comma locale");
  {
    // A German or Spanish host calls setlocale(), and %f then prints "0,5000":
    // in a JSON array that is two numbers, and after "peak" a syntax error.
    const char* commaLocales[] = {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "de-DE",
                                  "German_Germany.1252", "German", "fr_FR.UTF-8"};
    const char* chosen = nullptr;
    for (const char* name : commaLocales)
      if (std::setlocale(LC_NUMERIC, name)) {
        chosen = name;
        break;
      }
    if (chosen) {
      std::vector<float> half(100, 0.5f);
      const float* one[1] = {half.data()};
      AudioThumbnail t;
      t.build(one, 1, half.size(), 2);
      const std::string json = t.toJson();
      std::setlocale(LC_NUMERIC, "C");
      const std::string want =
          "{\"channels\":[{\"low\":[0.5,0.5],\"high\":[0.5,0.5],\"rms\":[0.5,0.5]}],"
          "\"buckets\":2,\"peak\":0.5}";
      std::string line = std::string("under ") + chosen + ", two buckets of 0.5 are two numbers each: " + json;
      check(json == want, line.c_str());
    } else {
      std::printf("  --   no comma locale installed; the locale check is skipped\n");
    }
  }

  section("AudioThumbnail: a file with a NaN or an infinity in it");
  {
    // A float WAV can hold anything. A NaN made its bucket's rms NaN and an
    // infinity its high and rms infinite: JSON with nan and inf in it, which
    // no page parses, and bytes loadFrom refuses -- so a cache holding that
    // one thumbnail saved fine and then refused to load at all, every other
    // waveform in it lost with it.
    std::vector<float> bad(100, 0.25f);
    bad[10] = std::numeric_limits<float>::quiet_NaN();
    bad[60] = std::numeric_limits<float>::infinity();
    bad[70] = -std::numeric_limits<float>::infinity();
    const float* one[1] = {bad.data()};
    AudioThumbnail t;
    t.build(one, 1, bad.size(), 2);
    const std::string json = t.toJson();
    std::string line = "its JSON holds only numbers: " + json.substr(0, 120);
    check(json.find("nan") == std::string::npos && json.find("inf") == std::string::npos &&
              json.find("NAN") == std::string::npos && json.find("INF") == std::string::npos,
          line.c_str());
    const sonore::ThumbnailBucket first = t.at(0, 0), second = t.at(0, 1);
    check(std::isfinite(first.rms) && first.low == 0.0f && first.high == 0.25f,
          "a NaN counts as silence in its bucket");
    check(second.high == (std::numeric_limits<float>::max)() &&
              second.low == -(std::numeric_limits<float>::max)() && std::isfinite(second.rms),
          "an infinity is the largest float, keeping its sign");
    std::vector<uint8_t> bytes;
    t.saveTo(bytes);
    AudioThumbnail back;
    check(back.loadFrom(bytes.data(), bytes.size()) && back == t, "and it loads back from its own bytes");
    AudioThumbnailCache cache;
    cache.storeThumb(thumbOf(220.0, 64), 1);
    cache.storeThumb(t, 2);
    cache.saveTo(bytes);
    AudioThumbnailCache next;
    check(next.loadFrom(bytes.data(), bytes.size()) && next.size() == 2,
          "a saved cache holding it loads again, both waveforms in it");
  }

  section("AudioThumbnail: built from interleaved audio, as from planar");
  {
    // buildInterleaved takes a decoded file's samples as they come. Its
    // buckets must be build()'s on the channels pulled apart, to the bit:
    // the same samples in the same order, NaN, infinities, -0.0 and more
    // buckets than frames included, in mono, stereo and three channels.
    uint32_t seed = 7;
    auto noise = [&seed]() {
      seed = seed * 1664525u + 1013904223u;
      return (float) (int32_t) seed / 2147483648.0f;
    };
    int same = 0, cases = 0;
    for (uint32_t channels : {1u, 2u, 3u})
      for (uint32_t frames : {1u, 7u, 1000u, 44101u})
        for (uint32_t buckets : {1u, 3u, 256u, 2048u, 50000u}) {
          std::vector<float> interleaved((size_t) frames * channels);
          std::vector<std::vector<float>> planar(channels, std::vector<float>(frames));
          for (uint32_t i = 0; i < frames; ++i)
            for (uint32_t c = 0; c < channels; ++c) {
              float v = noise() * (float) (c + 1);
              if (i % 997 == 5) v = std::numeric_limits<float>::quiet_NaN();
              if (i % 1499 == 9) v = (c & 1) ? -std::numeric_limits<float>::infinity()
                                            : std::numeric_limits<float>::infinity();
              if (i % 211 == 3) v = (i & 1) ? -0.0f : 0.0f;
              interleaved[(size_t) i * channels + c] = v;
              planar[c][i] = v;
            }
          std::vector<const float*> pointers;
          for (auto& p : planar) pointers.push_back(p.data());
          AudioThumbnail a, b;
          a.build(pointers.data(), channels, frames, buckets);
          b.buildInterleaved(interleaved.data(), channels, frames, buckets);
          std::vector<uint8_t> x, y;
          a.saveTo(x);
          b.saveTo(y);
          ++cases;
          if (x == y && a == b && b.numBuckets() == a.numBuckets()) ++same;
        }
    char line[128];
    std::snprintf(line, sizeof(line), "buildInterleaved == build, byte for byte, in %d of %d cases", same, cases);
    check(same == cases, line);
    AudioThumbnail empty = thumbOf(220.0, 10);
    empty.buildInterleaved(nullptr, 2, 100, 10);
    check(empty.empty() && empty.numBuckets() == 0, "no samples: an empty thumbnail, as build gives");
  }

  section("AudioThumbnailCache: a byte budget, least recently used out first");
  {
    const AudioThumbnail a = thumbOf(100.0, 200), b = thumbOf(200.0, 200), c = thumbOf(300.0, 200),
                         e = thumbOf(500.0, 200);
    const size_t cost = a.memoryBytes() + AudioThumbnailCache::kEntryOverhead;
    AudioThumbnailCache cache(cost * 3);
    check(cache.storeThumb(a, 1) && cache.storeThumb(b, 2) && cache.storeThumb(c, 3),
          "three fit in a budget of three");
    check(cache.size() == 3 && cache.bytesUsed() == cost * 3, "bytesUsed() counts buckets and overhead");
    AudioThumbnail got;
    check(cache.loadThumb(got, 1) && got == a, "loadThumb returns what was stored");
    check(cache.storeThumb(e, 5) && !cache.containsThumb(2) && cache.containsThumb(1),
          "a fourth evicts the least recently USED (2), not the oldest stored (1, just looked up)");
    check(cache.bytesUsed() <= cache.maxBytes(), "never over budget");
    AudioThumbnail untouched = e;
    check(!cache.loadThumb(untouched, 2) && untouched == e, "a miss leaves the destination alone");
    const AudioThumbnail huge = thumbOf(50.0, 20000, 100000);
    check(!cache.storeThumb(huge, 9) && cache.size() == 3, "one thumbnail bigger than the budget: refused, nothing evicted");
    check(!cache.storeThumb(AudioThumbnail(), 10), "an empty thumbnail: refused");
    check(cache.storeThumb(b, 1) && cache.size() == 3 && cache.loadThumb(got, 1) && got == b,
          "storing under an existing key replaces it");
    cache.setMaxBytes(cost);
    check(cache.size() == 1 && cache.containsThumb(1), "shrinking the budget evicts at once, keeping the most recent");
    check(cache.removeThumb(1) && cache.size() == 0 && cache.bytesUsed() == 0, "removeThumb");
    cache.setMaxBytes(cost * 10);
    cache.storeThumb(a, 1);
    cache.clear();
    check(cache.size() == 0 && cache.bytesUsed() == 0, "clear");
  }

  section("AudioThumbnailCache: the whole cache to bytes and back");
  {
    AudioThumbnailCache cache(1u << 20);
    std::vector<AudioThumbnail> thumbs;
    for (int i = 0; i < 6; ++i) {
      thumbs.push_back(thumbOf(100.0 + 50.0 * i, 64 + (uint32_t) i * 10));
      cache.storeThumb(thumbs.back(), 1000 + (uint64_t) i);
    }
    AudioThumbnail tmp;
    cache.loadThumb(tmp, 1000); // most recent now: 1000, then 1005, 1004...
    std::vector<uint8_t> bytes;
    cache.saveTo(bytes);
    AudioThumbnailCache copy(1u << 20);
    check(copy.loadFrom(bytes.data(), bytes.size()) && copy.size() == 6 &&
              copy.bytesUsed() == cache.bytesUsed(),
          "six thumbnails reloaded, the same bytes in use");
    bool all = true;
    for (int i = 0; i < 6; ++i) {
      AudioThumbnail t;
      all = all && copy.loadThumb(t, 1000 + (uint64_t) i) && t == thumbs[(size_t) i];
    }
    check(all, "each one identical, under its own key");
    // Recency survives the trip: reload into a budget of two, which keeps the
    // two most recent at save time -- 1000 (looked up last) and 1005.
    const size_t two = thumbs[0].memoryBytes() + thumbs[5].memoryBytes() +
                       2 * AudioThumbnailCache::kEntryOverhead;
    AudioThumbnailCache tight(two);
    check(tight.loadFrom(bytes.data(), bytes.size()) && tight.size() == 2 &&
              tight.containsThumb(1000) && tight.containsThumb(1005),
          "into a smaller budget: the most recently used survive");
    AudioThumbnailCache guard(1u << 20);
    guard.storeThumb(thumbs[0], 77);
    std::vector<uint8_t> bad = bytes;
    bad.resize(bad.size() - 5);
    check(!guard.loadFrom(bad.data(), bad.size()) && guard.size() == 1 && guard.containsThumb(77),
          "truncated: refused, and the cache keeps what it had");
    bad = bytes;
    bad[8] = 0xff;
    bad[9] = 0xff; // 65535+ entries claimed
    check(!guard.loadFrom(bad.data(), bad.size()) && guard.containsThumb(77), "an impossible entry count: refused");
    bad = bytes;
    bad.push_back(0);
    check(!guard.loadFrom(bad.data(), bad.size()), "a trailing byte: refused");
  }

  section("AudioThumbnailCache: keys");
  {
    check(AudioThumbnailCache::hashData("", 0) == 0xcbf29ce484222325ull, "FNV-1a 64 of \"\"");
    check(AudioThumbnailCache::hashData("a", 1) == 0xaf63dc4c8601ec8cull, "FNV-1a 64 of \"a\"");
    check(AudioThumbnailCache::hashData("foobar", 6) == 0x85944171f73967e8ull, "FNV-1a 64 of \"foobar\"");
    const std::string path = utf8TempPath("keyed é.wav");
    const std::vector<float> s = stereoSine(1000);
    sonore::writeWav(path.c_str(), s.data(), 1000, 2, kRate);
    const uint64_t k1 = AudioThumbnailCache::hashFile(path.c_str());
    check(k1 != 0 && k1 == AudioThumbnailCache::hashFile(path.c_str()), "a file's key is stable");
    sonore::writeWav(path.c_str(), s.data(), 900, 2, kRate);
    check(AudioThumbnailCache::hashFile(path.c_str()) != k1, "and changes when the file does");
    check(AudioThumbnailCache::hashFile(sonoretest::tempPath("absent.wav")) == 0, "a missing file keys to 0");
  }

  section("AudioThumbnailCache: two threads at once");
  {
    AudioThumbnailCache cache(64 * 1024);
    const AudioThumbnail t = thumbOf(330.0, 100);
    std::atomic<bool> bad{false};
    auto worker = [&](uint64_t base) {
      AudioThumbnail got;
      for (int i = 0; i < 4000; ++i) {
        const uint64_t key = base + (uint64_t) (i % 37);
        if (i % 3 == 0) cache.storeThumb(t, key);
        else if (cache.loadThumb(got, key) && got != t) bad = true;
        if (cache.bytesUsed() > cache.maxBytes()) bad = true;
      }
    };
    std::thread one(worker, 0), two(worker, 20);
    one.join();
    two.join();
    check(!bad, "8000 interleaved stores and lookups: every hit intact, never over budget");
  }
}

void testSystemVolume() {
  section("SystemAudioVolume");
  using sonore::SystemAudioVolume;
  const bool available = SystemAudioVolume::isAvailable();
  const float gain = SystemAudioVolume::getGain();
  std::printf("  ---- this machine: %s, gain %.4f, %s\n", available ? "available" : "not available",
              (double) gain, SystemAudioVolume::isMuted() ? "muted" : "not muted");
  if (available) check(gain >= 0.0f && gain <= 1.0f, "the gain reads as 0..1");
  else check(gain < 0.0f, "unavailable reads as -1");

  if (sonore::pathio::environment("SONORE_TEST_SYSTEM_VOLUME") != "1") {
    std::printf("  ---- set SONORE_TEST_SYSTEM_VOLUME=1 to change and restore it (never in CI)\n");
    return;
  }
  if (!available) {
    check(false, "SONORE_TEST_SYSTEM_VOLUME=1 but there is no volume to change");
    return;
  }
  const float savedGain = gain;
  const bool savedMute = SystemAudioVolume::isMuted();
  // Restored on every way out of this block, failure included.
  struct Restore {
    float g;
    bool m;
    ~Restore() {
      SystemAudioVolume::setGain(g);
      SystemAudioVolume::setMuted(m);
    }
  } restore{savedGain, savedMute};
  const float target = savedGain > 0.5f ? savedGain - 0.2f : savedGain + 0.2f;
  check(SystemAudioVolume::setGain(target), "setGain");
  char line[160];
  std::snprintf(line, sizeof(line), "reads back what was set (%.4f, set %.4f)",
                (double) SystemAudioVolume::getGain(), (double) target);
  check(std::fabs(SystemAudioVolume::getGain() - target) < 0.01f, line);
  check(SystemAudioVolume::setMuted(!savedMute) && SystemAudioVolume::isMuted() == !savedMute,
        "mute toggles and reads back");
  check(SystemAudioVolume::setGain(savedGain) && SystemAudioVolume::setMuted(savedMute),
        "restored");
  std::snprintf(line, sizeof(line), "exactly: gain %.6f (was %.6f), mute %d (was %d)",
                (double) SystemAudioVolume::getGain(), (double) savedGain,
                (int) SystemAudioVolume::isMuted(), (int) savedMute);
  check(SystemAudioVolume::getGain() == savedGain && SystemAudioVolume::isMuted() == savedMute,
        line);
}

/** The READ TOC response for a six-track disc whose MSF table MusicBrainz
 *  publishes (offsets 150 15363 32314 46592 63414 80489, lead-out 95462), in
 *  the LBA form IOCTL_CDROM_READ_TOC_EX returns -- every offset less 150. */
std::vector<uint8_t> knownToc(uint8_t lastControl = 0x00) {
  const int32_t lba[] = {0, 15213, 32164, 46442, 63264, 80339};
  std::vector<uint8_t> t;
  const size_t length = 2 + 8 * 7; // first/last + seven descriptors
  t.push_back((uint8_t) (length >> 8));
  t.push_back((uint8_t) length);
  t.push_back(1);
  t.push_back(6);
  auto descriptor = [&](uint8_t control, uint8_t number, int32_t address) {
    t.insert(t.end(), {0, (uint8_t) (0x10 | control), number, 0, (uint8_t) (address >> 24),
                       (uint8_t) (address >> 16), (uint8_t) (address >> 8), (uint8_t) address});
  };
  for (int i = 0; i < 6; ++i) descriptor(i == 5 ? lastControl : 0x00, (uint8_t) (i + 1), lba[i]);
  descriptor(0x00, 0xAA, 95312);
  t.resize(804, 0); // the whole CDROM_TOC buffer, as the drive fills it
  return t;
}

void testAudioCd() {
  using sonore::CDToc;
  section("audio CD: the table of contents");
  {
    const std::vector<uint8_t> raw = knownToc();
    CDToc toc;
    check(CDToc::parse(raw.data(), raw.size(), &toc), "a six-track disc parses");
    check(toc.firstTrack == 1 && toc.lastTrack == 6 && toc.tracks.size() == 6 &&
              toc.leadOutSector == 95312,
          "tracks 1..6, lead-out at sector 95312");
    check(toc.tracks[3].startSector == 46442 && toc.tracks[3].isAudio, "track 4 at sector 46442, audio");
    check(toc.trackStartSample(1) == 15213LL * 588 && toc.trackStartSample(0) == 0,
          "track starts in samples: 588 stereo frames a sector, from the first track");
    check(toc.lengthInSamples() == 95312LL * 588, "the disc's length: lead-out times 588");
    check(toc.trackStartSample(7) == -1, "past the end: -1");
    check(CDToc::msfToSector(0, 2, 0) == 0 && CDToc::msfToSector(1, 0, 0) == 4350 &&
              CDToc::msfToSector(21, 12, 62) == 95312,
          "MSF 00:02:00 is sector 0; 21:12:62 is the lead-out");
    // freedb's algorithm by hand: start seconds 2 204 430 621 845 1073 have
    // digit sums 2+6+7+9+17+11 = 52; length 1272 - 2 = 1270 s; six tracks.
    char line[120];
    std::snprintf(line, sizeof(line), "CDDB id 0x%08X (by hand: 0x3404F606)", toc.cddbId());
    check(toc.cddbId() == 0x3404F606u, line);
    const std::vector<uint8_t> enhanced = knownToc(0x04);
    CDToc e;
    check(CDToc::parse(enhanced.data(), enhanced.size(), &e) && !e.tracks[5].isAudio &&
              e.tracks[4].isAudio,
          "CONTROL bit 2 marks a data track (an Enhanced CD's last)");
  }
  {
    CDToc toc;
    toc.firstTrack = 99;
    const CDToc before = toc;
    auto refused = [&](std::vector<uint8_t> raw, const char* what) {
      check(!CDToc::parse(raw.data(), raw.size(), &toc) && toc.firstTrack == 99, what);
    };
    std::vector<uint8_t> raw = knownToc();
    refused(std::vector<uint8_t>(raw.begin(), raw.begin() + 40), "cut short: refused, output untouched");
    raw = knownToc();
    raw[2] = 7;
    refused(raw, "first track after the last: refused");
    raw = knownToc();
    raw[4 + 8 * 2 + 2] = 9;
    refused(raw, "track numbers out of order: refused");
    raw = knownToc();
    raw[4 + 8 * 3 + 6] = 0;
    raw[4 + 8 * 3 + 7] = 0; // track 4 starts before track 3
    refused(raw, "an address that goes backwards: refused");
    raw = knownToc();
    raw[4 + 8 * 6 + 2] = 7;
    refused(raw, "no lead-out: refused");
    raw = knownToc();
    raw[4 + 8 * 6 + 4] = 0;
    raw[4 + 8 * 6 + 5] = 0;
    raw[4 + 8 * 6 + 6] = 0;
    raw[4 + 8 * 6 + 7] = 1; // lead-out before the last track
    refused(raw, "a lead-out before the last track: refused");
    raw = knownToc();
    raw[0] = 0x10; // claims 4 KB
    refused(raw, "a length past the buffer: refused");
    raw = knownToc();
    raw[4 + 4] = 0x80; // a negative address
    refused(raw, "a negative address: refused");
    raw = knownToc();
    raw[4 + 8 * 6 + 4] = 0x7F;
    raw[4 + 8 * 6 + 5] = raw[4 + 8 * 6 + 6] = raw[4 + 8 * 6 + 7] = 0xFF; // lead-out at 0x7FFFFFFF
    refused(raw, "a lead-out past 99:59:74, the last address MSF can say: refused");
    raw = knownToc();
    const int32_t last = CDToc::msfToSector(99, 59, 74);
    raw[4 + 8 * 6 + 4] = (uint8_t) (last >> 24);
    raw[4 + 8 * 6 + 5] = (uint8_t) (last >> 16);
    raw[4 + 8 * 6 + 6] = (uint8_t) (last >> 8);
    raw[4 + 8 * 6 + 7] = (uint8_t) last;
    check(CDToc::parse(raw.data(), raw.size(), &toc) && toc.leadOutSector == 449849,
          "a lead-out at 99:59:74 itself parses");
    (void) before;
  }

  section("audio CD: preparing a track for the burner");
  {
    std::vector<float> s(44100 * 2);
    s[0] = 1.0f;
    s[1] = -1.0f;
    s[2] = 0.5f;
    s[3] = -0.5f;
    s[4] = 2.0f; // clipped
    s[5] = std::nanf("");
    const std::vector<uint8_t> b = sonore::cddetail::toCdAudio(s.data(), 44100, 2);
    check(b.size() == 300u * 2352u, "one second becomes the Red Book's four, in whole sectors");
    auto at = [&](size_t i) { return (int16_t) (uint16_t) (b[i * 2] | (b[i * 2 + 1] << 8)); };
    check(at(0) == 32767 && at(1) == -32767 && at(2) == 16384 && at(3) == -16384,
          "full scale and half scale, little-endian, rounded half away from zero");
    check(at(4) == 32767 && at(5) == 0, "over full scale clips; NaN is silence");
    const std::vector<float> longer = stereoSine(300 * 588 + 1);
    check(sonore::cddetail::toCdAudio(longer.data(), 300 * 588 + 1, 2).size() == 301u * 2352u,
          "a frame past a sector boundary pads to the next whole sector");
    std::vector<float> mono(1000, 0.25f);
    const std::vector<uint8_t> m = sonore::cddetail::toCdAudio(mono.data(), 1000, 1);
    std::vector<float> back(1000 * 2);
    sonore::cddetail::fromCdAudio(m.data(), 1000, back.data());
    check(std::fabs(back[0] - 0.25f) < 1.0f / 32768 && back[0] == back[1],
          "mono goes to both channels, and reads back within one step");
  }

  section("audio CD: drives (this machine)");
  {
    const std::vector<std::string> drives = sonore::AudioCDReader::getAvailableCDNames();
    const std::vector<std::string> burners = sonore::AudioCDBurner::findAvailableDevices();
    std::printf("  ---- %zu CD drive(s), %zu recorder(s)\n", drives.size(), burners.size());
    std::string err;
    check(!sonore::AudioCDReader::createReaderForCD(-1, &err) && !err.empty(),
          "a negative drive index: no reader, a reason");
    check(!sonore::AudioCDReader::createReaderForCD((int) drives.size(), &err) && !err.empty(),
          "an index past the last drive: no reader, a reason");
    check(!sonore::AudioCDBurner::openDevice((int) burners.size(), &err) && !err.empty(),
          "a recorder index past the last: no burner, a reason");
    if (drives.empty()) {
      sonore::AudioCDReader::createReaderForCD(0, &err);
#if defined(_WIN32)
      check(err == "this machine has no CD drive", "with no drive at all, it says exactly that");
#else
      check(!err.empty(), "off Windows, it says reading CDs is Windows-only");
#endif
    } else {
      std::unique_ptr<sonore::AudioCDReader> reader = sonore::AudioCDReader::createReaderForCD(0, &err);
      if (reader) {
        std::printf("  ---- %s: %d tracks, CDDB %08X\n", reader->name().c_str(), reader->getNumTracks(),
                    reader->getCDDBId());
        check(reader->getNumTracks() > 0 && reader->lengthInSamples() > 0, "a disc in drive 0 has a TOC");
      } else {
        std::printf("  ---- drive 0: %s\n", err.c_str());
        check(!err.empty(), "drive 0 has no readable disc, and says why");
      }
    }
  }
}

int realMain(const std::vector<std::string>& args) {
  if (!sonore::pathio::environment("SONORE_AUDIO_IO_FAKE_LAME").empty()) return fakeLame(args);
  int corruptIterations = 30;
  for (size_t i = 1; i + 1 < args.size(); ++i)
    if (args[i] == "--corrupt-iterations") corruptIterations = std::atoi(args[i + 1].c_str());

  std::printf("audio_io_test\n");
  testSystemDecoders(corruptIterations);
  testLame();
  testThumbnailCache();
  testSystemVolume();
  testAudioCd();

  std::printf("\n%d checks, %d failed\n%s\n", g_checks, g_failures,
              g_failures ? "AUDIO IO TESTS FAILED" : "AUDIO IO TESTS PASSED");
  return g_failures ? 1 : 0;
}

} // namespace

#if defined(_WIN32)
// wmain: the fake encoder must see its arguments as the C runtime split them
// from the command line, in full Unicode -- that IS the quoting test.
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i) args.push_back(sonore::pathio::narrow(argv[i]));
  wchar_t self[32768];
  const DWORD n = GetModuleFileNameW(nullptr, self, 32768);
  g_self = n > 0 && n < 32768 ? sonore::pathio::narrow(self) : args[0];
  return realMain(args);
}
#else
int main(int argc, char** argv) {
  std::vector<std::string> args(argv, argv + argc);
  char resolved[PATH_MAX];
  g_self = realpath(argv[0], resolved) ? std::string(resolved) : std::string(argv[0]);
  return realMain(args);
}
#endif
