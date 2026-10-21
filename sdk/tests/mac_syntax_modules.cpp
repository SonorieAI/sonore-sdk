// SPDX-License-Identifier: Apache-2.0
// The macOS branches of the module headers -- gfx/video.h's AVFoundation
// player, reader and capture, analytics.h and the button tracker -- compiled
// on Windows with __APPLE__ defined and _WIN32 undefined, against
// tests/au_shim. Nothing here runs; the compiler reading it is the test, with
// the limits au_shim/README.md states: every name exists and every call fits
// its declaration, which is not the same as the ABI being right.
//
// video.h resolves every CoreMedia, CoreVideo, QuartzCore and libdispatch
// function with dlsym and every AVFoundation class by name, so the only
// Apple declarations it needs are the Objective-C runtime and dlfcn.h, both
// already in the shim.
#if !defined(__APPLE__) || defined(_WIN32)
#error "build with __APPLE__ defined and _WIN32 undefined: see sonore_mac_syntax_* in CMakeLists.txt"
#endif

#include <sonore/analytics.h>
#include <sonore/gfx/box2d_renderer.h>
#include <sonore/gfx/button_tracker.h>
#include <sonore/gfx/video.h>
#include <sonore/gfx/widgets.h>

// Proof that the APPLE branch is what was read, rather than a stub that
// happened to compile: these types exist only there.
static_assert(sizeof(sonore::gfx::detail::avf::MediaTime) == 24, "CMTime is 24 bytes");
static_assert(sizeof(sonore::gfx::detail::avf::MediaTimeRange) == 48, "CMTimeRange is 48 bytes");

namespace {

// Every entry point referenced, so nothing can be discarded unread. None is
// called: there is no Objective-C runtime here to call into.
[[maybe_unused]] void touchEverything() {
  sonore::gfx::VideoComponent v;
  v.load("/tmp/x.mov");
  v.play();
  v.setPlayPosition(1.0);
  v.update();
  sonore::gfx::VideoReader r;
  r.open("/tmp/x.mov");
  sonore::gfx::Bitmap frame;
  r.readFrame(frame);
  r.setAudioFormat(48000, 2);
  float audio[64];
  r.readAudio(audio, 32);
  r.seek(0.5);
  auto camera = sonore::gfx::CameraDevice::openDevice(0);
  (void) sonore::gfx::CameraDevice::availableDevices();
  sonore::Analytics a;
  sonore::gfx::Button b;
  sonore::gfx::ButtonTracker t(b, "e", {}, 0, &a);
}

} // namespace
