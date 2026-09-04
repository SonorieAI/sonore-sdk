// SPDX-License-Identifier: Apache-2.0
// Proves the dependency-free licence verifier (sonore/license.h) accepts what
// Sonorie server signs and rejects everything else. The vectors below were minted
// once by the server's OWN signing code (src/lib/license/rsa-core.ts, format.ts)
// with a throwaway 2048-bit keypair: only the PUBLIC key and the signed blobs
// are here, never a private key. The scheme is raw RSA (sig^e mod n == SHA256),
// so a signature made there verifies here byte for byte; if this test fails, the
// two sides have drifted and every licensed plugin would reject valid keys.
#include <sonore/license.h>
#include <sonore/license_runtime.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace sonore::license;

static int fails = 0;
static void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) fails++;
}

// ── Fixed test vectors (server-minted, public data) ─────────────────────────
static const char* kPubKey =
    "10001,d3e24d84cf40afb72b6a4553f59b461f1f72a2368fa973479842b549b8153e9a"
    "59b58aa789c28cfa5fe3437bc538ba1147276caf5cca4972471a82396e52ec75c2183b47"
    "a66cdf093d2e5a6bb64529028786d57d8ab866c50f29f3a6354ad2cb55232581e9067825"
    "d4e623622e38808ad7bfc1f01efd4df9753cf3ceaab93267d8da5af23c8d195866ffa007"
    "86f21ff894e06262996563552b666a73183106a691859df8d24c6c91d77b7ada738dc048"
    "17c54becd4edab6cec6335c035dfa9ea65ee56413ba8ec57a1a7583eadd28cb9bd75aead"
    "4c4f5f9fb889bb7897f4d062645f35fc7e0c0e42c36e3f20d95339d7029b725e9c76e231"
    "8babf68a77f47523";
static const char* kProduct = "SNRE-TEST0001";
static const char* kEmail = "Buyer@Example.com"; // mixed case: asciiLower must match
static const char* kMachine = "0123456789abcdef0123456789abcdef";

static const char* kKey =
    "SNRE1-eyJsaWQiOiJsaWNfZml4ZWRfMSIsImlhdCI6MTczNTAwMDAwMCwic2lnIjoiMmU5ZjY0"
    "YjYwNTU4NzBjMjc5ZTFlZDk2OWQ3NmFhZDRkNmM1OGEzZDA4MjQ0Nzg5ZjAwNzE0YmE2YjlkZmYy"
    "ZWI3Nzk0NjkzNTE3ZGI0OGFmNjMyOGJhYmM4NjdkYTMzNTlhZjVkODZmODYzNWU0NjQwNTc0ZGRk"
    "NGFlYmM4NmYwZDA4MWYzM2YxZmQ0NWJmY2ZlZWMzM2ViNTMyOTY1MTI0NjVkODBiNDZlYTk1OWU2"
    "OTRlODkzY2M2NDFjMDAwMTA3MDA4ZTgxN2U4ZWUxZTA1NzVlNGIyMWRiOGFlNjBlYjY3ZWQxOWRj"
    "OTc5MjA5YjI1NTNkMzQzMGIwZGIyMzc3MDM2N2UyNTY1OTRhNjQ2OTM3MGFmNTEwNjAwOTQ3MThj"
    "M2FhMWIzYzM2MGRlYmQ2MDVmNGE2M2E4YWQyYzJmMjkzNjFlMzY3MjkzZjZiZDhhMzE4MDZiMDMw"
    "ODVkZDY3YmZkMzViMzgwZDI1Y2E5NWU1MmIyMjA2Nzg3Nzc4ODYyZWZhOGNhZWQ1MTgwZjhmZTU2"
    "MjFkZDczZGRjNmIwN2EyMDIxMTk5ZWRiMWRiN2I2ZWM4NWQwMWFkZTEyZTZjZjE0N2FkZWRhZTM0"
    "MjZlMjc3ZjFkMzRlYWM3NTViNmM1ZDZlNGRiZjM2OTk0YmU5OTNkMWQ3OTk5MTY3YzcifQ==";

static const char* kToken =
    "SNRA1-eyJsaWQiOiJsaWNfZml4ZWRfMSIsIm1pZCI6IjAxMjM0NTY3ODlhYmNkZWYwMTIzNDU2"
    "Nzg5YWJjZGVmIiwiZXhwIjoyMDAwMDAwMDAwLCJzaWciOiI3MTY1N2Y3NzBiZTEyYjMxM2UyYmRh"
    "ZmU1MTZhNDg5OGU0OTkxYTlhYzg4MjY2OTYzNWQ1MmQ0MmI5MjhlMzE5OWE5NmZhZjMzNTI2NGVm"
    "YTE2ODM4MjVhYjE1YzcwMjAxOWU3ZTE2ZjgyMDA1YmM4ZjgwOTdiMzZmMTBlNjU3ZTYwMmVmZTc4"
    "NzI2MjEwYTc2MzAyNzg4OWNjYTI1Njg5YzkzM2I2ZmNmNWU2MDNhZmQ3NGQxZTVjMWU4NjkxYmU2"
    "OGUzMzhkZWM5MGQzMzMzOTA3N2VjOWFkZjQ2YmVlZWVmN2M3ZGMyMWRmNWQ2NWI2OTUxYTNjNWNl"
    "MzNiZGQwZGFiODIwNzY1ZjQ3NGVjYWQyZmMxYmNkOTZjNDMwMTRjZWRhN2U1YTBiMjE3YjcwMjky"
    "N2QzMzMzNjk5YzY0MDkyM2E3MDgyMzliYzQwYTFjODU0MzFkODdmOGNlYTViMzEwMmNiOGU5MDk3"
    "YWY2NmVjYzUxODVjM2Y4ZDcyOTRkYjU5MTYxMDFlYjZmOGZlNTNmNDJiNjlkZTYzYzVjNjUxY2Zh"
    "NjI4YjA2ZTkzY2IyNzA2ZWZlOGYyNzJhYjcxNjUwYWQxNjFhYjlhZTE1NTU0NDI2NmEwZGYzNjdi"
    "NTEwYzFiZGI5MjM2YzU3NTFkYjliMWJmNTdkMzE3NjliOCJ9";

int main() {
  std::printf("Sonore SDK licence test\n\n");

  // The primitive first: SHA-256("abc") is a FIPS-published vector.
  {
    uint8_t d[32];
    sha256("abc", d);
    const uint8_t want[32] = {0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,
                              0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
                              0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    bool eq = true;
    for (int i = 0; i < 32; ++i) if (d[i] != want[i]) eq = false;
    check(eq, "SHA-256(\"abc\") matches the FIPS test vector");
  }

  // A server-signed KEY verifies, and only for the exact (product, email) it signs.
  check(verifyKey(kKey, kProduct, kEmail, kPubKey),
        "a server-signed licence key verifies through the from-scratch RSA");
  check(verifyKey(kKey, kProduct, "BUYER@EXAMPLE.COM", kPubKey),
        "...email is ASCII-lowercased, so case does not matter");
  check(!verifyKey(kKey, kProduct, "attacker@example.com", kPubKey),
        "...FAILS for a different email (the signature covers it)");
  check(!verifyKey(kKey, "SNRE-OTHER000", kEmail, kPubKey),
        "...FAILS for a different product code");
  check(!verifyKey("SNRE1-not-base64!!", kProduct, kEmail, kPubKey),
        "...FAILS on a malformed key rather than crashing");
  check(!verifyKey("nope", kProduct, kEmail, kPubKey),
        "...FAILS on a key without the SNRE1- prefix");

  // A server-signed device TOKEN verifies for this machine while unexpired.
  const long long now = 1735000001LL;
  check(verifyToken(kToken, kProduct, kMachine, now, kPubKey),
        "a server-signed activation token verifies for this machine");
  check(!verifyToken(kToken, kProduct, "ffffffffffffffffffffffffffffffff", now, kPubKey),
        "...FAILS on a different machine (machine-bound)");
  check(!verifyToken(kToken, kProduct, kMachine, 2100000000LL, kPubKey),
        "...FAILS once expired (time-bound)");
  check(!verifyToken(kKey, kProduct, kMachine, now, kPubKey),
        "...a KEY is not a TOKEN (domain separation by the SNRA1: prefix)");


  // ── The RUNTIME half: identity, storage, degradation, activation ─────────
  // license.h is pure maths and takes the machine id as a parameter. Everything
  // below is license_runtime.h, which is where the maths meets an actual
  // computer -- and where a mistake ships a plugin that either never activates
  // or never protects anything.

  const std::string midA = machineId("SNRE-PRODUCT01");
  const std::string midB = machineId("SNRE-PRODUCT02");
  check(midA.size() == 32, "the machine id is 32 characters");
  bool allHex = true;
  for (char c : midA)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) allHex = false;
  check(allHex, "...lower-case hex, so it survives the server's toLowerCase");
  check(midA == machineId("SNRE-PRODUCT01"), "...stable across calls on one machine");
  check(midA != midB,
        "...but DIFFERENT per product, so two sellers cannot correlate a buyer");

  // The .lic round-trip. Written to the real per-user folder, because the
  // thing worth proving is that the path we chose is actually writable.
  const std::string probeProduct = "SNRE-SELFTEST";
  LicFile out;
  out.token = "SNRA1-token-goes-here";
  out.email = "buyer@example.com";
  out.key = "SNRE1-key-goes-here";
  const bool wrote = writeLic(probeProduct, out);
  check(wrote, "a .lic can be written to the per-user folder");
  LicFile back;
  check(wrote && readLic(probeProduct, &back), "...and read back");
  check(back.token == out.token && back.email == out.email && back.key == out.key,
        "...with all three lines intact (token, email, key)");
  LicFile missing;
  check(!readLic("SNRE-NOSUCHPRODUCT", &missing),
        "...a product with no .lic reads as absent, not as garbage");
  std::remove(licPath(probeProduct).c_str());

  // Demo degradation. The contract is auditable-but-not-usable: audible most
  // of the time, silent for a moment every 12 s, and never left untouched.
  {
    const double sr = 48000.0;
    const int frames = 512;
    std::vector<float> buf((size_t) frames, 0.5f);
    float* chans[1] = {buf.data()};
    double phase = 0.0;
    uint32_t rng = 0x9e3779b9u;
    applyDemo(chans, 1, frames, sr, &phase, &rng);
    bool touched = false;
    for (float v : buf)
      if (v != 0.5f) touched = true;
    check(touched, "the demo alters the signal (hiss is added)");
    bool loud = true;
    for (float v : buf)
      if (v < 0.4f) loud = false;
    check(loud, "...but passes audio through outside the mute window");

    // 11.8 s in: inside the 0.6 s mute that ends the 12 s period.
    std::fill(buf.begin(), buf.end(), 0.5f);
    phase = 11.8;
    rng = 1;
    applyDemo(chans, 1, frames, sr, &phase, &rng);
    bool silent = true;
    for (float v : buf)
      if (v > 0.01f) silent = false;
    check(silent, "...and mutes inside the periodic demo gap");
  }

  // The gate's answers. Each one sends the user somewhere different, so a
  // wrong answer here is a support ticket rather than a crash.
  {
    Gate gate(kProduct, kPubKey);
    check(!gate.licensed(), "a gate with no .lic starts unlicensed");
    check(gate.activate("hello") == ActivateResult::invalid,
          "...random text is refused as invalid");
    check(gate.activate(kKey) == ActivateResult::needsToken,
          "...a purchase KEY is told to go get a token, not called invalid");
    check(gate.activate(kToken) == ActivateResult::wrongMachine,
          "...a token minted for another machine says so exactly");
    check(!gate.licensed(), "...and none of those activated anything");
  }

  // The overlay is JS the page eval()s: a syntax error there is a blank panel
  // with no way to activate, so its shape is asserted rather than assumed.
  {
    const std::string js = overlayScript("0123456789abcdef0123456789abcdef");
    check(js.find("0123456789abcdef0123456789abcdef") != std::string::npos,
          "the overlay shows this machine's code");
    check(js.find("__activate") != std::string::npos,
          "...and can send a pasted code back to C++");
    long depth = 0;
    for (char c : js) {
      if (c == '(') depth++;
      if (c == ')') depth--;
    }
    check(depth == 0, "...with balanced parentheses (it has to parse)");
    check(overlayResultScript(true, "").find("true") != std::string::npos,
          "a success reply tells the page to remove the overlay");
  }


  // ── The machine id has to hold up on machines nobody here owns ───────────
  // Every check below is a way a real computer differs from this one, and each
  // was a defect in the first version: an id built from the computer NAME (lost
  // on rename), from C:\ specifically (wrong when Windows is elsewhere, worse
  // when C: is removable), read from the 32-bit registry view (a 32-bit and a
  // 64-bit plugin disagreeing on one PC), or falling back to the HOSTNAME
  // (which DHCP can change by itself).
  {
    const std::string raw = rawDeviceId();
    check(!raw.empty(), "a raw device id is always produced, on any machine");
    // The tag says WHICH source answered, which is the difference between
    // debugging a support ticket and guessing at it.
    const bool tagged = raw.compare(0, 4, "win:") == 0 || raw.compare(0, 4, "vol:") == 0 ||
                        raw.compare(0, 4, "mac:") == 0 || raw.compare(0, 4, "lin:") == 0 ||
                        raw.compare(0, 4, "gen:") == 0;
    check(tagged, "...tagged with the source it came from");
    check(raw.size() > 8, "...and long enough to identify anything");
    check(raw == rawDeviceId(), "...and identical when asked twice");

    // The tags are namespaced, so two sources can never collide into one id
    // even if their payloads happened to match.
    uint8_t d1[32], d2[32];
    sha256("x:" + std::string("win:ABC"), d1);
    sha256("x:" + std::string("vol:ABC"), d2);
    bool differ = false;
    for (int i = 0; i < 32; ++i)
      if (d1[i] != d2[i]) differ = true;
    check(differ, "...and two sources with the same payload still differ");
  }

  // The last-resort id must SURVIVE, or a machine that falls back to it
  // re-activates on every launch.
  {
    const std::string first = persistedDeviceId();
    check(first.size() >= 16, "the fallback device id is long enough to be unique");
    check(first == persistedDeviceId(), "...and is the SAME on the next call");
    bool hex = true;
    for (char c : first)
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) hex = false;
    check(hex, "...and is hex, so it survives the server's lowercasing");
  }

  // Stability is the whole contract: an id that drifts silently ends a licence
  // the buyer already paid for.
  {
    const std::string a = machineId("SNRE-STABLE01");
    for (int i = 0; i < 25; ++i) {
      if (machineId("SNRE-STABLE01") != a) {
        check(false, "the machine id is stable across repeated calls");
        break;
      }
      if (i == 24) check(true, "the machine id is stable across repeated calls");
    }
  }

  std::printf(fails == 0 ? "\nSONORE LICENSE TEST PASSED\n" : "\n%d failure(s)\n", fails);
  return fails == 0 ? 0 : 1;
}
