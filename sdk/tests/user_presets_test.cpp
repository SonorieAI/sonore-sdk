// SPDX-License-Identifier: Apache-2.0
// User presets: the file, the folder, the plugin, the preset bar, the host.
//
// What user_presets.h, gfx/preset_bar.h and the wrappers' preset paths
// promise, checked with real files in a private folder and a real plugin --
// the showcase example, compiled in as rt_safety_test and showcase_test do
// it, driven through its CLAP entry:
//
//   - the container: every field back, the state bit-exact, and every kind
//     of damage refused with a reason (truncation at every length, a flipped
//     bit, a newer format, sizes that lie, metadata that is not text);
//   - names: forbidden characters, reserved Windows names, leading dots and
//     trailing dots and spaces, length, Unicode -- and two names that make
//     one file name;
//   - the folder: save, the three answers to a name clash, overwrite, rename
//     (including case only), duplicate, delete to a trash and for good,
//     import (our files, .vstpreset in both byte orders of its class id,
//     another plugin's refused), export, and a rescan that sees files added
//     and removed from outside;
//   - the plugin: a preset is the host's save path's bytes exactly, it loads
//     through the host's restore path (rescan, mark_dirty), keeps the
//     session's editor size, reads an older or newer parameter count, marks
//     itself modified and clean again, browses factory then user, and its
//     selection comes back with the session;
//   - CLAP preset discovery and preset-load, driven as a host's indexer;
//   - the preset bar, headless: rendered, every menu item, the questions it
//     asks, and a preset load being ONE undo step in the showcase's APVTS;
//   - a preset loaded while audio runs: no allocation on the audio thread.
//
//   user_presets_test
//   SONORE_TEST_TRASH=1 user_presets_test   also moves a file to the real
//                                           Recycle Bin / Trash
#define SONORE_BUILD_VST3 1
#include "../examples/showcase/plugin.cpp"

#include <sonore/gfx/bitmap.h>
#include <sonore/gfx/component.h>
#include <sonore/gfx/preset_bar.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "temp_path.h"

// ── Allocations on the audio thread ──────────────────────────────────────────
// Counted only on a thread that says it is the audio thread: the main thread
// allocates freely while it loads a preset, and that is allowed.
namespace {
thread_local bool t_audioThread = false;
std::atomic<long> g_audioAllocs{0};
inline void countAlloc() {
  if (t_audioThread) g_audioAllocs.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

void* operator new(std::size_t n) {
  countAlloc();
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  countAlloc();
  return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept { return ::operator new(n, t); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace sonore;
using namespace sonore::gfx;
namespace dp = sonore::detail::presets;

int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}
void check(bool ok, const std::string& what) { check(ok, what.c_str()); }

void setEnv(const char* name, const std::string& utf8) {
#if defined(_WIN32)
  _wputenv_s(pathio::widen(name).c_str(), pathio::widen(utf8.c_str()).c_str());
#else
  setenv(name, utf8.c_str(), 1);
#endif
}

std::vector<uint8_t> readBytes(const std::string& path) {
  std::vector<uint8_t> out;
  if (std::FILE* f = pathio::open(path.c_str(), "rb")) {
    uint8_t chunk[4096];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) out.insert(out.end(), chunk, chunk + got);
    std::fclose(f);
  }
  return out;
}

bool writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::FILE* f = pathio::open(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  std::fclose(f);
  return ok;
}

/** A plausible state blob: "SNRS" and whatever follows. */
std::vector<uint8_t> fakeState(uint8_t seed, size_t size = 64) {
  std::vector<uint8_t> s = {'S', 'N', 'R', 'S'};
  for (size_t i = 4; i < size; ++i) s.push_back((uint8_t) (seed * 31u + i * 7u));
  return s;
}

/** Re-checksum a container after a test edited it on purpose. */
void reseal(std::vector<uint8_t>& file) {
  const uint32_t crc = dp::crc32(file.data(), file.size() - 4);
  for (int i = 0; i < 4; ++i) file[file.size() - 4 + (size_t) i] = (uint8_t) (crc >> (8 * i));
}

// ── The file format and the names ────────────────────────────────────────────

void testFormat() {
  std::printf("the container\n");
  PresetMetadata m;
  m.name = "Warm \xC3\x9Cnicode \xE9\x9F\xB3\xE8\x89\xB2";
  m.author = "Ana";
  m.comment = "Line one\nLine two";
  m.category = "Bass";
  m.tags = {"warm", "analog", "sub bass"};
  m.pluginId = "com.test.format";
  m.pluginVersion = "2.1.0";
  m.created = 1700000000;
  m.modified = 1700000123;
  const std::vector<uint8_t> state = fakeState(3, 1000);
  const std::vector<uint8_t> file = encodePresetFile(m, state);

  PresetFileContents back;
  std::string why;
  check(decodePresetFile(file.data(), file.size(), &back, &why), "a preset decodes");
  check(back.state == state, "the state comes back bit-exact (1000 bytes)");
  check(back.metadata.name == m.name && back.metadata.author == m.author && back.metadata.comment == m.comment &&
            back.metadata.category == m.category && back.metadata.tags == m.tags &&
            back.metadata.pluginId == m.pluginId && back.metadata.pluginVersion == m.pluginVersion &&
            back.metadata.created == m.created && back.metadata.modified == m.modified &&
            back.metadata.sdkVersion == SONORE_SDK_VERSION_STRING && back.metadata.factoryIndex == -1,
        "every metadata field comes back, Unicode and line breaks included");
  const uint32_t metaSize = dp::getU32(file.data() + 8);
  check(back.formatVersion == 1 && file.size() == 20 + (size_t) metaSize + state.size() + 4 &&
            dp::getU64(file.data() + 12) == state.size(),
        "format 1: a 20-byte header, " + std::to_string(metaSize) + " bytes of metadata, the state, a checksum");

  // Every truncation, and every single flipped bit in a smaller file.
  int truncRefused = 0;
  for (size_t n = 0; n < file.size(); ++n)
    if (!decodePresetFile(file.data(), n, &back, &why) && !why.empty()) ++truncRefused;
  check(truncRefused == (int) file.size(),
        "every truncation is refused with a reason (" + std::to_string(truncRefused) + " of " +
            std::to_string(file.size()) + ")");
  const std::vector<uint8_t> tiny = encodePresetFile(m, fakeState(4, 40));
  int flipsRefused = 0, flips = 0;
  for (size_t i = 0; i < tiny.size(); ++i)
    for (int b = 0; b < 8; ++b) {
      std::vector<uint8_t> bad = tiny;
      bad[i] ^= (uint8_t) (1u << b);
      ++flips;
      if (!decodePresetFile(bad.data(), bad.size(), &back, &why)) ++flipsRefused;
    }
  check(flipsRefused == flips, "every one of " + std::to_string(flips) + " single-bit flips is refused");
  check(back.state.empty() && back.metadata.name.empty(), "and a refused file leaves nothing behind");

  auto refusedWith = [&](std::vector<uint8_t> bytes, bool resealIt, const char* fragment) {
    if (resealIt) reseal(bytes);
    std::string reason;
    const bool refused = !decodePresetFile(bytes.data(), bytes.size(), nullptr, &reason);
    return refused && reason.find(fragment) != std::string::npos;
  };
  std::vector<uint8_t> magic = file;
  magic[0] = 'X';
  check(refusedWith(magic, true, "not a Sonore preset"), "the wrong magic: \"not a Sonore preset file\"");
  std::vector<uint8_t> newer = file;
  newer[6] = 2; // oldest reader 2
  check(refusedWith(newer, true, "newer version"), "a newer format: refused, and says it is newer");
  std::vector<uint8_t> forward = file;
  forward[4] = 7; // written by format 7, readable by 1
  reseal(forward);
  check(decodePresetFile(forward.data(), forward.size(), &back, &why) && back.formatVersion == 7 &&
            back.state == state,
        "a later format that older readers may read is read");
  std::vector<uint8_t> meta = file;
  meta[8] = 0xFF, meta[9] = 0xFF, meta[10] = 0xFF, meta[11] = 0x7F;
  check(refusedWith(meta, true, "metadata"), "a metadata size past the end is refused");
  std::vector<uint8_t> grown = file;
  grown.insert(grown.end() - 4, 0x00);
  check(refusedWith(grown, true, "truncated, or with something added"), "a byte added inside is refused");
  std::vector<uint8_t> huge = file;
  for (int i = 12; i < 20; ++i) huge[(size_t) i] = 0xFF;
  check(refusedWith(huge, true, "truncated, or with something added"), "a state size near 2^64 is refused");
  PresetMetadata other = m;
  const std::vector<uint8_t> notState = encodePresetFile(other, std::vector<uint8_t>(32, 0x41));
  check(refusedWith(notState, false, "no Sonore plugin state"), "a state that is not SNRS is refused");
  // Metadata that is not UTF-8: a bag hand-written with a broken name.
  {
    StateBag bag;
    const std::string broken = "bad \xC0\xAF name";
    bag.setString("name", broken);
    bag.setString("pluginId", "com.test.format");
    std::vector<uint8_t> metaBytes;
    bag.serialise(metaBytes);
    std::vector<uint8_t> f = {'S', 'N', 'R', 'P', 1, 0, 1, 0};
    dp::putU32(f, (uint32_t) metaBytes.size());
    dp::putU64(f, (uint64_t) state.size());
    f.insert(f.end(), metaBytes.begin(), metaBytes.end());
    f.insert(f.end(), state.begin(), state.end());
    dp::putU32(f, 0);
    check(refusedWith(f, true, "not text"), "metadata that is not valid UTF-8 is refused");
  }

  std::printf("names\n");
  check(presetFileStem("Bass: sub / 808?") == "Bass_ sub _ 808_", "< > : \" / \\ | ? * become _");
  check(presetFileStem("CON") == "CON_" && presetFileStem("nul.x") == "nul_.x" && presetFileStem("Com7") == "Com7_" &&
            presetFileStem("LPT1 ") == "LPT1_" && presetFileStem("COM\xC2\xB9") == "COM\xC2\xB9_" &&
            presetFileStem("CONSOLE") == "CONSOLE" && presetFileStem("conin$") == "conin$_",
        "reserved device names (CON, NUL.x, COM7, LPT1, COM\xC2\xB9, CONIN$) are made ordinary; CONSOLE is not one");
  check(presetFileStem("  ..hidden. . ") == "hidden" && presetFileStem("...") == "Preset" &&
            presetFileStem("") == "Preset",
        "leading dots, trailing dots and spaces go; nothing left is \"Preset\"");
  check(presetFileStem("tab\there\x01") == "tab here", "control characters go, a tab is a space");
  std::string longName;
  for (int i = 0; i < 300; ++i) longName += "\xC3\xA9"; // é, two bytes
  const std::string longStem = presetFileStem(longName);
  check(longStem.size() <= 120 && longStem.size() % 2 == 0 && StateBag::isUtf8Text((const uint8_t*) longStem.data(), longStem.size()),
        "a 300-character name is cut to " + std::to_string(longStem.size()) + " bytes, on a character boundary");
  check(cleanPresetName("  a\nb \xFF ") == "a b \xEF\xBF\xBD", "a name: one line, broken bytes become U+FFFD, trimmed");
  check(presetFileStem("\xE9\x9F\xB3\xE8\x89\xB2 Pad") == "\xE9\x9F\xB3\xE8\x89\xB2 Pad", "Unicode is kept as it is");

  std::printf("the VST3 class id\n");
  {
    // vst3_wrapper.h presents the id; user_presets.h compares against it.
    // If the two derivations ever disagree in more than byte order, a
    // .vstpreset the VST3 build wrote would stop importing -- this says so.
    const Steinberg_int8* uid = vst3::componentUid();
    std::string plain, com;
    static const int kCom[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; ++i) {
      char hex[3];
      std::snprintf(hex, sizeof(hex), "%02X", (unsigned char) uid[i]);
      plain += hex;
      std::snprintf(hex, sizeof(hex), "%02X", (unsigned char) uid[kCom[i]]);
      com += hex;
    }
    check(vstPresetMatchesPlugin(plain, kDesc.id) && vstPresetMatchesPlugin(com, kDesc.id),
          "the class id vst3::componentUid() presents is recognised in memory order and in COM order");
    std::string lower = plain;
    for (char& c : lower) c = (char) std::tolower((unsigned char) c);
    check(vstPresetMatchesPlugin(lower, kDesc.id) && !vstPresetMatchesPlugin(plain, "com.other.plugin") &&
              !vstPresetMatchesPlugin(plain.substr(1), kDesc.id),
          "case-blind; another plugin's id and a short id are not ours");
  }
}

// ── The folder ───────────────────────────────────────────────────────────────

void testStore(const std::string& root) {
  std::printf("the folder\n");
  const std::string folder = joinPath(root, "store \xC3\xB1");
  UserPresetStore store;
  store.setFolder(folder);
  store.setPluginId("com.test.store");
  store.setPluginVersion("1.2.3");
  store.setAuthor("Tester");
  std::string why, path;
  check(store.save("Bass", fakeState(1), UserPresetStore::IfExists::Fail, &path, &why),
        "a preset saves into a folder that did not exist");
  check(fileName(path) == "Bass.sonorepreset" && fileExists(path) && store.entries().size() == 1,
        "as Bass.sonorepreset, and it is listed");
  const PresetMetadata first = store.entries()[0].metadata;
  check(first.name == "Bass" && first.author == "Tester" && first.pluginVersion == "1.2.3" && first.created > 0 &&
            first.modified >= first.created,
        "with its name, author, plugin version and times");

  const bool clash = !store.save("bass", fakeState(2), UserPresetStore::IfExists::Fail, nullptr, &why) &&
                     why.find("already exists") != std::string::npos;
  check(clash, "a clash is refused when asked to fail: \"" + why + "\"");
  check(store.save("bass", fakeState(2), UserPresetStore::IfExists::KeepBoth, &path, &why) &&
            store.entries().size() == 2 && store.indexOfName("Bass (2)") >= 0,
        "keep both: \"Bass (2)\"");
  const int bass = store.indexOfName("Bass");
  check(store.save("BASS", fakeState(9), UserPresetStore::IfExists::Replace, &path, &why) &&
            store.entries().size() == 2,
        "replace writes over it: still two presets");
  PresetFileContents c;
  check(store.read(store.indexOfName("BASS"), &c, &why) && c.state == fakeState(9) && c.metadata.created == first.created &&
            bass >= 0,
        "with the new state, the new spelling, and the time it was first made");

  const std::string unicode = "\xC3\x9Cnic\xC3\xB6" "de \xE9\x9F\xB3\xE8\x89\xB2 \xF0\x9F\x8E\xB9";
  check(store.save(unicode, fakeState(3), UserPresetStore::IfExists::Fail, &path, &why) && fileExists(path) &&
            fileName(path) == unicode + ".sonorepreset",
        "a Unicode name (Latin, CJK, an emoji) is a Unicode file name");
  check(store.save("CON", fakeState(4), UserPresetStore::IfExists::Fail, &path, &why) &&
            fileName(path) == "CON_.sonorepreset" && store.indexOfName("CON") >= 0,
        "\"CON\" is saved as CON_.sonorepreset and still called CON");
  std::string p1, p2;
  check(store.save("A/B", fakeState(5), UserPresetStore::IfExists::Fail, &p1, &why) &&
            store.save("A_B", fakeState(6), UserPresetStore::IfExists::Fail, &p2, &why) &&
            fileName(p1) == "A_B.sonorepreset" && fileName(p2) == "A_B (2).sonorepreset",
        "\"A/B\" and \"A_B\" are two presets and two files");
  check(!store.save("   ", fakeState(1), UserPresetStore::IfExists::Fail, nullptr, &why), "a blank name is refused");

  // Overwrite.
  {
    const int i = store.indexOfName("A/B");
    const int64_t created = store.entries()[(size_t) i].metadata.created;
    check(store.overwrite(i, fakeState(42), &why) && store.read(store.indexOfName("A/B"), &c, &why) &&
              c.state == fakeState(42) && c.metadata.name == "A/B" && c.metadata.created == created,
          "overwrite: the new state under the same name and creation time");
  }
  // Rename.
  {
    const int i = store.indexOfName("A/B");
    const std::string oldPath = store.entries()[(size_t) i].path;
    check(!store.rename(i, "Bass", nullptr, &why), "renaming onto another preset's name is refused");
    std::string renamed;
    check(store.rename(i, "Lead", &renamed, &why) && !fileExists(oldPath) && fileExists(renamed) &&
              store.indexOfName("Lead") >= 0 && store.indexOfName("A/B") < 0,
          "rename: the new file is there, the old one is gone");
    const int l = store.indexOfName("Lead");
    check(store.rename(l, "LEAD", &renamed, &why) && store.indexOfName("LEAD") >= 0 &&
              store.entries()[(size_t) store.indexOfName("LEAD")].metadata.name == "LEAD",
          "a rename that only changes case works on every filesystem");
    check(store.read(store.indexOfName("LEAD"), &c, &why) && c.state == fakeState(42), "and the state went with it");
  }
  // Duplicate.
  check(store.duplicate(store.indexOfName("LEAD"), &path, &why) && store.indexOfName("LEAD copy") >= 0,
        "duplicate: \"LEAD copy\"");
  // Delete, for good and to a trash.
  {
    const size_t before = store.entries().size();
    const std::string gone = store.entries()[(size_t) store.indexOfName("LEAD copy")].path;
    check(store.remove(store.indexOfName("LEAD copy"), UserPresetStore::Removal::Permanently, &why) &&
              !fileExists(gone) && store.entries().size() == before - 1,
          "delete for good");
    const std::string trashFolder = joinPath(root, "fake trash");
    ensureDirectory(trashFolder);
    store.trashFile = [&](const std::string& p, std::string*) {
      return pathio::rename(p.c_str(), joinPath(trashFolder, fileName(p)).c_str()) == 0;
    };
    const std::string trashed = store.entries()[(size_t) store.indexOfName("CON")].path;
    check(store.remove(store.indexOfName("CON"), UserPresetStore::Removal::ToTrash, &why) && !fileExists(trashed) &&
              fileExists(joinPath(trashFolder, "CON_.sonorepreset")),
          "delete to the trash goes through the store's trash");
    store.trashFile = [](const std::string&, std::string* w) {
      if (w) *w = "no trash here";
      return false;
    };
    const int i = store.indexOfName("Bass (2)");
    check(!store.remove(i, UserPresetStore::Removal::ToTrash, &why) && why == "no trash here" &&
              store.indexOfName("Bass (2)") >= 0,
          "a trash that refuses leaves the preset where it was -- never deleted instead");
    store.trashFile = nullptr;
#if !defined(_WIN32) && !defined(__APPLE__)
    // The freedesktop trash, in a private data home.
    setEnv("XDG_DATA_HOME", joinPath(root, "xdg"));
    const std::string victim = store.entries()[(size_t) i].path;
    const bool moved = store.remove(i, UserPresetStore::Removal::ToTrash, &why) && !fileExists(victim) &&
                       fileExists(joinPath(root, "xdg/Trash/files/Bass (2).sonorepreset")) &&
                       fileExists(joinPath(root, "xdg/Trash/info/Bass (2).sonorepreset.trashinfo"));
    check(moved, "the freedesktop trash: files/ and info/ (" + why + ")");
#else
    if (pathio::environment("SONORE_TEST_TRASH") == "1") {
      const std::string victim = store.entries()[(size_t) i].path;
      why.clear();
      const bool moved = store.remove(i, UserPresetStore::Removal::ToTrash, &why) && !fileExists(victim);
      check(moved, "SONORE_TEST_TRASH: moved to the system's trash (" + why + ")");
    } else {
      std::printf("  skip moving to the system trash (SONORE_TEST_TRASH=1 to run it)\n");
    }
#endif
  }
  // Rescan sees the outside world.
  {
    check(!store.rescan(), "a rescan of an unchanged folder reports no change");
    PresetMetadata m;
    m.name = "From Outside";
    m.pluginId = "com.test.store";
    check(writePresetFile(joinPath(folder, "dropped in.sonorepreset"), m, fakeState(11)) && store.rescan() &&
              store.indexOfName("From Outside") >= 0,
          "a preset copied in from outside appears at the next rescan");
    pathio::remove(joinPath(folder, "dropped in.sonorepreset").c_str());
    check(store.rescan() && store.indexOfName("From Outside") < 0, "and one deleted outside disappears");
    writeBytes(joinPath(folder, "junk.sonorepreset"), {1, 2, 3, 4, 5});
    PresetMetadata foreign = m;
    foreign.pluginId = "com.someone.else";
    writePresetFile(joinPath(folder, "foreign.sonorepreset"), foreign, fakeState(12));
    store.rescan();
    bool junk = false, theirs = false;
    for (const auto& r : store.refused()) {
      junk = junk || (fileName(r.path) == "junk.sonorepreset" && !r.reason.empty());
      theirs = theirs || (fileName(r.path) == "foreign.sonorepreset" && r.reason.find("another plugin") != std::string::npos);
    }
    check(junk && theirs && store.indexOfName("From Outside") < 0,
          "a damaged file and another plugin's preset are listed as refused, with reasons, and not offered");
    pathio::remove(joinPath(folder, "junk.sonorepreset").c_str());
    pathio::remove(joinPath(folder, "foreign.sonorepreset").c_str());
    store.rescan();
    bool sorted = true;
    for (size_t k = 1; k < store.entries().size(); ++k)
      sorted = sorted && !asciiLess(store.entries()[k].metadata.name, store.entries()[k - 1].metadata.name);
    check(sorted, "the list is sorted by name, case-blind");
  }
  // Import and export.
  {
    const std::string outside = joinPath(root, "outside");
    ensureDirectory(outside);
    PresetMetadata m;
    m.name = "Imported";
    m.pluginId = "com.test.store";
    m.author = "Someone";
    writePresetFile(joinPath(outside, "x.sonorepreset"), m, fakeState(21));
    check(store.importFile(joinPath(outside, "x.sonorepreset"), &path, &why) && store.indexOfName("Imported") >= 0 &&
              fileExists(joinPath(outside, "x.sonorepreset")),
          "import copies a preset in under its own name, and leaves the original");
    check(store.importFile(joinPath(outside, "x.sonorepreset"), &path, &why) && store.indexOfName("Imported (2)") >= 0,
          "importing it again keeps both");
    m.pluginId = "com.someone.else";
    writePresetFile(joinPath(outside, "y.sonorepreset"), m, fakeState(22));
    const bool refused = !store.importFile(joinPath(outside, "y.sonorepreset"), &path, &why) &&
                         why.find("another plugin") != std::string::npos;
    check(refused, "another plugin's preset is refused: \"" + why + "\"");
    writeBytes(joinPath(outside, "z.sonorepreset"), {'S', 'N', 'R', 'P', 0});
    check(!store.importFile(joinPath(outside, "z.sonorepreset"), &path, &why), "a damaged file is refused");
    // .vstpreset, in both spellings of the class id.
    uint8_t uid[16];
    dp::vst3ComponentUid("com.test.store", uid);
    const std::string plain = dp::uidHex(uid, false), com = dp::uidHex(uid, true);
    writeVstPreset(joinPath(outside, "From VST3.vstpreset").c_str(), plain, fakeState(23));
    writeVstPreset(joinPath(outside, "COM order.vstpreset").c_str(), com, fakeState(24));
    writeVstPreset(joinPath(outside, "Not ours.vstpreset").c_str(), std::string(32, 'A'), fakeState(25));
    check(store.importFile(joinPath(outside, "From VST3.vstpreset"), &path, &why) &&
              store.read(store.indexOfName("From VST3"), &c, &why) && c.state == fakeState(23),
          "a .vstpreset for this plugin imports, named after the file, its state bit-exact");
    check(store.importFile(joinPath(outside, "COM order.vstpreset"), &path, &why),
          "one whose class id is spelled in COM order too");
    check(!store.importFile(joinPath(outside, "Not ours.vstpreset"), &path, &why) &&
              why.find("another plugin") != std::string::npos,
          "a .vstpreset for another plugin is refused");
    // Export: ours, and .vstpreset.
    PresetMetadata e;
    e.name = "Exported";
    check(store.exportTo(e, fakeState(31), joinPath(outside, "e.sonorepreset"), &why) &&
              readPresetFile(joinPath(outside, "e.sonorepreset"), &c, &why) && c.state == fakeState(31) &&
              c.metadata.pluginId == "com.test.store",
          "export writes a preset file outside the folder, with this plugin's id");
    Vst3PresetFile v;
    check(store.exportTo(e, fakeState(32), joinPath(outside, "e.vstpreset"), &why) &&
              readVstPreset(joinPath(outside, "e.vstpreset").c_str(), &v) && v.classId == plain &&
              v.componentState == fakeState(32),
          "and a .vstpreset with this plugin's class id when that is the extension asked for");
    // Read whole and unbounded, a file picked in an import dialog could be
    // gigabytes in memory before the first check refused it. Bounded, at a
    // cap the test can afford: the same file refused under its own size.
    Vst3PresetFile capped;
    check(!readVstPreset(joinPath(outside, "e.vstpreset").c_str(), &capped, 16) &&
              capped.componentState.empty(),
          "a .vstpreset larger than the read's cap is refused, not read whole");
    // A folder is not a preset, by name or not: refused before any open.
    std::string notAFile;
    check(!readPresetFile(outside, &c, &notAFile) && !notAFile.empty(),
          "a path that is not a regular file is refused before it is opened, with a reason");
  }
  // A name already as long as a name may be: the number that makes a second
  // one unique has to fit inside the cap too.
  {
    std::string longest;
    while (longest.size() + 3 <= dp::kMaxNameBytes) longest += "\xE9\x9F\xB3"; // 音, three bytes each
    longest += "ab";
    check(longest.size() == dp::kMaxNameBytes && cleanPresetName(longest) == longest, "a 200-byte name is a name");
    auto namesUnique = [&] {
      for (size_t a = 0; a < store.entries().size(); ++a)
        for (size_t b = a + 1; b < store.entries().size(); ++b)
          if (dp::equalsIgnoringAsciiCase(store.entries()[a].metadata.name, store.entries()[b].metadata.name))
            return false;
      return true;
    };
    std::string saved, kept;
    check(store.save(longest, fakeState(41), UserPresetStore::IfExists::Fail, &saved, &why) &&
              store.save(longest, fakeState(42), UserPresetStore::IfExists::KeepBoth, &kept, &why) && saved != kept,
          "saved twice, keeping both");
    const int second = store.indexOfPath(kept);
    const std::string keptName = second >= 0 ? store.entries()[(size_t) second].metadata.name : std::string();
    check(keptName != longest && keptName.size() <= dp::kMaxNameBytes && cleanPresetName(keptName) == keptName &&
              namesUnique(),
          "the second is named apart from the first, inside the cap (" + std::to_string(keptName.size()) +
              " bytes)");
    check(store.duplicate(store.indexOfPath(saved), &kept, &why) && namesUnique(),
          "and a duplicate of it is named apart from both");
    PresetMetadata m;
    m.name = longest;
    m.pluginId = "com.test.store";
    const std::string source = joinPath(joinPath(root, "outside"), "long.sonorepreset");
    writePresetFile(source, m, fakeState(43));
    check(store.importFile(source, &kept, &why) && namesUnique(), "and so is an import of a preset of that name");
  }
  // A store with no plugin id, a browser of every plugin's presets, in the
  // same folder. It stamped its empty id over every preset it wrote, so a
  // rename there left a preset its own plugin then refused.
  {
    std::string owned;
    check(store.save("Owned", fakeState(51), UserPresetStore::IfExists::Fail, &owned, &why),
          "the plugin's store saves \"Owned\"");
    UserPresetStore browser;
    browser.setFolder(folder);
    browser.rescan();
    std::string renamed;
    const int at = browser.indexOfName("Owned");
    const bool moved = at >= 0 && browser.rename(at, "Owned Renamed", &renamed, &why);
    PresetFileContents back;
    const bool read = moved && readPresetFile(renamed, &back, &why);
    store.rescan();
    check(read && back.metadata.pluginId == "com.test.store" && store.indexOfName("Owned Renamed") >= 0,
          "renamed by a store with no id, it keeps its plugin's id (\"" + back.metadata.pluginId +
              "\") and its plugin still lists it");
    const bool overwritten = read && browser.overwrite(browser.indexOfPath(renamed), fakeState(52), &why) &&
                             readPresetFile(renamed, &back, &why) && back.metadata.pluginId == "com.test.store";
    std::string copy;
    const bool duplicated = browser.duplicate(browser.indexOfPath(renamed), &copy, &why) &&
                            readPresetFile(copy, &back, &why) && back.metadata.pluginId == "com.test.store";
    check(overwritten && duplicated, "and so does one it writes over, and a duplicate it makes");
    // A new preset from it has to say whose it is.
    why.clear();
    const bool refused = !browser.save("Nobody's", fakeState(53), UserPresetStore::IfExists::Fail, nullptr, &why);
    check(refused && !why.empty(), "it refuses to save a preset it cannot say is whose: \"" + why + "\"");
    PresetMetadata details;
    details.pluginId = "com.test.store";
    std::string given;
    check(browser.save("Somebody's", fakeState(54), UserPresetStore::IfExists::Fail, &given, &why, &details) &&
              store.rescan() && store.indexOfName("Somebody's") >= 0,
          "and saves one with the id it is given, which that plugin lists");
    pathio::remove(renamed.c_str());
    pathio::remove(copy.c_str());
    pathio::remove(given.c_str());
    store.rescan();
  }
}

// ── A host, for the plugin ───────────────────────────────────────────────────

struct HostLog {
  int rescans = 0, dirty = 0, restarts = 0, loaded = 0, errors = 0;
  uint32_t loadedKind = 99;
  std::string loadedLocation, loadedKey, lastError;
} g_host;

void hostRescan(const clap_host_t*, clap_param_rescan_flags flags) {
  if (flags & CLAP_PARAM_RESCAN_VALUES) ++g_host.rescans;
}
void hostClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
void hostRequestFlush(const clap_host_t*) {}
const clap_host_params_t kHostParams = {hostRescan, hostClear, hostRequestFlush};
void hostMarkDirty(const clap_host_t*) { ++g_host.dirty; }
const clap_host_state_t kHostState = {hostMarkDirty};
void hostPresetError(const clap_host_t*, uint32_t, const char*, const char*, int32_t, const char* msg) {
  ++g_host.errors;
  g_host.lastError = msg ? msg : "";
}
void hostPresetLoaded(const clap_host_t*, uint32_t kind, const char* location, const char* key) {
  ++g_host.loaded;
  g_host.loadedKind = kind;
  g_host.loadedLocation = location ? location : "";
  g_host.loadedKey = key ? key : "";
}
const clap_host_preset_load_t kHostPresetLoad = {hostPresetError, hostPresetLoaded};
const void* hostGetExtension(const clap_host_t*, const char* id) {
  if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &kHostParams;
  if (std::strcmp(id, CLAP_EXT_STATE) == 0) return &kHostState;
  if (std::strcmp(id, CLAP_EXT_PRESET_LOAD) == 0) return &kHostPresetLoad;
  return nullptr;
}
void hostRequestRestart(const clap_host_t*) { ++g_host.restarts; }
void hostRequest(const clap_host_t*) {}
const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "com.sonorie.user-presets-test", "Sonorie", "", "1.0",
                           hostGetExtension,  hostRequestRestart, hostRequest, hostRequest};

int64_t writeCb(const clap_ostream_t* s, const void* data, uint64_t n) {
  auto* out = static_cast<std::vector<uint8_t>*>(s->ctx);
  out->insert(out->end(), (const uint8_t*) data, (const uint8_t*) data + n);
  return (int64_t) n;
}
struct Reader {
  const std::vector<uint8_t>* bytes;
  size_t pos;
};
int64_t readCb(const clap_istream_t* s, void* data, uint64_t n) {
  auto* r = static_cast<Reader*>(s->ctx);
  const size_t left = r->bytes->size() - r->pos;
  const size_t take = n < left ? (size_t) n : left;
  std::memcpy(data, r->bytes->data() + r->pos, take);
  r->pos += take;
  return (int64_t) take;
}

struct Plugin {
  const clap_plugin_t* plugin = nullptr;
  const clap_plugin_state_t* state = nullptr;
  clapwrap::Instance* inst = nullptr;

  Plugin() {
    const auto* factory =
        static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    plugin = factory->create_plugin(factory, &kHost, kDesc.id);
    if (plugin && plugin->init(plugin)) {
      state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
      inst = clapwrap::self(plugin);
    }
  }
  ~Plugin() {
    if (plugin) plugin->destroy(plugin);
  }
  Plugin(const Plugin&) = delete;
  Plugin& operator=(const Plugin&) = delete;

  std::vector<uint8_t> save() const {
    std::vector<uint8_t> out;
    clap_ostream_t os{&out, writeCb};
    state->save(plugin, &os);
    return out;
  }
  bool load(const std::vector<uint8_t>& bytes) const {
    Reader r{&bytes, 0};
    clap_istream_t is{&r, readCb};
    return state->load(plugin, &is);
  }
  PresetManager& presets() const { return clapwrap::presetsOf(inst); }
  float param(int i) const { return inst->params[i]; }
  void setParam(int i, float v) const { inst->params[i] = clampToRange(kDesc.params[i], v); }
};

std::string describeIndex(const PresetManager& m) {
  const int i = m.currentIndex();
  return i < 0 ? std::string("none") : m.itemAt(i).name;
}

// ── The plugin ───────────────────────────────────────────────────────────────

void testPlugin(const std::string& root) {
  std::printf("the plugin\n");
  Plugin p;
  check(p.inst != nullptr && p.state != nullptr, "the showcase instantiates through its CLAP entry");
  if (!p.inst) return;
  PresetManager& m = p.presets();
  const std::string expectedFolder = joinPath(joinPath(joinPath(root, "Sonorie"), "Sonore Showcase"), "Presets");
  check(m.store().folder() == expectedFolder, "its folder: <root>/Sonorie/Sonore Showcase/Presets");
  check(m.numFactory() == 3 && m.numUser() == 0 && !m.hasSelection() && !m.isDirty(),
        "three factory presets, no user presets, nothing selected, nothing modified");

  // A sound of the user's own, then saved: the bytes are the host's.
  p.setParam(showcase::kDrive, 27.0f);
  p.setParam(showcase::kTone, 5000.0f);
  p.inst->dsp.setExpression("abs(tanh(x))");
  const std::vector<uint8_t> hostBlob = p.save();
  std::string why;
  check(m.saveAs("My Sound", UserPresetStore::IfExists::Fail, &why), "Save As \"My Sound\"");
  PresetFileContents c;
  check(m.store().read(m.store().indexOfName("My Sound"), &c, &why) && c.state == hostBlob,
        "the preset file holds the host save path's bytes, exactly (" + std::to_string(hostBlob.size()) + ")");
  check(c.metadata.pluginId == kDesc.id && c.metadata.pluginVersion == kDesc.version &&
            c.metadata.sdkVersion == SONORE_SDK_VERSION_STRING,
        "with the plugin's id and version and the SDK's");
  check(m.hasSelection() && m.currentName() == "My Sound" && !m.isDirty() && m.currentIsUser(),
        "and it becomes the current preset, unmodified");

  // Modified, and clean again.
  p.setParam(showcase::kMix, 40.0f);
  check(m.isDirty(), "a knob moved: modified");
  p.setParam(showcase::kMix, 100.0f);
  check(!m.isDirty(), "moved back: not modified");
  p.inst->dsp.setExpression("sin(x)");
  check(m.isDirty(), "the typed curve changed (the plugin's own state, not a parameter): modified");
  p.inst->guiWidth = 1000;
  p.inst->dsp.setExpression("abs(tanh(x))");
  check(!m.isDirty(), "the editor's size is not the sound: not modified");
  p.inst->guiWidth = 780;

  // Loading: the restore path, and the host told.
  p.setParam(showcase::kDrive, 3.0f);
  p.inst->dsp.setExpression("x");
  p.inst->guiWidth = 900;
  const int rescans = g_host.rescans, dirty = g_host.dirty;
  check(m.load(m.numFactory() + m.store().indexOfName("My Sound"), &why), "the user preset loads");
  check(std::fabs(p.param(showcase::kDrive) - 27.0f) < 1e-4f && std::fabs(p.param(showcase::kTone) - 5000.0f) < 1e-2f &&
            p.inst->dsp.expression == "abs(tanh(x))",
        "its parameters and the plugin's own state (the curve) are back");
  check(g_host.rescans == rescans + 1 && g_host.dirty == dirty + 1,
        "the host is asked to rescan the values and told the session is dirty");
  check(p.inst->guiWidth == 900, "the session keeps its editor size: a preset is a sound, not a window");
  check(g_host.loaded >= 1 && g_host.loadedKind == CLAP_PRESET_DISCOVERY_LOCATION_FILE &&
            fileName(g_host.loadedLocation) == "My Sound.sonorepreset",
        "and its browser is told which preset file the plugin loaded (preset-load \"loaded\")");

  // An older and a newer version of the plugin's state, in a preset.
  {
    // One parameter fewer: the last value cut out, the count lowered.
    std::vector<uint8_t> older = hostBlob;
    uint32_t count = 0;
    std::memcpy(&count, older.data() + 8, 4);
    const size_t lastAt = 12 + 4 * (size_t) (count - 1);
    older.erase(older.begin() + (long) lastAt, older.begin() + (long) lastAt + 4);
    count -= 1;
    std::memcpy(older.data() + 8, &count, 4);
    p.setParam(showcase::kAutoGain, 0.0f);
    check(m.store().save("Older", older, UserPresetStore::IfExists::Fail, nullptr, &why) &&
              m.load(m.numFactory() + m.store().indexOfName("Older"), &why) &&
              p.param(showcase::kAutoGain) == kDesc.params[showcase::kAutoGain].defaultValue &&
              std::fabs(p.param(showcase::kDrive) - 27.0f) < 1e-4f,
          "a preset from a version with one parameter fewer loads; the new one is at its default");
    // Two parameters more: extra values the plugin does not know.
    std::vector<uint8_t> newer = hostBlob;
    std::memcpy(&count, newer.data() + 8, 4);
    const size_t endOfValues = 12 + 4 * (size_t) count;
    const float extra[2] = {0.5f, 0.25f};
    newer.insert(newer.begin() + (long) endOfValues, (const uint8_t*) extra, (const uint8_t*) extra + 8);
    count += 2;
    std::memcpy(newer.data() + 8, &count, 4);
    p.setParam(showcase::kDrive, 1.0f);
    check(m.store().save("Newer", newer, UserPresetStore::IfExists::Fail, nullptr, &why) &&
              m.load(m.numFactory() + m.store().indexOfName("Newer"), &why) &&
              std::fabs(p.param(showcase::kDrive) - 27.0f) < 1e-4f && p.inst->dsp.expression == "abs(tanh(x))",
          "one from a version with two parameters more loads what this version has");
    // A state from a NEWER state format is refused, and the plugin unchanged.
    std::vector<uint8_t> future = hostBlob;
    const uint32_t v = 99;
    std::memcpy(future.data() + 4, &v, 4);
    p.setParam(showcase::kDrive, 2.0f);
    const bool refused = m.store().save("Future", future, UserPresetStore::IfExists::Fail, nullptr, &why) &&
                         !m.load(m.numFactory() + m.store().indexOfName("Future"), &why) && !why.empty() &&
                         std::fabs(p.param(showcase::kDrive) - 2.0f) < 1e-4f;
    check(refused, "a preset whose state a newer version wrote is refused, and changes nothing: \"" + why + "\"");
  }

  // Factory presets through the same path.
  {
    p.inst->dsp.setExpression("sin(x)");
    check(m.load(1, &why) && m.currentName() == "Crunch" && !m.isDirty(), "factory preset 2, \"Crunch\", loads");
    bool same = true;
    for (int i = 0; i < SONORE_NUM_PARAMS; ++i)
      same = same && std::fabs(p.param(i) - clampToRange(kDesc.params[i], showcase::kPresetCrunch[i])) < 1e-4f;
    check(same && p.inst->dsp.expression == "sin(x)" && p.inst->selectedPreset == 1,
          "with its values; the plugin's own state is left alone, as a factory preset always did");
  }

  // Next and previous: factory, then the user's by name, round and round.
  {
    m.clearSelection();
    std::vector<std::string> order;
    for (int k = 0; k < m.count() + 1; ++k) {
      m.loadNext(&why);
      order.push_back(describeIndex(m));
    }
    std::string joined;
    for (const std::string& s : order) joined += (joined.empty() ? "" : ", ") + s;
    // "Future" cannot load (a newer state format): next steps over it rather
    // than stopping there for ever.
    const bool expected = m.count() == 7 && order.size() == 8 && order[0] == "Warm Glue" && order[1] == "Crunch" &&
                          order[2] == "Folded" && order[3] == "My Sound" && order[4] == "Newer" &&
                          order[5] == "Older" && order[6] == "Warm Glue" && order[7] == "Crunch";
    check(expected, "next walks factory then user, by name, steps over one that will not load, and wraps: " + joined);
    m.clearSelection();
    m.loadPrevious(&why);
    check(describeIndex(m) == "Older", "previous from nothing is the last preset (" + describeIndex(m) + ")");
    m.loadPrevious(&why);
    check(describeIndex(m) == "Newer", "and then the one before it");
    m.load(0, &why);
    m.loadPrevious(&why);
    check(describeIndex(m) == "Older", "previous from the first wraps to the last");
  }

  // The selection comes back with the session.
  {
    m.load(m.numFactory() + m.store().indexOfName("My Sound"), &why);
    const std::vector<uint8_t> clean = p.save();
    p.setParam(showcase::kOutput, -6.0f);
    const std::vector<uint8_t> modified = p.save();
    Plugin q;
    check(q.load(clean) && q.presets().currentName() == "My Sound" && !q.presets().isDirty() &&
              q.presets().currentIsUser(),
          "a session saved on \"My Sound\" reopens on \"My Sound\", unmodified");
    check(q.load(modified) && q.presets().currentName() == "My Sound" && q.presets().isDirty(),
          "and one saved after a tweak reopens on it, modified");
    m.load(2, &why);
    check(q.load(p.save()) && q.presets().currentName() == "Folded" && q.presets().currentIndex() == 2,
          "a factory preset too");

    // A session saved by the build before the last parameter was added: one
    // value fewer, and the baseline THAT build's hash gave it -- FNV-1a over
    // its values, then the DSP's own state, which is the blob's tail.
    struct Layout {
      uint32_t count = 0;
      size_t baselineAt = 0, bagAt = 0;
    };
    const auto layoutOf = [](const std::vector<uint8_t>& b) {
      Layout l;
      std::memcpy(&l.count, b.data() + 8, 4);
      const size_t sel = 12 + 4 * (size_t) l.count + 1 + 4 + 8;
      uint32_t n = 0, f = 0;
      std::memcpy(&n, b.data() + sel + 5, 4);
      std::memcpy(&f, b.data() + sel + 9 + n, 4);
      l.baselineAt = sel + 13 + n + f;
      l.bagAt = l.baselineAt + 8;
      return l;
    };
    const auto olderHash = [&](const std::vector<uint8_t>& b) {
      const Layout l = layoutOf(b);
      uint64_t h = 1469598103934665603ull;
      auto mix = [&h](const uint8_t* bytes, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ bytes[i]) * 1099511628211ull;
      };
      mix(b.data() + 12, 4 * ((size_t) l.count - 1));
      mix(b.data() + l.bagAt, b.size() - l.bagAt);
      return h;
    };
    const auto olderBuild = [&](std::vector<uint8_t> b, uint64_t baseline) {
      const Layout l = layoutOf(b);
      std::memcpy(b.data() + l.baselineAt, &baseline, 8);
      const size_t last = 12 + 4 * ((size_t) l.count - 1);
      b.erase(b.begin() + (long) last, b.begin() + (long) (last + 4));
      const uint32_t fewer = l.count - 1;
      std::memcpy(b.data() + 8, &fewer, 4);
      return b;
    };
    const uint64_t onPreset = olderHash(clean);
    check(q.load(olderBuild(clean, onPreset)) && q.presets().currentName() == "My Sound" &&
              !q.presets().isDirty(),
          "a session from the build before a parameter was added reopens on \"My Sound\" "
          "unmodified: the update is not the user's change");
    check(q.load(olderBuild(modified, onPreset)) && q.presets().currentName() == "My Sound" &&
              q.presets().isDirty(),
          "and one tweaked in that build still reopens modified");

    std::vector<uint8_t> v5 = clean;
    const uint32_t five = 5;
    std::memcpy(v5.data() + 4, &five, 4);
    // A v5 blob has no selection field: cut it out (it sits after the editor size).
    uint32_t count = 0;
    std::memcpy(&count, v5.data() + 8, 4);
    const size_t at = 12 + 4 * (size_t) count + 1 + 4 + 8;
    uint32_t nameLength = 0, fileLength = 0;
    std::memcpy(&nameLength, v5.data() + at + 5, 4);
    std::memcpy(&fileLength, v5.data() + at + 9 + nameLength, 4);
    v5.erase(v5.begin() + (long) at, v5.begin() + (long) (at + 13 + nameLength + fileLength + 8));
    check(q.load(v5) && !q.presets().hasSelection() && std::fabs(q.param(showcase::kDrive) - 27.0f) < 1e-4f,
          "a v5 session (before presets were remembered) loads, with nothing selected");
  }

  // A preset file with no name of its own, handed over by path (a host's
  // browser, a drop): it is called what the list calls it, its file name
  // without the extension -- "Untitled Thing.sonorepreset" was the name.
  {
    PresetMetadata nameless;
    nameless.pluginId = kDesc.id;
    const std::string path = joinPath(m.store().folder(), "Untitled Thing.sonorepreset");
    check(writePresetFile(path, nameless, p.save(), &why), "a preset with no name is written into the folder");
    m.rescan();
    const bool loaded = m.loadFile(path, true, &why);
    const int at = m.currentIndex();
    const std::string listed = at >= 0 ? m.itemAt(at).name : std::string();
    check(loaded && m.currentName() == "Untitled Thing" && listed == "Untitled Thing",
          "loaded by its path, it is \"" + m.currentName() + "\", as the list shows it (\"" + listed + "\")");
    pathio::remove(path.c_str());
    m.rescan();
  }
}

// ── An edit still queued when a preset loads ─────────────────────────────────

struct HostEvents {
  std::vector<std::pair<clap_id, double>> values;
};
bool capturePush(const clap_output_events_t* list, const clap_event_header_t* e) {
  if (e->space_id == CLAP_CORE_EVENT_SPACE_ID && e->type == CLAP_EVENT_PARAM_VALUE) {
    const auto* v = reinterpret_cast<const clap_event_param_value_t*>(e);
    static_cast<HostEvents*>(list->ctx)->values.push_back({v->param_id, v->value});
  }
  return true;
}
uint32_t noEventsSize(const clap_input_events_t*) { return 0; }
const clap_event_header_t* noEventsGet(const clap_input_events_t*, uint32_t) { return nullptr; }

/**
 * The editor writes a knob into the parameter at once and queues it for the
 * audio side, which hands it to the host when the queue drains: at the next
 * block, or at the flush a stopped host makes. A preset or a session that
 * loads before that drain has replaced the value, and the queued one is older
 * than it.
 */
void testQueuedEdits() {
  std::printf("an edit still queued when a preset loads\n");
  Plugin p;
  PresetManager& m = p.presets();
  const auto* params = static_cast<const clap_plugin_params_t*>(p.plugin->get_extension(p.plugin, CLAP_EXT_PARAMS));
  clap_input_events_t in{nullptr, noEventsSize, noEventsGet};
  HostEvents told;
  clap_output_events_t out{&told, capturePush};
  EditorHost host = clapwrap::makeEditorHost(p.inst);
  std::string why;

  // An edit with nothing after it reaches the host whole.
  host.beginGesture(showcase::kTone);
  host.setParameter(showcase::kTone, 2000.0f);
  host.endGesture(showcase::kTone);
  params->flush(p.plugin, &in, &out);
  bool toneTold = false;
  for (const auto& v : told.values) toneTold = toneTold || (v.first == showcase::kTone && std::fabs(v.second - 2000.0) < 1e-3);
  check(std::fabs(p.param(showcase::kTone) - 2000.0f) < 1e-2f && toneTold,
        "an edit in the editor stays, and the flush tells the host");

  // Drive turned, then Crunch loaded before the host drained the queue.
  told.values.clear();
  host.beginGesture(showcase::kDrive);
  host.setParameter(showcase::kDrive, 33.0f);
  host.endGesture(showcase::kDrive);
  check(m.load(1, &why) && std::fabs(p.param(showcase::kDrive) - showcase::kPresetCrunch[0]) < 1e-4f,
        "Crunch loads over a Drive edit still in the queue");
  params->flush(p.plugin, &in, &out);
  char line[160];
  std::snprintf(line, sizeof(line), "the flush after it leaves Crunch's drive, %.2f (the edit was 33), and Crunch unmodified",
                (double) p.param(showcase::kDrive));
  check(std::fabs(p.param(showcase::kDrive) - showcase::kPresetCrunch[0]) < 1e-4f && !m.isDirty(), line);
  bool staleTold = false;
  for (const auto& v : told.values) staleTold = staleTold || (v.first == showcase::kDrive && std::fabs(v.second - 33.0) < 1e-3);
  check(!staleTold, "and the host is not sent the older value as a live edit");

  // A session the host restores, the same.
  const std::vector<uint8_t> session = p.save();
  told.values.clear();
  host.setParameter(showcase::kMix, 12.0f);
  check(p.load(session) && std::fabs(p.param(showcase::kMix) - 12.0f) > 1e-3f, "a session restores over a Mix edit still queued");
  params->flush(p.plugin, &in, &out);
  check(std::fabs(p.param(showcase::kMix) - 12.0f) > 1e-3f, "and the flush after it leaves the session's Mix");
}

// ── CLAP preset discovery and preset-load ────────────────────────────────────

struct Indexed {
  std::vector<std::string> filetypes;
  std::vector<clap_preset_discovery_location_t> locations;
  std::vector<std::string> locationPaths, locationNames;
};

bool indexerFiletype(const clap_preset_discovery_indexer_t* ix, const clap_preset_discovery_filetype_t* t) {
  static_cast<Indexed*>(ix->indexer_data)->filetypes.push_back(t->file_extension ? t->file_extension : "");
  return true;
}
bool indexerLocation(const clap_preset_discovery_indexer_t* ix, const clap_preset_discovery_location_t* l) {
  auto* x = static_cast<Indexed*>(ix->indexer_data);
  x->locations.push_back(*l);
  x->locationPaths.push_back(l->location ? l->location : "");
  x->locationNames.push_back(l->name ? l->name : "");
  return true;
}
bool indexerSoundpack(const clap_preset_discovery_indexer_t*, const clap_preset_discovery_soundpack_t*) {
  return true;
}
const void* indexerExtension(const clap_preset_discovery_indexer_t*, const char*) { return nullptr; }

struct Received {
  int begins = 0, errors = 0;
  std::vector<std::string> names, keys, pluginIds, creators, features;
  bool nullKeys = true, flagsSet = false;
  clap_timestamp created = 0, modified = 0;
  std::string error;
};
Received* rx(const clap_preset_discovery_metadata_receiver_t* r) { return static_cast<Received*>(r->receiver_data); }
void rxError(const clap_preset_discovery_metadata_receiver_t* r, int32_t, const char* msg) {
  ++rx(r)->errors;
  rx(r)->error = msg ? msg : "";
}
bool rxBegin(const clap_preset_discovery_metadata_receiver_t* r, const char* name, const char* key) {
  ++rx(r)->begins;
  rx(r)->names.push_back(name ? name : "");
  rx(r)->keys.push_back(key ? key : "");
  if (key) rx(r)->nullKeys = false;
  return true;
}
void rxPlugin(const clap_preset_discovery_metadata_receiver_t* r, const clap_universal_plugin_id_t* id) {
  rx(r)->pluginIds.push_back(std::string(id->abi) + ":" + id->id);
}
void rxSoundpack(const clap_preset_discovery_metadata_receiver_t*, const char*) {}
void rxFlags(const clap_preset_discovery_metadata_receiver_t* r, uint32_t) { rx(r)->flagsSet = true; }
void rxCreator(const clap_preset_discovery_metadata_receiver_t* r, const char* c) { rx(r)->creators.push_back(c); }
void rxDescription(const clap_preset_discovery_metadata_receiver_t*, const char*) {}
void rxTimes(const clap_preset_discovery_metadata_receiver_t* r, clap_timestamp a, clap_timestamp b) {
  rx(r)->created = a;
  rx(r)->modified = b;
}
void rxFeature(const clap_preset_discovery_metadata_receiver_t* r, const char* f) { rx(r)->features.push_back(f); }
void rxExtra(const clap_preset_discovery_metadata_receiver_t*, const char*, const char*) {}

Received metadataFor(const clap_preset_discovery_provider_t* provider, uint32_t kind, const char* location,
                     bool* returned) {
  Received out;
  clap_preset_discovery_metadata_receiver_t r = {&out,         rxError,   rxBegin,       rxPlugin,
                                                  rxSoundpack, rxFlags,   rxCreator,     rxDescription,
                                                  rxTimes,     rxFeature, rxExtra};
  const bool ok = provider->get_metadata(provider, kind, location, &r);
  if (returned) *returned = ok;
  return out;
}

void testDiscovery(const std::string& root) {
  std::printf("CLAP preset discovery\n");
  const auto* factory = static_cast<const clap_preset_discovery_factory_t*>(
      clap_entry.get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
  check(factory && factory->count(factory) == 1, "the plugin offers one preset provider");
  if (!factory) return;
  const clap_preset_discovery_provider_descriptor_t* d = factory->get_descriptor(factory, 0);
  Indexed indexed;
  clap_preset_discovery_indexer_t indexer = {CLAP_VERSION_INIT, "test", "Sonorie", "", "1.0", &indexed,
                                              indexerFiletype,   indexerLocation, indexerSoundpack,
                                              indexerExtension};
  const clap_preset_discovery_provider_t* provider = factory->create(factory, &indexer, d->id);
  check(provider && provider->init(provider), "it is created and initialised");
  if (!provider) return;
  check(indexed.filetypes.size() == 1 && indexed.filetypes[0] == "sonorepreset", "it declares the .sonorepreset type");
  const std::string factoryFolder = joinPath(joinPath(joinPath(root, "Sonorie"), "Sonore Showcase"), "Factory Presets");
  const std::string userFolder = joinPath(joinPath(joinPath(root, "Sonorie"), "Sonore Showcase"), "Presets");
  check(indexed.locations.size() == 2 && indexed.locations[0].kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE &&
            indexed.locations[0].flags == CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT &&
            indexed.locationPaths[0] == factoryFolder && indexed.locations[1].kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE &&
            indexed.locations[1].flags == CLAP_PRESET_DISCOVERY_IS_USER_CONTENT && indexed.locationPaths[1] == userFolder,
        "two FILE locations: the factory presets as files (factory content) and the user's folder (user content)");
  const std::vector<FileEntry> factoryFiles = listDirectory(factoryFolder, {".sonorepreset"}, false);
  check(factoryFiles.size() == 3 && factoryFiles[0].name == "01 Warm Glue.sonorepreset" &&
            factoryFiles[2].name == "03 Folded.sonorepreset",
        "the factory folder holds 01 Warm Glue, 02 Crunch, 03 Folded");
  const std::vector<uint8_t> before = readBytes(factoryFiles[0].path);
  {
    Indexed again;
    clap_preset_discovery_indexer_t ix2 = indexer;
    ix2.indexer_data = &again;
    const clap_preset_discovery_provider_t* p2 = factory->create(factory, &ix2, d->id);
    p2->init(p2);
    p2->destroy(p2);
  }
  check(readBytes(factoryFiles[0].path) == before, "a second scan leaves the same bytes there");

  // One preset per file: single-file presets, no load key, flags from the location.
  bool allGood = true;
  std::string got;
  for (const FileEntry& f : factoryFiles) {
    bool ok = false;
    const Received r = metadataFor(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, f.path.c_str(), &ok);
    allGood = allGood && ok && r.begins == 1 && r.nullKeys && !r.flagsSet && r.pluginIds.size() == 1 &&
              r.pluginIds[0] == std::string("clap:") + kDesc.id && r.errors == 0;
    got += (got.empty() ? "" : ", ") + (r.names.empty() ? std::string("?") : r.names[0]);
  }
  check(allGood && got == "Warm Glue, Crunch, Folded",
        "each factory file is one preset, no load key, this plugin's id: " + got);
  const std::vector<FileEntry> userFiles = listDirectory(userFolder, {".sonorepreset"}, false);
  int userPresets = 0;
  for (const FileEntry& f : userFiles) {
    bool ok = false;
    const Received r = metadataFor(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, f.path.c_str(), &ok);
    if (ok && r.begins == 1 && r.nullKeys && r.modified > 0) ++userPresets;
  }
  check(userPresets == (int) userFiles.size() && userPresets >= 4,
        "and each user file too, with its times (" + std::to_string(userPresets) + " files)");
  {
    // Metadata a host can use: author, comment, category and tags as features.
    PresetMetadata m;
    m.name = "Tagged";
    m.author = "Ana";
    m.comment = "for testing";
    m.category = "Bass";
    m.tags = {"warm", "dark"};
    m.pluginId = kDesc.id;
    m.created = 1700000000;
    const std::string path = joinPath(userFolder, "Tagged.sonorepreset");
    writePresetFile(path, m, fakeState(7));
    bool ok = false;
    const Received r = metadataFor(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path.c_str(), &ok);
    check(ok && r.names.size() == 1 && r.names[0] == "Tagged" && r.creators.size() == 1 && r.creators[0] == "Ana" &&
              r.features.size() == 3 && r.features[0] == "Bass" && r.features[2] == "dark" && r.created == 1700000000,
          "author, category and tags reach the host as creator and features, the times as timestamps");
    pathio::remove(path.c_str());
  }
  {
    const std::string junk = joinPath(userFolder, "junk.sonorepreset");
    writeBytes(junk, {'S', 'N', 'R', 'P', 1, 0, 1, 0, 0, 0});
    bool ok = true;
    const Received r = metadataFor(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, junk.c_str(), &ok);
    check(!ok && r.errors == 1 && !r.error.empty() && r.begins == 0,
          "a damaged file is reported through on_error with the reason: \"" + r.error + "\"");
    pathio::remove(junk.c_str());
    PresetMetadata foreign;
    foreign.name = "Theirs";
    foreign.pluginId = "com.someone.else";
    const std::string theirs = joinPath(userFolder, "theirs.sonorepreset");
    writePresetFile(theirs, foreign, fakeState(8));
    const Received t = metadataFor(provider, CLAP_PRESET_DISCOVERY_LOCATION_FILE, theirs.c_str(), &ok);
    check(ok && t.begins == 0 && t.errors == 0, "another plugin's preset in the folder declares nothing, and is no error");
    pathio::remove(theirs.c_str());
  }
  {
    bool ok = false;
    const Received r = metadataFor(provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &ok);
    check(ok && r.begins == 3 && r.keys[1] == "1",
          "the compiled-in container (the fallback location) still answers, load key = index");
  }
  provider->destroy(provider);

  std::printf("CLAP preset-load\n");
  Plugin p;
  const auto* load = static_cast<const clap_plugin_preset_load_t*>(p.plugin->get_extension(p.plugin, CLAP_EXT_PRESET_LOAD));
  check(load != nullptr, "preset-load is offered");
  if (!load) return;
  const int loadedBefore = g_host.loaded;
  check(load->from_location(p.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, factoryFiles[1].path.c_str(), nullptr) &&
            p.presets().currentName() == "Crunch" && p.presets().selection().origin == PresetManager::Origin::Factory &&
            std::fabs(p.param(showcase::kDrive) - showcase::kPresetCrunch[showcase::kDrive]) < 1e-4f,
        "a factory preset FILE loads, and is that factory preset");
  const std::string mine = joinPath(userFolder, "My Sound.sonorepreset");
  check(load->from_location(p.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE, mine.c_str(), nullptr) &&
            p.presets().currentName() == "My Sound" && p.presets().currentIsUser() &&
            p.inst->dsp.expression == "abs(tanh(x))",
        "a user preset file loads, curve and all");
  check(load->from_location(p.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "2") &&
            p.presets().currentName() == "Folded",
        "a PLUGIN location loads by index");
  check(g_host.loaded == loadedBefore, "none of those is reported back: the host asked for them");
  const int errors = g_host.errors;
  check(!load->from_location(p.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "7") &&
            !load->from_location(p.plugin, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, "1x") &&
            !load->from_location(p.plugin, CLAP_PRESET_DISCOVERY_LOCATION_FILE,
                                 joinPath(userFolder, "missing.sonorepreset").c_str(), nullptr) &&
            g_host.errors == errors + 3 && !g_host.lastError.empty(),
        "an index out of range, a malformed key and a missing file are refused, each told to on_error");
}

// ── The preset bar, headless ─────────────────────────────────────────────────

Bitmap render(Component& c) {
  Bitmap bmp((int) c.bounds().w, (int) c.bounds().h);
  Graphics g(bmp);
  c.paintTree(g);
  return bmp;
}

void clickCentre(MouseRouter& router, Component& c) {
  const Rect b = c.bounds();
  const Point at = c.parent() ? c.parent()->localToRoot({b.x + b.w * 0.5f, b.y + b.h * 0.5f})
                              : Point(b.w * 0.5f, b.h * 0.5f);
  router.mouseDown(at);
  router.mouseUp(at);
}

bool pressAlertButton(AlertOverlay& alert, const std::string& label) {
  for (Component* child : alert.children())
    if (Widget* w = child->asWidget()) {
      Button* b = static_cast<Button*>(w); // an AlertOverlay holds only buttons
      if (b->label() == label && b->onClick) {
        b->onClick();
        return true;
      }
    }
  return false;
}

void testBar(const std::string& root) {
  std::printf("the preset bar, headless\n");
  Plugin p;
  PresetManager& m = p.presets();
  EditorHost host = clapwrap::makeEditorHost(p.inst);
  check(host.presets == &m, "the wrapper hands the editor its instance's preset manager");

  std::unique_ptr<AudioProcessorEditor> made = p.inst->dsp.createEditor(host);
  auto* editor = static_cast<showcase::ShowcaseEditor*>(made.get());
  editor->setBounds({0.0f, 0.0f, 780.0f, 500.0f});
  MouseRouter router(*editor);
  PresetBar& bar = editor->presetBar();
  check(bar.manager() == &m && bar.parent() == editor && bar.bounds().y == 0.0f && bar.bounds().x > 100.0f,
        "the showcase puts the bar in its menu strip, right of the menu names");
  int shown = 0;
  bar.showFolder = [&](const std::string& folder) {
    ++shown;
    return folder == m.store().folder();
  };

  // Rendered.
  bar.tick();
  check(bar.displayedName() == "No preset", "a fresh instance shows \"No preset\"");
  {
    const Bitmap bmp = render(*editor);
    const Rect nb = bar.nameField().bounds();
    const float x0 = bar.bounds().x + nb.x, y0 = bar.bounds().y + nb.y;
    int distinct = 0;
    const PremulColour ref = bmp.pixelAt((int) (x0 + 3), (int) (y0 + nb.h * 0.5f));
    for (int x = (int) x0 + 8; x < (int) (x0 + nb.w * 0.5f); ++x) {
      const PremulColour px = bmp.pixelAt(x, (int) (y0 + nb.h * 0.5f));
      if (std::abs((int) px.r - (int) ref.r) + std::abs((int) px.g - (int) ref.g) > 60) ++distinct;
    }
    check(distinct > 3 || !editor->font().isValid(),
          "it renders: the name is drawn in the field (" + std::to_string(distinct) + " text pixels)");
  }

  // The menu.
  bar.openMenu();
  check(bar.menuOverlay().isOpen(), "clicking the name drops the menu down");
  std::vector<std::string> items;
  for (int i = 0; i < bar.menuOverlay().menu().numItems(); ++i) items.push_back(bar.menuOverlay().menu().itemAt(i).text);
  auto has = [&](const std::string& t) { return std::find(items.begin(), items.end(), t) != items.end(); };
  check(has("Save As...") && has("Rename...") && has("Delete...") && has("Factory") && has("Warm Glue") &&
            has("Crunch") && has("Folded") && has("User") && has("My Sound") && has("Import...") && has("Export...") &&
            has("Show Folder"),
        "with Save, Save As, Rename, Delete, the factory list, the user's list, Import, Export, Show Folder");
  bar.menuOverlay().dismiss(PresetBar::kFactoryBase + 1);
  check(m.currentName() == "Crunch" && std::fabs(p.param(showcase::kDrive) - showcase::kPresetCrunch[0]) < 1e-4f,
        "choosing Crunch from it loads Crunch");
  bar.tick();
  check(bar.displayedName() == "Crunch", "and the bar says so");

  // ONE undo step, parameters and the plugin's own state together.
  {
    p.inst->dsp.setExpression("clamp(x, -1, 1)");
    const float driveBefore = p.param(showcase::kDrive);
    const int user = m.store().indexOfName("My Sound");
    check(bar.perform(PresetBar::kUserBase + user) && m.currentName() == "My Sound" &&
              p.inst->dsp.expression == "abs(tanh(x))",
          "loading \"My Sound\" from the bar brings its curve");
    check(p.inst->dsp.parameters.canUndo() &&
              p.inst->dsp.parameters.getUndoManager()->undoLabel() == "Load preset \"My Sound\"",
          "and is one undo step: \"" + p.inst->dsp.parameters.getUndoManager()->undoLabel() + "\"");
    const size_t steps = p.inst->dsp.parameters.getUndoManager()->size();
    editor->menuItemSelected(showcase::ShowcaseEditor::kUndo, 1);
    check(std::fabs(p.param(showcase::kDrive) - driveBefore) < 1e-4f && p.inst->dsp.expression == "clamp(x, -1, 1)" &&
              m.currentName() == "Crunch",
          "Edit > Undo puts back the knobs, the curve and the preset it was on");
    editor->menuItemSelected(showcase::ShowcaseEditor::kRedo, 1);
    check(m.currentName() == "My Sound" && p.inst->dsp.expression == "abs(tanh(x))" &&
              p.inst->dsp.parameters.getUndoManager()->size() == steps,
          "Redo brings \"My Sound\" back, still one step");
  }

  // Modified marker.
  {
    p.setParam(showcase::kMix, 33.0f);
    for (int i = 0; i < 8; ++i) bar.tick();
    check(bar.displayedName() == "My Sound *", "a knob moved: \"My Sound *\"");
    bar.saveButton().onClick();
    bar.tick();
    check(bar.displayedName() == "My Sound" && !m.isDirty(), "Save writes it over \"My Sound\": clean again");
    PresetFileContents c;
    std::string why;
    check(m.store().read(m.store().indexOfName("My Sound"), &c, &why) && !c.state.empty(), "and the file has it");
  }

  // Save As, with a name, then the same name again.
  {
    bar.perform(PresetBar::kSaveAs);
    check(bar.prompt().isShowing() && bar.prompt().parent() == editor, "Save As asks for a name, over the whole editor");
    bar.prompt().field().setText("   ");
    bar.prompt().okButton().onClick();
    check(bar.prompt().isShowing() && !bar.prompt().message().empty(),
          "a blank name keeps the question up: \"" + bar.prompt().message() + "\"");
    bar.prompt().field().setText("Bar \xE9\x9F\xB3");
    bar.prompt().okButton().onClick();
    check(!bar.prompt().isShowing() && m.currentName() == "Bar \xE9\x9F\xB3" && m.store().indexOfName("Bar \xE9\x9F\xB3") >= 0,
          "a name saves the preset and selects it");
    bar.perform(PresetBar::kSaveAs);
    bar.prompt().field().setText("bar \xE9\x9F\xB3");
    bar.prompt().okButton().onClick();
    check(bar.alert().isShowing(), "the same name again asks: replace, or keep both?");
    check(pressAlertButton(bar.alert(), "Keep both") && m.currentName() == "bar \xE9\x9F\xB3 (2)",
          "Keep both saves \"bar " "\xE9\x9F\xB3" " (2)\"");
    bar.perform(PresetBar::kSaveAs);
    bar.prompt().field().setText("Bar \xE9\x9F\xB3");
    bar.prompt().okButton().onClick();
    const size_t count = m.store().entries().size();
    check(pressAlertButton(bar.alert(), "Replace") && m.store().entries().size() == count &&
              m.currentName() == "Bar \xE9\x9F\xB3",
          "Replace writes over it: no new file");
  }

  // Rename, refused then accepted.
  {
    bar.perform(PresetBar::kRename);
    bar.prompt().field().setText("My Sound");
    bar.prompt().okButton().onClick();
    check(bar.prompt().isShowing() && bar.prompt().message().find("already exists") != std::string::npos,
          "Rename onto an existing name stays up and says why");
    bar.prompt().field().setText("Renamed");
    bar.prompt().okButton().onClick();
    bar.tick();
    check(!bar.prompt().isShowing() && m.currentName() == "Renamed" && m.store().indexOfName("Bar \xE9\x9F\xB3") < 0 &&
              bar.displayedName() == "Renamed",
          "a free name renames it, file and all, and the bar follows");
  }

  // Duplicate, then Delete -- to a trash that refuses once, then for good.
  {
    bar.perform(PresetBar::kDuplicate);
    check(m.store().indexOfName("Renamed copy") >= 0, "Duplicate makes \"Renamed copy\"");
    m.load(m.numFactory() + m.store().indexOfName("Renamed copy"));
    int trashed = 0;
    m.store().trashFile = [&](const std::string&, std::string* why) {
      ++trashed;
      if (why) *why = "the test's trash is full";
      return false;
    };
    bar.perform(PresetBar::kDelete);
    check(bar.alert().isShowing(), "Delete asks first");
    pressAlertButton(bar.alert(), "Delete");
    check(trashed == 1 && bar.alert().isShowing() && m.store().indexOfName("Renamed copy") >= 0,
          "a trash that refuses: the preset stays, and the bar asks about deleting it for good");
    pressAlertButton(bar.alert(), "Delete permanently");
    bar.tick();
    check(m.store().indexOfName("Renamed copy") < 0 && !m.hasSelection() && bar.displayedName() == "No preset",
          "deleted for good: gone, and nothing is selected");
    m.store().trashFile = nullptr;
  }

  // The same, while the folder changes under the question: the preset deleted
  // for good is the one the question named, not whichever now sits where it
  // sat in the list.
  {
    std::string why;
    check(m.saveAs("Zz Doomed", UserPresetStore::IfExists::Fail, &why), "Save As \"Zz Doomed\"");
    const std::string neighbour = m.itemAt(m.currentIndex() - 1).name;
    m.store().trashFile = [](const std::string&, std::string* w) {
      if (w) *w = "the test's trash is full";
      return false;
    };
    bar.perform(PresetBar::kDelete);
    // Another instance of the plugin saves a preset while the question is up,
    // one that sorts before every other.
    PresetMetadata elsewhere;
    elsewhere.name = "Aaa From Elsewhere";
    elsewhere.pluginId = kDesc.id;
    std::vector<uint8_t> blob;
    m.captureState(blob);
    const std::string elsewherePath = joinPath(m.store().folder(), "Aaa From Elsewhere.sonorepreset");
    writePresetFile(elsewherePath, elsewhere, blob);
    pressAlertButton(bar.alert(), "Delete");
    pressAlertButton(bar.alert(), "Delete permanently");
    m.rescan();
    check(m.store().indexOfName("Zz Doomed") < 0 && m.store().indexOfName(neighbour) >= 0 &&
              m.store().indexOfName("Aaa From Elsewhere") >= 0,
          "deleted for good is \"Zz Doomed\", the one asked about -- not \"" + neighbour + "\", now in its place");
    m.store().trashFile = nullptr;
    pathio::remove(elsewherePath.c_str());
    m.rescan();
  }

  // Rename, tried again after a refusal that rescanned the folder: the
  // preset renamed is still the one the prompt was opened on.
#if defined(_WIN32)
  {
    std::string why;
    check(m.saveAs("Zz Held", UserPresetStore::IfExists::Fail, &why), "Save As \"Zz Held\"");
    const std::string held = m.store().entries()[(size_t) (m.currentIndex() - m.numFactory())].path;
    const std::string neighbour = m.itemAt(m.currentIndex() - 1).name;
    bar.perform(PresetBar::kRename);
    // Another program has the file open, so the old file cannot be removed
    // (Windows refuses to delete an open file), and another instance saves a
    // preset that sorts first.
    std::FILE* open = pathio::open(held.c_str(), "rb");
    PresetMetadata elsewhere;
    elsewhere.name = "Aaa From Elsewhere";
    elsewhere.pluginId = kDesc.id;
    std::vector<uint8_t> blob;
    m.captureState(blob);
    const std::string elsewherePath = joinPath(m.store().folder(), "Aaa From Elsewhere.sonorepreset");
    writePresetFile(elsewherePath, elsewhere, blob);
    bar.prompt().field().setText("Zz Moved");
    bar.prompt().okButton().onClick();
    check(bar.prompt().isShowing() && !bar.prompt().message().empty(),
          "a rename the file system refuses keeps the prompt up: \"" + bar.prompt().message() + "\"");
    if (open) std::fclose(open);
    bar.prompt().okButton().onClick();
    m.rescan();
    check(!bar.prompt().isShowing() && m.store().indexOfName("Zz Moved") >= 0 && m.store().indexOfName("Zz Held") < 0 &&
              m.store().indexOfName(neighbour) >= 0,
          "tried again, it renames \"Zz Held\" -- not \"" + neighbour + "\", now in its place");
    pathio::remove(elsewherePath.c_str());
    m.rescan();
  }
#endif

  // Next and previous, by clicking.
  {
    m.load(0);
    clickCentre(router, bar.nextButton());
    bar.tick();
    check(m.currentName() == "Crunch" && bar.displayedName() == "Crunch", "clicking > loads the next preset");
    clickCentre(router, bar.previousButton());
    clickCentre(router, bar.previousButton());
    bar.tick();
    const std::string last = m.itemAt(m.count() - 1).name;
    check(m.currentName() == last, "clicking < twice wraps round to the last one: \"" + last + "\"");
  }

  // Import, Export, Show Folder -- with no dialog.
  {
    const std::string outside = joinPath(root, "bar outside");
    ensureDirectory(outside);
    PresetMetadata im;
    im.name = "Brought In";
    im.pluginId = kDesc.id;
    p.setParam(showcase::kTone, 1234.0f);
    std::vector<uint8_t> blob;
    m.captureState(blob);
    writePresetFile(joinPath(outside, "brought.sonorepreset"), im, blob);
    p.setParam(showcase::kTone, 9999.0f);
    bar.chooseFile = [&](bool saving, const std::string& suggested) {
      return saving ? joinPath(outside, suggested) : joinPath(outside, "brought.sonorepreset");
    };
    bar.perform(PresetBar::kImport);
    check(m.currentName() == "Brought In" && std::fabs(p.param(showcase::kTone) - 1234.0f) < 0.01f,
          "Import copies the file in and loads it");
    bar.perform(PresetBar::kExport);
    PresetFileContents c;
    std::string why;
    check(readPresetFile(joinPath(outside, "Brought In.sonorepreset"), &c, &why) && c.metadata.name == "Brought In",
          "Export writes the current sound where the dialog said");
    bar.perform(PresetBar::kShowFolder);
    check(shown == 1, "Show Folder opens the preset folder");
    bar.chooseFile = [](bool, const std::string&) { return std::string(); };
    const std::string current = m.currentName();
    bar.perform(PresetBar::kImport);
    check(m.currentName() == current, "a cancelled dialog does nothing");
    bar.chooseFile = [&](bool, const std::string&) { return joinPath(outside, "nonexistent.sonorepreset"); };
    bar.perform(PresetBar::kImport);
    check(bar.alert().isShowing() && !bar.lastError().empty(), "a failed import says why: \"" + bar.lastError() + "\"");
    pressAlertButton(bar.alert(), "OK");
  }

  // The Presets menu in the showcase's menu bar is the bar's menu.
  {
    editor->menuBar().showMenu(0);
    const PopupMenu& menu = editor->menuBar().menuOverlay().menu();
    bool saveAs = false, curves = false;
    for (int i = 0; i < menu.numItems(); ++i) {
      saveAs = saveAs || menu.itemAt(i).text == "Save As...";
      curves = curves || menu.itemAt(i).text == "Curves";
    }
    editor->menuBar().menuOverlay().dismiss(PresetBar::kFactoryBase + 2);
    check(saveAs && curves && m.currentName() == "Folded",
          "the showcase's Presets menu holds the bar's items and its curves, and a choice there loads");
  }
  made.reset();

#if defined(SONORE_HAS_NATIVE_WINDOW_BACKEND)
  // The generic editor, in a real window, gets the bar too.
  if (NativeEditor::isAvailable()) {
    NativeEditor native;
    if (native.open(nullptr, kDesc.params, kDesc.numParams, host, 420, 300)) {
      PresetBar* nb = native.presetBar();
      check(nb && nb->manager() == &m && nb->bounds().y == 0.0f && nb->bounds().h == PresetBar::kHeight &&
                nb->bounds().w == 420.0f,
            "the generic native editor opens with the preset bar along its top");
      native.setSize(600, 320);
      check(nb && nb->bounds().w == 600.0f, "and lays it out again when resized");
      native.close();
    } else {
      check(false, "the generic native editor opens");
    }
  }
#endif
}

// ── A preset loaded while audio runs ─────────────────────────────────────────

void testAudio() {
  std::printf("a preset loaded while audio runs\n");
  Plugin p;
  PresetManager& m = p.presets();
  m.rescan();
  constexpr uint32_t kFrames = 64;
  float inL[kFrames], inR[kFrames], outL[kFrames], outR[kFrames];
  for (uint32_t i = 0; i < kFrames; ++i) inL[i] = inR[i] = 0.25f * std::sin(0.05f * (float) i);
  float* inPtrs[2] = {inL, inR};
  float* outPtrs[2] = {outL, outR};
  clap_audio_buffer_t inBuf{}, outBuf{};
  inBuf.data32 = inPtrs;
  inBuf.channel_count = 2;
  outBuf.data32 = outPtrs;
  outBuf.channel_count = 2;
  clap_process_t process{};
  process.frames_count = kFrames;
  process.steady_time = -1;
  process.audio_inputs = &inBuf;
  process.audio_inputs_count = 1;
  process.audio_outputs = &outBuf;
  process.audio_outputs_count = 1;
  check(p.plugin->activate(p.plugin, 48000.0, 1, kFrames), "it activates");

  std::atomic<bool> stop{false}, armed{false};
  std::atomic<long> blocks{0};
  std::atomic<bool> finite{true};
  std::thread audio([&] {
    p.plugin->start_processing(p.plugin);
    for (int i = 0; i < 200; ++i) p.plugin->process(p.plugin, &process); // warm up
    t_audioThread = true;
    armed.store(true);
    while (!stop.load(std::memory_order_acquire)) {
      p.plugin->process(p.plugin, &process);
      for (uint32_t i = 0; i < kFrames; ++i)
        if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) finite.store(false);
      blocks.fetch_add(1, std::memory_order_relaxed);
    }
    t_audioThread = false;
    p.plugin->stop_processing(p.plugin);
  });
  while (!armed.load()) std::this_thread::yield();
  int loads = 0, attempts = 0;
  std::string why;
  // At least a second and sixty loads, whichever is later, and never more
  // than fifteen seconds: a sanitizer build loads a few a second (every user
  // preset recompiles the showcase's curve), a release build hundreds.
  const auto start = std::chrono::steady_clock::now();
  auto elapsed = [&] { return std::chrono::steady_clock::now() - start; };
  while ((loads < 60 || elapsed() < std::chrono::seconds(1)) && elapsed() < std::chrono::seconds(15)) {
    // Factory and user presets in turn: parameters, and the curve through
    // the DSP's own loadState. ("Future" is refused, which is a load that
    // changes nothing -- also while audio runs.)
    if (m.load(attempts++ % m.count(), &why)) ++loads;
  }
  stop.store(true, std::memory_order_release);
  audio.join();
  p.plugin->deactivate(p.plugin);
  char line[200];
  std::snprintf(line, sizeof(line), "%d preset loads against %ld blocks", loads, blocks.load());
  check(loads >= 20 && blocks.load() > 200, line);
  std::snprintf(line, sizeof(line), "no allocation on the audio thread (%ld)", g_audioAllocs.load());
  check(g_audioAllocs.load() == 0, line);
  check(finite.load(), "and every sample it made was finite");
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // Everything this test writes goes under a private folder, through a name
  // with a non-ASCII character in it, so every path is a UTF-8 path.
  const std::string root = joinPath(sonoretest::tempDir(), "presets \xC3\xB1\xE9\x9F\xB3");
  ensureDirectory(root);
  setEnv("SONORE_PRESET_ROOT", root);
  clap_entry.init("");
  testFormat();
  testStore(root);
  testPlugin(root);
  testQueuedEdits();
  testDiscovery(root);
  testBar(root);
  testAudio();
  clap_entry.deinit();
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  std::printf(g_failures == 0 ? "USER PRESETS TESTS PASSED\n" : "USER PRESETS TESTS FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
