// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: WAV files.
//
// A reader and a writer for the one format that matters in practice: RIFF/WAVE
// carrying 16-bit PCM, 24-bit PCM or 32-bit float. That covers what users
// actually drop on a plugin (impulse responses, samples) and what a standalone
// build renders out. Compressed formats are somebody else's job: the OS ships
// codecs, and a plugin that decodes MP3 itself is carrying liability, not value.
//
// Offline-only by design: these functions allocate and do file IO, so they
// belong in prepare()-time code and tools, never in process().
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace sonore {

/** Decoded audio: interleaved floats, -1..1, plus the facts about them. */
struct WavData {
  std::vector<float> samples; // interleaved
  uint32_t sampleRate = 0;
  uint16_t numChannels = 0;
  size_t numFrames() const {
    return numChannels ? samples.size() / numChannels : 0;
  }
};

/**
 * The most audio any decoder in this SDK will materialise from one file.
 *
 * A crash-prevention ceiling, not a policy: several of these formats carry a
 * DECLARED length in a header (FLAC STREAMINFO, an Ogg granule position) that a
 * crafted file sets to billions, and `reserve`/`assign` on that number asks the
 * allocator for hundreds of gigabytes -> std::bad_alloc -> the host goes down.
 * A file claiming more than this is refused outright rather than truncated,
 * because a silently shortened sample is a corruption the user cannot see.
 *
 * 256 Mi samples is ~1 GB at 4 bytes -- ~46 min of 48 kHz stereo, ~11 min of
 * 192 kHz stereo -- past any real impulse response or one-shot, and small
 * enough that even hitting it does not itself OOM a DAW machine.
 */
static constexpr size_t kMaxDecodedSamples = 256u * 1024u * 1024u;

namespace wavdetail {

inline uint32_t readU32(const uint8_t* p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
         ((uint32_t) p[3] << 24);
}
inline uint16_t readU16(const uint8_t* p) { return (uint16_t) (p[0] | (p[1] << 8)); }

inline void writeU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back((uint8_t) (v & 0xff));
  out.push_back((uint8_t) ((v >> 8) & 0xff));
  out.push_back((uint8_t) ((v >> 16) & 0xff));
  out.push_back((uint8_t) ((v >> 24) & 0xff));
}
inline void writeU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back((uint8_t) (v & 0xff));
  out.push_back((uint8_t) ((v >> 8) & 0xff));
}

} // namespace wavdetail

/**
 * Read a WAV file. Returns false, leaving `out` empty, on anything it does
 * not understand, rather than guessing at bytes: a wrong guess here becomes a
 * blast of full-scale noise through a user's monitors.
 */
inline bool readWav(const char* path, WavData* out) {
  if (!path || !out) return false;
  out->samples.clear();
  out->sampleRate = 0;
  out->numChannels = 0;

  std::FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  // 1 GB of WAV is not an impulse response; refuse rather than swallow RAM.
  if (size < 44 || size > (long) (1u << 30)) {
    std::fclose(f);
    return false;
  }
  std::vector<uint8_t> bytes((size_t) size);
  const size_t got = std::fread(bytes.data(), 1, (size_t) size, f);
  std::fclose(f);
  if (got != (size_t) size) return false;

  using namespace wavdetail;
  if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
    return false;

  // Walk the chunks. Real files carry LIST/INFO/bext chunks before data, so
  // assuming fixed offsets (the classic shortcut) breaks on real material.
  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  const uint8_t* data = nullptr;
  uint32_t dataSize = 0;

  size_t pos = 12;
  while (pos + 8 <= bytes.size()) {
    const uint8_t* header = bytes.data() + pos;
    const uint32_t chunkSize = readU32(header + 4);
    const size_t body = pos + 8;
    if (body + chunkSize > bytes.size()) break;

    if (std::memcmp(header, "fmt ", 4) == 0 && chunkSize >= 16) {
      format = readU16(bytes.data() + body);
      channels = readU16(bytes.data() + body + 2);
      rate = readU32(bytes.data() + body + 4);
      bits = readU16(bytes.data() + body + 14);
      // WAVE_FORMAT_EXTENSIBLE wraps the real format in a GUID whose first two
      // bytes are the classic tag.
      if (format == 0xFFFE && chunkSize >= 40) format = readU16(bytes.data() + body + 24);
    } else if (std::memcmp(header, "data", 4) == 0) {
      data = bytes.data() + body;
      dataSize = chunkSize;
    }
    pos = body + chunkSize + (chunkSize & 1); // chunks are word-aligned
  }

  if (!data || !rate || !channels || channels > 32) return false;

  // Unsigned 8-bit and 32-bit integer PCM are read too: both turn up in
  // sample libraries (old drum machines, Pro Tools bounces), and refusing
  // them read as "the sampler cannot open this WAV" for a file every other
  // program plays.
  const bool pcm8 = format == 1 && bits == 8;
  const bool pcm16 = format == 1 && bits == 16;
  const bool pcm24 = format == 1 && bits == 24;
  const bool pcm32 = format == 1 && bits == 32;
  const bool float32 = format == 3 && bits == 32;
  if (!pcm8 && !pcm16 && !pcm24 && !pcm32 && !float32) return false;

  const uint32_t bytesPerSample = bits / 8;
  const size_t count = dataSize / bytesPerSample;
  out->samples.resize(count);
  out->sampleRate = rate;
  out->numChannels = channels;

  if (pcm8) {
    for (size_t i = 0; i < count; ++i) out->samples[i] = ((float) data[i] - 128.0f) / 128.0f;
  } else if (pcm32) {
    for (size_t i = 0; i < count; ++i)
      out->samples[i] = (float) ((double) (int32_t) readU32(data + i * 4) / 2147483648.0);
  } else if (pcm16) {
    for (size_t i = 0; i < count; ++i) {
      const int16_t v = (int16_t) readU16(data + i * 2);
      out->samples[i] = (float) v / 32768.0f;
    }
  } else if (pcm24) {
    for (size_t i = 0; i < count; ++i) {
      const uint8_t* p = data + i * 3;
      int32_t v = (int32_t) ((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 24);
      v >>= 8; // sign-extend from the top
      out->samples[i] = (float) v / 8388608.0f;
    }
  } else {
    // float32: byte order is little-endian on every target we ship to.
    // Copy `count * bytesPerSample`, NOT the raw chunk size. count truncates
    // dataSize down to a whole number of samples, so a chunk whose size is not
    // a multiple of 4 allocated `count` floats but memcpy'ing `dataSize` bytes
    // wrote 1-3 bytes PAST the buffer -- a controllable heap overflow from a
    // downloaded sample. The trailing partial sample is discarded, which is
    // the only correct thing to do with it. The guard also covers a
    // zero-length chunk, where an empty vector's data() may be null and memcpy
    // declares both pointers nonnull even at size 0.
    const size_t copyBytes = count * bytesPerSample;
    if (copyBytes > 0) std::memcpy(out->samples.data(), data, copyBytes);
  }
  return true;
}

/**
 * Write interleaved floats as a 32-bit float WAV. Float, not 16-bit PCM,
 * because the standalone's renders are for INSPECTION: quantising the thing
 * you are trying to measure defeats the purpose.
 */
inline bool writeWav(const char* path, const float* interleaved, size_t numFrames,
                     uint16_t numChannels, uint32_t sampleRate) {
  if (!path || !interleaved || !numChannels || !sampleRate) return false;
  using namespace wavdetail;

  // RIFF sizes are 32-bit. Past 4 GB the header would wrap and every reader
  // would see a file whose chunks disagree with its length; refused instead.
  const uint64_t wide = (uint64_t) numFrames * numChannels * sizeof(float);
  if (wide > 0xFFFFFFFFull - 44ull) return false;
  const uint32_t dataBytes = (uint32_t) wide;
  std::vector<uint8_t> out;
  out.reserve(44 + dataBytes);

  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  writeU32(out, 36 + dataBytes);
  out.insert(out.end(), {'W', 'A', 'V', 'E'});
  out.insert(out.end(), {'f', 'm', 't', ' '});
  writeU32(out, 16);
  writeU16(out, 3); // IEEE float
  writeU16(out, numChannels);
  writeU32(out, sampleRate);
  writeU32(out, sampleRate * numChannels * (uint32_t) sizeof(float));
  writeU16(out, (uint16_t) (numChannels * sizeof(float)));
  writeU16(out, 32);
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  writeU32(out, dataBytes);

  const auto* p = (const uint8_t*) interleaved;
  out.insert(out.end(), p, p + dataBytes);

  std::FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const size_t wrote = std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
  return wrote == out.size();
}

} // namespace sonore
