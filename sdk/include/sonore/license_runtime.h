// SPDX-License-Identifier: Apache-2.0
#pragma once
// Sonorie licensing RUNTIME -- the platform half of license.h.
//
// license.h is pure verify crypto: it takes a machine id as a PARAMETER and has
// no idea what a machine, a file or a user is. That separation is deliberate --
// the crypto is provable in isolation and the same header runs in a unit test
// with no OS at all. This header is the other half, and every line of it is
// POLICY rather than mathematics: where the device id comes from, where the
// `.lic` lives, what an unlicensed build sounds like, and what the user sees
// when they have to activate.
//
// THE CONTRACT, which the rest of the design follows from: after ONE network
// moment (the server must witness a device once to count seats) every later
// check is pure local computation for the token's whole year -- an embedded
// public key and a file. NOTHING here may ever gate audio or UI on a network
// call. That is why activation is a PASTE: the user takes their machine code to
// sonorie.com/activate on any device and brings a token back. It works on an
// air-gapped machine, it works on Linux where there is no HTTP client to
// speak of, and it needs no TLS stack inside the plugin.
//
// Accepted losses, both industry-standard: an offline machine re-activates once
// a year, and moving the clock backwards defeats expiry.
//
// This header costs nothing unless a build opts in. `SONORE_LICENSE_PRODUCT`
// and `SONORE_LICENSE_PUBKEY` are what the marketplace generator defines; a
// personal export defines neither and compiles byte-identically to before.

#include "license.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  // windows.h's min/max MACROS break std::min/std::max everywhere else in the
  // translation unit. This bit us once already; it is not optional.
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <direct.h>
#else
  #include <sys/stat.h>
  #include <unistd.h>
  #if defined(__APPLE__)
    #include <uuid/uuid.h>
  #endif
#endif

namespace sonore {
namespace license {

// ── Device identity ─────────────────────────────────────────────────────────

// Defined below, with the rest of the .lic plumbing.
inline std::string licDir();

/** A random id generated once and kept beside the licence, for machines whose
 *  platform id cannot be read.
 *
 *  The last resort is deliberately NOT the hostname, which is what the first
 *  version of this used. A hostname is not an identity: renaming the computer
 *  changes it, and on a laptop DHCP can change it by itself -- both of which
 *  would silently invalidate a licence the user already activated. A random id
 *  written once is stable for as long as the user's profile exists, which is
 *  the same lifetime the `.lic` sitting next to it has anyway.
 *
 *  It is not a secret and does not need to be unpredictable: the server treats
 *  a machine id as opaque and anyone can send it any string. It only has to be
 *  unique and to stay put. */
inline std::string persistedDeviceId() {
  const std::string dir = licDir();
  if (dir.empty()) return std::string();
#if defined(_WIN32)
  const std::string path = dir + "\\device-id";
#else
  const std::string path = dir + "/device-id";
#endif
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    char buf[80];
    const size_t n = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);
    std::string id;
    for (size_t i = 0; i < n; ++i)
      if (buf[i] > ' ') id.push_back(buf[i]);
    if (id.size() >= 16) return id;
  }
  // Mint one. random_device is seeded by the OS everywhere we ship; the clock
  // and an address are mixed in so a platform whose random_device is a weak
  // PRNG (some MinGW builds) still cannot hand two machines the same id.
  std::string id;
  {
    std::random_device rd;
    uint64_t a = ((uint64_t) rd() << 32) ^ (uint64_t) rd();
    uint64_t b = ((uint64_t) rd() << 32) ^ (uint64_t) rd();
    a ^= (uint64_t) std::time(nullptr) * 0x9e3779b97f4a7c15ull;
    b ^= (uint64_t) (uintptr_t) &id;
    static const char* kHex = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) id.push_back(kHex[(a >> (i * 4)) & 0xF]);
    for (int i = 15; i >= 0; --i) id.push_back(kHex[(b >> (i * 4)) & 0xF]);
  }
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(id.data(), 1, id.size(), f);
    std::fclose(f);
  }
  // Returned even if it could not be written: a read-only profile then gets a
  // per-session id, which means activation will not stick -- and the user is
  // told that by `saveFailed` when the .lic write fails for the same reason,
  // rather than by a licence that mysteriously lapses.
  return id;
}

/** The raw, PLATFORM identifier for this machine. Never leaves the process:
 *  it is salted and hashed by `machineId()` below, because a raw hardware id
 *  is exactly the kind of thing that should not travel to a server. */
inline std::string rawDeviceId() {
#if defined(_WIN32)
  // MachineGuid: written by Windows at install and untouched by renaming the
  // computer, changing hardware or moving the disk. The volume serial this
  // used to read changes on a reformat AND on a drive-letter reassignment, and
  // the computer name changed the identity every time somebody renamed their
  // PC -- both silently ending an activation the user had already done.
  //
  // KEY_WOW64_64KEY is load-bearing: HKLM\SOFTWARE\Microsoft\Cryptography is a
  // REDIRECTED key, so without it a 32-bit plugin reads WOW6432Node and a
  // 64-bit one reads the real key. Same computer, two different machine ids,
  // and a user who activated their 64-bit host would find the 32-bit build
  // still in demo mode with no way to explain it.
  {
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
      char buf[128];
      DWORD size = (DWORD) sizeof(buf);
      DWORD type = 0;
      const LONG r =
          RegQueryValueExA(key, "MachineGuid", nullptr, &type, (LPBYTE) buf, &size);
      RegCloseKey(key);
      // REG_SZ is not required to be null-terminated, so the length is taken
      // from what the API reported, not from the bytes.
      if (r == ERROR_SUCCESS && type == REG_SZ && size > 1) {
        std::string id;
        for (DWORD i = 0; i < size && buf[i] != '\0'; ++i) id.push_back(buf[i]);
        if (!id.empty()) return "win:" + id;
      }
    }
  }
  // Locked-down or oddly-imaged machine: the serial of the drive Windows is
  // actually installed on. GetSystemDirectory rather than a hardcoded "C:\\",
  // because Windows does not have to be on C: -- and where it is not, C: may
  // be a REMOVABLE drive whose serial changes the day the user swaps the stick.
  {
    char sysDir[MAX_PATH];
    const UINT n = GetSystemDirectoryA(sysDir, (UINT) sizeof(sysDir));
    if (n >= 3 && n < sizeof(sysDir)) {
      const char root[4] = {sysDir[0], sysDir[1], sysDir[2], '\0'}; // "C:\"
      DWORD serial = 0;
      if (GetVolumeInformationA(root, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0) &&
          serial != 0) {
        return "vol:" + std::to_string((unsigned long) serial);
      }
    }
  }
  return "gen:" + persistedDeviceId();
#elif defined(__APPLE__)
  // gethostuuid() is libSystem, so this needs no framework link and no
  // Objective-C -- the same reason webview_cocoa.h talks to the runtime
  // directly rather than being written in .mm.
  {
    uuid_t id;
    struct timespec wait;
    wait.tv_sec = 0;
    wait.tv_nsec = 0;
    if (gethostuuid(id, &wait) == 0) {
      char text[37];
      uuid_unparse(id, text);
      return std::string("mac:") + text;
    }
  }
  // Sandboxed hosts (GarageBand, and anything else running under App Sandbox)
  // can be refused the hardware UUID outright. A generated id is used rather
  // than the hostname, which on a laptop changes by itself.
  return "gen:" + persistedDeviceId();
#else
  // systemd's machine-id, then dbus's copy of it (which predates systemd and
  // is still what some distributions ship). Hashing it with the product code
  // is not just our own privacy preference -- it is what systemd's own
  // documentation requires of anyone exposing this value outside the machine.
  for (const char* path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
    if (FILE* f = std::fopen(path, "rb")) {
      char buf[128];
      const size_t n = std::fread(buf, 1, sizeof(buf), f);
      std::fclose(f);
      std::string id;
      for (size_t i = 0; i < n; ++i)
        if (buf[i] > ' ') id.push_back(buf[i]);
      // An EMPTY machine-id is a real state, not a corrupt one: the file is
      // created before first boot completes on some images, and every machine
      // imaged from it would otherwise share one identity.
      if (id.size() >= 8) return "lin:" + id;
    }
  }
  return "gen:" + persistedDeviceId();
#endif
}

/** This device's id for THIS product: 32 hex characters.
 *
 *  Salted with the product code so the same machine reads differently to two
 *  different plugins -- one seller's activation records cannot be correlated
 *  with another's. Hashed so the raw hardware id never leaves the machine;
 *  what reaches the server identifies a device to us and nothing to anyone
 *  else. The server treats it as opaque (any string up to 128 chars) and
 *  lowercases it for the signed message, which `verifyToken` mirrors. */
inline std::string machineId(const std::string& productCode) {
  uint8_t digest[32];
  sha256(productCode + ":" + rawDeviceId(), digest);
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(32);
  for (int i = 0; i < 16; ++i) {
    out.push_back(kHex[(digest[i] >> 4) & 0xF]);
    out.push_back(kHex[digest[i] & 0xF]);
  }
  return out;
}

// ── The .lic file ───────────────────────────────────────────────────────────

/** The per-user directory holding activations, created if missing.
 *
 *  `Sonore/` -- not "Sonorie" -- on purpose: it is one of the frozen technical
 *  tokens, and renaming it would orphan every activation already on disk.
 *  Under %APPDATA% / ~/Library/Application Support / ~/.config, i.e. always a
 *  place a plugin can write without being installed as an administrator. */
inline std::string licDir() {
#if defined(_WIN32)
  const char* base = std::getenv("APPDATA");
  if (!base || !*base) base = std::getenv("USERPROFILE");
  if (!base || !*base) return std::string();
  const std::string dir = std::string(base) + "\\Sonore";
  _mkdir(dir.c_str());
  return dir;
#else
  const char* home = std::getenv("HOME");
  if (!home || !*home) return std::string();
  #if defined(__APPLE__)
  const std::string parent = std::string(home) + "/Library/Application Support";
  #else
  const std::string parent = std::string(home) + "/.config";
  #endif
  ::mkdir(parent.c_str(), 0755);
  const std::string dir = parent + "/Sonore";
  ::mkdir(dir.c_str(), 0755);
  return dir;
#endif
}

/** Where THIS product's activation lives. Empty when there is no home to
 *  write to, which the caller must treat as "cannot save", never as "no
 *  licence" -- those are different answers to the user. */
inline std::string licPath(const std::string& productCode) {
  const std::string dir = licDir();
  if (dir.empty()) return std::string();
#if defined(_WIN32)
  return dir + "\\" + productCode + ".lic";
#else
  return dir + "/" + productCode + ".lic";
#endif
}

/** The `.lic` payload, v2: token, then email, then the purchase key.
 *
 *  The email and key ride along so the plugin can RENEW silently before the
 *  token expires, without asking the user for anything again. An activation
 *  done through the web page has no key to store, and simply cannot
 *  auto-renew -- which is a smaller loss than it sounds, because that user
 *  already knows where the activation page is. */
struct LicFile {
  std::string token;
  std::string email;
  std::string key;
};

inline bool readLic(const std::string& productCode, LicFile* out) {
  const std::string path = licPath(productCode);
  if (path.empty() || !out) return false;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::string all;
  char buf[1024];
  size_t n = 0;
  // A licence file is ~1 KB. The cap is what stops a corrupt or hostile file
  // from being read into memory forever.
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && all.size() < 64u * 1024u)
    all.append(buf, n);
  std::fclose(f);

  std::string lines[3];
  int line = 0;
  for (char c : all) {
    if (c == '\n') {
      if (++line > 2) break;
      continue;
    }
    if (c != '\r') lines[line].push_back(c);
  }
  out->token = lines[0];
  out->email = lines[1];
  out->key = lines[2];
  return !out->token.empty();
}

inline bool writeLic(const std::string& productCode, const LicFile& in) {
  const std::string path = licPath(productCode);
  if (path.empty()) return false;
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const std::string body = in.token + "\n" + in.email + "\n" + in.key + "\n";
  const size_t wrote = std::fwrite(body.data(), 1, body.size(), f);
  std::fclose(f);
  return wrote == body.size();
}

// ── Demo degradation ────────────────────────────────────────────────────────

/** What an unlicensed build sounds like: a short mute every ~12 s plus a faint
 *  hiss. Auditionable, deliberately not usable on a record.
 *
 *  Realtime-safe by construction -- no allocation, no locks, no branches on
 *  anything but arithmetic. The phase advances with the block so the mute
 *  lands at the same wall-clock interval whatever the buffer size, and the
 *  per-channel RNG offset keeps the hiss decorrelated (identical noise in both
 *  channels reads as a centred artefact rather than as noise). */
template <typename Sample>
inline void applyDemo(Sample* const* channels, int numChannels, int frames,
                      double sampleRate, double* phase, uint32_t* rng) {
  if (!channels || numChannels <= 0 || frames <= 0 || sampleRate <= 0.0 || !phase || !rng)
    return;
  const double period = 12.0, mute = 0.6;
  for (int ch = 0; ch < numChannels; ++ch) {
    Sample* d = channels[ch];
    if (!d) continue;
    uint32_t r = *rng + (uint32_t) ch * 2654435761u;
    for (int i = 0; i < frames; ++i) {
      double t = *phase + (double) i / sampleRate;
      t -= period * (double) (long long) (t / period); // fmod without the call
      const Sample gain = (t > period - mute) ? (Sample) 0 : (Sample) 1;
      r = r * 1664525u + 1013904223u;
      const Sample hiss = (Sample) ((((float) (r >> 8) / 8388608.0f) - 1.0f) * 0.0022f);
      d[i] = d[i] * gain + hiss;
    }
  }
  *rng = *rng * 1664525u + 1013904223u;
  *phase += (double) frames / sampleRate;
  if (*phase > 1.0e6) *phase = 0.0; // never let the accumulator lose precision
}

// ── The gate ────────────────────────────────────────────────────────────────

/** Why an activation attempt failed, in the user's terms rather than ours. */
enum class ActivateResult {
  activated,
  /** Not a Sonorie code at all, or not signed by us. */
  invalid,
  /** A valid token, but minted for a different machine. */
  wrongMachine,
  /** A valid token whose year is up. */
  expired,
  /** A purchase KEY where a device TOKEN is needed -- the user pasted the
   *  thing from their receipt, which is the commonest mistake and must not
   *  read as "your licence is wrong". */
  needsToken,
  /** Verified, but the file could not be written (a locked-down machine).
   *  Distinct from `invalid` on purpose: a correct key that cannot be saved
   *  must never be reported as the wrong key. */
  saveFailed,
};

/**
 * One plugin instance's view of the licence.
 *
 * `licensed()` is read on the AUDIO thread every block, so the flag is atomic
 * and nothing else here is touched from there. Everything that reads a file or
 * verifies a signature happens on the main thread: at construction, when an
 * editor opens, and when the user pastes a code.
 */
class Gate {
 public:
  Gate(std::string productCode, std::string publicKey)
      : product_(std::move(productCode)),
        publicKey_(std::move(publicKey)),
        machine_(machineId(product_)) {
    refresh();
  }

  /** [audio-thread safe] */
  bool licensed() const { return licensed_.load(std::memory_order_relaxed); }

  /** [audio-thread] Degrade one block, when and only when unlicensed.
   *
   *  The cadence state lives here rather than per-instance on purpose: this is
   *  ONE product, and every instance of it muting together reads as a demo
   *  rather than as a broken plugin. Relaxed atomics because two instances
   *  really can process concurrently, and a plain double would be a data race;
   *  what a race can cost here is a shifted hiss sample, never the mute. */
  template <typename Sample>
  void demoBlock(Sample* const* channels, int numChannels, int frames) {
    double phase = phase_.load(std::memory_order_relaxed);
    uint32_t rng = rng_.load(std::memory_order_relaxed);
    applyDemo(channels, numChannels, frames, rate_.load(std::memory_order_relaxed), &phase, &rng);
    phase_.store(phase, std::memory_order_relaxed);
    rng_.store(rng, std::memory_order_relaxed);
  }

  /** [main-thread] Told at prepare() where a wrapper knows it. Purely the demo
   *  CADENCE: the default is a working rate, not a guess that can break
   *  anything, so a format that never calls this still degrades correctly --
   *  its mute simply lands a few percent early or late, which no listener of a
   *  demo can perceive or care about. */
  void setSampleRate(double hz) {
    if (hz > 0.0) rate_.store(hz, std::memory_order_relaxed);
  }


  const std::string& machine() const { return machine_; }
  const std::string& product() const { return product_; }

  /** [main-thread] Re-read the `.lic` and re-verify it.
   *
   *  Called when an editor opens, because THIS instance may predate an
   *  activation performed in another instance -- or another host entirely --
   *  and a plugin that stays in demo mode until it is reloaded reads as a
   *  failed activation. */
  void refresh() {
    LicFile lic;
    bool ok = false;
    if (readLic(product_, &lic)) ok = tokenValid(lic.token);
    licensed_.store(ok, std::memory_order_relaxed);
  }

  /**
   * [main-thread] Try to activate from whatever the user pasted.
   *
   * Accepts a device TOKEN (`SNRA1-`), which is what the activation page hands
   * back. A purchase KEY (`SNRE1-`) is recognised and refused with its own
   * result, because the two look alike to a user and telling them "invalid"
   * when they pasted a perfectly good key would send them to support.
   */
  ActivateResult activate(const std::string& pasted) {
    const std::string text = stripWhitespace(pasted);
    if (text.rfind("SNRE1-", 0) == 0) return ActivateResult::needsToken;
    if (text.rfind("SNRA1-", 0) != 0) return ActivateResult::invalid;

    if (!tokenValid(text)) {
      // Signature-valid but rejected: say WHICH, so the user knows whether to
      // re-activate this machine or just re-download a fresh token.
      return diagnose(text);
    }
    LicFile existing;
    readLic(product_, &existing); // keep the email/key if we already had them
    LicFile lic;
    lic.token = text;
    lic.email = existing.email;
    lic.key = existing.key;
    if (!writeLic(product_, lic)) return ActivateResult::saveFailed;
    licensed_.store(true, std::memory_order_relaxed);
    return ActivateResult::activated;
  }

 private:
  bool tokenValid(const std::string& token) const {
    if (token.empty()) return false;
    return verifyToken(token, product_, machine_, (long long) std::time(nullptr), publicKey_);
  }

  /** Why a token that did not verify did not verify.
   *
   *  Reads the fields directly rather than re-running verifyToken with one
   *  condition relaxed: that trick answers "wrong machine" for a CORRUPT
   *  token too, and sending someone to re-activate a machine when the real
   *  problem is a truncated paste is worse than saying nothing. */
  ActivateResult diagnose(const std::string& token) const {
    const std::string t = stripWhitespace(token);
    std::string json;
    if (t.size() <= 6 || !base64Decode(t.substr(6), json)) return ActivateResult::invalid;
    std::string mid;
    long long exp = 0;
    if (!jsonString(json, "mid", mid) || !jsonNumber(json, "exp", exp))
      return ActivateResult::invalid;
    if (asciiLower(mid) != asciiLower(machine_)) return ActivateResult::wrongMachine;
    if (exp <= (long long) std::time(nullptr)) return ActivateResult::expired;
    return ActivateResult::invalid; // fields fine, signature is not ours
  }

 private:
  std::atomic<double> phase_{0.0};
  std::atomic<uint32_t> rng_{0x9e3779b9u};
  std::atomic<double> rate_{48000.0};

  std::string product_;
  std::string publicKey_;
  std::string machine_;
  std::atomic<bool> licensed_{false};
};

// ── The activation face ─────────────────────────────────────────────────────
//
// The whole plugin face IS a webview here, which removes the problem the
// PREVIOUS build had to work around: there, the webview was a native child
// window that
// drew above every sibling, so an activation panel on top of it was invisible
// AND unclickable, and the only fix was hiding the webview entirely. With one
// webview and nothing else, the overlay is just a div with the top z-index --
// it covers the page because the page is underneath it, not beside it.

/** JS that puts the activation overlay on the page (idempotent: re-running it
 *  is how "still not activated" is re-asserted after a failed attempt). The
 *  machine id is 32 hex characters by construction, so there is nothing here
 *  that needs escaping -- and nothing user-supplied is ever interpolated. */
inline std::string overlayScript(const std::string& machineId) {
  const std::string css =
      "position:fixed;inset:0;z-index:2147483647;display:flex;align-items:center;"
      "justify-content:center;background:#12121a;color:#eaeaf0;"
      "font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif";
  const std::string field =
      "width:100%;box-sizing:border-box;padding:8px 10px;border-radius:6px;"
      "border:1px solid #33334a;background:#1c1c26;color:#eaeaf0;"
      "font-family:ui-monospace,Menlo,Consolas,monospace;font-size:11.5px";
  std::string js;
  js.reserve(3072);
  js += "(function(){var W=window,d=document;";
  js += "if(W.__sonoreLicense&&W.__sonoreLicense.el){W.__sonoreLicense.el.style.display='flex';return;}";
  js += "var el=d.createElement('div');el.style.cssText=\"" + css + "\";";
  js += "var box=d.createElement('div');box.style.cssText='max-width:420px;padding:28px;text-align:center';";
  js += "var h=d.createElement('div');h.style.cssText='font-size:15px;font-weight:600;margin-bottom:6px';";
  js += "h.textContent='Activate this plugin';box.appendChild(h);";
  js += "var p=d.createElement('div');p.style.cssText='font-size:12.5px;line-height:1.6;color:#a0a0b0';";
  js += "p.textContent='Go to sonorie.com/activate, paste the machine code below, and bring the activation code back here.';";
  js += "box.appendChild(p);";
  js += "var l1=d.createElement('div');l1.style.cssText='font-size:11px;color:#7a7a8a;margin:16px 0 4px;text-align:left';";
  js += "l1.textContent='Machine code';box.appendChild(l1);";
  js += "var mid=d.createElement('input');mid.readOnly=true;mid.value='" + machineId + "';";
  js += "mid.style.cssText=\"" + field + "\";box.appendChild(mid);";
  js += "var l2=d.createElement('div');l2.style.cssText='font-size:11px;color:#7a7a8a;margin:14px 0 4px;text-align:left';";
  js += "l2.textContent='Activation code';box.appendChild(l2);";
  js += "var tok=d.createElement('textarea');tok.rows=3;tok.placeholder='SNRA1-...';";
  js += "tok.style.cssText=\"" + field + ";resize:vertical\";box.appendChild(tok);";
  js += "var go=d.createElement('button');go.textContent='Activate';";
  js += "go.style.cssText='width:100%;margin-top:12px;padding:9px;border:0;border-radius:6px;";
  js += "background:#6e8bff;color:#0b0b12;font-size:12.5px;font-weight:700;cursor:pointer';box.appendChild(go);";
  js += "var msg=d.createElement('div');msg.style.cssText='min-height:16px;margin-top:10px;font-size:11.5px';";
  js += "box.appendChild(msg);el.appendChild(box);d.body.appendChild(el);";
  // Selecting the machine code has to be possible: the previous build learned
  // this the hard way with a label nobody could copy out of.
  js += "mid.onclick=function(){mid.select();};";
  // Strip anything outside the code alphabet BEFORE sending: the C++ side reads
  // JSON with a deliberately minimal parser that stops at the first quote, so a
  // stray character would truncate the message into a mystery rather than a
  // clean "that is not a valid code".
  js += "go.onclick=function(){var t=(tok.value||'').replace(/[^A-Za-z0-9+/=_.:-]/g,'');";
  js += "if(!t){msg.style.color='#ff8b8b';msg.textContent='Paste the activation code first.';return;}";
  js += "msg.style.color='#a0a0b0';msg.textContent='Checking...';";
  js += "if(W.sonore&&W.sonore.__activate)W.sonore.__activate(t);};";
  js += "W.__sonoreLicense={el:el,result:function(ok,text){";
  js += "if(ok){el.remove();W.__sonoreLicense.el=null;return;}";
  js += "msg.style.color='#ff8b8b';msg.textContent=text;}};";
  js += "})();";
  return js;
}

/** JS that answers one activation attempt. `ok` removes the overlay for good;
 *  anything else leaves it up with a reason the user can act on. */
inline std::string overlayResultScript(bool ok, const std::string& message) {
  std::string safe;
  for (char c : message) {
    // These are our own literals, so this cannot fire -- it exists so that it
    // still cannot fire if someone later routes a server message through here.
    if (c == 0x27 || c == 0x5C) continue; // a quote or a backslash
    if ((unsigned char) c >= 0x20) safe.push_back(c);
  }
  return std::string("if(window.__sonoreLicense)window.__sonoreLicense.result(") +
         (ok ? "true" : "false") + ",'" + safe + "');";
}

/** What a user should read for each outcome. Written for someone who just
 *  bought a plugin and wants to use it, not for us. */
inline const char* activateMessage(ActivateResult r) {
  switch (r) {
    case ActivateResult::activated:
      return "Activated.";
    case ActivateResult::needsToken:
      return "That is your purchase key. Paste it at sonorie.com/activate to get an "
             "activation code for this computer.";
    case ActivateResult::wrongMachine:
      return "This code was issued for a different computer. Activate this one at "
             "sonorie.com/activate.";
    case ActivateResult::expired:
      return "This activation code has expired. Get a fresh one at sonorie.com/activate.";
    case ActivateResult::saveFailed:
      return "The code is valid but could not be saved. Check permissions on your user "
             "folder and try again.";
    case ActivateResult::invalid:
      break;
  }
  return "That does not look like a Sonorie activation code.";
}

/** One activation attempt, start to finish: the JS to send back to the page.
 *  Shared by every wrapper so the five formats cannot answer differently. */
inline std::string handleActivate(Gate& gate, const std::string& pasted) {
  const ActivateResult r = gate.activate(pasted);
  return overlayResultScript(r == ActivateResult::activated, activateMessage(r));
}

} // namespace license
} // namespace sonore
