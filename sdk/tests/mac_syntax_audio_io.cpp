// SPDX-License-Identifier: Apache-2.0
// The macOS halves of the audio-I/O headers, compiled on Windows. Nothing
// here runs; the compiler reading it is the test.
//
// CoreAudioFormat (ExtAudioFile), SystemAudioVolume (the default output's
// virtual main volume and mute), and the posix_spawn launcher behind
// LAMEEncoderAudioFormat are Mac code that no Mac has compiled: macOS CI is
// not running (see mac_syntax_plugin.cpp). Built like sonore_mac_syntax_* --
// __APPLE__ defined, _WIN32 undefined, Apple's declarations from
// tests/au_shim, the POSIX names MSVC lacks from tests/au_shim/msvc. What
// that proves is what au_shim/README.md says: every name exists and every
// call fits its declaration -- not that the declarations match Apple's ABI.
//
// Every entry point is CALLED below, because an inline function or a
// template nobody calls is a function the compiler only skims.
#if !defined(__APPLE__) || defined(_WIN32)
#error "build with __APPLE__ defined and _WIN32 undefined: see sonore_mac_syntax_* in CMakeLists.txt"
#endif

#include <sonore/audio_cd.h>
#include <sonore/lame_encoder.h>
#include <sonore/system_audio_format.h>
#include <sonore/system_volume.h>
#include <sonore/thumbnail_cache.h>

// The Mac branches, and not others, are what was compiled: these names exist
// only inside each header's __APPLE__ branch.
static_assert(sonore::sysvoldetail::kVirtualMainVolume == 0x766D7663, "the Core Audio volume branch");
static_assert(sizeof(&sonore::sysaudiodetail::readExtAudioFile) == sizeof(void*),
              "the ExtAudioFile branch");

int macSyntaxAudioIo() {
  int n = 0;
  sonore::WavData d;
  std::string err;
  n += sonore::CoreAudioFormat::read("a.m4a", &d, &err) ? 1 : 0;
  n += sonore::readAudioFileWithSystemCodecs("a.m4a", &d, &err) ? 1 : 0;
  sonore::enableSystemAudioFormats(true);
  n += sonore::readAudioFile("a.m4a", &d) ? 1 : 0;

  n += sonore::SystemAudioVolume::isAvailable() ? 1 : 0;
  n += sonore::SystemAudioVolume::setGain(0.5f) ? 1 : 0;
  n += sonore::SystemAudioVolume::isMuted() ? 1 : 0;
  n += sonore::SystemAudioVolume::setMuted(false) ? 1 : 0;

  sonore::LAMEEncoderAudioFormat lame("/usr/local/bin/lame");
  const float silence[4] = {0, 0, 0, 0};
  std::vector<uint8_t> mp3;
  n += lame.encode(silence, 2, 2, 44100, sonore::LAMEEncoderAudioFormat::Options(), &mp3) ? 1 : 0;
  n += lame.write("/tmp/a.mp3", silence, 2, 2, 44100) ? 1 : 0;
  n += sonore::lamedetail::runProcess("/bin/true", {"x"}, 1.0).exitCode;

  sonore::AudioThumbnailCache cache;
  n += (int) (sonore::AudioThumbnailCache::hashFile("/tmp/a.wav") & 1);
  n += (int) cache.size();

  n += (int) sonore::AudioCDReader::getAvailableCDNames().size();
  n += sonore::AudioCDReader::createReaderForCD(0, &err) ? 1 : 0;
  n += (int) sonore::AudioCDBurner::findAvailableDevices().size();
  n += sonore::AudioCDBurner::openDevice(0, &err) ? 1 : 0;
  return n;
}
