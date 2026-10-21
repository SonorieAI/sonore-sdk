// SPDX-License-Identifier: Apache-2.0
//
// Video: a file played inside an editor, and frames from a camera -- both as
// Bitmaps, drawn through Graphics like everything else.
//
// ── Why frames come back as Bitmaps ─────────────────────────────────────────
//
// The reference's video component is a native child window: a platform media
// view parented into the editor. That is the one thing an editor here cannot
// have. Everything an editor shows is rasterised into one bitmap and blitted
// (gfx/bitmap.h says why), so a native view would sit on top of the drawing
// with its own z-order, ignore clipping and scrolling, and miss every check
// the rasteriser makes possible. So the platform decodes, the decoded picture
// is copied into a Bitmap, and VideoComponent::paint() draws it -- clipped,
// scrolled, scaled and composited exactly like any image.
//
// The cost is a copy per frame: 8 MB for 1080p, a few milliseconds a second
// at 30 frames. For a picture in a plugin editor -- a reference video to score
// against, a preview -- that is the right trade. A 4K player is not what this
// is for.
//
// ── Threads, and update() ───────────────────────────────────────────────────
//
// Decoding happens off the message thread. VideoComponent::update() picks up
// the newest frame and is the ONE call that touches the component from the
// playback side: call it from the editor's tick (sync(), or a MultiTimer
// slot). It repaints when a frame arrived and fires onPlaybackEnded and
// onErrorOccurred there -- so every callback runs on the message thread, and
// nothing ever repaints a component from another thread.
//
// CameraDevice is the opposite and says so: its frames arrive on the capture
// thread, as the reference's do, because a camera consumer is usually a
// processing step (a still, a level meter, a recorder) rather than a widget.
// A listener that wants to show one copies it and repaints from its tick.
//
// ── The soundtrack ──────────────────────────────────────────────────────────
//
// MUTED BY DEFAULT, and opt-in with setAudioEnabled(true). A plugin's audio
// belongs to the host: it is mixed, delayed for latency compensation and
// bounced. A video component that opened an output of its own would play next
// to the session rather than in it -- not in the bounce, not under the
// plugin's own gain, and on whatever device the OS says. For picture-to-
// sound work the soundtrack is usually already on a track in the session, and
// the picture is what the plugin is for.
//
// When enabled: on Windows the soundtrack is decoded by the same Media
// Foundation reader and played through the SDK's own WASAPI output (the one
// the standalone uses), at volume, while playing at 1x. At any other speed it
// is silent rather than chipmunked -- a time-stretcher is not something a
// video player should smuggle in. On macOS AVPlayer plays it, and does pitch-
// corrected varispeed itself. The video clock is the master on Windows and
// audio starts from it; over a long file the two drift by the sound card's
// clock error (tens of ppm, a few milliseconds a minute) until the next seek
// or play re-aligns them.
//
// ── Platforms ───────────────────────────────────────────────────────────────
//
// Windows: Media Foundation. IMFSourceReader with video processing converts
// whatever the installed decoders accept (H.264, HEVC with its extension,
// VP9, MJPEG...) to RGB32; a worker thread clocks the frames. Cameras through
// MFEnumDeviceSources and the same reader. RUN by modules_test.cpp.
//
// macOS: AVFoundation, through the Objective-C runtime like window_cocoa.h,
// with every C function resolved by dlsym -- so a project links no framework
// it did not already. AVPlayer + AVPlayerItemVideoOutput for playback,
// AVAssetReader for VideoReader, AVCaptureSession for cameras. COMPILED
// against tests/au_shim, never run: no machine here can.
//
// Linux: neither. Every entry point reports "not available on this platform"
// rather than pretending; GStreamer or V4L2 would be the way in.
#pragma once

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#if defined(_MSC_VER)
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "ole32.lib")
#endif
#include "../audio_ring.h"
#include "../audio_wasapi.h"
#include "../path_io.h"
#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "bitmap.h"
#include "component.h"
#include "graphics.h"

namespace sonore {
namespace gfx {

namespace detail {

/**
 * Rows of 32-bit BGRX -- what Media Foundation's RGB32 and CoreVideo's
 * 32BGRA both are in memory -- into an opaque RGBA Bitmap.
 *
 * `pitch` is the distance from one row to the next and may be NEGATIVE: a
 * bottom-up image hands over its top row's address and a negative pitch, and
 * walking it this way is what keeps a decoded frame the right way up whatever
 * the decoder's habit. The fourth byte is ignored rather than trusted -- in
 * RGB32 it is undefined, and a decoder that leaves it zero would otherwise
 * produce a fully transparent picture.
 */
inline void copyBgrxToBitmap(const uint8_t* topRow, std::ptrdiff_t pitch, int width, int height,
                             Bitmap& out) {
  if (!topRow || width <= 0 || height <= 0) return;
  if (out.width() != width || out.height() != height) out.resize(width, height);
  uint8_t* dst = out.data();
  for (int y = 0; y < height; ++y) {
    const uint8_t* s = topRow + (std::ptrdiff_t) y * pitch;
    uint8_t* d = dst + (size_t) y * (size_t) width * 4u;
    for (int x = 0; x < width; ++x, s += 4, d += 4) {
      d[0] = s[2];
      d[1] = s[1];
      d[2] = s[0];
      d[3] = 255;
    }
  }
}

inline double videoSteadySeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** What a player counts, for a plugin's diagnostics and for the tests. */
struct VideoPlaybackStats {
  uint64_t framesDecoded = 0;
  uint64_t framesShown = 0;
  /** Decoded too late to be worth showing -- a decoder slower than the clock. */
  uint64_t framesDropped = 0;
  uint64_t audioFramesQueued = 0;
  /** An output device is open for the soundtrack (playing or not). */
  bool audioDeviceOpen = false;
  /** Why the soundtrack is not playing, when it was asked to and is not. */
  std::string audioError;
};

/** The platform half of VideoComponent. Message-thread calls only. */
class VideoBackend {
public:
  virtual ~VideoBackend() = default;
  virtual bool open(const std::string& path, std::string& error) = 0;
  virtual int width() const = 0;
  virtual int height() const = 0;
  virtual double duration() const = 0;
  virtual void play() = 0;
  virtual void pause() = 0;
  virtual bool isPlaying() const = 0;
  virtual void setPosition(double seconds) = 0;
  virtual double position() const = 0;
  virtual void setSpeed(double speed) = 0;
  virtual void setVolume(float volume) = 0;
  virtual void setAudioEnabled(bool enabled) = 0;
  /** Swap the newest frame into `into`, if one arrived since the last call. */
  virtual bool takeFrame(Bitmap& into) = 0;
  /** True ONCE when playback reached the end. */
  virtual bool takeEnded() = 0;
  virtual std::string takeError() = 0;
  virtual VideoPlaybackStats stats() const = 0;
};

using CameraFrameSink = std::function<void(const Bitmap&)>;
using CameraErrorSink = std::function<void(const std::string&)>;

/** The platform half of CameraDevice. */
class CameraBackend {
public:
  virtual ~CameraBackend() = default;
  virtual bool open(int index, int minWidth, int minHeight, int maxWidth, int maxHeight,
                    std::string& error, CameraFrameSink frames, CameraErrorSink errors) = 0;
  virtual std::string name() const = 0;
  virtual int width() const = 0;
  virtual int height() const = 0;
};

} // namespace detail

// ═════════════════════════════════════════════════════════════════════════════
// Platform plumbing
// ═════════════════════════════════════════════════════════════════════════════

#if defined(_WIN32)
namespace detail {
namespace mf {

/** A COM reference that releases itself. Local to this file so it carries no
 *  dependency on WRL or ATL, which a MinGW build does not have. */
template <typename T>
class Ref {
public:
  Ref() = default;
  ~Ref() { reset(); }
  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;
  Ref(Ref&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  Ref& operator=(Ref&& o) noexcept {
    if (this != &o) {
      reset();
      p_ = o.p_;
      o.p_ = nullptr;
    }
    return *this;
  }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  T** put() {
    reset();
    return &p_;
  }
  void reset() {
    if (p_) {
      p_->Release();
      p_ = nullptr;
    }
  }
  explicit operator bool() const { return p_ != nullptr; }

private:
  T* p_ = nullptr;
};

/**
 * COM on this thread for as long as the scope lives -- in whatever apartment
 * the thread already has, or `apartment` if it has none.
 *
 * A worker thread asks for the multithreaded apartment, which is what Media
 * Foundation's own work queues use. A thread that might be the UI's must never
 * be put into it: the first CoInitializeEx on a thread wins, and an MTA there
 * makes WebView2 and OLE drag and drop, initialised later on the same thread,
 * fail with RPC_E_CHANGED_MODE (audio_wasapi.h tells the same story).
 */
class ComScope {
public:
  explicit ComScope(DWORD apartment) { owns_ = SUCCEEDED(CoInitializeEx(nullptr, apartment)); }
  ~ComScope() {
    if (owns_) CoUninitialize();
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;

private:
  bool owns_ = false;
};

/** MFStartup/MFShutdown, which are reference counted process-wide. */
class Startup {
public:
  Startup() { ok_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL)); }
  ~Startup() {
    if (ok_) MFShutdown();
  }
  Startup(const Startup&) = delete;
  Startup& operator=(const Startup&) = delete;
  bool ok() const { return ok_; }

private:
  bool ok_ = false;
};

inline std::string hresultMessage(const char* what, HRESULT hr) {
  char buffer[200];
  // The two a user can do something about, said in words.
  const char* hint = "";
  if (hr == E_ACCESSDENIED) hint = " -- access was refused (Windows privacy settings?)";
  else if (hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE || hr == MF_E_INVALIDMEDIATYPE ||
           hr == MF_E_TOPO_CODEC_NOT_FOUND)
    hint = " -- no decoder for this format is installed";
  std::snprintf(buffer, sizeof(buffer), "%s (0x%08lx)%s", what, (unsigned long) hr, hint);
  return buffer;
}

inline LONGLONG toHundredNanoseconds(double seconds) {
  if (!(seconds > 0.0)) return 0;
  if (seconds > 1e9) seconds = 1e9;
  return (LONGLONG) (seconds * 1e7 + 0.5);
}

/** Copy one RGB32 sample out of Media Foundation into a Bitmap, cropping to
 *  the visible area. Through IMF2DBuffer when the buffer offers it, because
 *  that is the only place the TRUE pitch and orientation are stated; a
 *  contiguous buffer is read with the media type's default stride instead. */
inline bool copyRgb32Sample(IMFSample* sample, LONG defaultStride, int bufferWidth, int bufferHeight,
                            int cropX, int cropY, int width, int height, Bitmap& out) {
  Ref<IMFMediaBuffer> buffer;
  if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())) || !buffer) return false;
  Ref<IMF2DBuffer> twoD;
  if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(twoD.put()))) && twoD) {
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    if (SUCCEEDED(twoD->Lock2D(&scan0, &pitch)) && scan0) {
      const LONG absPitch = pitch < 0 ? -pitch : pitch;
      const bool fits = absPitch >= (LONG) bufferWidth * 4;
      if (fits)
        copyBgrxToBitmap(scan0 + (std::ptrdiff_t) cropY * pitch + (std::ptrdiff_t) cropX * 4, pitch,
                         width, height, out);
      twoD->Unlock2D();
      return fits;
    }
  }
  BYTE* data = nullptr;
  DWORD capacity = 0, length = 0;
  if (FAILED(buffer->Lock(&data, &capacity, &length)) || !data) return false;
  const LONG absStride = defaultStride < 0 ? -defaultStride : defaultStride;
  // A buffer shorter than its format says is a corrupt or truncated frame:
  // refused, not read past.
  const bool fits = absStride >= (LONG) bufferWidth * 4 &&
                    (size_t) length >= (size_t) absStride * (size_t) bufferHeight;
  if (fits) {
    const BYTE* top = defaultStride < 0 ? data + (size_t) absStride * (size_t) (bufferHeight - 1) : data;
    copyBgrxToBitmap(top + (std::ptrdiff_t) cropY * defaultStride + (std::ptrdiff_t) cropX * 4,
                     defaultStride, width, height, out);
  }
  buffer->Unlock();
  return fits;
}

/** The video capture devices, in the order Media Foundation lists them. The
 *  caller releases each and CoTaskMemFrees the array. */
inline HRESULT enumerateCameras(IMFActivate*** devices, UINT32* count) {
  *devices = nullptr;
  *count = 0;
  Ref<IMFAttributes> attributes;
  HRESULT hr = MFCreateAttributes(attributes.put(), 1);
  if (FAILED(hr)) return hr;
  hr = attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                           MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(hr)) return hr;
  return MFEnumDeviceSources(attributes.get(), devices, count);
}

inline std::string activateName(IMFActivate* device) {
  WCHAR* name = nullptr;
  UINT32 length = 0;
  std::string out;
  if (SUCCEEDED(device->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length)) && name) {
    out = pathio::narrow(name);
    CoTaskMemFree(name);
  }
  return out;
}

} // namespace mf
} // namespace detail

#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)

namespace detail {
namespace avf {

/** CMTime, by layout. CoreMedia's header is not included -- nothing here links
 *  it -- so the struct is spelled out: value, timescale, flags, epoch. */
struct MediaTime {
  int64_t value;
  int32_t timescale;
  uint32_t flags;
  int64_t epoch;
};
struct MediaTimeRange {
  MediaTime start;
  MediaTime duration;
};
struct PixelSize {
  double width, height;
};

/** kCMTimeZero and kCMTimePositiveInfinity, by value. */
inline MediaTime timeZero() { return {0, 1, 1u, 0}; }
inline MediaTime timeForever() { return {0, 0, 1u | 4u, 0}; }

/**
 * Every C function and constant the Apple branch needs, resolved with dlsym
 * from the frameworks' fixed paths -- the CoreGraphicsApi idea in
 * window_cocoa.h, for the same reason: a generated project links nothing it
 * did not name, and a machine without a symbol reports "not available"
 * instead of failing to load the plugin at all.
 */
struct Api {
  bool ok = false;
  bool canCapture = false;
  void (*CFRelease)(const void*) = nullptr;
  const void* (*CFRetain)(const void*) = nullptr;
  MediaTime (*CMTimeMakeWithSeconds)(double, int32_t) = nullptr;
  double (*CMTimeGetSeconds)(MediaTime) = nullptr;
  void* (*CMSampleBufferGetImageBuffer)(void*) = nullptr;
  MediaTime (*CMSampleBufferGetPresentationTimeStamp)(void*) = nullptr;
  void* (*CMSampleBufferGetDataBuffer)(void*) = nullptr;
  size_t (*CMBlockBufferGetDataLength)(void*) = nullptr;
  int32_t (*CMBlockBufferCopyDataBytes)(void*, size_t, size_t, void*) = nullptr;
  int32_t (*CVPixelBufferLockBaseAddress)(void*, uint64_t) = nullptr;
  int32_t (*CVPixelBufferUnlockBaseAddress)(void*, uint64_t) = nullptr;
  void* (*CVPixelBufferGetBaseAddress)(void*) = nullptr;
  size_t (*CVPixelBufferGetBytesPerRow)(void*) = nullptr;
  size_t (*CVPixelBufferGetWidth)(void*) = nullptr;
  size_t (*CVPixelBufferGetHeight)(void*) = nullptr;
  double (*CACurrentMediaTime)() = nullptr;
  void* (*dispatch_queue_create)(const char*, void*) = nullptr;
  void (*dispatch_release)(void*) = nullptr;
  void (*dispatch_sync_f)(void*, void*, void (*)(void*)) = nullptr;
  // Constants: NSString* (or CFStringRef) globals, read once.
  id pixelFormatKey = nil;
  id mediaTypeVideo = nil;
  id mediaTypeAudio = nil;
  id formatIdKey = nil, sampleRateKey = nil, channelsKey = nil, bitDepthKey = nil;
  id isFloatKey = nil, isNonInterleavedKey = nil, isBigEndianKey = nil;
  id presetHigh = nil, preset640 = nil, preset1280 = nil;
  id deviceTypeBuiltIn = nil, deviceTypeExternal = nil;
};

inline id readConstant(void* library, const char* name) {
  if (!library) return nil;
  void* address = dlsym(library, name);
  return address ? *(id*) address : nil;
}

inline const Api& api() {
  static Api a = [] {
    Api r;
    void* av = dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_LAZY);
    void* cm = dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_LAZY);
    void* cv = dlopen("/System/Library/Frameworks/CoreVideo.framework/CoreVideo", RTLD_LAZY);
    void* cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_LAZY);
    void* qc = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_LAZY);
    if (!av || !cm || !cv || !cf || !qc) return r;
    r.CFRelease = (void (*)(const void*)) dlsym(cf, "CFRelease");
    r.CFRetain = (const void* (*) (const void*)) dlsym(cf, "CFRetain");
    r.CMTimeMakeWithSeconds = (MediaTime(*)(double, int32_t)) dlsym(cm, "CMTimeMakeWithSeconds");
    r.CMTimeGetSeconds = (double (*)(MediaTime)) dlsym(cm, "CMTimeGetSeconds");
    r.CMSampleBufferGetImageBuffer = (void* (*) (void*) ) dlsym(cm, "CMSampleBufferGetImageBuffer");
    r.CMSampleBufferGetPresentationTimeStamp =
        (MediaTime(*)(void*)) dlsym(cm, "CMSampleBufferGetPresentationTimeStamp");
    r.CMSampleBufferGetDataBuffer = (void* (*) (void*) ) dlsym(cm, "CMSampleBufferGetDataBuffer");
    r.CMBlockBufferGetDataLength = (size_t(*)(void*)) dlsym(cm, "CMBlockBufferGetDataLength");
    r.CMBlockBufferCopyDataBytes =
        (int32_t(*)(void*, size_t, size_t, void*)) dlsym(cm, "CMBlockBufferCopyDataBytes");
    r.CVPixelBufferLockBaseAddress = (int32_t(*)(void*, uint64_t)) dlsym(cv, "CVPixelBufferLockBaseAddress");
    r.CVPixelBufferUnlockBaseAddress =
        (int32_t(*)(void*, uint64_t)) dlsym(cv, "CVPixelBufferUnlockBaseAddress");
    r.CVPixelBufferGetBaseAddress = (void* (*) (void*) ) dlsym(cv, "CVPixelBufferGetBaseAddress");
    r.CVPixelBufferGetBytesPerRow = (size_t(*)(void*)) dlsym(cv, "CVPixelBufferGetBytesPerRow");
    r.CVPixelBufferGetWidth = (size_t(*)(void*)) dlsym(cv, "CVPixelBufferGetWidth");
    r.CVPixelBufferGetHeight = (size_t(*)(void*)) dlsym(cv, "CVPixelBufferGetHeight");
    r.CACurrentMediaTime = (double (*)()) dlsym(qc, "CACurrentMediaTime");
    r.dispatch_queue_create = (void* (*) (const char*, void*) ) dlsym(RTLD_DEFAULT, "dispatch_queue_create");
    r.dispatch_release = (void (*)(void*)) dlsym(RTLD_DEFAULT, "dispatch_release");
    r.dispatch_sync_f = (void (*)(void*, void*, void (*)(void*))) dlsym(RTLD_DEFAULT, "dispatch_sync_f");
    r.pixelFormatKey = readConstant(cv, "kCVPixelBufferPixelFormatTypeKey");
    r.mediaTypeVideo = readConstant(av, "AVMediaTypeVideo");
    r.mediaTypeAudio = readConstant(av, "AVMediaTypeAudio");
    r.formatIdKey = readConstant(av, "AVFormatIDKey");
    r.sampleRateKey = readConstant(av, "AVSampleRateKey");
    r.channelsKey = readConstant(av, "AVNumberOfChannelsKey");
    r.bitDepthKey = readConstant(av, "AVLinearPCMBitDepthKey");
    r.isFloatKey = readConstant(av, "AVLinearPCMIsFloatKey");
    r.isNonInterleavedKey = readConstant(av, "AVLinearPCMIsNonInterleaved");
    r.isBigEndianKey = readConstant(av, "AVLinearPCMIsBigEndianKey");
    r.presetHigh = readConstant(av, "AVCaptureSessionPresetHigh");
    r.preset640 = readConstant(av, "AVCaptureSessionPreset640x480");
    r.preset1280 = readConstant(av, "AVCaptureSessionPreset1280x720");
    r.deviceTypeBuiltIn = readConstant(av, "AVCaptureDeviceTypeBuiltInWideAngleCamera");
    // "External" is macOS 14; "ExternalUnknown" is its deprecated name since
    // 10.15. Whichever this system has.
    r.deviceTypeExternal = readConstant(av, "AVCaptureDeviceTypeExternal");
    if (!r.deviceTypeExternal) r.deviceTypeExternal = readConstant(av, "AVCaptureDeviceTypeExternalUnknown");
    r.ok = r.CFRelease && r.CFRetain && r.CMTimeMakeWithSeconds && r.CMTimeGetSeconds &&
           r.CMSampleBufferGetImageBuffer && r.CMSampleBufferGetPresentationTimeStamp &&
           r.CMSampleBufferGetDataBuffer && r.CMBlockBufferGetDataLength && r.CMBlockBufferCopyDataBytes &&
           r.CVPixelBufferLockBaseAddress && r.CVPixelBufferUnlockBaseAddress &&
           r.CVPixelBufferGetBaseAddress && r.CVPixelBufferGetBytesPerRow && r.CVPixelBufferGetWidth &&
           r.CVPixelBufferGetHeight && r.CACurrentMediaTime && r.pixelFormatKey && r.mediaTypeVideo &&
           r.mediaTypeAudio;
    r.canCapture = r.ok && r.dispatch_queue_create && r.dispatch_release && r.dispatch_sync_f;
    return r;
  }();
  return a;
}

/** objc_msgSend through a correctly typed pointer (see window_cocoa.h's msg
 *  for why the cast is not optional on arm64). */
template <typename Ret, typename... Args>
inline Ret send(id target, const char* selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector), args...);
}

/** A message returning a struct LARGER than 16 bytes -- a CMTime is 24 --
 *  which on x86_64 comes back through a hidden pointer and objc_msgSend_stret.
 *  window_cocoa.h's msgRect explains the bug the plain call would be. */
template <typename Ret, typename... Args>
inline Ret sendForStruct(id target, const char* selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
#if defined(__x86_64__)
  return reinterpret_cast<Fn>(objc_msgSend_stret)(target, sel_registerName(selector), args...);
#else
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector), args...);
#endif
}

inline id classNamed(const char* name) { return (id) objc_getClass(name); }

inline id nsString(const std::string& s) {
  return send<id>(classNamed("NSString"), "stringWithUTF8String:", s.c_str());
}

inline std::string fromNSString(id s) {
  if (!s) return {};
  const char* utf8 = send<const char*>(s, "UTF8String");
  return utf8 ? utf8 : "";
}

inline id nsNumberUnsigned(unsigned value) {
  return send<id>(classNamed("NSNumber"), "numberWithUnsignedInt:", value);
}
inline id nsNumberDouble(double value) {
  return send<id>(classNamed("NSNumber"), "numberWithDouble:", value);
}
inline id nsNumberBool(bool value) {
  return send<id>(classNamed("NSNumber"), "numberWithBool:", (BOOL) (value ? 1 : 0));
}

inline id nsDictionary(const id* objects, const id* keys, unsigned long count) {
  return send<id>(classNamed("NSDictionary"), "dictionaryWithObjects:forKeys:count:", objects, keys, count);
}

inline unsigned long nsCount(id array) { return array ? send<unsigned long>(array, "count") : 0ul; }
inline id nsAt(id array, unsigned long index) { return send<id>(array, "objectAtIndex:", index); }

inline void nsRelease(id object) {
  if (object) send<void>(object, "release");
}
inline id nsRetain(id object) { return object ? send<id>(object, "retain") : nil; }

/** An autorelease pool for the scope. Calls arrive on the plugin's threads,
 *  which may have none, and every convenience constructor below autoreleases. */
class Pool {
public:
  Pool() { pool_ = send<id>(send<id>(classNamed("NSAutoreleasePool"), "alloc"), "init"); }
  ~Pool() {
    if (pool_) send<void>(pool_, "drain");
  }
  Pool(const Pool&) = delete;
  Pool& operator=(const Pool&) = delete;

private:
  id pool_ = nil;
};

/** {kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA}: the
 *  layout copyBgrxToBitmap reads. */
inline id bgraSettings() {
  const id keys[1] = {api().pixelFormatKey};
  const id values[1] = {nsNumberUnsigned(0x42475241u)}; // 'BGRA'
  return nsDictionary(values, keys, 1);
}

/** A locked CVPixelBuffer's rows into a Bitmap. */
inline bool copyPixelBuffer(void* pixelBuffer, Bitmap& out) {
  const Api& a = api();
  if (!pixelBuffer) return false;
  if (a.CVPixelBufferLockBaseAddress(pixelBuffer, 1u /* read only */) != 0) return false;
  const uint8_t* base = (const uint8_t*) a.CVPixelBufferGetBaseAddress(pixelBuffer);
  const size_t pitch = a.CVPixelBufferGetBytesPerRow(pixelBuffer);
  const size_t w = a.CVPixelBufferGetWidth(pixelBuffer), h = a.CVPixelBufferGetHeight(pixelBuffer);
  const bool fits = base && w > 0 && h > 0 && w <= 16384 && h <= 16384 && pitch >= w * 4;
  if (fits) copyBgrxToBitmap(base, (std::ptrdiff_t) pitch, (int) w, (int) h, out);
  a.CVPixelBufferUnlockBaseAddress(pixelBuffer, 1u);
  return fits;
}

} // namespace avf
} // namespace detail
#endif

// ═════════════════════════════════════════════════════════════════════════════
// VideoReader
// ═════════════════════════════════════════════════════════════════════════════

/**
 * A video file, decoded frame by frame, in presentation order -- what
 * VideoComponent plays on Windows, and on its own what a thumbnail strip, an
 * offline analysis or a test needs: every frame, as fast as it decodes, with
 * no clock in the way.
 *
 * One thread at a time. On Windows, COM is initialised on the opening thread
 * if nobody had (single-threaded apartment, the UI's kind) and released by
 * close() if close() runs on that same thread.
 *
 * Frame size is the decoded picture's VISIBLE size, cropped to the display
 * aperture a codec padded to its block size (1920x1088 -> 1920x1080). Pixel
 * aspect ratio and rotation metadata are not applied: a phone video shot
 * upright decodes on its side, as it is stored.
 */
class VideoReader {
public:
  /** Nothing open. */
  VideoReader() = default;
  /** close(), on whichever thread destroys it. */
  ~VideoReader() { close(); }
  /** Not copyable: it holds the platform's decoder. */
  VideoReader(const VideoReader&) = delete;
  /** Not copyable. */
  VideoReader& operator=(const VideoReader&) = delete;

  /** Whether this platform can decode video at all: true on Windows, true
   *  on macOS when AVFoundation resolved, false on Linux. */
  static bool isAvailable() {
#if defined(_WIN32)
    return true;
#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
    return detail::avf::api().ok;
#else
    return false;
#endif
  }

  /** Open the file at `path` (UTF-8), closing whatever was open, and
   *  position at the first frame. False, with why in lastError() and in
   *  `error` when given, when the file cannot be opened, has no video
   *  stream, or cannot be decoded to RGB. */
  bool open(const std::string& path, std::string* error = nullptr) {
    close();
    error_.clear();
    path_ = path;
    if (!openPlatform()) {
      if (error) *error = error_;
      closePlatform();
      return false;
    }
    open_ = true;
    return true;
  }

  /** Release the decoder and the file. Safe when nothing is open. The path,
   *  lastError() and the format setAudioFormat() asked for are kept; what
   *  the soundtrack delivered, and where it was, are not. */
  void close() {
    closePlatform();
    open_ = false;
    width_ = height_ = 0;
    duration_ = frameRate_ = 0.0;
    hasAudio_ = false;
    skipBefore_ = audioSkipBefore_ = -1.0;
    audioPending_.clear();
    audioOffset_ = 0;
    // The last file's soundtrack format and position went on into the next:
    // audioSampleRate() answered for a soundtrack not set up, and the next
    // file's sound started where the last one's had been sought to.
    audioRate_ = audioChannels_ = 0;
    audioPosition_ = 0.0;
  }

  /** Whether the last open succeeded and close has not been called since. */
  bool isOpen() const { return open_; }
  /** The path the last open was given, whether or not it opened. */
  const std::string& path() const { return path_; }
  /** The frame width in pixels (the visible picture); 0 when not open. */
  int width() const { return width_; }
  /** The frame height in pixels (the visible picture); 0 when not open. */
  int height() const { return height_; }
  /** Seconds; 0 when the container does not say. */
  double duration() const { return duration_; }
  /** The nominal rate; 0 when the container does not say (variable-rate
   *  recordings often do not). */
  double frameRate() const { return frameRate_; }
  /** Whether the file has a soundtrack, which readAudio can deliver. */
  bool hasAudio() const { return hasAudio_; }
  /** Why the last open, read or seek failed, in words for a log; empty at a
   *  clean end of file. */
  const std::string& lastError() const { return error_; }

  /**
   * The next frame, into `frame` (resized to width() x height() if it is not
   * already). False at the end of the file, or on an error -- lastError()
   * says which: it is empty at a clean end.
   */
  bool readFrame(Bitmap& frame, double* timestampSeconds = nullptr) {
    if (!open_) return false;
    return readFramePlatform(frame, timestampSeconds);
  }

  /** Make the next readFrame return the first frame AT OR AFTER `seconds`
   *  (decoding forward from the keyframe before it, so it is exact rather
   *  than "the nearest keyframe"). Moves the soundtrack too. */
  bool seek(double seconds) {
    if (!open_) return false;
    if (!(seconds > 0.0)) seconds = 0.0;
    if (!seekVideoPlatform(seconds)) return false;
    skipBefore_ = seconds;
    seekAudio(seconds);
    return true;
  }

  // ── The soundtrack ────────────────────────────────────────────────────

  /**
   * Ask for the soundtrack as 32-bit float, interleaved, at this rate and
   * channel count -- converted by the platform. 48 kHz stereo until called.
   * False when the file has no soundtrack or the conversion is refused;
   * audioSampleRate()/audioChannels() say what is actually delivered.
   */
  bool setAudioFormat(int sampleRate, int channels) {
    wantRate_ = sampleRate > 0 ? sampleRate : 48000;
    wantChannels_ = channels > 0 ? channels : 2;
    if (!open_ || !hasAudio_) return false;
    audioPending_.clear();
    audioOffset_ = 0;
    return configureAudioPlatform();
  }

  /** The rate readAudio delivers, in Hz, once the soundtrack is set up --
   *  by setAudioFormat or the first readAudio; 0 until one has been. */
  int audioSampleRate() const { return audioRate_; }
  /** The channels per frame readAudio delivers, once set up; 0 until one
   *  has been. */
  int audioChannels() const { return audioChannels_; }

  /** Up to `maxFrames` frames into `interleaved` (audioChannels() floats
   *  each). Returns how many were written; 0 at the end. */
  size_t readAudio(float* interleaved, size_t maxFrames) {
    if (!open_ || !hasAudio_ || !interleaved || maxFrames == 0) return 0;
    if (!audioReady_ && !configureAudioPlatform()) return 0;
    const size_t ch = (size_t) audioChannels_;
    size_t written = 0;
    while (written < maxFrames) {
      if (audioOffset_ < audioPending_.size()) {
        const size_t frames = (audioPending_.size() - audioOffset_) / ch;
        const size_t take = (std::min)(frames, maxFrames - written);
        if (take == 0) {
          audioPending_.clear();
          audioOffset_ = 0;
          continue;
        }
        std::memcpy(interleaved + written * ch, audioPending_.data() + audioOffset_, take * ch * sizeof(float));
        audioOffset_ += take * ch;
        written += take;
        continue;
      }
      audioPending_.clear();
      audioOffset_ = 0;
      double start = 0.0;
      if (!readAudioPacketPlatform(start)) break;
      // After a seek, a packet that starts before the target loses its head.
      if (audioSkipBefore_ >= 0.0 && !audioPending_.empty()) {
        const double frames = (double) (audioPending_.size() / ch);
        const double end = start + frames / (double) audioRate_;
        if (end <= audioSkipBefore_) {
          audioPending_.clear();
          continue;
        }
        if (start < audioSkipBefore_) {
          size_t drop = (size_t) ((audioSkipBefore_ - start) * (double) audioRate_ + 0.5);
          drop = (std::min)(drop, audioPending_.size() / ch);
          audioOffset_ = drop * ch;
        }
        audioSkipBefore_ = -1.0;
      }
    }
    return written;
  }

  /** Move the soundtrack alone, leaving the picture where it is -- what a
   *  player does when it starts the sound partway through. */
  bool seekAudio(double seconds) {
    if (!open_ || !hasAudio_) return false;
    if (!(seconds > 0.0)) seconds = 0.0;
    audioPending_.clear();
    audioOffset_ = 0;
    audioSkipBefore_ = seconds;
    audioPosition_ = seconds;
    if (!audioReady_) return true; // applied when the soundtrack is first read
    return seekAudioPlatform(seconds);
  }

private:
  bool fail(const std::string& message) {
    error_ = message;
    return false;
  }

  std::string path_, error_;
  bool open_ = false;
  int width_ = 0, height_ = 0;
  double duration_ = 0.0, frameRate_ = 0.0;
  double skipBefore_ = -1.0;
  bool hasAudio_ = false, audioReady_ = false;
  int wantRate_ = 48000, wantChannels_ = 2;
  int audioRate_ = 0, audioChannels_ = 0;
  std::vector<float> audioPending_;
  size_t audioOffset_ = 0;
  double audioSkipBefore_ = -1.0;
  double audioPosition_ = 0.0;

#if defined(_WIN32)
  detail::mf::Ref<IMFSourceReader> video_, audio_;
  DWORD comThread_ = 0;
  bool comOwned_ = false, mfStarted_ = false;
  int bufferWidth_ = 0, bufferHeight_ = 0, cropX_ = 0, cropY_ = 0;
  LONG stride_ = 0;

  static DWORD firstVideo() { return (DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM; }
  static DWORD firstAudio() { return (DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM; }

  bool failHr(const char* what, HRESULT hr) { return fail(detail::mf::hresultMessage(what, hr)); }

  bool openPlatform() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(co)) {
      comOwned_ = true;
      comThread_ = GetCurrentThreadId();
    }
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) return fail("Media Foundation would not start");
    mfStarted_ = true;

    if (!createReader(video_, true)) return false;
    if (!requestRgb32()) return false;

    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(video_->GetPresentationAttribute((DWORD) MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION,
                                                   &value)) &&
        value.vt == VT_UI8)
      duration_ = (double) value.uhVal.QuadPart / 1e7;
    PropVariantClear(&value);

    detail::mf::Ref<IMFMediaType> audioType;
    hasAudio_ = SUCCEEDED(video_->GetNativeMediaType(firstAudio(), 0, audioType.put()));
    return true;
  }

  bool createReader(detail::mf::Ref<IMFSourceReader>& reader, bool forVideo) {
    detail::mf::Ref<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(attributes.put(), 1);
    if (FAILED(hr)) return failHr("no attribute store", hr);
    // The Win7 processor rather than the advanced one: it does exactly the
    // YUV -> RGB32 conversion asked for, in software, identically on every
    // machine -- the advanced path may take the GPU and hand back frames in
    // video memory.
    if (forVideo) attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    // NOT widenPath: its long-path \\?\ prefix is for the C library, and the
    // source resolver reads a URL.
    const std::wstring wide = pathio::widen(path_.c_str());
    hr = MFCreateSourceReaderFromURL(wide.c_str(), attributes.get(), reader.put());
    if (FAILED(hr)) return failHr("the file could not be opened as media", hr);
    // ONE stream per reader. A reader with both selected keeps every audio
    // packet it decodes while the caller only asks for frames, and memory
    // grows for as long as the video plays.
    reader->SetStreamSelection((DWORD) MF_SOURCE_READER_ALL_STREAMS, FALSE);
    hr = reader->SetStreamSelection(forVideo ? firstVideo() : firstAudio(), TRUE);
    if (FAILED(hr)) return failHr(forVideo ? "the file has no video stream" : "the file has no audio stream", hr);
    return true;
  }

  bool requestRgb32() {
    detail::mf::Ref<IMFMediaType> wanted;
    HRESULT hr = MFCreateMediaType(wanted.put());
    if (FAILED(hr)) return failHr("no media type", hr);
    wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = video_->SetCurrentMediaType(firstVideo(), nullptr, wanted.get());
    if (FAILED(hr)) return failHr("the video could not be decoded to RGB", hr);
    return readVideoFormat();
  }

  bool readVideoFormat() {
    detail::mf::Ref<IMFMediaType> current;
    HRESULT hr = video_->GetCurrentMediaType(firstVideo(), current.put());
    if (FAILED(hr)) return failHr("no current video format", hr);
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(current.get(), MF_MT_FRAME_SIZE, &w, &h);
    if (w == 0 || h == 0 || w > 16384 || h > 16384) return fail("the video reports no usable frame size");
    bufferWidth_ = (int) w;
    bufferHeight_ = (int) h;
    UINT32 stride = 0;
    stride_ = SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG) stride : (LONG) w * 4;

    // The visible area: the output type's own, else the native one's. A
    // codec decodes in whole macroblocks, and the rows past the picture are
    // padding nobody should see.
    cropX_ = cropY_ = 0;
    width_ = (int) w;
    height_ = (int) h;
    MFVideoArea area{};
    bool haveArea = SUCCEEDED(current->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*) &area, sizeof(area), nullptr));
    if (!haveArea) {
      detail::mf::Ref<IMFMediaType> native;
      if (SUCCEEDED(video_->GetNativeMediaType(firstVideo(), 0, native.put())))
        haveArea = SUCCEEDED(native->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*) &area, sizeof(area), nullptr));
    }
    if (haveArea) {
      const int x = area.OffsetX.value, y = area.OffsetY.value;
      const int cw = (int) area.Area.cx, ch = (int) area.Area.cy;
      if (x >= 0 && y >= 0 && cw > 0 && ch > 0 && x + cw <= (int) w && y + ch <= (int) h) {
        cropX_ = x;
        cropY_ = y;
        width_ = cw;
        height_ = ch;
      }
    }
    UINT32 num = 0, den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(current.get(), MF_MT_FRAME_RATE, &num, &den)) && den != 0)
      frameRate_ = (double) num / (double) den;
    return true;
  }

  bool readFramePlatform(Bitmap& frame, double* timestamp) {
    error_.clear();
    for (;;) {
      DWORD stream = 0, flags = 0;
      LONGLONG time = 0;
      detail::mf::Ref<IMFSample> sample;
      const HRESULT hr = video_->ReadSample(firstVideo(), 0, &stream, &flags, &time, sample.put());
      if (FAILED(hr)) return failHr("decoding failed", hr);
      if (flags & MF_SOURCE_READERF_ERROR) return fail("decoding failed");
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return false;
      if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && !readVideoFormat()) return false;
      if (!sample) continue; // a gap in the stream: nothing to show yet
      const double seconds = (double) time / 1e7;
      if (skipBefore_ >= 0.0) {
        // Half a frame of tolerance: a timestamp of 0.49999 is the frame at 0.5.
        const double half = frameRate_ > 0.0 ? 0.5 / frameRate_ : 0.001;
        if (seconds < skipBefore_ - half) continue;
        skipBefore_ = -1.0;
      }
      if (!detail::mf::copyRgb32Sample(sample.get(), stride_, bufferWidth_, bufferHeight_, cropX_, cropY_,
                                       width_, height_, frame))
        return fail("a decoded frame did not match its format");
      if (timestamp) *timestamp = seconds;
      return true;
    }
  }

  bool seekVideoPlatform(double seconds) {
    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = detail::mf::toHundredNanoseconds(seconds);
    const HRESULT hr = video_->SetCurrentPosition(GUID_NULL, position);
    if (FAILED(hr)) return failHr("seeking failed", hr);
    return true;
  }

  bool configureAudioPlatform() {
    audioReady_ = false;
    if (!audio_ && !createReader(audio_, false)) return false;
    detail::mf::Ref<IMFMediaType> wanted;
    HRESULT hr = MFCreateMediaType(wanted.put());
    if (FAILED(hr)) return failHr("no media type", hr);
    wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    wanted->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    wanted->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) wantRate_);
    wanted->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32) wantChannels_);
    wanted->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    wanted->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32) wantChannels_ * 4u);
    wanted->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) wantRate_ * (UINT32) wantChannels_ * 4u);
    hr = audio_->SetCurrentMediaType(firstAudio(), nullptr, wanted.get());
    if (FAILED(hr)) return failHr("the soundtrack could not be converted to that format", hr);
    detail::mf::Ref<IMFMediaType> current;
    hr = audio_->GetCurrentMediaType(firstAudio(), current.put());
    if (FAILED(hr)) return failHr("no current audio format", hr);
    UINT32 rate = 0, channels = 0;
    current->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    current->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    if (rate == 0 || channels == 0 || channels > 32) return fail("the soundtrack reports no usable format");
    audioRate_ = (int) rate;
    audioChannels_ = (int) channels;
    audioReady_ = true;
    return seekAudioPlatform(audioPosition_);
  }

  bool seekAudioPlatform(double seconds) {
    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = detail::mf::toHundredNanoseconds(seconds);
    const HRESULT hr = audio_->SetCurrentPosition(GUID_NULL, position);
    if (FAILED(hr)) return failHr("seeking the soundtrack failed", hr);
    audioSkipBefore_ = seconds;
    return true;
  }

  bool readAudioPacketPlatform(double& start) {
    for (;;) {
      DWORD stream = 0, flags = 0;
      LONGLONG time = 0;
      detail::mf::Ref<IMFSample> sample;
      const HRESULT hr = audio_->ReadSample(firstAudio(), 0, &stream, &flags, &time, sample.put());
      if (FAILED(hr)) return failHr("decoding the soundtrack failed", hr);
      if (flags & MF_SOURCE_READERF_ERROR) return fail("decoding the soundtrack failed");
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return false;
      if (!sample) continue;
      detail::mf::Ref<IMFMediaBuffer> buffer;
      if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())) || !buffer) continue;
      BYTE* data = nullptr;
      DWORD capacity = 0, length = 0;
      if (FAILED(buffer->Lock(&data, &capacity, &length)) || !data) continue;
      const size_t ch = (size_t) audioChannels_;
      const size_t frames = (size_t) length / (sizeof(float) * ch);
      audioPending_.resize(frames * ch);
      if (frames) std::memcpy(audioPending_.data(), data, frames * ch * sizeof(float));
      buffer->Unlock();
      start = (double) time / 1e7;
      if (frames) return true;
    }
  }

  void closePlatform() {
    video_.reset();
    audio_.reset();
    audioReady_ = false;
    if (mfStarted_) {
      MFShutdown();
      mfStarted_ = false;
    }
    // Only on the thread that initialised it. Uninitialising another thread's
    // COM is not possible; leaving one reference on a thread that already
    // had COM costs nothing.
    if (comOwned_ && comThread_ == GetCurrentThreadId()) CoUninitialize();
    comOwned_ = false;
  }

#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
  id asset_ = nil, videoTrack_ = nil, audioTrack_ = nil;
  id videoReader_ = nil, videoOutput_ = nil, audioReader_ = nil, audioOutput_ = nil;

  bool openPlatform() {
    using namespace detail::avf;
    const Api& a = api();
    if (!a.ok) return fail("AVFoundation is not available");
    Pool pool;
    id url = send<id>(classNamed("NSURL"), "fileURLWithPath:", nsString(path_));
    asset_ = nsRetain(send<id>(classNamed("AVURLAsset"), "URLAssetWithURL:options:", url, (id) nil));
    if (!asset_) return fail("the file could not be opened as media");
    id videoTracks = send<id>(asset_, "tracksWithMediaType:", a.mediaTypeVideo);
    if (nsCount(videoTracks) == 0) return fail("the file has no video stream");
    videoTrack_ = nsRetain(nsAt(videoTracks, 0));
    const PixelSize size = send<PixelSize>(videoTrack_, "naturalSize");
    width_ = (int) (size.width + 0.5);
    height_ = (int) (size.height + 0.5);
    if (width_ <= 0 || height_ <= 0 || width_ > 16384 || height_ > 16384)
      return fail("the video reports no usable frame size");
    frameRate_ = (double) send<float>(videoTrack_, "nominalFrameRate");
    duration_ = a.CMTimeGetSeconds(sendForStruct<MediaTime>(asset_, "duration"));
    if (!std::isfinite(duration_) || duration_ < 0.0) duration_ = 0.0;
    id audioTracks = send<id>(asset_, "tracksWithMediaType:", a.mediaTypeAudio);
    hasAudio_ = nsCount(audioTracks) > 0;
    if (hasAudio_) audioTrack_ = nsRetain(nsAt(audioTracks, 0));
    return startVideoReader(0.0);
  }

  /** An AVAssetReader cannot be rewound, so a seek is a new one whose time
   *  range starts at the target. */
  bool startVideoReader(double from) {
    using namespace detail::avf;
    const Api& a = api();
    Pool pool;
    nsRelease(videoReader_);
    nsRelease(videoOutput_);
    videoReader_ = videoOutput_ = nil;
    videoReader_ = send<id>(send<id>(classNamed("AVAssetReader"), "alloc"), "initWithAsset:error:", asset_,
                            (id*) nullptr);
    if (!videoReader_) return fail("the video could not be read");
    videoOutput_ = nsRetain(send<id>(classNamed("AVAssetReaderTrackOutput"),
                                     "assetReaderTrackOutputWithTrack:outputSettings:", videoTrack_,
                                     bgraSettings()));
    if (!videoOutput_) return fail("the video could not be decoded to BGRA");
    send<void>(videoReader_, "addOutput:", videoOutput_);
    if (from > 0.0) {
      const MediaTimeRange range{a.CMTimeMakeWithSeconds(from, 600), timeForever()};
      send<void>(videoReader_, "setTimeRange:", range);
    }
    if (!send<BOOL>(videoReader_, "startReading")) return fail("the video could not be read");
    return true;
  }

  bool readFramePlatform(Bitmap& frame, double* timestamp) {
    using namespace detail::avf;
    const Api& a = api();
    error_.clear();
    Pool pool;
    for (;;) {
      void* sample = send<void*>(videoOutput_, "copyNextSampleBuffer");
      if (!sample) return false; // the end, or a reader that failed: status says which
      const double seconds = a.CMTimeGetSeconds(a.CMSampleBufferGetPresentationTimeStamp(sample));
      if (skipBefore_ >= 0.0) {
        const double half = frameRate_ > 0.0 ? 0.5 / frameRate_ : 0.001;
        if (seconds < skipBefore_ - half) {
          a.CFRelease(sample);
          continue;
        }
        skipBefore_ = -1.0;
      }
      const bool ok = copyPixelBuffer(a.CMSampleBufferGetImageBuffer(sample), frame);
      a.CFRelease(sample);
      if (!ok) return fail("a decoded frame did not match its format");
      if (timestamp) *timestamp = seconds;
      return true;
    }
  }

  bool seekVideoPlatform(double seconds) { return startVideoReader(seconds); }

  bool configureAudioPlatform() {
    audioReady_ = false;
    if (!audioTrack_) return fail("the file has no audio stream");
    // Stereo or mono only: more channels than two need a channel layout
    // spelled out as well, which is a format question this reader does not
    // answer for its caller.
    if (wantChannels_ > 2) wantChannels_ = 2;
    audioRate_ = wantRate_;
    audioChannels_ = wantChannels_;
    audioReady_ = true;
    return seekAudioPlatform(audioPosition_);
  }

  bool seekAudioPlatform(double seconds) {
    using namespace detail::avf;
    const Api& a = api();
    Pool pool;
    nsRelease(audioReader_);
    nsRelease(audioOutput_);
    audioReader_ = audioOutput_ = nil;
    audioReader_ = send<id>(send<id>(classNamed("AVAssetReader"), "alloc"), "initWithAsset:error:", asset_,
                            (id*) nullptr);
    if (!audioReader_) return fail("the soundtrack could not be read");
    const id keys[7] = {a.formatIdKey, a.isFloatKey, a.bitDepthKey, a.isNonInterleavedKey,
                        a.isBigEndianKey, a.sampleRateKey, a.channelsKey};
    const id values[7] = {nsNumberUnsigned(0x6C70636Du) /* 'lpcm' */, nsNumberBool(true),
                          nsNumberUnsigned(32u), nsNumberBool(false), nsNumberBool(false),
                          nsNumberDouble((double) audioRate_), nsNumberUnsigned((unsigned) audioChannels_)};
    for (const id k : keys)
      if (!k) return fail("AVFoundation's audio settings keys are missing");
    audioOutput_ = nsRetain(send<id>(classNamed("AVAssetReaderTrackOutput"),
                                     "assetReaderTrackOutputWithTrack:outputSettings:", audioTrack_,
                                     nsDictionary(values, keys, 7)));
    if (!audioOutput_) return fail("the soundtrack could not be converted to that format");
    send<void>(audioReader_, "addOutput:", audioOutput_);
    if (seconds > 0.0) {
      const MediaTimeRange range{a.CMTimeMakeWithSeconds(seconds, 600), timeForever()};
      send<void>(audioReader_, "setTimeRange:", range);
    }
    if (!send<BOOL>(audioReader_, "startReading")) return fail("the soundtrack could not be read");
    audioSkipBefore_ = seconds;
    return true;
  }

  bool readAudioPacketPlatform(double& start) {
    using namespace detail::avf;
    const Api& a = api();
    Pool pool;
    for (;;) {
      void* sample = send<void*>(audioOutput_, "copyNextSampleBuffer");
      if (!sample) return false;
      start = a.CMTimeGetSeconds(a.CMSampleBufferGetPresentationTimeStamp(sample));
      void* block = a.CMSampleBufferGetDataBuffer(sample);
      const size_t bytes = block ? a.CMBlockBufferGetDataLength(block) : 0;
      const size_t ch = (size_t) audioChannels_;
      const size_t frames = bytes / (sizeof(float) * ch);
      audioPending_.resize(frames * ch);
      const bool copied = frames && a.CMBlockBufferCopyDataBytes(block, 0, frames * ch * sizeof(float),
                                                                 audioPending_.data()) == 0;
      a.CFRelease(sample);
      if (copied) return true;
      audioPending_.clear();
    }
  }

  void closePlatform() {
    using namespace detail::avf;
    if (!api().ok) return;
    Pool pool;
    for (id* object : {&videoReader_, &videoOutput_, &audioReader_, &audioOutput_, &videoTrack_,
                       &audioTrack_, &asset_}) {
      if (*object) {
        if (object == &videoReader_ || object == &audioReader_) send<void>(*object, "cancelReading");
        nsRelease(*object);
        *object = nil;
      }
    }
    audioReady_ = false;
  }

#else
  bool openPlatform() { return fail("video decoding is not available on this platform"); }
  bool readFramePlatform(Bitmap&, double*) { return false; }
  bool seekVideoPlatform(double) { return false; }
  bool configureAudioPlatform() { return false; }
  bool seekAudioPlatform(double) { return false; }
  bool readAudioPacketPlatform(double&) { return false; }
  void closePlatform() {}
#endif
};

// ═════════════════════════════════════════════════════════════════════════════
// Playback backends
// ═════════════════════════════════════════════════════════════════════════════

#if defined(_WIN32)
namespace detail {

/**
 * Windows playback: a worker thread that owns a VideoReader, decodes ahead by
 * one frame, and hands each over when the clock reaches it.
 *
 * The CLOCK is media time = anchor + (now - anchorWall) * speed, kept under
 * the lock and re-anchored on every play, pause, seek and speed change -- so
 * position() is exact between frames and a speed change does not jump.
 *
 * Frames move by SWAP, never by copy: decoded into `pending`, swapped into
 * `front_` when due, swapped out into the component's bitmap by takeFrame().
 * Three buffers cycling, and no 8 MB memcpy on either thread.
 */
class MfVideoBackend final : public VideoBackend {
public:
  ~MfVideoBackend() override { shutdown(); }

  bool open(const std::string& path, std::string& error) override {
    path_ = path;
    worker_ = std::thread([this]() { run(); });
    std::unique_lock<std::mutex> lock(mutex_);
    opened_.wait(lock, [this]() { return openState_ != 0; });
    if (openState_ < 0) {
      error = openError_;
      lock.unlock();
      shutdown();
      return false;
    }
    return true;
  }

  int width() const override { return width_; }
  int height() const override { return height_; }
  double duration() const override { return duration_; }

  void play() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (playing_) return;
    // At the end, play means from the top: a player that stays at the end
    // when asked to play looks broken.
    if (duration_ > 0.0 && anchorMedia_ >= duration_ - 1e-6) {
      anchorMedia_ = 0.0;
      seekPending_ = true;
      seekTarget_ = 0.0;
    }
    playing_ = true;
    ended_ = false;
    anchorWall_ = videoSteadySeconds();
    wake_.notify_all();
  }

  void pause() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!playing_) return;
    anchorMedia_ = clampTime(mediaTimeLocked());
    anchorWall_ = videoSteadySeconds();
    playing_ = false;
    wake_.notify_all();
  }

  bool isPlaying() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return playing_;
  }

  void setPosition(double seconds) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const double t = clampTime(seconds);
    anchorMedia_ = t;
    anchorWall_ = videoSteadySeconds();
    seekPending_ = true;
    seekTarget_ = t;
    ended_ = false;
    wake_.notify_all();
  }

  double position() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return clampTime(mediaTimeLocked());
  }

  void setSpeed(double speed) override {
    std::lock_guard<std::mutex> lock(mutex_);
    anchorMedia_ = mediaTimeLocked();
    anchorWall_ = videoSteadySeconds();
    speed_ = speed;
    wake_.notify_all();
  }

  void setVolume(float volume) override { volume_.store(volume, std::memory_order_relaxed); }

  void setAudioEnabled(bool enabled) override {
    std::lock_guard<std::mutex> lock(mutex_);
    audioWanted_ = enabled;
    audioFailed_ = false; // asking again is asking to try again
    wake_.notify_all();
  }

  bool takeFrame(Bitmap& into) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!newFrame_) return false;
    std::swap(into, front_);
    newFrame_ = false;
    return true;
  }

  bool takeEnded() override {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool e = ended_;
    ended_ = false;
    return e;
  }

  std::string takeError() override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string e;
    e.swap(pendingError_);
    return e;
  }

  VideoPlaybackStats stats() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

private:
  double mediaTimeLocked() const {
    return playing_ ? anchorMedia_ + (videoSteadySeconds() - anchorWall_) * speed_ : anchorMedia_;
  }

  double clampTime(double t) const {
    if (!(t > 0.0)) return 0.0;
    return duration_ > 0.0 && t > duration_ ? duration_ : t;
  }

  void publishLocked(Bitmap& frame) {
    std::swap(front_, frame);
    newFrame_ = true;
    ++stats_.framesShown;
    lastPublishWall_ = videoSteadySeconds();
  }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      quit_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  void run() {
    mf::ComScope com(COINIT_MULTITHREADED);
    VideoReader reader;
    std::string error;
    Bitmap pending;
    double pendingTime = 0.0;
    bool opened = reader.open(path_, &error);
    bool havePoster = false;
    if (opened) havePoster = reader.readFrame(pending, &pendingTime);
    if (opened && !havePoster && !reader.lastError().empty()) {
      error = reader.lastError();
      opened = false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (opened) {
        width_ = reader.width();
        height_ = reader.height();
        duration_ = reader.duration();
        frameRate_ = reader.frameRate();
        hasAudio_ = reader.hasAudio();
        // The first frame, shown before anything plays -- a poster rather
        // than a black rectangle until the user presses play.
        if (havePoster) {
          ++stats_.framesDecoded;
          publishLocked(pending);
        }
        openState_ = 1;
      } else {
        openError_ = error.empty() ? std::string("the video could not be opened") : error;
        openState_ = -1;
      }
    }
    opened_.notify_all();
    if (!opened) return;

    bool havePending = false, atEnd = !havePoster;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!quit_) {
      if (seekPending_) {
        const double target = seekTarget_;
        seekPending_ = false;
        // Silent until the soundtrack is refilled from the new position.
        closeGate();
        lock.unlock();
        bool got = reader.seek(target);
        if (got) got = reader.readFrame(pending, &pendingTime);
        lock.lock();
        havePending = false;
        atEnd = !got;
        if (got) {
          ++stats_.framesDecoded;
          publishLocked(pending);
        }
        continue;
      }

      updateAudio(lock, reader);
      if (quit_ || seekPending_) continue;

      if (!playing_) {
        wake_.wait(lock);
        continue;
      }

      if (!havePending && !atEnd) {
        lock.unlock();
        const bool got = reader.readFrame(pending, &pendingTime);
        const std::string why = got ? std::string() : reader.lastError();
        lock.lock();
        if (got) {
          havePending = true;
          ++stats_.framesDecoded;
        } else {
          atEnd = true;
          if (!why.empty()) pendingError_ = why;
        }
        continue;
      }

      const double now = mediaTimeLocked();
      // Wake at least fifty times a second while playing: the soundtrack's
      // ring is topped up from this loop.
      const double tick = 0.02;
      if (havePending) {
        if (pendingTime <= now) {
          const double frame = frameRate_ > 0.0 ? 1.0 / frameRate_ : 1.0 / 30.0;
          // Two frames late and something shown recently: skip it and catch
          // up. Never skip for longer than a tenth of a second, or a decoder
          // that is merely slow would show nothing at all.
          if (pendingTime + 2.0 * frame < now && videoSteadySeconds() - lastPublishWall_ < 0.1) {
            ++stats_.framesDropped;
          } else {
            publishLocked(pending);
          }
          havePending = false;
          continue;
        }
        const double wait = (pendingTime - now) / (speed_ > 0.0 ? speed_ : 1.0);
        wake_.wait_for(lock, std::chrono::duration<double>((std::min)(wait, tick)));
        continue;
      }

      // Decoded to the end: run the clock out to the duration, then stop.
      if (duration_ <= 0.0 || now >= duration_) {
        playing_ = false;
        anchorMedia_ = duration_ > 0.0 ? duration_ : now;
        anchorWall_ = videoSteadySeconds();
        ended_ = true;
        continue;
      }
      wake_.wait_for(lock, std::chrono::duration<double>(
                               (std::min)((duration_ - now) / (speed_ > 0.0 ? speed_ : 1.0), tick)));
    }
    lock.unlock();
    closeDevice();
  }

  // ── The soundtrack (worker thread only, except render) ────────────────
  //
  // The DEVICE lives for as long as the soundtrack is enabled, playing or
  // not: opening a WASAPI stream takes from tens to hundreds of milliseconds
  // (half a second, measured, the first time a process does it), and paying
  // that on every play, pause and seek froze the picture each time. What
  // play and pause move instead is a GATE: closed, render() writes silence.
  //
  // A seek, or opening the gate again, must not play what was queued for the
  // old position. The ring has one reader and one writer and cannot be reset
  // under a running reader, so the worker asks render() to empty it (a FLUSH
  // request, by counter) and waits for the answer before refilling it from
  // the new position.

  void updateAudio(std::unique_lock<std::mutex>& lock, VideoReader& reader) {
    // The device: open while the soundtrack is wanted at all.
    const bool wantDevice = audioWanted_ && !audioFailed_ && hasAudio_;
    if (wantDevice && !output_) {
      lock.unlock();
      std::string why;
      const bool opened = openDevice(reader, why);
      lock.lock();
      if (!opened) {
        audioFailed_ = true;
        stats_.audioError = why;
        pendingError_ = "the soundtrack cannot play: " + why;
      } else {
        stats_.audioError.clear();
        stats_.audioDeviceOpen = true;
      }
      return; // the state may have moved on while the lock was let go
    }
    if (!wantDevice && output_) {
      lock.unlock();
      closeDevice();
      lock.lock();
      stats_.audioDeviceOpen = false;
      return;
    }
    if (!output_) return;

    // The gate: open while playing at 1x.
    const bool sounding = playing_ && speed_ == 1.0;
    if (sounding && !gateOpen_) {
      lock.unlock();
      const bool flushed = flushRing();
      lock.lock();
      if (!flushed) {
        // The device stopped calling back -- unplugged, or taken over. Closed
        // here; it is reopened on the next pass if still wanted.
        lock.unlock();
        closeDevice();
        lock.lock();
        stats_.audioDeviceOpen = false;
        return;
      }
      if (!(playing_ && speed_ == 1.0) || seekPending_) return;
      // From the clock's position NOW: the decode below takes milliseconds,
      // and the picture is re-anchored to the same instant the sound starts.
      const double from = clampTime(mediaTimeLocked());
      lock.unlock();
      reader.seekAudio(from);
      const uint64_t queued = topUpAudio(reader);
      lock.lock();
      stats_.audioFramesQueued += queued;
      if (playing_ && !seekPending_) {
        anchorMedia_ = from;
        anchorWall_ = videoSteadySeconds();
      }
      gate_.store(true, std::memory_order_release);
      gateOpen_ = true;
    } else if (!sounding && gateOpen_) {
      closeGate();
    } else if (sounding) {
      lock.unlock();
      const uint64_t queued = topUpAudio(reader);
      lock.lock();
      stats_.audioFramesQueued += queued;
    }
  }

  bool openDevice(VideoReader& reader, std::string& why) {
    output_.reset(new standalone::WasapiOutput());
    gate_.store(false);
    gateOpen_ = false;
    if (!output_->open([this](float* out, uint32_t frames, uint32_t channels) { render(out, frames, channels); })) {
      why = output_->error();
      output_.reset();
      return false;
    }
    const int rate = (int) output_->sampleRate();
    // Decoded as STEREO at the device's rate whatever the device has, and
    // spread onto its channels in render(): Media Foundation will resample,
    // but not every soundtrack upmixes to a surround device's eight.
    if (!reader.setAudioFormat(rate, 2) || reader.audioSampleRate() != rate || reader.audioChannels() != 2) {
      why = reader.lastError().empty() ? std::string("the soundtrack could not be converted") : reader.lastError();
      output_->stop();
      output_.reset();
      return false;
    }
    // Sized while no render thread exists: these are the only allocations,
    // and render() never makes one.
    ring_.reset((size_t) rate / 2, 2); // half a second ahead
    renderScratch_.assign(kRenderChunk * 2, 0.0f);
    flushRequest_.store(0);
    flushDone_.store(0);
    if (!output_->run()) {
      why = output_->error();
      output_.reset();
      return false;
    }
    return true;
  }

  void closeDevice() {
    if (output_) {
      output_->stop(); // joins the render thread
      output_.reset();
    }
    gate_.store(false);
    gateOpen_ = false;
  }

  void closeGate() {
    gate_.store(false, std::memory_order_release);
    gateOpen_ = false;
  }

  /** Ask render() to discard what the ring holds, and wait until it has. */
  bool flushRing() {
    const uint32_t request = flushRequest_.fetch_add(1) + 1;
    for (int waited = 0; waited < 500; ++waited) {
      if (flushDone_.load(std::memory_order_acquire) == request) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  }

  uint64_t topUpAudio(VideoReader& reader) {
    uint64_t queued = 0;
    for (int guard = 0; guard < 32; ++guard) {
      const size_t room = ring_.capacityFrames() - ring_.available();
      if (room < 256) break;
      const size_t want = (std::min)(room, (size_t) 4096);
      decodeScratch_.resize(want * 2);
      const size_t got = reader.readAudio(decodeScratch_.data(), want);
      if (got == 0) break;
      ring_.write(decodeScratch_.data(), got);
      queued += got;
    }
    return queued;
  }

  /** The WASAPI thread: stereo from the ring, onto however many channels the
   *  device has, at volume -- or silence while the gate is closed. No
   *  allocation and no lock. */
  void render(float* out, uint32_t frames, uint32_t channels) {
    const uint32_t request = flushRequest_.load(std::memory_order_acquire);
    if (request != flushDone_.load(std::memory_order_relaxed)) {
      while (ring_.readInterleaved(renderScratch_.data(), kRenderChunk) > 0) {
      }
      flushDone_.store(request, std::memory_order_release);
    }
    if (!gate_.load(std::memory_order_acquire)) {
      std::memset(out, 0, sizeof(float) * (size_t) frames * channels);
      return;
    }
    const float volume = volume_.load(std::memory_order_relaxed);
    size_t done = 0;
    while (done < frames) {
      const size_t chunk = (std::min)((size_t) frames - done, kRenderChunk);
      const size_t got = ring_.readInterleaved(renderScratch_.data(), chunk);
      for (size_t i = got; i < chunk; ++i) renderScratch_[i * 2] = renderScratch_[i * 2 + 1] = 0.0f;
      float* o = out + done * channels;
      for (size_t i = 0; i < chunk; ++i, o += channels) {
        const float l = renderScratch_[i * 2] * volume, r = renderScratch_[i * 2 + 1] * volume;
        if (channels == 1) {
          o[0] = 0.5f * (l + r);
        } else {
          o[0] = l;
          o[1] = r;
          for (uint32_t c = 2; c < channels; ++c) o[c] = 0.0f;
        }
      }
      done += chunk;
    }
  }

  static constexpr size_t kRenderChunk = 2048;

  std::string path_;
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable wake_, opened_;
  int openState_ = 0;
  std::string openError_, pendingError_;
  int width_ = 0, height_ = 0;
  double duration_ = 0.0, frameRate_ = 0.0;
  bool hasAudio_ = false;
  bool quit_ = false, playing_ = false, ended_ = false, newFrame_ = false;
  bool seekPending_ = false;
  double seekTarget_ = 0.0;
  double anchorMedia_ = 0.0, anchorWall_ = 0.0, speed_ = 1.0;
  double lastPublishWall_ = 0.0;
  Bitmap front_;
  VideoPlaybackStats stats_;
  bool audioWanted_ = false, audioFailed_ = false;
  bool gateOpen_ = false; // worker thread only: what it last told render()
  std::atomic<bool> gate_{false};
  std::atomic<uint32_t> flushRequest_{0}, flushDone_{0};
  std::atomic<float> volume_{1.0f};
  std::unique_ptr<standalone::WasapiOutput> output_;
  AudioRing ring_;
  std::vector<float> decodeScratch_, renderScratch_;
};

/** Windows capture: a worker that owns the device's source reader and reads
 *  frames until told to stop. */
class MfCameraBackend final : public CameraBackend {
public:
  ~MfCameraBackend() override {
    quit_.store(true);
    if (worker_.joinable()) worker_.join();
  }

  bool open(int index, int minWidth, int minHeight, int maxWidth, int maxHeight, std::string& error,
            CameraFrameSink frames, CameraErrorSink errors) override {
    frames_ = std::move(frames);
    errors_ = std::move(errors);
    worker_ = std::thread(
        [this, index, minWidth, minHeight, maxWidth, maxHeight]() { run(index, minWidth, minHeight, maxWidth, maxHeight); });
    std::unique_lock<std::mutex> lock(mutex_);
    opened_.wait(lock, [this]() { return openState_ != 0; });
    if (openState_ < 0) {
      error = openError_;
      lock.unlock();
      quit_.store(true);
      worker_.join();
      return false;
    }
    return true;
  }

  std::string name() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return name_;
  }
  int width() const override { return width_.load(); }
  int height() const override { return height_.load(); }

private:
  void report(int state, const std::string& error) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      openState_ = state;
      openError_ = error;
    }
    opened_.notify_all();
  }

  void run(int index, int minWidth, int minHeight, int maxWidth, int maxHeight) {
    mf::ComScope com(COINIT_MULTITHREADED);
    mf::Startup startup;
    if (!startup.ok()) return report(-1, "Media Foundation would not start");

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    HRESULT hr = mf::enumerateCameras(&devices, &count);
    if (FAILED(hr)) return report(-1, mf::hresultMessage("cameras could not be listed", hr));
    mf::Ref<IMFMediaSource> source;
    std::string deviceName;
    if (index >= 0 && (UINT32) index < count) {
      deviceName = mf::activateName(devices[index]);
      hr = devices[index]->ActivateObject(IID_PPV_ARGS(source.put()));
    }
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);
    if (index < 0 || (UINT32) index >= count) {
      char message[96];
      std::snprintf(message, sizeof(message), "there is no camera %d (%u found)", index, (unsigned) count);
      return report(-1, message);
    }
    if (FAILED(hr) || !source) return report(-1, mf::hresultMessage("the camera would not open", hr));

    mf::Ref<IMFAttributes> attributes;
    MFCreateAttributes(attributes.put(), 1);
    if (attributes) attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    mf::Ref<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.get(), attributes.get(), reader.put());
    if (FAILED(hr)) {
      source->Shutdown();
      return report(-1, mf::hresultMessage("the camera's stream would not open", hr));
    }
    const DWORD video = (DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    chooseNativeType(reader.get(), minWidth, minHeight, maxWidth, maxHeight);

    mf::Ref<IMFMediaType> wanted;
    MFCreateMediaType(wanted.put());
    wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    wanted->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = reader->SetCurrentMediaType(video, nullptr, wanted.get());
    mf::Ref<IMFMediaType> current;
    if (SUCCEEDED(hr)) hr = reader->GetCurrentMediaType(video, current.put());
    UINT32 w = 0, h = 0, strideBits = 0;
    if (SUCCEEDED(hr)) MFGetAttributeSize(current.get(), MF_MT_FRAME_SIZE, &w, &h);
    if (FAILED(hr) || w == 0 || h == 0 || w > 16384 || h > 16384) {
      source->Shutdown();
      return report(-1, mf::hresultMessage("the camera's picture could not be converted to RGB", hr));
    }
    const LONG stride = SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &strideBits)) ? (LONG) strideBits
                                                                                         : (LONG) w * 4;
    width_.store((int) w);
    height_.store((int) h);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      name_ = deviceName;
    }
    report(1, std::string());

    Bitmap frame;
    while (!quit_.load()) {
      DWORD stream = 0, flags = 0;
      LONGLONG time = 0;
      mf::Ref<IMFSample> sample;
      hr = reader->ReadSample(video, 0, &stream, &flags, &time, sample.put());
      if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
        // Unplugged, taken by another application, or switched off by the
        // privacy setting while running. Said once, and the thread ends.
        if (errors_) errors_(mf::hresultMessage("the camera stopped delivering frames", FAILED(hr) ? hr : E_FAIL));
        break;
      }
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
      if (!sample) continue;
      if (mf::copyRgb32Sample(sample.get(), stride, (int) w, (int) h, 0, 0, (int) w, (int) h, frame) && frames_)
        frames_(frame);
    }
    reader.reset();
    source->Shutdown();
  }

  /** Of the formats the device offers natively, the largest within the
   *  requested bounds -- picked BEFORE asking for RGB, because the converter
   *  converts whatever the device was left at, and a webcam's default is
   *  often its smallest. Nothing in bounds: whatever it defaults to. */
  static void chooseNativeType(IMFSourceReader* reader, int minWidth, int minHeight, int maxWidth,
                               int maxHeight) {
    const DWORD video = (DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    mf::Ref<IMFMediaType> best;
    UINT64 bestArea = 0;
    for (DWORD i = 0; i < 512; ++i) {
      mf::Ref<IMFMediaType> type;
      if (FAILED(reader->GetNativeMediaType(video, i, type.put()))) break;
      UINT32 w = 0, h = 0;
      if (FAILED(MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &w, &h))) continue;
      if ((int) w < minWidth || (int) h < minHeight) continue;
      if (maxWidth > 0 && (int) w > maxWidth) continue;
      if (maxHeight > 0 && (int) h > maxHeight) continue;
      const UINT64 area = (UINT64) w * h;
      if (area > bestArea) {
        bestArea = area;
        best = std::move(type);
      }
    }
    if (best) reader->SetCurrentMediaType(video, nullptr, best.get());
  }

  std::thread worker_;
  std::atomic<bool> quit_{false};
  mutable std::mutex mutex_;
  std::condition_variable opened_;
  int openState_ = 0;
  std::string openError_, name_;
  std::atomic<int> width_{0}, height_{0};
  CameraFrameSink frames_;
  CameraErrorSink errors_;
};

inline std::unique_ptr<VideoBackend> makeVideoBackend() { return std::unique_ptr<VideoBackend>(new MfVideoBackend()); }
inline std::unique_ptr<CameraBackend> makeCameraBackend() {
  return std::unique_ptr<CameraBackend>(new MfCameraBackend());
}

inline std::vector<std::string> listCameras() {
  std::vector<std::string> names;
  mf::ComScope com(COINIT_APARTMENTTHREADED);
  mf::Startup startup;
  if (!startup.ok()) return names;
  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  if (FAILED(mf::enumerateCameras(&devices, &count))) return names;
  for (UINT32 i = 0; i < count; ++i) {
    names.push_back(mf::activateName(devices[i]));
    devices[i]->Release();
  }
  CoTaskMemFree(devices);
  return names;
}

} // namespace detail

#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
namespace detail {

/**
 * macOS playback: AVPlayer does the clock, the sound, the rate and the seek;
 * AVPlayerItemVideoOutput hands over the frame for "now" when takeFrame asks.
 * Everything here runs on the message thread, which is the thread AVPlayer
 * wants to be driven from. No worker.
 */
class AvfVideoBackend final : public VideoBackend {
public:
  ~AvfVideoBackend() override {
    using namespace avf;
    if (!api().ok) return;
    Pool pool;
    if (player_) send<void>(player_, "pause");
    if (item_ && output_) send<void>(item_, "removeOutput:", output_);
    nsRelease(output_);
    nsRelease(player_);
    nsRelease(item_);
  }

  bool open(const std::string& path, std::string& error) override {
    using namespace avf;
    const Api& a = api();
    if (!a.ok) {
      error = "AVFoundation is not available";
      return false;
    }
    Pool pool;
    id url = send<id>(classNamed("NSURL"), "fileURLWithPath:", nsString(path));
    id asset = send<id>(classNamed("AVURLAsset"), "URLAssetWithURL:options:", url, (id) nil);
    id tracks = asset ? send<id>(asset, "tracksWithMediaType:", a.mediaTypeVideo) : nil;
    if (nsCount(tracks) == 0) {
      error = "the file has no video stream, or could not be opened";
      return false;
    }
    const PixelSize size = send<PixelSize>(nsAt(tracks, 0), "naturalSize");
    width_ = (int) (size.width + 0.5);
    height_ = (int) (size.height + 0.5);
    // Synchronous: the asset loads its duration on first ask, blocking. Fine
    // for a local file, and it means duration() is right from the start.
    duration_ = a.CMTimeGetSeconds(sendForStruct<MediaTime>(asset, "duration"));
    if (!std::isfinite(duration_) || duration_ < 0.0) duration_ = 0.0;

    item_ = nsRetain(send<id>(classNamed("AVPlayerItem"), "playerItemWithAsset:", asset));
    output_ = send<id>(send<id>(classNamed("AVPlayerItemVideoOutput"), "alloc"),
                       "initWithPixelBufferAttributes:", bgraSettings());
    if (!item_ || !output_) {
      error = "the video could not be prepared for playback";
      return false;
    }
    send<void>(item_, "addOutput:", output_);
    player_ = nsRetain(send<id>(classNamed("AVPlayer"), "playerWithPlayerItem:", item_));
    if (!player_) {
      error = "the video could not be prepared for playback";
      return false;
    }
    send<void>(player_, "setActionAtItemEnd:", (long) 1); // AVPlayerActionAtItemEndPause
    send<void>(player_, "setMuted:", (BOOL) (audio_ ? 0 : 1));
    send<void>(player_, "setVolume:", volume_);
    return true;
  }

  int width() const override { return width_; }
  int height() const override { return height_; }
  double duration() const override { return duration_; }

  void play() override {
    using namespace avf;
    if (!player_) return;
    if (duration_ > 0.0 && position() >= duration_ - 1e-3) setPosition(0.0);
    send<void>(player_, "setRate:", (float) speed_);
    playing_ = true;
    endedReported_ = false;
  }

  void pause() override {
    if (!player_) return;
    avf::send<void>(player_, "pause");
    playing_ = false;
  }

  bool isPlaying() const override { return playing_; }

  void setPosition(double seconds) override {
    using namespace avf;
    if (!player_) return;
    const double t = seconds > 0.0 ? (duration_ > 0.0 && seconds > duration_ ? duration_ : seconds) : 0.0;
    send<void>(player_, "seekToTime:toleranceBefore:toleranceAfter:", api().CMTimeMakeWithSeconds(t, 600),
               timeZero(), timeZero());
    endedReported_ = false;
  }

  double position() const override {
    using namespace avf;
    if (!player_) return 0.0;
    const double t = api().CMTimeGetSeconds(sendForStruct<MediaTime>(player_, "currentTime"));
    return std::isfinite(t) && t > 0.0 ? t : 0.0;
  }

  void setSpeed(double speed) override {
    speed_ = speed;
    if (player_ && playing_) avf::send<void>(player_, "setRate:", (float) speed_);
  }

  void setVolume(float volume) override {
    volume_ = volume;
    if (player_) avf::send<void>(player_, "setVolume:", volume_);
  }

  void setAudioEnabled(bool enabled) override {
    audio_ = enabled;
    if (player_) avf::send<void>(player_, "setMuted:", (BOOL) (audio_ ? 0 : 1));
  }

  bool takeFrame(Bitmap& into) override {
    using namespace avf;
    const Api& a = api();
    if (!output_) return false;
    Pool pool;
    const MediaTime now = sendForStruct<MediaTime>(output_, "itemTimeForHostTime:", a.CACurrentMediaTime());
    if (!send<BOOL>(output_, "hasNewPixelBufferForItemTime:", now)) return false;
    void* pixels = send<void*>(output_, "copyPixelBufferForItemTime:itemTimeForDisplay:", now, (MediaTime*) nullptr);
    if (!pixels) return false;
    const bool ok = copyPixelBuffer(pixels, into);
    a.CFRelease(pixels);
    if (ok) ++stats_.framesShown;
    return ok;
  }

  bool takeEnded() override {
    if (!player_ || !playing_ || endedReported_) return false;
    const float rate = avf::send<float>(player_, "rate");
    if (rate == 0.0f && duration_ > 0.0 && position() >= duration_ - 0.05) {
      playing_ = false;
      endedReported_ = true;
      return true;
    }
    return false;
  }

  std::string takeError() override { return {}; }
  VideoPlaybackStats stats() const override { return stats_; }

private:
  id player_ = nil, item_ = nil, output_ = nil;
  int width_ = 0, height_ = 0;
  double duration_ = 0.0, speed_ = 1.0;
  float volume_ = 1.0f;
  bool audio_ = false, playing_ = false, endedReported_ = false;
  VideoPlaybackStats stats_;
};

/**
 * macOS capture: an AVCaptureSession with a video data output whose delegate
 * is a class registered at runtime, called on a serial dispatch queue.
 *
 * The delegate holds a pointer to a small Link, not to this object. Teardown
 * stops the session, detaches the delegate, then runs an empty function
 * synchronously ON the delegate's queue: when that returns, any callback that
 * was already queued has finished, and nothing can reach the Link again.
 */
class AvfCameraBackend final : public CameraBackend {
public:
  struct Link {
    CameraFrameSink frames;
    Bitmap scratch;
    std::atomic<int> width{0}, height{0};
  };

  ~AvfCameraBackend() override { teardown(); }

  bool open(int index, int minWidth, int minHeight, int maxWidth, int maxHeight, std::string& error,
            CameraFrameSink frames, CameraErrorSink errors) override {
    using namespace avf;
    (void) minWidth;
    (void) minHeight;
    (void) errors;
    const Api& a = api();
    if (!a.canCapture) {
      error = "AVFoundation capture is not available";
      return false;
    }
    Pool pool;
    // Asked BEFORE anything touches the device. A process whose Info.plist
    // has no NSCameraUsageDescription is TERMINATED by the system the moment
    // it starts a capture session -- and in a plugin that process is the
    // host. Refusing here is the difference between an error message and
    // taking the user's session down.
    const long status = send<long>(classNamed("AVCaptureDevice"), "authorizationStatusForMediaType:", a.mediaTypeVideo);
    if (status == 1 || status == 2) {
      error = "camera access is denied for this application (System Settings > Privacy & Security > Camera)";
      return false;
    }
    if (status == 0) {
      id bundle = send<id>(classNamed("NSBundle"), "mainBundle");
      id usage = bundle ? send<id>(bundle, "objectForInfoDictionaryKey:", nsString("NSCameraUsageDescription")) : nil;
      if (!usage) {
        error = "the host application does not declare camera use (NSCameraUsageDescription); "
                "macOS would terminate it if a plugin opened the camera";
        return false;
      }
    }
    id devices = cameraList();
    if (index < 0 || (unsigned long) index >= nsCount(devices)) {
      char message[96];
      std::snprintf(message, sizeof(message), "there is no camera %d (%lu found)", index, nsCount(devices));
      error = message;
      return false;
    }
    id device = nsAt(devices, (unsigned long) index);
    name_ = fromNSString(send<id>(device, "localizedName"));
    id input = send<id>(classNamed("AVCaptureDeviceInput"), "deviceInputWithDevice:error:", device, (id*) nullptr);
    if (!input) {
      error = "the camera would not open (in use, or access refused)";
      return false;
    }
    session_ = send<id>(send<id>(classNamed("AVCaptureSession"), "alloc"), "init");
    send<void>(session_, "beginConfiguration");
    // A capture session takes a preset, not a size: the largest of the three
    // that fits inside the bounds asked for.
    const bool fits640 = (maxWidth <= 0 || maxWidth >= 640) && (maxHeight <= 0 || maxHeight >= 480);
    const bool fits1280 = (maxWidth <= 0 || maxWidth >= 1280) && (maxHeight <= 0 || maxHeight >= 720);
    id preset = a.presetHigh;
    if (!fits1280 && fits640 && a.preset640) preset = a.preset640;
    else if (fits1280 && maxWidth > 0 && maxWidth < 1920 && a.preset1280) preset = a.preset1280;
    if (preset && send<BOOL>(session_, "canSetSessionPreset:", preset)) send<void>(session_, "setSessionPreset:", preset);
    if (!send<BOOL>(session_, "canAddInput:", input)) {
      error = "the camera cannot be added to a capture session";
      teardown();
      return false;
    }
    send<void>(session_, "addInput:", input);

    output_ = send<id>(send<id>(classNamed("AVCaptureVideoDataOutput"), "alloc"), "init");
    send<void>(output_, "setVideoSettings:", bgraSettings());
    send<void>(output_, "setAlwaysDiscardsLateVideoFrames:", (BOOL) 1);
    link_ = new Link();
    link_->frames = std::move(frames);
    delegate_ = send<id>(send<id>(delegateClass(), "alloc"), "init");
    object_setInstanceVariable(delegate_, "sonoreLink", link_);
    queue_ = a.dispatch_queue_create("sonore.camera", nullptr);
    send<void>(output_, "setSampleBufferDelegate:queue:", delegate_, (id) queue_);
    if (!send<BOOL>(session_, "canAddOutput:", output_)) {
      error = "the camera's frames cannot be delivered as BGRA";
      teardown();
      return false;
    }
    send<void>(session_, "addOutput:", output_);
    send<void>(session_, "commitConfiguration");
    send<void>(session_, "startRunning");
    if (!send<BOOL>(session_, "isRunning")) {
      error = "the capture session would not start";
      teardown();
      return false;
    }
    return true;
  }

  std::string name() const override { return name_; }
  int width() const override { return link_ ? link_->width.load() : 0; }
  int height() const override { return link_ ? link_->height.load() : 0; }

  static id cameraList() {
    using namespace avf;
    const Api& a = api();
    id cls = classNamed("AVCaptureDeviceDiscoverySession");
    if (cls && a.deviceTypeBuiltIn) {
      const id types[2] = {a.deviceTypeBuiltIn, a.deviceTypeExternal};
      id array = send<id>(classNamed("NSArray"), "arrayWithObjects:count:", types,
                          (unsigned long) (a.deviceTypeExternal ? 2 : 1));
      id session = send<id>(cls, "discoverySessionWithDeviceTypes:mediaType:position:", array, a.mediaTypeVideo,
                            (long) 0 /* unspecified */);
      if (session) return send<id>(session, "devices");
    }
    return send<id>(classNamed("AVCaptureDevice"), "devicesWithMediaType:", a.mediaTypeVideo);
  }

private:
  static void drain(void*) {}

  void teardown() {
    using namespace avf;
    const Api& a = api();
    if (!a.canCapture) return;
    Pool pool;
    if (session_ && send<BOOL>(session_, "isRunning")) send<void>(session_, "stopRunning");
    if (output_) send<void>(output_, "setSampleBufferDelegate:queue:", (id) nil, (id) nil);
    if (queue_) a.dispatch_sync_f(queue_, nullptr, &AvfCameraBackend::drain);
    nsRelease(delegate_);
    nsRelease(output_);
    nsRelease(session_);
    if (queue_) a.dispatch_release(queue_);
    delegate_ = output_ = session_ = nil;
    queue_ = nullptr;
    delete link_;
    link_ = nullptr;
  }

  /** captureOutput:didOutputSampleBuffer:fromConnection:, on the queue. */
  static void didOutput(id self, SEL, id, void* sampleBuffer, id) {
    using namespace avf;
    void* raw = nullptr;
    object_getInstanceVariable(self, "sonoreLink", &raw);
    Link* link = static_cast<Link*>(raw);
    if (!link || !sampleBuffer) return;
    if (!copyPixelBuffer(api().CMSampleBufferGetImageBuffer(sampleBuffer), link->scratch)) return;
    link->width.store(link->scratch.width());
    link->height.store(link->scratch.height());
    if (link->frames) link->frames(link->scratch);
  }

  static id delegateClass() {
    static id registered = [] {
      Class existing = objc_getClass("SonoreCameraDelegate");
      if (existing) return (id) existing;
      Class c = objc_allocateClassPair(objc_getClass("NSObject"), "SonoreCameraDelegate", 0);
      class_addIvar(c, "sonoreLink", sizeof(void*), (uint8_t) 3 /* log2 of 8 */, "^v");
      class_addMethod(c, sel_registerName("captureOutput:didOutputSampleBuffer:fromConnection:"),
                      reinterpret_cast<IMP>(&AvfCameraBackend::didOutput), "v@:@^v@");
      objc_registerClassPair(c);
      return (id) c;
    }();
    return registered;
  }

  id session_ = nil, output_ = nil, delegate_ = nil;
  void* queue_ = nullptr;
  Link* link_ = nullptr;
  std::string name_;
};

inline std::unique_ptr<VideoBackend> makeVideoBackend() {
  return std::unique_ptr<VideoBackend>(new AvfVideoBackend());
}
inline std::unique_ptr<CameraBackend> makeCameraBackend() {
  return std::unique_ptr<CameraBackend>(new AvfCameraBackend());
}

inline std::vector<std::string> listCameras() {
  using namespace avf;
  std::vector<std::string> names;
  if (!api().ok) return names;
  Pool pool;
  id devices = AvfCameraBackend::cameraList();
  for (unsigned long i = 0; i < nsCount(devices); ++i)
    names.push_back(fromNSString(send<id>(nsAt(devices, i), "localizedName")));
  return names;
}

} // namespace detail

#else
namespace detail {
inline std::unique_ptr<VideoBackend> makeVideoBackend() { return nullptr; }
inline std::unique_ptr<CameraBackend> makeCameraBackend() { return nullptr; }
inline std::vector<std::string> listCameras() { return {}; }
} // namespace detail
#endif

// ═════════════════════════════════════════════════════════════════════════════
// VideoComponent
// ═════════════════════════════════════════════════════════════════════════════

/**
 * A video file, played inside an editor. See the header comment for the
 * threading rule (call update() from the editor's tick) and the soundtrack
 * (muted until setAudioEnabled(true)).
 */
class VideoComponent : public Component {
public:
  /** What stats() returns: frames decoded, shown and dropped, audio frames
   *  queued, whether a soundtrack device is open, and why the soundtrack is
   *  not playing when it was asked to. */
  using Stats = detail::VideoPlaybackStats;

  /** Nothing loaded, showing the background colour (black). Muted, at speed
   *  1 and volume 1. */
  VideoComponent() = default;
  /** Stops playback and closes the file; no callback fires. Message thread. */
  ~VideoComponent() override { backend_.reset(); }

  /** Whether this platform can play video at all. */
  static bool isAvailable() {
#if defined(_WIN32)
    return true;
#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
    return detail::avf::api().ok;
#else
    return false;
#endif
  }

  /**
   * Open a file, paused at its first frame. False, with the reason in
   * `error` and through onErrorOccurred, if it cannot be played -- not a
   * file, no decoder, or no video playback on this platform.
   */
  bool load(const std::string& path, std::string* error = nullptr) {
    closeVideo();
    std::unique_ptr<detail::VideoBackend> backend = detail::makeVideoBackend();
    std::string why;
    if (!backend) why = "video playback is not available on this platform";
    else if (!backend->open(path, why)) backend.reset();
    if (!backend) {
      if (error) *error = why;
      if (onErrorOccurred) onErrorOccurred(why);
      return false;
    }
    backend->setSpeed(speed_);
    backend->setVolume(volume_);
    backend->setAudioEnabled(audioEnabled_);
    backend_ = std::move(backend);
    file_ = path;
    update();
    repaint();
    return true;
  }

  /** Stop and close the file, and show only the background. No callback
   *  fires. Safe when nothing is open. */
  void closeVideo() {
    if (!backend_ && frame_.isEmpty()) return;
    backend_.reset();
    file_.clear();
    frame_ = Bitmap();
    repaint();
  }

  /** Whether a file is loaded, playing or not. */
  bool isVideoOpen() const { return backend_ != nullptr; }
  /** The path given to the load that succeeded; empty when nothing is open. */
  const std::string& currentVideoFile() const { return file_; }

  /** Start or resume at playSpeed(); at the end of the file, from the
   *  start. Fires onPlaybackStarted. Nothing happens when nothing is open or
   *  it is already playing. */
  void play() {
    if (!backend_ || backend_->isPlaying()) return;
    backend_->play();
    if (onPlaybackStarted) onPlaybackStarted();
  }

  /** Stop where it is. */
  void pause() {
    if (!backend_ || !backend_->isPlaying()) return;
    backend_->pause();
    if (onPlaybackStopped) onPlaybackStopped();
  }

  /** Stop, and go back to the start. */
  void stop() {
    if (!backend_) return;
    const bool was = backend_->isPlaying();
    backend_->pause();
    backend_->setPosition(0.0);
    if (was && onPlaybackStopped) onPlaybackStopped();
  }

  /** Playing, as opposed to paused, stopped, ended or not loaded. False
   *  again at the end of the file -- on macOS, from the update() that
   *  reports the end. */
  bool isPlaying() const { return backend_ && backend_->isPlaying(); }

  /** Seconds, clamped to the file. The frame there arrives on a following
   *  update(), paused or not. */
  void setPlayPosition(double seconds) {
    if (backend_) backend_->setPosition(std::isfinite(seconds) ? seconds : 0.0);
  }
  /** Where playback is now, in seconds from the start of the file; 0 when
   *  nothing is open. */
  double playPosition() const { return backend_ ? backend_->position() : 0.0; }
  /** The file's length in seconds; 0 when nothing is open or the container
   *  does not say. */
  double videoDuration() const { return backend_ ? backend_->duration() : 0.0; }
  /** The picture's width in pixels; 0 when nothing is open. */
  int videoWidth() const { return backend_ ? backend_->width() : 0; }
  /** The picture's height in pixels; 0 when nothing is open. */
  int videoHeight() const { return backend_ ? backend_->height() : 0; }

  /** 1 is normal; clamped to 1/16 .. 16. */
  void setPlaySpeed(double speed) {
    speed_ = std::isfinite(speed) ? (std::min)(16.0, (std::max)(1.0 / 16.0, speed)) : 1.0;
    if (backend_) backend_->setSpeed(speed_);
  }
  /** The speed set, 1 by default; kept across load(). */
  double playSpeed() const { return speed_; }

  /** 0 to 1. Applies once the soundtrack is enabled. */
  void setAudioVolume(float volume) {
    volume_ = std::isfinite(volume) ? (std::min)(1.0f, (std::max)(0.0f, volume)) : 1.0f;
    if (backend_) backend_->setVolume(volume_);
  }
  /** The volume set, 0 to 1 (linear gain), 1 by default; kept across load(). */
  float audioVolume() const { return volume_; }

  /** Off by default; see "The soundtrack" in the header. */
  void setAudioEnabled(bool enabled) {
    audioEnabled_ = enabled;
    if (backend_) backend_->setAudioEnabled(enabled);
  }
  /** Whether the soundtrack was asked for; stats().audioError says when it
   *  was asked for and is not playing. */
  bool isAudioEnabled() const { return audioEnabled_; }

  /**
   * The playback side's one way in: call from the message thread, every tick.
   * Takes the newest frame (repainting if there is one), and fires
   * onPlaybackEnded / onPlaybackStopped / onErrorOccurred for whatever
   * happened since the last call. True if a new frame arrived.
   */
  bool update() {
    if (!backend_) return false;
    const bool fresh = backend_->takeFrame(frame_);
    if (fresh) repaint();
    const std::string error = backend_->takeError();
    const bool ended = backend_->takeEnded();
    // Callbacks LAST, after everything was read: one of them may well close
    // the video, and nothing after it may touch the backend.
    if (!error.empty() && onErrorOccurred) onErrorOccurred(error);
    if (ended) {
      if (onPlaybackEnded) onPlaybackEnded();
      if (onPlaybackStopped) onPlaybackStopped();
    }
    return fresh;
  }

  /** The frame on screen: empty before the first one arrives. */
  const Bitmap& currentFrame() const { return frame_; }

  /** The player's counters since load(); all zero when nothing is open. */
  Stats stats() const { return backend_ ? backend_->stats() : Stats(); }

  /** The colour around the frame, and in place of one when there is none.
   *  Black by default. */
  void setBackgroundColour(Colour c) {
    background_ = c;
    repaint();
  }

  /** The frame, as large as fits, centred, at its own aspect ratio; the
   *  background colour around it. */
  void paint(Graphics& g) override {
    g.setColour(background_);
    g.fillRect(localBounds());
    if (frame_.isEmpty()) return;
    const Rect area = localBounds().withAspect((float) frame_.width() / (float) frame_.height());
    if (!area.isEmpty()) g.drawImage(frame_, area);
  }

  /** Inside play(), when it started playback. Message thread. */
  std::function<void()> onPlaybackStarted;
  /** After pause(), stop() while playing, and reaching the end. */
  std::function<void()> onPlaybackStopped;
  /** From the update() that sees playback reach the end of the file,
   *  before onPlaybackStopped. Message thread; it may close the video. */
  std::function<void()> onPlaybackEnded;
  /** With the reason, in words for a log: from a load() that failed, or
   *  from the update() after the playback side hit an error. Message
   *  thread. */
  std::function<void(const std::string&)> onErrorOccurred;

private:
  std::unique_ptr<detail::VideoBackend> backend_;
  std::string file_;
  Bitmap frame_;
  Colour background_{0, 0, 0};
  double speed_ = 1.0;
  float volume_ = 1.0f;
  bool audioEnabled_ = false;
};

// ═════════════════════════════════════════════════════════════════════════════
// CameraDevice
// ═════════════════════════════════════════════════════════════════════════════

/**
 * A camera, delivering frames as Bitmaps to listeners -- ON THE CAPTURE
 * THREAD (see the header comment). The bitmap is valid for the duration of
 * the call; a listener that keeps one copies it.
 *
 * removeListener() waits for a delivery in progress, so once it returns the
 * listener is not called again and may be destroyed.
 */
class CameraDevice {
public:
  /** Receives each frame, on the CAPTURE thread, not the message thread: copy
   *  the bitmap to keep it, and hand anything for the UI across. Not owned;
   *  once removeListener() has returned it is not called again and may be
   *  destroyed. */
  class Listener {
  public:
    /** Removing it from the device first is the caller's: the device does
     *  not hold it alive. */
    virtual ~Listener() = default;
    /** One frame, on the capture thread, under the device's lock: `image`
     *  is valid only during the call. Must not delete the device. */
    virtual void imageReceived(const Bitmap& image) = 0;
  };

  /** The cameras, by name, in the order openDevice() indexes them. Empty
   *  where there are none -- or no camera support on this platform. */
  static std::vector<std::string> availableDevices() { return detail::listCameras(); }

  /**
   * Open camera `index`, preferring the largest native format inside the
   * bounds (0 for no bound). Null, with the reason in `error`, if there is no
   * such camera or it will not open.
   */
  static std::unique_ptr<CameraDevice> openDevice(int index, int minWidth = 0, int minHeight = 0,
                                                  int maxWidth = 1920, int maxHeight = 1080,
                                                  std::string* error = nullptr) {
    std::unique_ptr<CameraDevice> device(new CameraDevice());
    std::unique_ptr<detail::CameraBackend> backend = detail::makeCameraBackend();
    std::string why;
    CameraDevice* target = device.get();
    if (!backend) {
      why = "cameras are not available on this platform";
    } else if (!backend->open(
                   index, minWidth, minHeight, maxWidth, maxHeight, why,
                   [target](const Bitmap& frame) { target->deliver(frame); },
                   [target](const std::string& message) { target->deliverError(message); })) {
      backend.reset();
    }
    if (!backend) {
      if (error) *error = why;
      return nullptr;
    }
    device->backend_ = std::move(backend);
    return device;
  }

  /** Stops capture and waits for the capture thread, so no listener is
   *  called once it returns. Not from inside a listener. */
  ~CameraDevice() { backend_.reset(); }

  /** Not copyable: it is one open camera. */
  CameraDevice(const CameraDevice&) = delete;
  /** Not copyable. */
  CameraDevice& operator=(const CameraDevice&) = delete;

  /** The camera's name, as availableDevices() lists it. */
  std::string name() const { return backend_ ? backend_->name() : std::string(); }
  /** The size frames arrive at; 0 before the first on some platforms. */
  int width() const { return backend_ ? backend_->width() : 0; }
  /** The frame height in pixels; 0 before the first frame on some
   *  platforms. */
  int height() const { return backend_ ? backend_->height() : 0; }

  /** Start sending frames to `listener`, which is not owned; adding it twice
   *  adds it once. Any thread; null is ignored. */
  void addListener(Listener* listener) {
    if (!listener) return;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (std::find(listeners_.begin(), listeners_.end(), listener) == listeners_.end())
      listeners_.push_back(listener);
  }

  /** Stop sending frames to `listener`. Any thread, also from inside a
   *  listener's own call; from another thread it waits for a delivery in
   *  progress, so afterwards the listener may be destroyed. */
  void removeListener(Listener* listener) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener), listeners_.end());
  }

  /** The next frame, once, to `callback` -- on the capture thread. */
  void takeStillPicture(std::function<void(const Bitmap&)> callback) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    still_ = std::move(callback);
  }

  /** Told, on the capture thread, when the camera stops delivering. */
  void setErrorHandler(std::function<void(const std::string&)> handler) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    onError_ = std::move(handler);
  }

  /** The last error the capture side reported, as the error handler was
   *  told it; empty while all is well. Any thread. */
  std::string lastError() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return lastError_;
  }

  /** Frames delivered since the device opened. Any thread, without a lock. */
  uint64_t framesReceived() const { return frames_.load(); }

private:
  CameraDevice() = default;

  void deliver(const Bitmap& frame) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    frames_.fetch_add(1);
    // A copy of the list, each entry re-checked before its call: a listener
    // may remove another (or itself) from inside imageReceived.
    const std::vector<Listener*> current = listeners_;
    for (Listener* l : current)
      if (std::find(listeners_.begin(), listeners_.end(), l) != listeners_.end()) l->imageReceived(frame);
    if (still_) {
      std::function<void(const Bitmap&)> once;
      once.swap(still_);
      once(frame);
    }
  }

  void deliverError(const std::string& message) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    lastError_ = message;
    if (onError_) onError_(message);
  }

  mutable std::recursive_mutex mutex_;
  std::vector<Listener*> listeners_;
  std::function<void(const Bitmap&)> still_;
  std::function<void(const std::string&)> onError_;
  std::string lastError_;
  std::atomic<uint64_t> frames_{0};
  // LAST, so it is destroyed FIRST: the capture thread stops before the
  // listeners and the lock it delivers under go away.
  std::unique_ptr<detail::CameraBackend> backend_;
};

} // namespace gfx
} // namespace sonore
