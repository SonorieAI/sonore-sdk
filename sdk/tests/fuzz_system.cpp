// SPDX-License-Identifier: Apache-2.0
// Mutation fuzzing for the system layer's parsers of text and bytes that
// arrive from somewhere else: URLs (a redirect's Location is the server's
// choice), IP and MAC address text, HTTP header lines, the framing of
// InterprocessConnection (a peer process is not trusted with an allocation),
// NetworkServiceDiscovery datagrams (anyone on the network can send one),
// wildcard patterns, search-path lists, and command lines split without a
// shell -- plus, on Windows, the argv quoting checked against
// CommandLineToArgvW itself for every case.
//
// The scheme is fuzz_parsers.cpp's: each seed, then every truncation, random
// byte flips, runs of the bytes that break grammars, and truncation plus
// flips, each case under a watchdog. Beyond "it came back", several targets
// check an INVARIANT that must hold for any input, and a violation is a
// failure like a crash:
//
//   url        parse -> toString -> parse -> toString is stable; the same for
//              every resolution result (RFC 3986 5.2 against a base, both ways)
//   ipaddress  whatever parses formats to text that parses to the same address
//   mac        likewise
//   frames     bytes fed whole and fed in random pieces decode to the same
//              messages and the same verdict
//   cmdline    (Windows) quoting a split command line and splitting it again
//              with CommandLineToArgvW gives back the same arguments
//
//   fuzz_system                     every target, a quick pass (ctest)
//   fuzz_system --iterations 1500   the deep pass (over 20,000 cases), under ASan
//   fuzz_system --target url        one target
//   fuzz_system --rng 7             other random streams (the default is 0x5eed)
#include <sonore/file_types.h>
#include <sonore/ipc.h>
#include <sonore/net.h>
#include <sonore/process.h>
#if defined(_WIN32)
#include <shellapi.h> // CommandLineToArgvW; after the windows.h the SDK headers bring
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace sonore;

namespace {

std::atomic<long long> g_deadlineMs{0};
std::string g_target;
const char* g_phase = "";
size_t g_case = 0;
std::vector<uint8_t> g_current;
std::atomic<bool> g_done{false};
size_t g_violations = 0;

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void saveCurrent(const char* name) {
  if (FILE* f = std::fopen(name, "wb")) {
    if (!g_current.empty()) std::fwrite(g_current.data(), 1, g_current.size(), f);
    std::fclose(f);
  }
}

using SignalHandler = void (*)(int);
SignalHandler g_previous[64] = {};

void onCrash(int sig) {
  saveCurrent("fuzz_system_crash.bin");
  std::fprintf(stderr, "  CRASH signal %d in %s phase=%s case=%zu -- the input is in fuzz_system_crash.bin\n", sig,
               g_target.c_str(), g_phase, g_case);
  std::fflush(stderr);
  const SignalHandler previous = (sig >= 0 && sig < 64) ? g_previous[sig] : nullptr;
  if (previous && previous != SIG_DFL && previous != SIG_IGN) {
    std::signal(sig, previous);
    std::raise(sig);
  }
  std::_Exit(98);
}

void watchdog() {
  while (!g_done.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const long long deadline = g_deadlineMs.load();
    if (deadline != 0 && nowMs() > deadline) {
      std::fprintf(stderr, "  HANG %s phase=%s case=%zu -- the input is in fuzz_system_hang.bin\n", g_target.c_str(),
                   g_phase, g_case);
      saveCurrent("fuzz_system_hang.bin");
      std::fflush(stderr);
      std::_Exit(99);
    }
  }
}

/** An invariant broken: reported with the input, counted, and the run fails. */
void violated(const char* what, const std::string& detail) {
  ++g_violations;
  if (g_violations <= 10) {
    std::string shown;
    for (uint8_t c : g_current) {
      char hex[5];
      if (c >= 32 && c < 127 && c != '\\') shown.push_back((char) c);
      else {
        std::snprintf(hex, sizeof(hex), "\\x%02x", c);
        shown += hex;
      }
    }
    std::fprintf(stderr, "  VIOLATION %s: %s\n    input: %s\n    %s\n", g_target.c_str(), what, shown.c_str(),
                 detail.c_str());
  }
}

std::string textOf(const std::vector<uint8_t>& b) { return std::string(b.begin(), b.end()); }
std::vector<uint8_t> bytesOf(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// ── The targets ─────────────────────────────────────────────────────────────

void fuzzUrl(const std::vector<uint8_t>& b) {
  const std::string text = textOf(b);
  const URL u(text);
  const std::string once = u.toString();
  const std::string twice = URL(once).toString();
  if (once != twice) violated("toString is not stable under reparsing", once + " -> " + twice);
  (void) u.isWellFormed();
  (void) u.getFileName();
  (void) u.getParameters();
  (void) u.getSubPath(true);
  (void) u.getPortOrDefault();
  (void) u.getParentURL().toString();
  (void) u.getChildURL(text).toString();
  (void) u.withParameter(text, text).getParameters();
  (void) URL::removeEscapeChars(text);
  (void) URL::addEscapeChars(text, (b.size() & 1) != 0);
  (void) netdetail::requestTarget(u);
  static const URL base("http://a/b/c/d;p?q");
  for (const URL& r : {base.resolve(u), u.resolve(base), u.resolve(u)}) {
    const std::string s = r.toString();
    const std::string again = URL(s).toString();
    if (s != again) violated("a resolution result does not reparse to itself", s + " -> " + again);
  }
}

void fuzzIpAddress(const std::vector<uint8_t>& b) {
  const std::string text = textOf(b);
  IPAddress ip;
  if (!IPAddress::parse(text, ip)) return;
  const std::string formatted = ip.toString();
  IPAddress back;
  if (!IPAddress::parse(formatted, back) || !(back == ip))
    violated("a parsed address does not survive formatting", text + " -> " + formatted);
  (void) ip.isLoopback();
  (void) ip.isMulticast();
  (void) IPAddress::convertIPv4MappedAddressToIPv4(ip).toString();
}

void fuzzMac(const std::vector<uint8_t>& b) {
  const MACAddress mac(textOf(b));
  if (mac.isNull()) return;
  if (MACAddress(mac.toString(":")) != mac) violated("a MAC address does not survive formatting", mac.toString());
}

void fuzzHttpHead(const std::vector<uint8_t>& b) {
  const std::string text = textOf(b);
  netdetail::HeaderList headers;
  (void) netdetail::parseHeaderBlock(text, headers);
  (void) netdetail::findHeader(headers, "content-length");
  std::string name, value;
  (void) netdetail::parseHeaderLine(text, name, value);
  const int status = netdetail::parseStatusLine(text);
  if (status < 0 || status > 999) violated("a status code out of range", std::to_string(status));
  URL::InputStreamOptions options;
  // withExtraHeaders splits at CR and LF, so what it keeps is a value
  // WebInputStream's own check (no CR, LF or NUL) lets through unless it
  // carried a NUL -- and a token never has a space in it.
  for (const auto& h : options.withExtraHeaders(text).extraHeaders) {
    if (!netdetail::isHttpFieldValue(h.second) && h.second.find('\0') == std::string::npos)
      violated("a header value withExtraHeaders kept is refused as one", h.second);
    if (netdetail::isHttpToken(h.first) && h.first.find_first_of(" \t:") != std::string::npos)
      violated("a header name with a space or colon passes as a token", h.first);
  }
  (void) netdetail::base64(text);
}

/** The same bytes whole and in random pieces must give the same messages. */
void fuzzFrames(const std::vector<uint8_t>& b) {
  const uint32_t magic = 0xf2b49e2cu;
  auto decode = [&](const std::vector<size_t>& cuts, std::vector<std::vector<uint8_t>>& out) {
    ipcdetail::FrameDecoder decoder(magic, 4096);
    size_t at = 0;
    bool ok = true;
    for (size_t cut : cuts) {
      const size_t end = cut < b.size() ? cut : b.size();
      if (end > at) {
        ok = decoder.feed(b.data() + at, end - at, [&](const std::vector<uint8_t>& m) { out.push_back(m); }) && ok;
        at = end;
      }
    }
    if (at < b.size())
      ok = decoder.feed(b.data() + at, b.size() - at, [&](const std::vector<uint8_t>& m) { out.push_back(m); }) && ok;
    return ok;
  };
  std::vector<std::vector<uint8_t>> whole, pieces;
  const bool okWhole = decode({}, whole);
  std::vector<size_t> cuts;
  uint32_t state = (uint32_t) b.size() * 2654435761u + 1;
  for (size_t at = 0; at < b.size();) {
    state = state * 1664525u + 1013904223u;
    at += 1 + (state >> 24) % 13;
    cuts.push_back(at);
  }
  const bool okPieces = decode(cuts, pieces);
  if (okWhole != okPieces || whole != pieces)
    violated("framing depends on how the bytes were split", std::to_string(whole.size()) + " vs " +
                                                                std::to_string(pieces.size()) + " message(s)");
}

void fuzzAdvertisement(const std::vector<uint8_t>& b) {
  ipcdetail::Advertisement ad;
  if (ipcdetail::parseAdvertisement(b.data(), b.size(), ad)) {
    if (ad.port <= 0 || ad.port > 65535 || ad.type.empty() || ad.instanceID.empty())
      violated("an accepted advertisement breaks its own rules", ad.type + "/" + ad.instanceID);
    const std::string again = ipcdetail::encodeAdvertisement(ad.type, ad.instanceID, ad.port, ad.description);
    ipcdetail::Advertisement back;
    if (!ipcdetail::parseAdvertisement((const uint8_t*) again.data(), again.size(), back) || back.port != ad.port ||
        back.instanceID != ad.instanceID)
      violated("a re-encoded advertisement does not parse back", again);
  }
}

void fuzzWildcard(const std::vector<uint8_t>& b) {
  const std::string text = textOf(b);
  const size_t split = text.find('|');
  const std::string pattern = split == std::string::npos ? text : text.substr(0, split);
  const std::string name = split == std::string::npos ? std::string("kick drum 01.wav") : text.substr(split + 1);
  (void) WildcardFileFilter::matchesWildcard(name, pattern);
  (void) WildcardFileFilter::matchesWildcard(pattern, name);
  const WildcardFileFilter filter(pattern, pattern, pattern);
  (void) filter.isFileSuitable(name);
  if (!WildcardFileFilter::matchesWildcard(name, "*")) violated("'*' did not match a name", name);
  const FileSearchPath path(text);
  (void) FileSearchPath(path.toString()).getNumPaths();
}

void fuzzCommandLine(const std::vector<uint8_t>& b) {
  std::string text = textOf(b);
  const std::vector<std::string> args = processdetail::splitCommandLine(text);
  (void) processdetail::legalObjectName(text, 40);
#if defined(_WIN32)
  // Quote, then let Windows' own parser undo it. NUL cannot be in a command
  // line, and malformed UTF-8 becomes U+FFFD on the way in, so the expected
  // arguments are what survives that conversion.
  std::vector<std::string> expected{"prog.exe"};
  for (std::string a : args) {
    a.erase(std::remove(a.begin(), a.end(), '\0'), a.end());
    expected.push_back(pathio::narrow(pathio::widen(a.c_str()).c_str()));
  }
  const std::string line = processdetail::windowsCommandLine(expected);
  int count = 0;
  LPWSTR* parsed = CommandLineToArgvW(pathio::widen(line.c_str()).c_str(), &count);
  std::vector<std::string> got;
  for (int i = 0; i < count; ++i) got.push_back(pathio::narrow(parsed[i]));
  if (parsed) LocalFree(parsed);
  if (got != expected) violated("CommandLineToArgvW does not undo windowsCommandLine", line);
#endif
}

void runOnce(const std::string& target, const std::vector<uint8_t>& b) {
  if (target == "url") fuzzUrl(b);
  else if (target == "ipaddress") fuzzIpAddress(b);
  else if (target == "mac") fuzzMac(b);
  else if (target == "http") fuzzHttpHead(b);
  else if (target == "frames") fuzzFrames(b);
  else if (target == "nsd") fuzzAdvertisement(b);
  else if (target == "wildcard") fuzzWildcard(b);
  else if (target == "cmdline") fuzzCommandLine(b);
}

void guarded(const std::string& target, const std::vector<uint8_t>& b) {
  g_current = b;
  g_deadlineMs.store(nowMs() + 8000);
  runOnce(target, b);
  g_deadlineMs.store(0);
}

size_t fuzzOne(const std::string& target, const std::vector<uint8_t>& seed, size_t iterations, uint32_t rngSeed) {
  size_t cases = 0;
  g_target = target;
  g_phase = "seed";
  g_case = 0;
  guarded(target, seed);
  ++cases;
  g_phase = "truncate";
  for (size_t len = 0; len < seed.size(); ++len) {
    g_case = len;
    guarded(target, std::vector<uint8_t>(seed.begin(), seed.begin() + (long) len));
    ++cases;
  }
  std::mt19937 rng(rngSeed);
  g_phase = "flip";
  for (size_t it = 0; it < iterations && !seed.empty(); ++it) {
    g_case = it;
    std::vector<uint8_t> b = seed;
    const int flips = 1 + (int) (rng() % 8);
    for (int k = 0; k < flips; ++k) b[rng() % b.size()] = (uint8_t) rng();
    guarded(target, b);
    ++cases;
  }
  // Runs of the characters that break URL, header and command-line grammars,
  // and the bytes that break size fields.
  g_phase = "runs";
  static const uint8_t kPatterns[] = {0x00, 0xFF, 0x7F, 0x80, '%', '/', '.', ':', '[', ']', '@', '?', '#',
                                      '"', '\\', ' ', '*', '\n', '\r', '=', ';', 0xC3};
  for (size_t it = 0; it < iterations && !seed.empty(); ++it) {
    g_case = it;
    std::vector<uint8_t> b = seed;
    const size_t at = rng() % b.size();
    const size_t len = 1 + rng() % 16;
    const uint8_t pat = kPatterns[rng() % sizeof(kPatterns)];
    for (size_t k = 0; k < len; ++k) {
      const uint8_t v = (rng() % 3 == 0) ? (uint8_t) rng() : pat;
      if (rng() % 2 && at + k <= b.size()) b.insert(b.begin() + (long) (at + k), v); // grow, too
      else if (at + k < b.size()) b[at + k] = v;
    }
    guarded(target, b);
    ++cases;
  }
  g_phase = "mixed";
  for (size_t it = 0; it < iterations / 2 && !seed.empty(); ++it) {
    g_case = it;
    std::vector<uint8_t> b(seed.begin(), seed.begin() + (long) (rng() % seed.size()));
    for (int k = 0; k < 3 && !b.empty(); ++k) b[rng() % b.size()] = (uint8_t) rng();
    guarded(target, b);
    ++cases;
  }
  return cases;
}

std::vector<uint8_t> framesSeed() {
  std::vector<uint8_t> out;
  for (const std::string& m : {std::string(), std::string("hello"), std::string(300, 'x'), std::string("\x01ping", 5)}) {
    const auto f = ipcdetail::encodeFrame(0xf2b49e2cu, m.data(), m.size());
    out.insert(out.end(), f.begin(), f.end());
  }
  return out;
}

struct Job {
  std::string target;
  std::vector<uint8_t> seed;
};

} // namespace

int main(int argc, char** argv) {
  size_t iterations = 150;
  std::string only;
  uint32_t seedValue = 0x5eed;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--iterations" && i + 1 < argc) iterations = (size_t) std::atol(argv[++i]);
    else if (a == "--target" && i + 1 < argc) only = argv[++i];
    else if (a == "--rng" && i + 1 < argc) seedValue = (uint32_t) std::strtoul(argv[++i], nullptr, 0);
    else {
      std::fprintf(stderr, "usage: fuzz_system [--iterations N] [--target name] [--rng first-seed]\n");
      return 2;
    }
  }
  for (int sig : {SIGSEGV, SIGABRT, SIGILL, SIGFPE}) {
    const SignalHandler previous = std::signal(sig, onCrash);
    if (sig >= 0 && sig < 64) g_previous[sig] = previous == SIG_ERR ? nullptr : previous;
  }
  std::thread dog(watchdog);

  const std::vector<Job> jobs = {
      {"url", bytesOf("HTTP://user:pw@[2001:DB8::1]:8080/a/./b/../c%20d.wav;p?x=1&y=two+words#frag")},
      {"url", bytesOf("../../g;x=1/./y?q#s/../x")},
      {"url", bytesOf("mailto:someone@example.com")},
      {"ipaddress", bytesOf("2001:db8:0:0:1:0:0:1")},
      {"ipaddress", bytesOf("::ffff:192.0.2.128")},
      {"ipaddress", bytesOf("192.168.100.255")},
      {"mac", bytesOf("aa:bb:cc:dd:ee:ff")},
      {"http", bytesOf("HTTP/1.1 301 Moved Permanently\r\nLocation: /x\r\nContent-Length: 0\r\nX-A:  b c \r\n\r\n")},
      {"frames", framesSeed()},
      {"nsd", bytesOf(ipcdetail::encodeAdvertisement("com.sonore.test", "1234-5678", 4242, "Studio A"))},
      {"wildcard", bytesOf("*.wav;k?ck*.AIF*,*a*a*a*b|kick drum 01.aiff")},
      {"cmdline", bytesOf("\"C:\\Program Files\\x.exe\" -a \"b c\" 'd e' f\\\"g \"h\\\\\" \\\\server\\share\\ \"\"")},
      // Round 24: the authority shapes a request is now checked against
      // (ports past 16 bits, user info with '@', a zone in a literal), header
      // blocks with folded and bare-CR lines, a frame whose length is the
      // limit exactly, and arguments ending in runs of backslashes.
      {"url", bytesOf("http://u@v@h:65616/x?y#z")},
      {"url", bytesOf("https://[fe80::1%25eth0]:0/a/../b?%0d%0a")},
      {"http", bytesOf("HTTP/1.1 200 OK\r\nX-Folded: a\r\n b\r\nX-Bare: c\rd\r\nTE:\r\n\r\n")},
      {"frames", [] {
         std::vector<uint8_t> f = ipcdetail::encodeFrame(0xf2b49e2cu, std::string(4096, 'y').data(), 4096);
         const auto tail = ipcdetail::encodeFrame(0xf2b49e2cu, "z", 1);
         f.insert(f.end(), tail.begin(), tail.end());
         return f;
       }()},
      {"cmdline", bytesOf("a\\\\\\ \"b\\\\\\\\\" \"\" c\\\\ \"d \\\\\\\"e\" '' f")},
  };

  std::printf("Sonore SDK system-layer fuzz -- %zu iterations per phase, first rng seed %u\n", iterations,
              (unsigned) seedValue);
  size_t total = 0;
  for (const Job& j : jobs) {
    if (!only.empty() && j.target != only) continue;
    const size_t before = g_violations;
    const size_t n = fuzzOne(j.target, j.seed, iterations, seedValue++);
    total += n;
    std::printf("  %-10s %6zu cases%s\n", j.target.c_str(), n,
                g_violations > before ? "  INVARIANT VIOLATED" : "");
    std::fflush(stdout);
  }
  g_done = true;
  dog.join();
  std::printf("%zu cases, %zu invariant violation(s)\n", total, g_violations);
  std::printf("%s\n", g_violations ? "SYSTEM FUZZ FAILED" : "SYSTEM FUZZ PASSED");
  return g_violations ? 1 : 0;
}
