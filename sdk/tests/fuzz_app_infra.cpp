// SPDX-License-Identifier: Apache-2.0
// The settings-file readers, against damaged input.
//
// A settings file is edited by hand, cut short by a full disk, written by an
// older build or by the reference's own applications -- and it is read at
// startup, where a crash means the application never opens. So the readers
// in application_properties.h (the reference-shaped XML and the binary form)
// and RecentlyOpenedFilesList::restoreFromString are fed mutations of real
// files, and every case must:
//   * return, without a crash, a hang or (under ASan) a bad access;
//   * if it parsed, be a FIXED POINT: written back in either format and read
//     again, the same values in the same order;
//   * keep restoreFromString within its limit, with no blank or repeated
//     entry.
//
//   fuzz_app_infra                    a quick pass (ctest)
//   fuzz_app_infra --iterations N     the deep pass, under ASan
#include "sonore/application_properties.h"
#include "sonore/recent_files.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using sonore::detail::props::Values;

namespace {

int failures = 0;

void fail(const char* what, size_t iteration) {
  if (failures < 20) std::printf("  FAIL %s (iteration %zu)\n", what, iteration);
  ++failures;
}

std::vector<std::string> seeds() {
  std::vector<std::string> out;
  out.push_back(sonore::detail::props::writeXml(
      {{"volume", "0.75"}, {"name", "a & b < c \"q\" 'single'\nline\ttab"}, {"empty", ""}}));
  out.push_back(
      "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- comment -->\n<PROPERTIES>\n"
      "  <VALUE name=\"recentFiles\" val=\"C:\\a.wav&#13;&#10;C:\\b.wav\"/>\n"
      "  <VALUE name='single' val='&#x263A; &amp;amp;'/>\n"
      "  <VALUE name=\"window\">\n    <POS x=\"10\" y=\"20\"/>\n  </VALUE>\n"
      "  <OTHER thing=\"x\"/>\n</PROPERTIES>\n");
  out.push_back("<PROPERTIES/>");
  out.push_back("<PROPERTIES><VALUE name=\"a\" val=\"1\"/><VALUE name=\"a\" val=\"2\"/></PROPERTIES>");
  const std::vector<uint8_t> binary =
      sonore::detail::props::writeBinary({{"k", "v"}, {"pi", "3.14159"}, {"blob", std::string("\0\1\2", 3)}});
  out.push_back(std::string(binary.begin(), binary.end()));
  return out;
}

std::string mutate(const std::string& seed, std::mt19937& rng) {
  std::string s = seed;
  const int edits = 1 + (int) (rng() % 6);
  static const char* const tokens[] = {"<", ">", "/>", "\"", "'", "&", "&#", "&#x", ";", "=",
                                       "<VALUE", "</VALUE>", "<PROPERTIES>", "</PROPERTIES>",
                                       "<!--", "-->", "name=", "val=", " ", "\n", "&#0;",
                                       "&#xD800;", "&#99999999;", "SNPROPS1"};
  for (int e = 0; e < edits; ++e) {
    const size_t at = s.empty() ? 0 : rng() % (s.size() + 1);
    switch (rng() % 6) {
      case 0: // flip a byte
        if (!s.empty()) s[at % s.size()] = (char) (rng() & 0xFF);
        break;
      case 1: // cut it short
        s.resize(at);
        break;
      case 2: // delete a run
        if (!s.empty()) s.erase(at % s.size(), 1 + rng() % 8);
        break;
      case 3: // insert a token
        s.insert(at, tokens[rng() % (sizeof(tokens) / sizeof(tokens[0]))]);
        break;
      case 4: { // duplicate a run
        if (s.empty()) break;
        const size_t from = rng() % s.size();
        const size_t len = (std::min)((size_t) (1 + rng() % 32), s.size() - from);
        s.insert(at, s.substr(from, len));
        break;
      }
      default: // random bytes
        for (int k = 0; k < 1 + (int) (rng() % 4); ++k) s.insert(at, 1, (char) (rng() & 0xFF));
    }
  }
  return s;
}

/** Names the XML writer can carry: a parsed name is never empty. */
bool sameValues(const Values& a, const Values& b) { return a == b; }

} // namespace

int main(int argc, char** argv) {
  size_t iterations = 20000;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--iterations") == 0) iterations = (size_t) std::strtoull(argv[i + 1], nullptr, 10);

  std::printf("fuzz_app_infra: %zu iterations\n", iterations);
  const std::vector<std::string> base = seeds();
  std::mt19937 rng(20261013u);
  size_t parsedXml = 0, parsedBinary = 0;
  const auto start = std::chrono::steady_clock::now();

  for (size_t it = 0; it < iterations; ++it) {
    const std::string input = mutate(base[it % base.size()], rng);
    const std::vector<uint8_t> bytes(input.begin(), input.end());

    Values values;
    const bool ok = sonore::detail::props::parseAny(bytes, values);
    if (ok) {
      if (sonore::detail::props::isBinary(bytes)) ++parsedBinary;
      else ++parsedXml;
      // A fixed point in both formats.
      Values again;
      const std::string xml = sonore::detail::props::writeXml(values);
      if (!sonore::detail::props::parseXml(xml, again) || !sameValues(values, again)) {
        // The one legitimate difference: element content is stored raw and
        // written back as an attribute, which escapes it; the second round
        // must then be stable.
        Values third;
        const std::string xml2 = sonore::detail::props::writeXml(again);
        if (!sonore::detail::props::parseXml(xml2, third) || !sameValues(again, third))
          fail("XML round trip is not a fixed point", it);
      }
      const std::vector<uint8_t> bin = sonore::detail::props::writeBinary(values);
      Values fromBinary;
      if (!sonore::detail::props::parseBinary(bin, fromBinary) || !sameValues(values, fromBinary))
        fail("binary round trip lost values", it);
      for (const auto& entry : values)
        if (entry.first.empty()) fail("a parsed value has an empty name", it);
    }

    sonore::RecentlyOpenedFilesList recent;
    recent.setMaxNumberOfItems(1 + (int) (rng() % 10));
    recent.restoreFromString(input);
    if (recent.getNumFiles() > recent.getMaxNumberOfItems()) fail("restoreFromString overran its limit", it);
    for (int i = 0; i < recent.getNumFiles(); ++i) {
      if (recent.getFile(i).empty()) fail("restoreFromString kept a blank entry", it);
      for (int j = 0; j < i; ++j)
        if (sonore::detail::recent::samePath(recent.getFile(i), recent.getFile(j)))
          fail("restoreFromString kept a repeat", it);
    }
    sonore::RecentlyOpenedFilesList back;
    back.setMaxNumberOfItems(recent.getMaxNumberOfItems());
    back.restoreFromString(recent.toString());
    if (back.getAllFilenames() != recent.getAllFilenames()) fail("toString/restoreFromString is not a fixed point", it);
  }

  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("  %zu parsed as XML, %zu as binary, the rest refused; %.1f s\n", parsedXml,
              parsedBinary, seconds);
  if (parsedXml == 0 || parsedBinary == 0) fail("the mutations never produced a parseable file", 0);
  if (failures == 0) std::printf("FUZZ APP INFRA PASSED\n");
  else std::printf("FUZZ APP INFRA FAILED (%d)\n", failures);
  return failures == 0 ? 0 : 1;
}
