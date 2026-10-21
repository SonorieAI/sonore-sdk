// SPDX-License-Identifier: Apache-2.0
// Random hands on the MIDI 2.0 assemblers, the MIDI collector and the OSC
// pattern matcher.
//
// Four kinds of case, in rotation:
//
//   * UMP word streams. Several messages -- Flex Data text, SysEx8, Mixed Data
//     Set chunks, stream names, SysEx7, single-packet setup and stream
//     messages, channel voice -- are encoded and their packets merged in a
//     random order that keeps each message's own order. Half the cases are
//     left alone, and then every assembler must hand back exactly what went
//     in. The other half are mutated -- bits flipped, packets dropped,
//     duplicated, words inserted, the run cut short -- and then nothing may
//     crash, read out of bounds or hand back more than its buffer holds.
//   * OSC patterns. A random pattern against random addresses, compared with
//     a deliberately naive recursive matcher written separately below: the
//     two must agree on every valid input, and the real one must not take
//     exponential time on the inputs where the naive one would.
//   * OSC datagrams. Valid messages with every argument type, colours among
//     them, sometimes in bundles, sometimes mutated, decoded and dispatched
//     through listeners.
//   * The MIDI collector, with NaN, infinite and backwards timestamps and
//     block sizes from negative to large: every position stays inside its
//     block, and nothing queued is lost.
//
//   fuzz_midi_osc [--cases N] [--seed S]
#include <sonore/midi_collector.h>
#include <sonore/osc.h>
#include <sonore/ump128.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace sonore;

namespace {

int g_failures = 0;
long g_compared = 0, g_positive = 0, g_roundTrips = 0, g_mutated = 0, g_datagrams = 0, g_blocks = 0;

void fail(const char* what, long caseIndex) {
  if (++g_failures <= 20) std::printf("  FAIL case %ld: %s\n", caseIndex, what);
}

struct Rng {
  std::mt19937 gen;
  explicit Rng(uint32_t seed) : gen(seed) {}
  uint32_t u32() { return gen(); }
  int below(int n) { return n > 0 ? (int) (gen() % (uint32_t) n) : 0; }
  bool chance(int percent) { return below(100) < percent; }
};

// ═══ UMP ═════════════════════════════════════════════════════════════════════

/** One message's packets, and what an assembler should give back for it. */
struct Msg {
  enum Kind { FlexText, Sysex8, Mds, StreamName, Sysex7, Single } kind = Single;
  std::vector<std::vector<uint32_t>> packets;
  std::vector<uint8_t> payload;
  uint32_t key = 0;
};

std::string randomText(Rng& r, int maxLen) {
  std::string s;
  const int n = r.below(maxLen + 1);
  for (int i = 0; i < n; ++i) s.push_back((char) (1 + r.below(255))); // any byte but 0
  return s;
}

std::vector<uint8_t> randomBytes(Rng& r, int maxLen) {
  std::vector<uint8_t> v((size_t) r.below(maxLen + 1));
  for (auto& b : v) b = (uint8_t) r.u32();
  return v;
}

struct Collect {
  std::vector<std::vector<uint32_t>>* out;
  void operator()(const uint32_t* w, size_t n) { out->emplace_back(w, w + n); }
};

void addSingle(Rng& r, Msg& m) {
  using namespace ump;
  Packet128 p{};
  switch (r.below(10)) {
    case 0: p = setTempo(r.u32(), (uint8_t) r.below(16)); break;
    case 1: {
      TimeSignature t;
      t.numerator = (uint8_t) r.u32();
      t.denominatorPower = (uint8_t) r.u32();
      p = setTimeSignature(t);
      break;
    }
    case 2: {
      KeySignature k;
      k.sharpsFlats = (int8_t) (r.below(16) - 8);
      k.tonic = (uint8_t) r.below(8);
      p = setKeySignature(k);
      break;
    }
    case 3: {
      ChordName c;
      c.chordType = (uint8_t) r.u32();
      c.tonicSharpsFlats = (int8_t) (r.below(16) - 8);
      p = setChordName(c);
      break;
    }
    case 4: {
      EndpointInfo e;
      e.numFunctionBlocks = (uint8_t) r.below(128);
      p = endpointInfoNotification(e);
      break;
    }
    case 5: {
      FunctionBlockInfo f;
      f.number = (uint8_t) r.below(32);
      f.firstGroup = (uint8_t) r.below(16);
      p = functionBlockInfoNotification(f);
      break;
    }
    case 6: p = endpointDiscovery((uint8_t) r.u32()); break;
    case 7: p = r.chance(50) ? startOfClip() : endOfClip(); break;
    case 8: {
      midici::DeviceIdentity id;
      id.family = (uint16_t) r.u32();
      id.model = (uint16_t) r.u32();
      p = deviceIdentityNotification(id);
      break;
    }
    default: {
      // Channel voice, one or two words.
      if (r.chance(50)) m.packets.push_back({0x20000000u | (r.u32() & 0x0FFFFFFFu)});
      else m.packets.push_back({0x40000000u | (r.u32() & 0x0FFFFFFFu), r.u32()});
      return;
    }
  }
  m.packets.emplace_back(p.begin(), p.end());
}

/** A case's messages: keys unique within the case, so that an unmutated
 *  merge has one right answer. */
std::vector<Msg> makeMessages(Rng& r) {
  using namespace ump;
  std::vector<Msg> msgs;
  const int count = 1 + r.below(7);
  std::vector<uint32_t> used;
  auto fresh = [&](uint32_t key) {
    if (std::find(used.begin(), used.end(), key) != used.end()) return false;
    used.push_back(key);
    return true;
  };
  int streamTexts = 0;
  for (int i = 0; i < count; ++i) {
    Msg m;
    Collect c{&m.packets};
    switch (r.below(6)) {
      case 0: {
        const uint8_t bank = (uint8_t) (1 + r.below(2)), status = (uint8_t) r.below(13);
        const uint8_t grp = (uint8_t) r.below(16), addr = (uint8_t) r.below(2), ch = (uint8_t) r.below(16);
        const uint32_t key = 0x10000000u | ((uint32_t) grp << 20) | ((uint32_t) addr << 16) |
                             ((uint32_t) ch << 12) | ((uint32_t) bank << 8) | status;
        if (!fresh(key)) continue;
        const std::string text = randomText(r, 70);
        toFlexText(bank, status, text.data(), text.size(), c, grp, addr, ch);
        m.kind = Msg::FlexText;
        m.payload.assign(text.begin(), text.end());
        m.key = key;
        break;
      }
      case 1: {
        const uint8_t grp = (uint8_t) r.below(16), stream = (uint8_t) r.below(4);
        const uint32_t key = 0x20000000u | ((uint32_t) grp << 8) | stream;
        if (!fresh(key)) continue;
        m.payload = randomBytes(r, 80);
        toSysex8(m.payload.data(), m.payload.size(), c, stream, grp);
        m.kind = Msg::Sysex8;
        m.key = key;
        break;
      }
      case 2: {
        MixedDataSetHeader h;
        h.group = (uint8_t) r.below(16);
        h.mdsId = (uint8_t) r.below(16);
        h.manufacturerId = (uint16_t) r.u32();
        const uint32_t key = 0x30000000u | ((uint32_t) h.group << 4) | h.mdsId;
        if (!fresh(key)) continue;
        m.payload = randomBytes(r, 100);
        toMixedDataSetChunk(h, m.payload.data(), m.payload.size(), c);
        m.kind = Msg::Mds;
        m.key = key;
        break;
      }
      case 3: {
        if (streamTexts >= 3) continue; // the assembler holds four at once
        const int which = r.below(3);
        const uint8_t block = (uint8_t) r.below(32);
        const uint32_t key = 0x40000000u | ((uint32_t) which << 8) | (which == 2 ? block : 0);
        if (!fresh(key)) continue;
        std::string text = randomText(r, 110);
        int n = 0;
        if (which == 0) n = endpointNameNotification(text.data(), text.size(), c);
        else if (which == 1) n = productInstanceIdNotification(text.data(), text.size(), c);
        else n = functionBlockNameNotification(block, text.data(), text.size(), c);
        (void) n;
        // What the encoder kept, after the length limit and the UTF-8 cut.
        const size_t limit = which == 0 ? kEndpointNameMax : (which == 1 ? kProductInstanceIdMax : kFunctionBlockNameMax);
        text.resize(ump::detail::utf8Prefix(text.data(), text.size(), limit));
        m.kind = Msg::StreamName;
        m.payload.assign(text.begin(), text.end());
        m.key = key;
        ++streamTexts;
        break;
      }
      case 4: {
        const uint8_t grp = (uint8_t) r.below(16);
        const uint32_t key = 0x50000000u | grp;
        if (!fresh(key)) continue;
        std::vector<uint8_t> sx = randomBytes(r, 40);
        for (auto& b : sx) b &= 0x7F;
        sx.insert(sx.begin(), 0xF0);
        sx.push_back(0xF7);
        toSysex7(sx.data(), sx.size(), c, grp);
        m.kind = Msg::Sysex7;
        m.payload = sx;
        m.key = key;
        break;
      }
      default:
        addSingle(r, m);
        m.kind = Msg::Single;
        break;
    }
    msgs.push_back(std::move(m));
  }
  return msgs;
}

struct Assemblers {
  ump::FlexTextAssembler flex;
  ump::StreamTextAssembler names;
  ump::Sysex8Assembler sx8;
  ump::MixedDataSetAssembler mds;
  ump::Sysex7Assembler sx7;
};

struct Got {
  int kind;
  std::vector<uint8_t> bytes;
  bool operator==(const Got& o) const { return kind == o.kind && bytes == o.bytes; }
  bool operator<(const Got& o) const { return kind != o.kind ? kind < o.kind : bytes < o.bytes; }
};

void decodeEverything(const uint32_t* w, size_t n) {
  using namespace ump;
  uint32_t u = 0;
  TimeSignature ts;
  Metronome met;
  KeySignature ks;
  ChordName cn;
  EndpointInfo ei;
  midici::DeviceIdentity id;
  StreamConfiguration sc;
  FunctionBlockInfo fb;
  MixedDataSetHeader mh;
  FlexHeader fh;
  uint8_t a = 0, b = 0, c = 0;
  decodeSetTempo(w, n, &u);
  decodeSetTimeSignature(w, n, &ts);
  decodeSetMetronome(w, n, &met);
  decodeSetKeySignature(w, n, &ks);
  decodeSetChordName(w, n, &cn);
  decodeEndpointDiscovery(w, n, &a, &b, &c);
  decodeEndpointInfo(w, n, &ei);
  decodeDeviceIdentity(w, n, &id);
  decodeStreamConfiguration(w, n, &sc);
  decodeFunctionBlockDiscovery(w, n, &a, &b);
  decodeFunctionBlockInfo(w, n, &fb);
  decodeMixedDataSetHeader(w, n, &mh);
  flexHeaderOf(w, n, &fh);
  isStartOfClip(w, n);
  isEndOfClip(w, n);
  MidiMessage mm;
  toMidi1(w[0], &mm);
  toSystem(w[0], &mm);
  toMidi1Narrowing(w, n, &mm);
}

void feed(Assemblers& as, const uint32_t* w, size_t n, std::vector<Got>& got, long caseIndex) {
  decodeEverything(w, n);
  if (as.flex.push(w, n)) {
    if (as.flex.size() > (size_t) ump::FlexTextAssembler::kCapacity || as.flex.text()[as.flex.size()] != 0)
      fail("flex text out of bounds or unterminated", caseIndex);
    got.push_back({0, std::vector<uint8_t>(as.flex.text(), as.flex.text() + as.flex.size())});
  }
  if (as.names.push(w, n)) {
    if (as.names.size() > (size_t) ump::StreamTextAssembler::kCapacity || as.names.text()[as.names.size()] != 0)
      fail("stream name out of bounds or unterminated", caseIndex);
    got.push_back({3, std::vector<uint8_t>(as.names.text(), as.names.text() + as.names.size())});
  }
  if (as.sx8.push(w, n)) {
    if (as.sx8.size() > (size_t) ump::Sysex8Assembler::kCapacity) fail("sysex8 over capacity", caseIndex);
    got.push_back({1, std::vector<uint8_t>(as.sx8.data(), as.sx8.data() + as.sx8.size())});
  }
  if (as.mds.push(w, n)) {
    if (as.mds.size() != as.mds.header().validBytes) fail("mds size disagrees with its header", caseIndex);
    got.push_back({2, std::vector<uint8_t>(as.mds.data(), as.mds.data() + as.mds.size())});
  }
  if (as.sx7.push(w, n)) {
    if (as.sx7.size() > (size_t) ump::Sysex7Assembler::kCapacity || as.sx7.size() < 2 ||
        as.sx7.data()[0] != 0xF0 || as.sx7.data()[as.sx7.size() - 1] != 0xF7)
      fail("sysex7 malformed on completion", caseIndex);
    got.push_back({4, std::vector<uint8_t>(as.sx7.data(), as.sx7.data() + as.sx7.size())});
  }
}

void umpCase(Rng& r, Assemblers& as, long caseIndex) {
  std::vector<Msg> msgs = makeMessages(r);

  // Merge: repeatedly take the next packet of a random message that has one.
  std::vector<size_t> next(msgs.size(), 0);
  std::vector<std::vector<uint32_t>> merged;
  for (;;) {
    std::vector<size_t> open;
    for (size_t i = 0; i < msgs.size(); ++i)
      if (next[i] < msgs[i].packets.size()) open.push_back(i);
    if (open.empty()) break;
    const size_t pick = open[(size_t) r.below((int) open.size())];
    merged.push_back(msgs[pick].packets[next[pick]++]);
  }

  const bool mutate = r.chance(50);
  std::vector<uint32_t> words;
  for (const auto& p : merged) words.insert(words.end(), p.begin(), p.end());
  if (mutate) {
    ++g_mutated;
    const int edits = 1 + r.below(6);
    for (int e = 0; e < edits && !words.empty(); ++e) {
      const size_t at = (size_t) r.below((int) words.size());
      switch (r.below(6)) {
        case 0: words[at] ^= 1u << r.below(32); break;                                   // a bit
        case 1: words[at] = r.u32(); break;                                              // a word
        case 2: words.erase(words.begin() + (std::ptrdiff_t) at); break;                 // a hole
        case 3: words.insert(words.begin() + (std::ptrdiff_t) at, r.u32()); break;       // a stray
        case 4: words.resize(at); break;                                                 // cut short
        default: {                                                                       // a repeat
          const size_t len = (size_t) (1 + r.below(4));
          if (at + len <= words.size())
            words.insert(words.begin() + (std::ptrdiff_t) at, words.begin() + (std::ptrdiff_t) at,
                         words.begin() + (std::ptrdiff_t) (at + len));
          break;
        }
      }
    }
  }

  std::vector<Got> got;
  const size_t used = ump::forEachPacket(words.data(), words.size(), [&](const uint32_t* w, size_t n) {
    feed(as, w, n, got, caseIndex);
    // And the same packet offered short, which every assembler must refuse.
    if (r.chance(5)) feed(as, w, (size_t) r.below((int) n), got, caseIndex);
  });
  if (used > words.size()) fail("forEachPacket consumed more than it was given", caseIndex);

  if (!mutate) {
    std::vector<Got> want;
    for (const Msg& m : msgs) {
      switch (m.kind) {
        case Msg::FlexText: want.push_back({0, m.payload}); break;
        case Msg::Sysex8: want.push_back({1, m.payload}); break;
        case Msg::Mds: want.push_back({2, m.payload}); break;
        case Msg::StreamName: want.push_back({3, m.payload}); break;
        case Msg::Sysex7: want.push_back({4, m.payload}); break;
        default: break;
      }
    }
    std::sort(want.begin(), want.end());
    std::sort(got.begin(), got.end());
    if (want == got) ++g_roundTrips;
    else fail("an unmutated interleaving did not come back exactly", caseIndex);
  }
  // Mutated or not, nothing may stay open forever: start the next case clean
  // only some of the time, so state left by a broken stream meets new input.
  if (r.chance(50)) {
    as.flex.reset();
    as.names.reset();
    as.sx8.reset();
    as.mds.reset();
    as.sx7.reset();
  }
}

// ═══ OSC patterns: a second, naive matcher to disagree with ═════════════════

bool naiveAddressChar(char c) {
  const unsigned char u = (unsigned char) c;
  if (u <= 32 || u == 127) return false;
  return std::string(" #*,/?[]{}").find(c) == std::string::npos;
}

std::vector<std::string> naiveSplit(const std::string& s, bool* ok) {
  std::vector<std::string> parts;
  *ok = s.size() >= 2 && s[0] == '/';
  if (!*ok) return parts;
  std::string cur;
  for (size_t i = 1; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '/') {
      if (cur.empty()) *ok = false;
      parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(s[i]);
    }
  }
  return parts;
}

bool naiveValidPart(const std::string& p) {
  for (size_t i = 0; i < p.size(); ++i) {
    const char c = p[i];
    if (c == '*' || c == '?') continue;
    if (c == '[') {
      size_t j = i + 1;
      if (j < p.size() && p[j] == '!') ++j;
      const size_t close = p.find(']', j);
      if (close == std::string::npos || close == j) return false;
      for (size_t k = j; k < close; ++k)
        if ((unsigned char) p[k] <= 32 || (unsigned char) p[k] == 127) return false;
      i = close;
      continue;
    }
    if (c == '{') {
      const size_t close = p.find('}', i);
      if (close == std::string::npos) return false;
      for (size_t k = i + 1; k < close; ++k)
        if (p[k] != ',' && !naiveAddressChar(p[k])) return false;
      i = close;
      continue;
    }
    if (!naiveAddressChar(c)) return false;
  }
  return true;
}

bool naiveMatch(const std::string& p, size_t pi, const std::string& s, size_t si) {
  if (pi == p.size()) return si == s.size();
  const char c = p[pi];
  if (c == '*') {
    for (size_t k = si; k <= s.size(); ++k)
      if (naiveMatch(p, pi + 1, s, k)) return true;
    return false;
  }
  if (c == '?') return si < s.size() && naiveMatch(p, pi + 1, s, si + 1);
  if (c == '[') {
    size_t j = pi + 1;
    bool negate = false;
    if (p[j] == '!') {
      negate = true;
      ++j;
    }
    const size_t close = p.find(']', j);
    if (si >= s.size()) return false;
    const unsigned char ch = (unsigned char) s[si];
    bool in = false;
    for (size_t k = j; k < close; ++k) {
      if (k + 2 < close && p[k + 1] == '-') {
        unsigned char a = (unsigned char) p[k], b = (unsigned char) p[k + 2];
        if (a > b) std::swap(a, b);
        if (ch >= a && ch <= b) in = true;
        k += 2;
      } else if ((unsigned char) p[k] == ch) {
        in = true;
      }
    }
    return in != negate && naiveMatch(p, close + 1, s, si + 1);
  }
  if (c == '{') {
    const size_t close = p.find('}', pi);
    size_t start = pi + 1;
    for (size_t k = pi + 1; k <= close; ++k) {
      if (k < close && p[k] != ',') continue;
      const std::string alt = p.substr(start, k - start);
      if (s.compare(si, alt.size(), alt) == 0 && si + alt.size() <= s.size() &&
          naiveMatch(p, close + 1, s, si + alt.size()))
        return true;
      start = k + 1;
    }
    return false;
  }
  return si < s.size() && s[si] == c && naiveMatch(p, pi + 1, s, si + 1);
}

bool naive(const std::string& pattern, const std::string& address) {
  bool pok = false, aok = false;
  const auto pp = naiveSplit(pattern, &pok);
  const auto ap = naiveSplit(address, &aok);
  if (!pok || !aok || pp.size() != ap.size()) return false;
  for (const auto& part : pp)
    if (!naiveValidPart(part)) return false;
  for (const auto& part : ap)
    for (char c : part)
      if (!naiveAddressChar(c)) return false;
  for (size_t i = 0; i < pp.size(); ++i)
    if (!naiveMatch(pp[i], 0, ap[i], 0)) return false;
  return true;
}

std::string randomPath(Rng& r, const char* alphabet, int maxLen) {
  std::string s = "/";
  const int n = r.below(maxLen + 1);
  const size_t a = std::strlen(alphabet);
  for (int i = 0; i < n; ++i) {
    if (r.chance(3)) s.push_back((char) (1 + r.below(255))); // anything at all
    else s.push_back(alphabet[r.below((int) a)]);
  }
  return s;
}

void patternCase(Rng& r, long caseIndex) {
  const bool shortCase = r.chance(85);
  std::string pattern = randomPath(r, "/abc?*[]!-{},", shortCase ? 10 : 300);
  int stars = 0;
  for (char c : pattern) stars += c == '*';
  const osc::OSCAddressPattern compiled(pattern);
  for (int i = 0; i < 6; ++i) {
    std::string address;
    if (i == 0 && compiled.isValid()) {
      // An address made to match, by replacing each wildcard with something
      // it could stand for, so the positive side gets exercised too.
      address = pattern;
      std::string built;
      for (size_t k = 0; k < address.size(); ++k) {
        const char c = address[k];
        if (c == '*' || c == '?') built += "ab";
        else if (c == '[') {
          const size_t close = address.find(']', k + 2);
          built.push_back('b');
          k = close == std::string::npos ? address.size() : close;
        } else if (c == '{') {
          const size_t close = address.find('}', k);
          const size_t comma = address.find(',', k);
          built += address.substr(k + 1, (comma < close ? comma : close) - k - 1);
          k = close == std::string::npos ? address.size() : close;
        } else built.push_back(c);
      }
      address = built;
    } else {
      address = randomPath(r, "/abc-!", shortCase ? 12 : 400);
    }
    const bool got = compiled.matches(address);
    if (got != osc::matchesPattern(pattern, address)) fail("OSCAddressPattern and matchesPattern disagree", caseIndex);
    // The naive matcher is exponential in stars; compare only where it is not.
    if (shortCase || stars <= 3) {
      ++g_compared;
      if (got) ++g_positive;
      if (got != naive(pattern, address)) {
        char msg[900];
        std::snprintf(msg, sizeof(msg), "pattern and naive matcher disagree: %s ~ %s (%d)", pattern.c_str(),
                      address.c_str(), (int) got);
        fail(msg, caseIndex);
      }
    }
  }
}

// ═══ OSC datagrams ══════════════════════════════════════════════════════════

osc::Argument randomArgument(Rng& r) {
  osc::Argument a;
  switch (r.below(10)) {
    case 0: return osc::Argument::makeInt((int32_t) r.u32());
    case 1: return osc::Argument::makeFloat((float) r.below(1000) / 7.0f);
    case 2: return osc::Argument::makeString(std::string((size_t) r.below(9), 'q'));
    case 3: return osc::Argument::makeBlob(randomBytes(r, 9));
    case 4: return osc::Argument::makeColour(osc::OSCColour::fromInt32RGBA(r.u32()));
    case 5: a.type = osc::Type::Int64; a.int64Value = (int64_t) r.u32() << 20; return a;
    case 6: a.type = osc::Type::Double; a.doubleValue = r.below(100) * 0.5; return a;
    case 7: a.type = osc::Type::BoolTrue; a.intValue = 1; return a;
    case 8: a.type = osc::Type::Nil; return a;
    default: a.type = osc::Type::Midi; a.intValue = (int32_t) r.u32(); return a;
  }
}

void datagramCase(Rng& r, const osc::Dispatcher& d, long caseIndex) {
  osc::Message m;
  m.address = r.chance(50) ? randomPath(r, "/abc?*[]{},", 12) : std::string("/fx/1/mix");
  const int args = r.below(5);
  for (int i = 0; i < args; ++i) m.arguments.push_back(randomArgument(r));
  std::vector<uint8_t> bytes = osc::encode(m);
  if (bytes.empty()) return;
  if (r.chance(30)) {
    // Wrapped in a bundle.
    std::vector<uint8_t> b = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const uint32_t n = (uint32_t) bytes.size();
    b.push_back((uint8_t) (n >> 24));
    b.push_back((uint8_t) (n >> 16));
    b.push_back((uint8_t) (n >> 8));
    b.push_back((uint8_t) n);
    b.insert(b.end(), bytes.begin(), bytes.end());
    bytes.swap(b);
  }
  const bool mutate = r.chance(50);
  if (mutate) {
    const int edits = 1 + r.below(4);
    for (int e = 0; e < edits && !bytes.empty(); ++e) {
      const size_t at = (size_t) r.below((int) bytes.size());
      switch (r.below(3)) {
        case 0: bytes[at] = (uint8_t) r.u32(); break;
        case 1: bytes.resize(at); break;
        default: bytes.insert(bytes.begin() + (std::ptrdiff_t) at, (uint8_t) r.u32()); break;
      }
    }
  }
  std::vector<osc::Message> out;
  const bool ok = osc::decodePacket(bytes.data(), bytes.size(), &out);
  ++g_datagrams;
  if (!mutate) {
    if (!ok || out.size() != 1 || out[0].arguments.size() != m.arguments.size()) {
      fail("an unmutated datagram did not decode", caseIndex);
      return;
    }
    for (size_t i = 0; i < m.arguments.size(); ++i)
      if (m.arguments[i].isColour() && out[0].arguments[i].getColour() != m.arguments[i].getColour())
        fail("a colour argument changed on the way through", caseIndex);
  }
  d.dispatch(out);
}

// ═══ The collector ══════════════════════════════════════════════════════════

double weirdTime(Rng& r, double base) {
  switch (r.below(12)) {
    case 0: return std::numeric_limits<double>::quiet_NaN();
    case 1: return std::numeric_limits<double>::infinity();
    case 2: return -std::numeric_limits<double>::infinity();
    case 3: return 1e300;
    case 4: return -5.0;
    default: return base + (double) (r.below(20000) - 2000) / 48000.0;
  }
}

void collectorCase(Rng& r, long caseIndex) {
  MidiMessageCollector c;
  const double rates[] = {44100.0, 48000.0, 96000.0, 0.0, -1.0, 8000.0};
  double t = 50.0;
  c.reset(rates[r.below(6)], r.chance(90) ? t : weirdTime(r, t));
  long queued = 0, delivered = 0;
  MidiBuffer out;
  const int ops = 1 + r.below(40);
  for (int op = 0; op < ops; ++op) {
    if (r.chance(60)) {
      const int n = r.below(300);
      for (int i = 0; i < n; ++i) {
        const MidiMessage m((int) (r.u32() & 0xFF), r.below(128), r.below(128));
        if (c.addMessageToQueue(m, r.chance(90) ? t + r.below(4000) / 48000.0 : weirdTime(r, t))) ++queued;
      }
    } else {
      const int sizes[] = {-3, 0, 1, 2, 64, 256, 512, 4096, 33};
      const int samples = sizes[r.below(9)];
      t += r.chance(90) ? (double) (samples > 0 ? samples : 1) / 48000.0 : (double) (r.below(200) - 100) / 1000.0;
      out.clear();
      c.removeNextBlockOfMessages(out, samples, r.chance(95) ? t : weirdTime(r, t));
      ++g_blocks;
      int last = 0;
      for (const auto& e : out) {
        if (e.samplePosition < 0 || (samples > 0 && e.samplePosition >= samples) || e.samplePosition < last)
          fail("a collector position outside its block, or out of order", caseIndex);
        last = e.samplePosition;
      }
      delivered += out.getNumEvents();
    }
  }
  // Everything queued comes out, given blocks to come out in.
  for (int i = 0; i < 64 && delivered < queued; ++i) {
    out.clear();
    t += 0.01;
    c.removeNextBlockOfMessages(out, 512, t);
    delivered += out.getNumEvents();
  }
  if (delivered != queued) fail("the collector lost messages", caseIndex);
}

} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  long cases = 24000;
  uint32_t seed = 20261013u;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--cases") == 0 && i + 1 < argc) cases = std::strtol(argv[++i], nullptr, 10);
    if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = (uint32_t) std::strtoul(argv[++i], nullptr, 10);
  }
  std::printf("fuzz_midi_osc: %ld cases, seed %u\n", cases, seed);
  Rng r(seed);
  // Large: the assemblers carry kilobytes of slots each.
  static Assemblers assemblers;
  osc::Dispatcher d;
  long dispatched = 0;
  d.addListener("/fx/1/mix", [&](const osc::Message&) { ++dispatched; });
  d.addListener("/fx/2/mix", [&](const osc::Message&) { ++dispatched; });
  d.addListener("/a/b", [&](const osc::Message&) { ++dispatched; });
  for (long i = 0; i < cases; ++i) {
    switch (i % 4) {
      case 0: umpCase(r, assemblers, i); break;
      case 1: patternCase(r, i); break;
      case 2: datagramCase(r, d, i); break;
      default: collectorCase(r, i); break;
    }
  }
  std::printf("  %ld UMP cases (%ld mutated), %ld exact round trips\n", (cases + 3) / 4, g_mutated, g_roundTrips);
  std::printf("  %ld pattern matches compared with the naive matcher, %ld of them matches\n", g_compared,
              g_positive);
  std::printf("  %ld datagrams decoded, %ld handler calls\n", g_datagrams, dispatched);
  std::printf("  %ld collector blocks drained\n", g_blocks);
  if (g_roundTrips == 0 || g_compared == 0 || g_positive == 0) fail("a case kind never ran its check", -1);
  std::printf("%s (%d failures)\n", g_failures == 0 ? "FUZZ MIDI-OSC PASSED" : "FUZZ MIDI-OSC FAILED", g_failures);
  return g_failures == 0 ? 0 : 1;
}
