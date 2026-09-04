// SPDX-License-Identifier: Apache-2.0
//
// Finding a typeface on the machine the plugin is running on.
//
// ── Why the SDK ships no font ───────────────────────────────────────────────
//
// Embedding one would guarantee identical text everywhere, which is the same
// argument that produced the software rasteriser, and it is a genuinely good
// argument. It loses to two others.
//
// A font is 150 KB to 400 KB in every plugin binary, and this SDK's whole
// case against the webview is weight. And every font worth embedding carries
// a licence -- SIL OFL, Apache -- whose terms have to be honoured by a
// commercial product that generates thousands of plugins. That is a decision
// for whoever owns the business, not a default the SDK should make quietly.
//
// So: look for one that is already there. A plugin that needs guaranteed text
// -- a specific face, a specific language -- supplies its own bytes to
// Typeface::load and never calls this.
//
// ── What happens when nothing is found ──────────────────────────────────────
//
// The controls still work. Font handles an invalid typeface by drawing
// nothing, so a knob is still a knob and a slider still drags; only the labels
// are missing. That is a degraded interface rather than a broken one, and it
// says so through Font::isValid() rather than pretending.
#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "truetype.h"

namespace sonore {
namespace gfx {

/** Reads a whole file, or an empty vector. */
inline std::vector<uint8_t> readFileBytes(const char* path) {
  std::vector<uint8_t> bytes;
  FILE* f = std::fopen(path, "rb");
  if (!f) return bytes;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0) {
    bytes.resize((size_t) size);
    if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
  }
  std::fclose(f);
  return bytes;
}

/**
 * The first readable typeface from the usual places, or null.
 *
 * Ordered by how much a UI-shaped face is wanted rather than by what is most
 * likely present: Segoe UI before Arial on Windows, DejaVu before FreeSans on
 * Linux. A plugin's labels look better in the face the desktop uses for its
 * own.
 *
 * Cached, because a host opens and closes an editor repeatedly and parsing a
 * 4000-glyph font each time would be visible.
 */
inline std::shared_ptr<Typeface> systemTypeface() {
  static std::shared_ptr<Typeface> cached;
  static bool tried = false;
  if (tried) return cached;
  tried = true;

  static const char* const kCandidates[] = {
#if defined(_WIN32)
      "C:/Windows/Fonts/segoeui.ttf",
      "C:/Windows/Fonts/arial.ttf",
      "C:/Windows/Fonts/tahoma.ttf",
      "C:/Windows/Fonts/verdana.ttf",
#elif defined(__APPLE__)
      // .ttc is a COLLECTION and the parser takes its first face, which for
      // these is the regular weight.
      "/System/Library/Fonts/SFNS.ttf",
      "/System/Library/Fonts/Helvetica.ttc",
      "/Library/Fonts/Arial.ttf",
#else
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/dejavu/DejaVuSans.ttf",
#endif
  };

  for (const char* path : kCandidates) {
    std::vector<uint8_t> bytes = readFileBytes(path);
    if (bytes.empty()) continue;
    auto face = std::make_shared<Typeface>();
    if (face->load(std::move(bytes))) {
      cached = face;
      return cached;
    }
  }
  return cached; // null: labels are absent, controls still work
}

} // namespace gfx
} // namespace sonore
