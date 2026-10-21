// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the toolkit headers in two translation units of one program.
//
// A header-only SDK has to survive being included from more than one source
// file: a real plugin is rarely one file, and the standalone pulls
// audiofile.h in by itself. The vendored codecs' extern "C" blocks gave
// eighteen of their functions external linkage on MSVC, and this program
// failed to link with LNK2005 -- mp3dec_init already defined. That it builds
// is most of the test; main() checks both halves actually run.
//
// And a user's own stb_vorbis comes FIRST here: its global `stb_vorbis` struct
// once collided with the SDK's private copy (C2371), so a project decoding
// Vorbis itself could not include audiofile.h after it.
#include "two_tu_user_vorbis.h"
#include <sonore/audiofile.h>
#include <sonore/dsp.h>
#include <sonore/impulse.h>
#include <sonore/sampler.h>

#include <cstdio>

int secondUnitFrames(); // two_tu_b.cpp

int main() {
  int error = 0;
  stb_vorbis* none = stb_vorbis_open_memory(nullptr, 0, &error, nullptr); // the user's
  if (none) stb_vorbis_close(none);
  sonore::WavData empty;
  const bool readNothing = !sonore::readAudioFile("sonore-two-tu-missing.wav", &empty);
  const int other = secondUnitFrames();
  std::printf("  %s two translation units including the toolkit link and run (%d)\n",
              readNothing && other == 7 ? "ok  " : "FAIL", other);
  return readNothing && other == 7 ? 0 : 1;
}
