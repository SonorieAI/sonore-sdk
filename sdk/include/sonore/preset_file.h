// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: presets as FILES, in the format the rest of the world uses.
//
// A plugin's factory presets live inside it, and the hosting API can already
// list and load them. That is not where most presets are. People have folders
// of .vstpreset files: bought, downloaded, swapped, saved from another host
// years ago. A host that cannot open one is a host that starts empty however
// much the user already owns.
//
// The container is documented and small. A header naming the plugin, one or
// more chunks, and a list at the end saying where each chunk is:
//
//   "VST3" | version | class id, 32 hex chars | int64 offset of the list
//   ...chunk data...
//   "List" | int32 count | entries of { id[4], int64 offset, int64 size }
//
// Everything is little-endian, which is worth stating because the format is
// Steinberg's and AIFF-style big-endian would be the other reasonable guess.
//
// The chunk that matters is "Comp", the component state: the same bytes
// getState() hands over. "Cont" is the controller's own copy and "Info" is an
// XML blurb; both are optional, and this writes Comp only. A preset that
// carries a controller chunk is read and its Comp used, because the component
// state is what actually decides the sound.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sonore {

/** What came out of a .vstpreset: which plugin it is for, and its state. */
struct Vst3PresetFile {
  /** The plugin's class id as 32 uppercase hex characters, which is exactly
   *  how PluginDescription::id spells it. A preset for a different plugin is
   *  not an error to read and IS an error to apply, so the two are separate
   *  steps and this is what a caller compares. */
  std::string classId;
  std::vector<uint8_t> componentState;
  std::vector<uint8_t> controllerState;
};

namespace presetfile {

inline void putU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back((uint8_t) (v & 0xff));
  out.push_back((uint8_t) ((v >> 8) & 0xff));
  out.push_back((uint8_t) ((v >> 16) & 0xff));
  out.push_back((uint8_t) ((v >> 24) & 0xff));
}

inline void putU64(std::vector<uint8_t>& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back((uint8_t) ((v >> (i * 8)) & 0xff));
}

inline uint32_t getU32(const uint8_t* p) {
  return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
         ((uint32_t) p[3] << 24);
}

inline uint64_t getU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= (uint64_t) p[i] << (i * 8);
  return v;
}

} // namespace presetfile

/**
 * Write a .vstpreset holding one plugin's component state.
 *
 * `classId` must be the 32 hex characters that identify the plugin, because
 * that is the only thing in the file that says what it is for: a host
 * opening it has no other way to know, and applying a preset for the wrong
 * plugin is how a delay ends up with a filter's numbers in it.
 */
inline bool writeVstPreset(const char* path, const std::string& classId,
                           const std::vector<uint8_t>& componentState) {
  if (!path || classId.size() != 32) return false;
  using namespace presetfile;

  std::vector<uint8_t> out;
  const char* magic = "VST3";
  out.insert(out.end(), magic, magic + 4);
  putU32(out, 1); // version
  out.insert(out.end(), classId.begin(), classId.end());

  // The list offset has to be written before the data whose length decides
  // it, so a placeholder goes in and is filled once the size is known.
  const size_t listOffsetAt = out.size();
  putU64(out, 0);

  const uint64_t dataStart = (uint64_t) out.size();
  out.insert(out.end(), componentState.begin(), componentState.end());
  const uint64_t listOffset = (uint64_t) out.size();

  const char* listMagic = "List";
  out.insert(out.end(), listMagic, listMagic + 4);
  putU32(out, 1); // one entry
  const char* compId = "Comp";
  out.insert(out.end(), compId, compId + 4);
  putU64(out, dataStart);
  putU64(out, (uint64_t) componentState.size());

  for (int i = 0; i < 8; ++i) out[listOffsetAt + (size_t) i] = (uint8_t) ((listOffset >> (i * 8)) & 0xff);

  std::FILE* file = std::fopen(path, "wb");
  if (!file) return false;
  const size_t written = std::fwrite(out.data(), 1, out.size(), file);
  std::fclose(file);
  return written == out.size();
}

/**
 * Read a .vstpreset.
 *
 * Refuses anything it cannot make sense of rather than returning a partial
 * answer: a chunk whose offset points past the end of the file, a list whose
 * count does not fit, a header that is not a preset at all. A preset half
 * applied is worse than one not applied, because the user cannot tell which
 * half.
 */
inline bool readVstPreset(const char* path, Vst3PresetFile* out) {
  if (!path || !out) return false;
  using namespace presetfile;
  out->classId.clear();
  out->componentState.clear();
  out->controllerState.clear();

  std::FILE* file = std::fopen(path, "rb");
  if (!file) return false;
  std::vector<uint8_t> bytes;
  char buffer[8192];
  size_t got = 0;
  while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
    bytes.insert(bytes.end(), buffer, buffer + got);
  std::fclose(file);

  // 4 magic + 4 version + 32 class id + 8 offset.
  if (bytes.size() < 48) return false;
  if (std::memcmp(bytes.data(), "VST3", 4) != 0) return false;
  out->classId.assign((const char*) bytes.data() + 8, 32);

  const uint64_t listOffset = getU64(bytes.data() + 40);
  // Compared as `offset > size - 8`, never `offset + 8 > size`: the offset is
  // sixty-four bits straight from the file, and a value near 2^64 wraps the
  // addition to something small, passes the bound, and is then added to the
  // buffer pointer.
  if (listOffset > (uint64_t) bytes.size() - 8) return false;
  if (std::memcmp(bytes.data() + listOffset, "List", 4) != 0) return false;

  const uint32_t count = getU32(bytes.data() + listOffset + 4);
  // Each entry is 20 bytes. A count that cannot fit in the file is a corrupt
  // file, not a reason to allocate what it claims.
  if ((uint64_t) count * 20 + listOffset + 8 > bytes.size()) return false;

  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* entry = bytes.data() + listOffset + 8 + (size_t) i * 20;
    const uint64_t offset = getU64(entry + 4);
    const uint64_t size = getU64(entry + 12);
    if (offset > bytes.size() || size > bytes.size() - offset) return false;

    const uint8_t* start = bytes.data() + offset;
    if (std::memcmp(entry, "Comp", 4) == 0)
      out->componentState.assign(start, start + size);
    else if (std::memcmp(entry, "Cont", 4) == 0)
      out->controllerState.assign(start, start + size);
    // Anything else -- "Info" and whatever a future version adds -- is
    // skipped rather than refused. A file with more in it than this reader
    // knows about is still a valid preset.
  }
  return !out->componentState.empty();
}

} // namespace sonore
