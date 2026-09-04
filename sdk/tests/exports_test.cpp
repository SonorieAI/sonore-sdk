// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: what a built plugin EXPORTS, read from the binary.
//
// A plugin is a shared object loaded into somebody else's process beside
// other plugins, some of them built from this same SDK. Every symbol it
// exports beyond its entry point is a symbol the dynamic linker may resolve
// ACROSS plugins: two Sonore plugins in one host, each with its own inline
// `kDesc`, and the second one loaded answers a scan with the first one's
// name. That happened -- nine hand-built plugins all reported "Sonore Arp" --
// and the fix (hidden visibility plus -Bsymbolic) is a compiler flag, which
// is exactly the kind of fix that quietly stops being applied. So it is read
// back out of every artifact the build produced: the export table of a PE
// file, the dynamic symbol table of an ELF object. Anything beyond the
// format's entry points fails, and a binary with NO recognised entry point
// fails too, because that is what a broken parser looks like.
//
//   exports_test <dir> [<dir>...]      every .clap/.vst3/.so/.dll below them
//
// Mach-O is not parsed here: no Mac builds on this box, and a parser nobody
// can run is a parser nobody can trust. macOS CI runs `nm -gU` instead.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
}

static std::vector<uint8_t> readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static uint16_t u16(const std::vector<uint8_t>& b, size_t at) {
  return at + 2 <= b.size() ? (uint16_t) (b[at] | (b[at + 1] << 8)) : 0;
}
static uint32_t u32(const std::vector<uint8_t>& b, size_t at) {
  return at + 4 <= b.size() ? (uint32_t) b[at] | ((uint32_t) b[at + 1] << 8) |
                                  ((uint32_t) b[at + 2] << 16) | ((uint32_t) b[at + 3] << 24)
                            : 0;
}
static uint64_t u64(const std::vector<uint8_t>& b, size_t at) {
  return (uint64_t) u32(b, at) | ((uint64_t) u32(b, at + 4) << 32);
}
static std::string cstr(const std::vector<uint8_t>& b, size_t at) {
  std::string s;
  while (at < b.size() && b[at] != 0 && s.size() < 512) s += (char) b[at++];
  return s;
}

/** The names in a PE file's export directory. Empty for no exports. */
static bool peExports(const std::vector<uint8_t>& b, std::vector<std::string>& out) {
  if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') return false;
  const uint32_t pe = u32(b, 0x3C);
  if (pe + 24 > b.size() || std::memcmp(&b[pe], "PE\0\0", 4) != 0) return false;
  const uint16_t nSections = u16(b, pe + 6);
  const uint16_t optSize = u16(b, pe + 20);
  const size_t opt = pe + 24;
  const uint16_t magic = u16(b, opt);
  const size_t dataDir = magic == 0x20B ? opt + 112 : opt + 96;
  const uint32_t exportRva = u32(b, dataDir);
  const size_t sections = opt + optSize;
  auto rvaToOffset = [&](uint32_t rva) -> size_t {
    for (uint16_t i = 0; i < nSections; ++i) {
      const size_t s = sections + (size_t) i * 40;
      const uint32_t va = u32(b, s + 12), raw = u32(b, s + 16), ptr = u32(b, s + 20);
      const uint32_t vsize = u32(b, s + 8);
      const uint32_t span = raw > vsize ? raw : vsize;
      if (rva >= va && rva < va + span) return ptr + (rva - va);
    }
    return (size_t) -1;
  };
  if (exportRva == 0) return true; // a PE with no export directory at all
  const size_t ed = rvaToOffset(exportRva);
  if (ed == (size_t) -1) return false;
  const uint32_t nNames = u32(b, ed + 24);
  const size_t names = rvaToOffset(u32(b, ed + 32));
  if (names == (size_t) -1) return false;
  for (uint32_t i = 0; i < nNames; ++i) {
    const size_t at = rvaToOffset(u32(b, names + (size_t) i * 4));
    if (at != (size_t) -1) out.push_back(cstr(b, at));
  }
  return true;
}

/** Defined, global-or-weak, default-visibility symbols in an ELF64 .dynsym. */
static bool elfExports(const std::vector<uint8_t>& b, std::vector<std::string>& out) {
  if (b.size() < 64 || std::memcmp(b.data(), "\x7f" "ELF", 4) != 0) return false;
  if (b[4] != 2) return false; // 64-bit only; nothing here builds 32-bit
  const uint64_t shoff = u64(b, 0x28);
  const uint16_t shentsize = u16(b, 0x3A), shnum = u16(b, 0x3C);
  size_t dynsym = (size_t) -1, dynstr = (size_t) -1, dynsymSize = 0;
  for (uint16_t i = 0; i < shnum; ++i) {
    const size_t sh = (size_t) shoff + (size_t) i * shentsize;
    if (u32(b, sh + 4) == 11) { // SHT_DYNSYM
      dynsym = (size_t) u64(b, sh + 24);
      dynsymSize = (size_t) u64(b, sh + 32);
      const uint32_t link = u32(b, sh + 40);
      const size_t strSh = (size_t) shoff + (size_t) link * shentsize;
      dynstr = (size_t) u64(b, strSh + 24);
    }
  }
  if (dynsym == (size_t) -1 || dynstr == (size_t) -1) return true; // nothing dynamic
  for (size_t at = dynsym; at + 24 <= dynsym + dynsymSize; at += 24) {
    const uint32_t name = u32(b, at);
    const uint8_t info = b[at + 4], other = b[at + 5];
    const uint16_t shndx = u16(b, at + 6);
    const uint8_t bind = info >> 4, vis = other & 3;
    if (shndx == 0) continue;               // undefined: an import, not an export
    if (bind != 1 && bind != 2) continue;   // GLOBAL or WEAK
    if (vis != 0) continue;                 // DEFAULT visibility only is exported
    const std::string n = cstr(b, dynstr + name);
    if (n.empty()) continue;
    out.push_back(n);
  }
  return true;
}

int main(int argc, char** argv) {
  std::printf("── exported symbols, read from every built plugin ──────────────────\n");
  if (argc < 2) {
    std::printf("usage: exports_test <dir> [<dir>...]\n");
    return 2;
  }
  // What a format's loader looks for, and nothing else.
  static const std::set<std::string> kEntryPoints = {
      "clap_entry",                                   // CLAP
      "GetPluginFactory", "InitDll", "ExitDll",       // VST3 (Windows)
      "ModuleEntry", "ModuleExit",                    // VST3 (Linux)
      "bundleEntry", "bundleExit",                    // VST3 (macOS)
      "lv2_descriptor", "lv2ui_descriptor", "lv2_lib_descriptor", // LV2
  };
  // The linker's own bookkeeping on ELF: defined, global, default, and not ours.
  static const std::set<std::string> kLinkerNoise = {"_init", "_fini", "__bss_start", "_edata",
                                                    "_end", "__gmon_start__"};
  int binaries = 0;
  for (int a = 1; a < argc; ++a) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(argv[a], ec), end; it != end; it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file()) continue;
      // Fetched dependencies ship their own DLLs beside our build; those are
      // not ours to judge, and CMake's own scratch is not a plugin either.
      const std::string full = it->path().generic_string();
      if (full.find("/_deps/") != std::string::npos || full.find("/CMakeFiles/") != std::string::npos)
        continue;
      const std::string ext = it->path().extension().string();
      if (ext != ".clap" && ext != ".vst3" && ext != ".so" && ext != ".dll") continue;
      // A bare .so or .dll is a plugin only inside an LV2 bundle.
      if ((ext == ".so" || ext == ".dll") &&
          it->path().parent_path().extension().string() != ".lv2")
        continue;
      const std::vector<uint8_t> bytes = readAll(it->path());
      std::vector<std::string> names;
      bool parsed = false;
      if (bytes.size() > 4 && bytes[0] == 'M' && bytes[1] == 'Z') parsed = peExports(bytes, names);
      else if (bytes.size() > 4 && bytes[0] == 0x7f && bytes[1] == 'E') parsed = elfExports(bytes, names);
      else continue; // a Mach-O, a text file, a symlink target that is not a binary
      ++binaries;
      const std::string leaf = it->path().filename().string();
      if (!parsed) {
        check(false, leaf + ": the binary could not be parsed");
        continue;
      }
      std::vector<std::string> extra;
      int entries = 0;
      for (const std::string& n : names) {
        if (kEntryPoints.count(n)) { ++entries; continue; }
        if (kLinkerNoise.count(n)) continue;
        extra.push_back(n);
      }
      std::string line = leaf + ": " + std::to_string(entries) + " entry point(s), " +
                         std::to_string(extra.size()) + " other export(s)";
      check(entries > 0 && extra.empty(), line);
      for (size_t i = 0; i < extra.size() && i < 12; ++i)
        std::printf("       leaked: %s\n", extra[i].c_str());
      if (extra.size() > 12) std::printf("       ... and %zu more\n", extra.size() - 12);
    }
  }
  check(binaries > 0, std::to_string(binaries) + " plugin binaries examined");
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE EXPORTS TEST PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
