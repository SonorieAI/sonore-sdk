// SPDX-License-Identifier: Apache-2.0
// User preset files, as a folder full of other people's files hands them over.
//
// A preset folder is written by this plugin, by older and newer versions of
// it, by sync clients that truncate, by people who rename a JPEG to
// .sonorepreset, and by whoever sent the zip. Everything in user_presets.h
// that reads such a file is fed mutations of real ones here:
//
//   * decodePresetFile on mutated containers. A file that decodes must be a
//     fixed point -- encoded again from what was decoded, it decodes to the
//     same metadata and the same state -- and its state must be a Sonore
//     state. Nothing may crash, hang or read outside the buffer (ASan).
//   * readPresetMetadata, the header-only reader a folder listing uses, on
//     the same mutations written to disk: it must never accept a file the
//     full reader refuses for anything but the checksum, and when both
//     accept, they agree about the name.
//   * UserPresetStore::importFile on mutated .sonorepreset and .vstpreset
//     files: an import that succeeds reads back with exactly the state that
//     went in.
//   * PresetManager::loadFile into a real plugin (a probe compiled in through
//     the CLAP wrapper, with a StateBag of its own): a preset that loads
//     leaves every parameter finite and in range, and one that is refused
//     leaves the plugin's saved state exactly as it was, byte for byte.
//   * presetFileStem and cleanPresetName on arbitrary bytes: always valid
//     UTF-8, never a forbidden character, a reserved device name, a leading
//     dot or a trailing dot or space, never longer than the cap, and the
//     same when applied twice.
//
//   fuzz_user_presets                     a quick pass (ctest)
//   fuzz_user_presets --iterations 50000  the deep pass, under ASan
#define SONORE_NUM_PARAMS 4

#include <sonore/dsp.h>
#include <sonore/state_bag.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

struct SonoreDsp {
  std::string sample = "kick.wav";
  int mode = 0;
  void prepare(const sonore::ProcessSpec&) {}
  void process(sonore::AudioBlock<float>& io, const float*) { io.clear(); }
  void saveState(sonore::StateBag& bag) const {
    bag.setString("sample", sample);
    bag.setInt("mode", mode);
  }
  void loadState(const sonore::StateBag& bag) {
    sample = bag.getString("sample", "kick.wav");
    mode = (int) bag.getInt("mode", 0);
  }
};

#include <sonore/plugin.h>
#include <sonore/presets.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"gain", "Gain", "dB", -60.0f, 12.0f, 0.0f},
    {"cutoff", "Cutoff", "Hz", 20.0f, 20000.0f, 1000.0f},
    {"mode", "Mode", "", 0.0f, 3.0f, 0.0f, 4},
    {"mix", "Mix", "%", 0.0f, 100.0f, 100.0f},
};
static const float kWarm[SONORE_NUM_PARAMS] = {-3.0f, 2000.0f, 1.0f, 50.0f};
static const sonore::Preset kPresets[1] = {{"Warm", kWarm, SONORE_NUM_PARAMS}};
static const sonore::PluginDescriptor kDesc = [] {
  sonore::PluginDescriptor d;
  d.id = "com.sonorie.test.fuzzpresets";
  d.name = "Sonore Preset Fuzz Probe";
  d.vendor = "Sonorie";
  d.version = "1.0.0";
  d.params = kParamTable;
  d.numParams = SONORE_NUM_PARAMS;
  d.presets = kPresets;
  d.numPresets = 1;
  return d;
}();

#include <sonore/clap_wrapper.h>

#include "temp_path.h"

using namespace sonore;

namespace {

int g_failures = 0;
void fail(const char* what, long iteration) {
  if (g_failures < 20) std::printf("  FAIL %s (iteration %ld)\n", what, iteration);
  ++g_failures;
}

void setEnv(const char* name, const std::string& utf8) {
#if defined(_WIN32)
  _wputenv_s(pathio::widen(name).c_str(), pathio::widen(utf8.c_str()).c_str());
#else
  setenv(name, utf8.c_str(), 1);
#endif
}

bool writeBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::FILE* f = pathio::open(path.c_str(), "wb");
  if (!f) return false;
  const bool ok = bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  std::fclose(f);
  return ok;
}

/** Mutations a real file meets: flipped bits, bytes set to edges, cuts,
 *  insertions, a length field set to something enormous -- and, a third of
 *  the time, the checksum repaired afterwards, so the damage reaches the
 *  parser behind it instead of stopping at the checksum. */
std::vector<uint8_t> mutate(const std::vector<uint8_t>& seed, std::mt19937& rng, bool container) {
  std::vector<uint8_t> out = seed;
  auto pick = [&](size_t n) { return n ? (size_t) (rng() % n) : 0; };
  const int edits = 1 + (int) pick(4);
  for (int e = 0; e < edits; ++e) {
    const unsigned kind = (unsigned) pick(7);
    if (out.empty()) out.push_back(0);
    switch (kind) {
      case 0: out[pick(out.size())] ^= (uint8_t) (1u << pick(8)); break;
      case 1: {
        static const uint8_t edge[] = {0x00, 0x01, 0x7F, 0x80, 0xC0, 0xFF};
        out[pick(out.size())] = edge[pick(sizeof(edge))];
        break;
      }
      case 2: out.resize(pick(out.size() + 1)); break;
      case 3: out.insert(out.begin() + (long) pick(out.size() + 1), (uint8_t) rng()); break;
      case 4: { // a 32-bit field at a header offset, to an edge value
        static const uint32_t values[] = {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 1u << 20};
        static const size_t offsets[] = {4, 6, 8, 12, 16, 20, 24};
        const size_t at = offsets[pick(7)];
        const uint32_t v = values[pick(6)];
        for (int i = 0; i < 4 && at + (size_t) i < out.size(); ++i) out[at + (size_t) i] = (uint8_t) (v >> (8 * i));
        break;
      }
      case 5: { // a stretch duplicated
        const size_t from = pick(out.size()), len = pick(32);
        if (from + len <= out.size()) {
          std::vector<uint8_t> copy(out.begin() + (long) from, out.begin() + (long) (from + len));
          out.insert(out.begin() + (long) pick(out.size() + 1), copy.begin(), copy.end());
        }
        break;
      }
      default: out[pick(out.size())] = (uint8_t) rng(); break;
    }
  }
  if (container && out.size() >= 24 && pick(3) == 0) {
    const uint32_t crc = detail::presets::crc32(out.data(), out.size() - 4);
    for (int i = 0; i < 4; ++i) out[out.size() - 4 + (size_t) i] = (uint8_t) (crc >> (8 * i));
  }
  return out;
}

bool sameMetadata(const PresetMetadata& a, const PresetMetadata& b) {
  return a.name == b.name && a.author == b.author && a.comment == b.comment && a.category == b.category &&
         a.tags == b.tags && a.pluginId == b.pluginId && a.pluginVersion == b.pluginVersion &&
         a.created == b.created && a.modified == b.modified && a.factoryIndex == b.factoryIndex;
}

bool cleanStem(const std::string& stem) {
  if (stem.empty() || stem.size() > detail::presets::kMaxStemBytes) return false;
  if (!StateBag::isUtf8Text((const uint8_t*) stem.data(), stem.size())) return false;
  for (char c : stem)
    if (std::strchr("<>:\"/\\|?*", c) || (unsigned char) c < 0x20 || c == 0x7F) return false;
  if (stem[0] == '.' || stem[0] == ' ' || stem.back() == '.' || stem.back() == ' ') return false;
  const size_t dot = stem.find('.');
  return !detail::presets::isReservedDeviceName(dot == std::string::npos ? stem : stem.substr(0, dot));
}

std::vector<uint8_t> hostSave(const clap_plugin_t* plugin) {
  const auto* state = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
  std::vector<uint8_t> out;
  clap_ostream_t os{&out, [](const clap_ostream_t* s, const void* d, uint64_t n) -> int64_t {
                      auto* v = static_cast<std::vector<uint8_t>*>(s->ctx);
                      v->insert(v->end(), (const uint8_t*) d, (const uint8_t*) d + n);
                      return (int64_t) n;
                    }};
  state->save(plugin, &os);
  return out;
}

const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void hostRequest(const clap_host_t*) {}
const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "com.sonorie.fuzz-user-presets", "Sonorie", "", "1.0",
                           hostGetExtension,  hostRequest, hostRequest, hostRequest};

} // namespace

int main(int argc, char** argv) {
  long iterations = 2000;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--iterations") == 0) iterations = std::atol(argv[i + 1]);
  std::printf("fuzz_user_presets: %ld iterations per target\n", iterations);
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::mt19937 rng(20261014u);
  const std::string root = joinPath(sonoretest::tempDir(), "fuzz presets");
  ensureDirectory(root);
  setEnv("SONORE_PRESET_ROOT", root);

  // ── The seeds ──
  clap_entry.init("");
  const auto* factory = static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
  const clap_plugin_t* plugin = factory->create_plugin(factory, &kHost, kDesc.id);
  if (!plugin || !plugin->init(plugin)) {
    std::printf("  FAIL the probe does not instantiate\n");
    return 1;
  }
  clapwrap::Instance* inst = clapwrap::self(plugin);
  PresetManager& presets = clapwrap::presetsOf(inst);
  inst->params[0] = -12.0f;
  inst->dsp.sample = "snare \xC3\xB1.wav";
  std::string why;
  presets.saveAs("Seed", UserPresetStore::IfExists::Replace, &why);
  std::vector<uint8_t> state;
  presets.captureState(state);
  PresetMetadata meta;
  meta.name = "Seed \xE9\x9F\xB3";
  meta.author = "Fuzz";
  meta.comment = "two\nlines";
  meta.category = "Drums";
  meta.tags = {"a", "b"};
  meta.pluginId = kDesc.id;
  meta.created = 1700000000;
  meta.modified = 1700000001;
  const std::vector<uint8_t> rich = encodePresetFile(meta, state);
  PresetMetadata bare;
  bare.pluginId = kDesc.id;
  const std::vector<uint8_t> minimal = encodePresetFile(bare, {'S', 'N', 'R', 'S', 6, 0, 0, 0, 0, 0, 0, 0});
  std::vector<uint8_t> vst;
  {
    const std::string path = joinPath(root, "seed.vstpreset");
    writeVstPreset(path.c_str(), vstPresetClassIdFor(kDesc.id), state);
    if (std::FILE* f = pathio::open(path.c_str(), "rb")) {
      uint8_t b[4096];
      size_t got = 0;
      while ((got = std::fread(b, 1, sizeof(b), f)) > 0) vst.insert(vst.end(), b, b + got);
      std::fclose(f);
    }
  }

  // ── decodePresetFile ──
  long decoded = 0;
  for (long it = 0; it < iterations * 4; ++it) {
    const std::vector<uint8_t> blob = mutate(it & 1 ? rich : minimal, rng, true);
    PresetFileContents c;
    std::string reason;
    if (!decodePresetFile(blob.data(), blob.size(), &c, &reason)) {
      if (reason.empty()) fail("a refusal without a reason", it);
      continue;
    }
    ++decoded;
    if (c.state.size() < 12 || std::memcmp(c.state.data(), "SNRS", 4) != 0) fail("accepted a non-SNRS state", it);
    const std::vector<uint8_t> again = encodePresetFile(c.metadata, c.state);
    PresetFileContents d;
    if (!decodePresetFile(again.data(), again.size(), &d, &reason)) fail("a decoded preset does not re-encode", it);
    else if (!sameMetadata(c.metadata, d.metadata) || c.state != d.state) fail("decode is not a fixed point", it);
  }
  std::printf("  decode: %ld of %ld mutations were still presets\n", decoded, iterations * 4);

  // ── readPresetMetadata against the full reader, on disk ──
  long headerOnly = 0;
  const std::string scratch = joinPath(root, "scratch.sonorepreset");
  for (long it = 0; it < iterations / 4 + 1; ++it) {
    const std::vector<uint8_t> blob = mutate(rich, rng, true);
    writeBytes(scratch, blob);
    PresetMetadata m;
    PresetFileContents c;
    std::string a, b;
    const bool head = readPresetMetadata(scratch, &m, &a);
    const bool full = readPresetFile(scratch, &c, &b);
    if (head) ++headerOnly;
    if (full && !head) fail("the header reader refused a file the full reader accepts", it);
    if (head && !full && b.find("checksum") == std::string::npos)
      fail("the header reader accepted a file refused for more than its checksum", it);
    if (head && full && m.name != c.metadata.name) fail("the two readers disagree about the name", it);
  }
  std::printf("  header-only reads: %ld of %ld accepted\n", headerOnly, iterations / 4 + 1);

  // ── Imports ──
  UserPresetStore store;
  store.setFolder(joinPath(root, "imports"));
  store.setPluginId(kDesc.id);
  long imported = 0;
  for (long it = 0; it < iterations / 4 + 1; ++it) {
    const bool asVst = (it & 1) != 0;
    const std::vector<uint8_t> blob = asVst ? mutate(vst, rng, false) : mutate(rich, rng, true);
    const std::string source = joinPath(root, asVst ? "in.vstpreset" : "in.sonorepreset");
    writeBytes(source, blob);
    std::string path, reason;
    PresetFileContents before;
    const bool readable = store.readForImport(source, &before, &reason);
    if (!store.importFile(source, &path, &reason)) {
      if (readable) fail("a readable file did not import", it);
      continue;
    }
    ++imported;
    PresetFileContents back;
    if (!readPresetFile(path, &back, &reason) || back.state != before.state)
      fail("an imported preset does not read back with the state that went in", it);
    pathio::remove(path.c_str());
  }
  store.rescan();
  std::printf("  imports: %ld of %ld mutated files imported\n", imported, iterations / 4 + 1);

  // ── Into the plugin ──
  long applied = 0;
  const std::string incoming = joinPath(root, "incoming.sonorepreset");
  for (long it = 0; it < iterations / 2 + 1; ++it) {
    std::vector<uint8_t> blob;
    if (it % 3 == 0) {
      // The state inside mutated, the container intact: the plugin's own
      // loader is what is being fed.
      blob = encodePresetFile(meta, mutate(state, rng, false));
    } else {
      blob = mutate(rich, rng, true);
    }
    writeBytes(incoming, blob);
    const std::vector<uint8_t> savedBefore = hostSave(plugin);
    std::string reason;
    if (presets.loadFile(incoming, true, &reason)) {
      ++applied;
      for (int i = 0; i < SONORE_NUM_PARAMS; ++i) {
        const float v = inst->params[i];
        if (!std::isfinite(v) || v < kParamTable[i].minValue || v > kParamTable[i].maxValue)
          fail("a loaded preset left a parameter out of range", it);
      }
    } else if (hostSave(plugin) != savedBefore) {
      fail("a refused preset changed the plugin", it);
    }
  }
  std::printf("  plugin: %ld of %ld mutated presets loaded\n", applied, iterations / 2 + 1);
  plugin->destroy(plugin);
  clap_entry.deinit();

  // ── Names ──
  for (long it = 0; it < iterations * 4; ++it) {
    std::string name;
    const size_t length = (size_t) (rng() % 300);
    static const char* const pieces[] = {"CON", "nul", ".", " ", "COM\xC2\xB9", "\xE9\x9F\xB3", "\xF0\x9F\x8E\xB9",
                                         "/", "\\", ":", "*", "\t", "\n", "a", "Z", "\xC0\xAF", "\xFF", "lpt9"};
    while (name.size() < length) {
      if (rng() % 3 == 0) name.push_back((char) (rng() & 0xFF));
      else name += pieces[rng() % (sizeof(pieces) / sizeof(pieces[0]))];
    }
    const std::string stem = presetFileStem(name);
    if (!cleanStem(stem)) {
      if (g_failures < 6) {
        std::printf("  stem (%zu bytes):", stem.size());
        for (unsigned char ch : stem) std::printf(" %02X", ch);
        std::printf("\n");
      }
      fail("a stem a filesystem would refuse", it);
    }
    if (presetFileStem(stem) != stem) fail("a stem is not a fixed point", it);
    const std::string clean = cleanPresetName(name);
    if (clean.size() > detail::presets::kMaxNameBytes || !detail::presets::isCleanText(clean, false))
      fail("a cleaned name is not clean", it);
    if (cleanPresetName(clean) != clean) fail("cleaning a name is not a fixed point", it);
  }
  std::printf("  names: %ld names, every stem legal everywhere\n", iterations * 4);

  // ── The folder, operated on ──
  //
  // A random walk over everything a preset menu does -- save under each
  // answer to a clash, overwrite, rename, duplicate, delete, import -- with
  // names of every kind and length, while files arrive in the folder and
  // leave it from outside. What the store itself names is never a name
  // another preset already has, a file it reports written is there and
  // listed, and one it reports deleted is gone.
  {
    UserPresetStore walk;
    const std::string folder = joinPath(root, "walk");
    walk.setFolder(folder);
    walk.setPluginId(kDesc.id);
    walk.trashFile = [](const std::string& path, std::string* why) {
      if (why) *why = "this test has no trash";
      return pathio::remove(path.c_str()) == 0;
    };
    auto randomName = [&]() {
      static const char* const pieces[] = {"Bass", "bass", "BASS", " ", ".", "CON", "a/b", "?", "\xE9\x9F\xB3",
                                           "\xF0\x9F\x8E\xB9", " (2)", "copy", "x", "\t", "\xFF"};
      // A few names at the cap and past it, again and again: the clashes a
      // short random name almost never makes.
      static const size_t kLong[] = {196, 200, 240};
      if (rng() % 4 == 0) return std::string(kLong[rng() % 3], 'L');
      std::string name;
      const size_t length = (size_t) (rng() % 24);
      while (name.size() < length) name += pieces[rng() % (sizeof(pieces) / sizeof(pieces[0]))];
      return name;
    };
    auto pickEntry = [&]() { return walk.entries().empty() ? -1 : (int) (rng() % walk.entries().size()); };
    // Whether the preset at `path` has a name no other listed preset has.
    auto namedApart = [&](const std::string& path) {
      const int at = walk.indexOfPath(path);
      if (at < 0) return false;
      const std::string& name = walk.entries()[(size_t) at].metadata.name;
      for (size_t k = 0; k < walk.entries().size(); ++k)
        if ((int) k != at && detail::presets::equalsIgnoringAsciiCase(walk.entries()[k].metadata.name, name))
          return false;
      return true;
    };
    long done = 0;
    const std::string outside = joinPath(root, "walk outside.sonorepreset");
    // Fewer than the other targets: every write here is flushed to the disk,
    // as a saved preset is.
    for (long it = 0; it < iterations / 10 + 1; ++it) {
      std::string path, reason;
      const unsigned op = (unsigned) (rng() % 9);
      if (op == 0 || op == 1) {
        const auto how = (UserPresetStore::IfExists) (rng() % 3);
        if (!walk.save(randomName(), state, how, &path, &reason)) continue;
        if (!fileExists(path) || walk.indexOfPath(path) < 0) fail("a saved preset is not listed", it);
        else if (how == UserPresetStore::IfExists::KeepBoth && !namedApart(path))
          fail("keep both saved a second preset of the same name", it);
      } else if (op == 2) {
        const int i = pickEntry();
        if (i < 0 || !walk.overwrite(i, state, &reason)) continue;
      } else if (op == 3) {
        const int i = pickEntry();
        const std::string name = randomName();
        if (i < 0 || !walk.rename(i, name, &path, &reason)) continue;
        if (walk.indexOfPath(path) < 0 || !namedApart(path)) fail("a renamed preset is not listed apart", it);
      } else if (op == 4) {
        const int i = pickEntry();
        if (i < 0 || !walk.duplicate(i, &path, &reason)) continue;
        if (!namedApart(path)) fail("a duplicate has another preset's name", it);
      } else if (op == 5) {
        const int i = pickEntry();
        if (i < 0) continue;
        const std::string gone = walk.entries()[(size_t) i].path;
        const auto how = rng() % 2 ? UserPresetStore::Removal::ToTrash : UserPresetStore::Removal::Permanently;
        if (!walk.remove(i, how, &reason)) continue;
        if (fileExists(gone) || walk.indexOfPath(gone) >= 0) fail("a deleted preset is still there", it);
      } else if (op == 6) {
        PresetMetadata m = meta;
        m.name = randomName();
        writePresetFile(outside, m, state);
        if (!walk.importFile(outside, &path, &reason)) continue;
        if (!namedApart(path)) fail("an import has another preset's name", it);
      } else if (op == 7) {
        // From outside: a file of ours under any name, or the damage a sync
        // client leaves.
        PresetMetadata m = meta;
        m.name = randomName();
        const std::string dropped = joinPath(folder, presetFileStem(randomName()) + userPresetExtension());
        if (rng() % 2) writePresetFile(dropped, m, state);
        else writeBytes(dropped, mutate(encodePresetFile(m, state), rng, true));
        walk.rescan();
      } else {
        for (const FileEntry& f : listDirectory(folder, {userPresetExtension()}, false, true))
          if (rng() % 4 == 0) pathio::remove(f.path.c_str());
        walk.rescan();
      }
      ++done;
      for (const UserPresetStore::Entry& e : walk.entries())
        if (!fileExists(e.path)) fail("a listed preset has no file", it);
    }
    std::printf("  folder: %ld operations, %zu presets at the end\n", done, walk.entries().size());
  }

  std::printf(g_failures == 0 ? "FUZZ USER PRESETS PASSED\n" : "FUZZ USER PRESETS FAILED (%d)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
