// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the plugin's native interface.
//
// A generated plugin's face is an HTML document (the same `uihtml` the browser
// preview renders), shown natively in an OS webview and wired to the parameters
// through a `window.sonore` bridge that is API-identical to the preview's. That
// identity is the whole point: the picture the user approved in the studio is
// the picture that opens in their DAW, running the same markup.
//
// This header is platform-INDEPENDENT. It owns:
//   - the parameter/meter state a UI observes,
//   - the JS bridge source injected before the page loads,
//   - the message protocol in both directions,
//   - VU ballistics, computed here so the needle matches the preview's exactly.
// The window itself comes from a backend (webview_win32.h and friends).
//
// Threading: everything here is called from the MAIN thread except
// `pushMeter()`, which the audio thread calls once per block. That crossing is
// the only shared state, and it is a pair of atomics, never a lock, never an
// allocation, on the audio thread.
#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <type_traits>

#include "plugin.h"
#include "presets.h" // the bridge lists preset names, so it needs the full type
#include "state_bag.h" // the bridge shows the bag, so it needs the full type
#include "simd.h"      // measureBlock is on the audio thread, so it vectorises

namespace sonore {

/**
 * One block's peak and RMS, from whatever the plugin just produced.
 *
 * This loop existed four times -- CLAP twice, AU, the standalone -- and LV2
 * was the format that never got a copy, which is why an LV2 plugin's meters
 * had nothing to show. Four copies and one omission is the shape that says
 * the loop belongs somewhere both can reach.
 *
 * [audio-thread]. Reads the FIRST channel: a meter is a level display, not a
 * correlation analyser, and averaging channels would hide a dead side.
 */
struct BlockLevel {
  float peak = 0.0f; ///< the largest absolute sample, linear (1.0 is full scale)
  float rms = 0.0f;  ///< the root mean square over the block, linear
};

// ONE pass over the block, with two of each accumulator. It was two passes --
// simd::peakAbs, then simd::sumSquares -- each a single running max or sum, so
// every step waited for the one before it (three cycles for four samples) and
// the block was walked twice: 171 ns of a 512-frame block, more than the
// output guard and the scope together, on every block of every plugin whether
// an editor was open or not. Two chains of each, interleaved, keep the vector
// unit busy instead of waiting. The peak is the same float exactly -- a
// maximum does not depend on the order it is taken in -- for every block
// without a NaN in it, which the output guard has already silenced. The sum
// of squares is added in a different order, so the RMS can differ from the
// two-pass form in its last bit or so (relative 1e-6 at most, in float),
// which no meter can show.
//
// The 64-bit path, scalar on every backend, gets four independent chains for
// the same reason: one running double sum was a 512-long chain of dependent
// adds, two thirds of what a 64-bit host's whole block cost a plugin that does
// nothing.
/** [audio-thread] Peak and RMS of `frames` samples of ONE channel, float or
 *  double. Zeros for a null pointer or an empty block. No allocation, no lock. */
template <typename Sample>
inline BlockLevel measureBlock(const Sample* samples, size_t frames) {
  BlockLevel out;
  if (!samples || frames == 0) return out;
  size_t i = 0;
  double total = 0.0;
  float peak = 0.0f;
  if constexpr (std::is_same<Sample, float>::value) {
    simd::Vec4f peak0 = simd::Vec4f::zero(), peak1 = peak0, sum0 = peak0, sum1 = peak0;
    for (; i + 8 <= frames; i += 8) {
      const simd::Vec4f a = simd::Vec4f::load(samples + i);
      const simd::Vec4f b = simd::Vec4f::load(samples + i + 4);
      peak0 = (peak0.max)(a.abs());
      peak1 = (peak1.max)(b.abs());
      sum0 += a * a;
      sum1 += b * b;
    }
    if (i + 4 <= frames) {
      const simd::Vec4f a = simd::Vec4f::load(samples + i);
      peak0 = (peak0.max)(a.abs());
      sum0 += a * a;
      i += 4;
    }
    const simd::Vec4f lanes = (peak0.max)(peak1);
    for (int lane = 0; lane < 4; ++lane) {
      const float v = lanes.lane(lane);
      if (v > peak) peak = v;
    }
    total = (double) (sum0 + sum1).sum();
    for (; i < frames; ++i) {
      const float v = samples[i] < 0.0f ? -samples[i] : samples[i];
      if (v > peak) peak = v;
      total += (double) samples[i] * samples[i];
    }
  } else {
#if defined(SONORE_SIMD_SSE)
    // Two doubles a register, two registers a step. The peak is kept as the
    // largest |v| in double and rounded to float once: rounding never puts
    // two values in the other order, so that is the float the per-sample
    // rounding and compare found.
    if constexpr (std::is_same<Sample, double>::value) {
      const __m128d sign = _mm_set1_pd(-0.0);
      __m128d peak0 = _mm_setzero_pd(), peak1 = peak0, sum0 = peak0, sum1 = peak0;
      for (; i + 4 <= frames; i += 4) {
        const __m128d a = _mm_loadu_pd(samples + i);
        const __m128d b = _mm_loadu_pd(samples + i + 2);
        peak0 = _mm_max_pd(peak0, _mm_andnot_pd(sign, a));
        peak1 = _mm_max_pd(peak1, _mm_andnot_pd(sign, b));
        sum0 = _mm_add_pd(sum0, _mm_mul_pd(a, a));
        sum1 = _mm_add_pd(sum1, _mm_mul_pd(b, b));
      }
      double peaks[2], sums[2];
      _mm_storeu_pd(peaks, _mm_max_pd(peak0, peak1));
      _mm_storeu_pd(sums, _mm_add_pd(sum0, sum1));
      peak = (float) (peaks[0] > peaks[1] ? peaks[0] : peaks[1]);
      total = sums[0] + sums[1];
    }
#else
    double sums[4] = {0.0, 0.0, 0.0, 0.0};
    float peaks[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (; i + 4 <= frames; i += 4)
      for (int k = 0; k < 4; ++k) {
        const double v = (double) samples[i + (size_t) k];
        const float a = (float) (v < 0.0 ? -v : v);
        if (a > peaks[k]) peaks[k] = a;
        sums[k] += v * v;
      }
    total = (sums[0] + sums[1]) + (sums[2] + sums[3]);
    for (int k = 0; k < 4; ++k)
      if (peaks[k] > peak) peak = peaks[k];
#endif
    for (; i < frames; ++i) {
      const double v = (double) samples[i];
      const float a = (float) (v < 0.0 ? -v : v);
      if (a > peak) peak = a;
      total += v * v;
    }
  }
  out.peak = peak;
  out.rms = (float) std::sqrt(total / (double) frames);
  return out;
}

/**
 * What the audio thread publishes for the meters, and the UI thread reads.
 *
 * Two halves on two threads. push() is the audio thread's: one block's linear
 * peak and RMS, two relaxed atomic stores, nothing else. tick() and the
 * readers are the UI thread's: tick(dt) runs the ballistics -- a peak that
 * jumps up and decays with a 0.6 s time constant, and a VU needle calibrated
 * to 0 VU at -9 dBFS and settling in 0.35 s -- and level(), db() and vu() read
 * the result. Call tick() on a steady clock (the editors use their ~30 Hz
 * timer) with the seconds since the last call; the readings do not move
 * without it. pushGr() and gr() are the gain reduction a DSP reports, carried
 * by the same needle movement.
 *
 * The numbers match the browser preview's meter (the same calibration and the
 * same critically damped movement), so a needle looks the same in the studio
 * and in a DAW.
 */
class MeterState {
public:
  /** [audio thread] What measureBlock() just returned. The two-argument form
   *  below still exists; this one exists so a caller cannot swap them. */
  void push(const BlockLevel& level) { push(level.peak, level.rms); }

  /** [audio thread] One block's peak and RMS, in linear units. */
  void push(float peak, float rms) {
    peak_.store(peak, std::memory_order_relaxed);
    rms_.store(rms, std::memory_order_relaxed);
  }

  /** [main thread] How much the DSP is turning the signal down, in decibels
   *  (positive = reduction). Read off the DSP by whoever drives the tick,
   *  because nothing measured from the output can tell it apart from a gain
   *  knob. */
  void pushGr(float db) { gr_.store(db, std::memory_order_relaxed); }

  /** [main thread] Advance the ballistics and read the display values.
   *  `dt` is the seconds since the last call. */
  void tick(double dt) {
    // An infinite peak pegs the meter; it must not latch it. inf * decay is
    // inf, so one bad sample held the meter -- and the page's dB readout, at
    // 1.8e308 -- at the top until the editor was reopened. Held at +80 dBFS,
    // an over still reads as one, and is gone seconds after the signal is.
    const float peakIn = peak_.load(std::memory_order_relaxed);
    const float peak = peakIn < kPeakCeiling ? peakIn : kPeakCeiling;
    const float rms = rms_.load(std::memory_order_relaxed);

    // Peak meter: instant attack, exponential decay, the standard behaviour
    // that lets a transient show and then fall back visibly.
    const float decay = (float) std::exp(-dt / 0.6);
    display_ = peak > display_ ? peak : display_ * decay;

    // VU: a CALIBRATED analog needle: +3.0103 dB sine calibration, 0 VU at
    // -9 dBFS (below full scale and tuned to the preview's level, so a hot
    // signal swings PAST 0), riding the MOVEMENT below. The target is held to
    // the face before the needle sees it, and a NaN counts as no signal: the
    // movement solves e = x - target, an infinite target makes that inf - inf,
    // and a NaN in the state is a needle gone for good.
    const float rmsDb = 20.0f * std::log10(rms > 1e-9f ? rms : 1e-9f) + 3.0103f;
    const float targetVu = (rmsDb - kVuReferenceDb - kVuScaleMin) / (kVuScaleMax - kVuScaleMin);
    vuSwing_.step(targetVu > 0.0f ? (targetVu < 1.0f ? targetVu : 1.0f) : 0.0f, dt);
    vuSwing_.hold(0.0f, 1.0f);

    // Gain reduction is that SAME movement switched to another job, which is
    // what it is on the hardware: one needle, one mass, a switch. Passed
    // through untouched it slams -- a compressor moves tens of decibels of
    // reduction inside one repaint -- and a needle that teleports reads as a
    // counter rather than as an instrument.
    const float grIn = gr_.load(std::memory_order_relaxed);
    grSwing_.step(grIn > 0.0f ? (grIn < kGrCeiling ? grIn : kGrCeiling) : 0.0f, dt);
    grSwing_.hold(0.0f, kGrCeiling);
  }

  /** [main thread] tick() with the time that really passed since the last
   *  call, measured here.
   *
   *  The editors called tick(0.033) on every timer tick, and a timer asked
   *  for 33 ms is not one: Win32's fires on the system's 15.6 ms tick, every
   *  47 ms unless something raised the resolution, so the peak fell at 70% of
   *  its speed and the needle settled in half a second -- or at the right
   *  speed, in a host that happened to raise it. An LV2 host's idle() runs at
   *  whatever rate that host likes. The first call has nothing to measure
   *  from and takes one nominal tick; a gap past a second (an editor hidden
   *  and shown) counts as a second, which has already let any peak fall. */
  void tickNow() {
    const auto now = std::chrono::steady_clock::now();
    double dt = 0.033;
    if (clocked_) {
      dt = std::chrono::duration<double>(now - lastTick_).count();
      dt = dt < 0.0 ? 0.0 : (dt > 1.0 ? 1.0 : dt);
    }
    lastTick_ = now;
    clocked_ = true;
    tick(dt);
  }

  /** Peak-equivalent level, 0..1, for `sonore.level`. */
  float level() const { return display_; }
  /** Peak in dBFS, for `sonore.db`. */
  float db() const {
    return 20.0f * std::log10(display_ > 1e-9f ? display_ : 1e-9f);
  }
  /** Calibrated VU needle position, 0..1 across the -20..+3 VU scale, for
   *  `sonore.vu`. */
  float vu() const { return vuSwing_.x; }
  /** Decibels of gain reduction as the NEEDLE reads them, for `sonore.gr`. */
  float gr() const { return grSwing_.x; }

private:
  // -20..+3 VU scale with 0 VU referenced to -9 dBFS. Keep these three in step
  // with PluginCanvas.tsx or the native needle and the preview's disagree.
  static constexpr float kVuReferenceDb = -9.0f;
  static constexpr float kVuScaleMin = -20.0f;
  static constexpr float kVuScaleMax = 3.0f;
  /** +80 dBFS: past any real signal, and back below 0 dBFS in 5.5 s. */
  static constexpr float kPeakCeiling = 1e4f;
  /** Past any reduction a meter prints, and what a runaway is held to. */
  static constexpr float kGrCeiling = 60.0f;

  // THE MOVEMENT. This was a ONE-POLE, whose comment claimed to match the
  // browser preview's needle, which is second order. A one-pole leaves at full
  // speed; a coil starts from rest. Measured on one programme signal, the
  // one-pole threw the needle across 9 of a meter face's 100 frames in a
  // single repaint where the browser moved 5.
  //
  // Critically damped: the error solves e(t) = (A + B*t)*exp(-omega*t), so a
  // step reaches 99% in 0.350 s, NEVER overshoots from rest, and settles
  // EXACTLY on the target -- a meter that has stopped moving is telling the
  // truth.
  static constexpr double kOmega = 18.97;
  struct Swing {
    float x = 0.0f, v = 0.0f;
    void step(float target, double dt) {
      const double e0 = (double) x - target;
      const double decay = std::exp(-kOmega * dt);
      const double b = (double) v + kOmega * e0;
      x = (float) (target + (e0 + b * dt) * decay);
      v = (float) ((b - kOmega * (e0 + b * dt)) * decay);
    }
    /** Stop at the end of the scale, as the needle's pins do. */
    void hold(float lo, float hi) {
      if (x < lo) { x = lo; v = 0.0f; }
      else if (x > hi) { x = hi; v = 0.0f; }
    }
  };

  std::atomic<float> peak_{0.0f};
  std::atomic<float> rms_{0.0f};
  std::atomic<float> gr_{0.0f};
  float display_ = 0.0f;
  Swing vuSwing_;
  Swing grSwing_;
  std::chrono::steady_clock::time_point lastTick_{};
  bool clocked_ = false;
};

/** Format a number for a JAVASCRIPT context, immune to the C locale.
 *
 *  Hosts really do call setlocale(): a German DAW makes %g print "0,5", and a
 *  comma inside generated JS is a syntax error that kills the entire bridge.
 *  The fix is one pass over the digits, because only the decimal separator can
 *  legally vary in %g output. */
inline void jsNumber(char* out, size_t capacity, double v) {
  // And always a NUMBER. %g prints inf, -inf and nan, none of which is a JS
  // or JSON literal: a float WAV with one infinite sample made the waveform's
  // JSON unparseable and the page threw. NaN becomes 0; an infinity becomes
  // the largest double, which keeps its sign and its order.
  if (v != v) v = 0.0;
  else if (v > 1.7976931348623157e308) v = 1.7976931348623157e308;
  else if (v < -1.7976931348623157e308) v = -1.7976931348623157e308;
  // As few digits as give the same float back, up to the nine that any float
  // needs. %g's six turned 1234567 into 1.23457e+06 and 12345.68 into
  // 12345.7, so a page that read a value and set it back moved the
  // parameter. Shortest first, so 0.3f still prints 0.3, not 0.300000012.
  // Read back with strtod BEFORE the comma pass: the two follow the same
  // locale, so they agree with each other whatever it is.
  const float target = (float) v;
  for (int digits = 6; digits <= 9; ++digits) {
    std::snprintf(out, capacity, "%.*g", digits, v);
    if ((float) std::strtod(out, nullptr) == target) break;
  }
  for (char* p = out; *p; ++p)
    if (*p == ',') *p = '.';
}

/** Escape a string for an HTML text context. The fallback page interpolates
 *  the plugin's name and vendor into markup; JS escaping there rendered
 *  literal backslash sequences instead of the characters. */
inline std::string escapeHtml(const char* s) {
  std::string out;
  if (!s) return out;
  for (const char* p = s; *p; ++p) {
    switch (*p) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += *p; break;
    }
  }
  return out;
}

/** Escape a string for embedding inside a JS single-quoted literal. */
inline std::string escapeForJs(const char* s) {
  std::string out;
  if (!s) return out;
  for (const char* p = s; *p; ++p) {
    switch (*p) {
      case '\\': out += "\\\\"; break;
      case '\'': out += "\\'"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '<': out += "\\x3c"; break; // never let a payload close our <script>
      default: out += *p; break;
    }
  }
  return out;
}

/** Whether a webview editor may navigate -- its page or any frame in it --
 *  to `uri`. Only to content the page holds itself: about: (the document
 *  every backend loads, and srcdoc frames), data: and blob:. Everything else
 *  is refused: http(s), file:, a file dropped on the view, a link the page
 *  did not mean to follow.
 *
 *  The bridge is injected into whatever document the view shows, so a page
 *  that navigated to a remote site handed that site window.sonore, and the
 *  plugin took its parameter changes as the user's. On WebKit a remote
 *  iframe did not even need the bridge: the message channel is reachable
 *  from every frame. A plugin face is an appliance, not a browser -- a
 *  manual or a shop link belongs in the user's browser, opened by the
 *  plugin, never inside the editor. */
inline bool webviewMayNavigate(const std::string& uri) {
  const size_t colon = uri.find(':');
  if (colon == std::string::npos) return false;
  std::string scheme = uri.substr(0, colon);
  for (char& c : scheme)
    if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
  return scheme == "about" || scheme == "data" || scheme == "blob";
}

/**
 * The `window.sonore` bridge, injected before any page script runs.
 *
 * Deliberately API-identical to the preview's injected bridge: the SAME uihtml
 * must work in both, so a UI authored against the studio preview cannot
 * discover at export time that a method it used doesn't exist here.
 *
 *   sonore.name                  plugin name
 *   sonore.params                [{id,label,min,max,default,unit,step,readOnly,
 *                                  options,curve,group}]
 *                                readOnly: one the plugin reports (a meter);
 *                                set, begin and end ignore it. options: a
 *                                stepped control's positions by name (its
 *                                value where a step has none), null for a
 *                                continuous one; curve: 'log' or 'linear';
 *                                group: the parameter's group or null
 *   sonore.get(i) / set(i, v)    plain-value parameter access
 *   sonore.on(cb)                cb(values, index, value) on any change (host
 *                                or UI); values maps each id to its value
 *   sonore.onLevel(cb)           cb(level, db) ~30 Hz
 *   sonore.onVU(cb)              cb(vu) ~30 Hz, calibrated needle
 *   sonore.onGR(cb)              cb(dB) ~30 Hz, gain reduction in positive
 *                                decibels (0 if the DSP does not declare
 *                                gainReduction())
 *   sonore.noteOn/noteOff        instruments with an on-screen keyboard
 *   'sonore:ready' event         fired once the bridge is populated
 *
 * `values`, when given, holds the plugin's parameters as they are NOW, one
 * for each entry of desc.params, and each `value` starts there; without it
 * each starts at its default. An editor passes them. A page draws its first
 * frame from `value`, usually in its 'sonore:ready' handler, and the push
 * of the current values comes on the editor's next ~30 Hz tick after that:
 * a page given the defaults opened showing them, then moved every control
 * to where it really was up to 33 ms later.
 */
inline std::string bridgeScript(const PluginDescriptor& desc, const float* values = nullptr) {
  std::string js;
  js.reserve(4096);
  js += "(function(){\n";
  // The page's own document only. The script is injected into frames too on
  // WebView2, and the bridge in a frame is a second sender nobody asked for.
  js += "if(window.top!==window)return;\n";
  js += "var post=function(m){try{window.chrome.webview.postMessage(JSON.stringify(m));}"
        "catch(e){try{window.webkit.messageHandlers.sonore.postMessage(JSON.stringify(m));}"
        "catch(e2){}}};\n";
  // Keys the page did not use go to the HOST. A focused webview keeps every
  // keystroke, and WebView2's AcceleratorKeyPressed -- which the Windows
  // backend already forwards -- reports only modifier combinations, keys
  // with no character, and Escape. The spacebar has a character, so it, and
  // every one-letter DAW shortcut, stopped at the page: click the plugin,
  // press play, nothing. Here the rest go on, unless the page took the key
  // (preventDefault), a text field has it, or the page asked for the
  // keyboard (captureKeys, which the native side checks).
  //
  // The decision is the LAST listener of the event, so every one of the
  // page's has had its say. Registered on the window, it was the FIRST of the
  // window's own: this script is injected before the page's, and listeners on
  // one target run in the order they were added. A page that played notes
  // from the computer keyboard in window.addEventListener('keydown') and
  // called preventDefault had every key sent to the host as well -- REAPER's
  // actions, Live's own computer keyboard -- while the same handler on
  // document worked. So the window's capture phase, which runs before any of
  // that, moves the forwarder to the end of the window's bubbling list, and
  // it runs after whatever the page added there.
  //
  // A text field is an <input> that TAKES text. Every <input> was taken for
  // one, sliders and checkboxes too, so once the user had touched an
  // <input type=range> -- every slider of the fallback page, and of many a
  // designed one -- the spacebar stopped at the page until focus moved on.
  js += "var typesText=function(t){if(!t)return false;if(t.isContentEditable)return true;"
        "var n=t.tagName||'';if(n==='TEXTAREA'||n==='SELECT')return true;"
        "return n==='INPUT'&&!/^(range|checkbox|radio|button|submit|reset|image|color|file)$/i"
        ".test(t.type||'');};\n";
  js += "var keyFwd=function(e,down){if(e.defaultPrevented||e.ctrlKey||e.altKey||e.metaKey)return;"
        "if(typesText(e.target))return;var k=e.key||'';if(k.length!==1)return;"
        "var c=e.keyCode||(k===' '?32:k.toUpperCase().charCodeAt(0));"
        "post({type:'key',code:c|0,down:down?1:0});e.preventDefault();};\n";
  js += "var keyLast=function(type,fn){window.addEventListener(type,function(){"
        "window.removeEventListener(type,fn);window.addEventListener(type,fn);},true);"
        "window.addEventListener(type,fn);};\n";
  js += "keyLast('keydown',function(e){keyFwd(e,1);});\n";
  js += "keyLast('keyup',function(e){keyFwd(e,0);});\n";
  // And a text field takes the WHOLE keyboard while it has focus, whether
  // or not the page thought to ask. The character keys above stay with it,
  // but Delete, the arrows, Home, End, Tab and every Ctrl shortcut are the
  // native side's, and it forwards them to the host unless the keyboard was
  // asked for -- which only a page that knew to call captureKeys did. In any
  // other text field Delete deleted the DAW's selection and Ctrl+V pasted
  // nothing; the licence overlay's activation code field is one. So the
  // keyboard is asked for while a text field has focus, or while the page
  // has asked for it itself, and given back when neither is true. Checked
  // after the focus has moved, so going from one field to the next never
  // lets a key out in between; a shadow root is looked into for the field
  // actually focused.
  js += "var keysPage=false,keysSent=-1;\n";
  js += "var keysTell=function(){var a=document.activeElement;"
        "while(a&&a.shadowRoot&&a.shadowRoot.activeElement)a=a.shadowRoot.activeElement;"
        "var on=(keysPage||typesText(a))?1:0;"
        "if(on!==keysSent){keysSent=on;post({type:'keys',capture:on});}};\n";
  js += "var keysSoon=function(){setTimeout(keysTell,0);};\n";
  js += "document.addEventListener('focusin',keysSoon,true);\n";
  js += "document.addEventListener('focusout',keysSoon,true);\n";
  js += "var params=[";
  for (int i = 0; i < desc.numParams; ++i) {
    const ParamInfo& p = desc.params[i];
    // Strings are appended, never snprintf'd into a fixed buffer: a long label
    // truncated MID-JAVASCRIPT is a syntax error that silently kills the whole
    // bridge, the least debuggable failure an interface can have. Only the
    // numbers go through a buffer, and their length is bounded by arithmetic.
    char mn[40], mx[40], df[40], sk[40], st[16];
    jsNumber(mn, sizeof(mn), (double) p.minValue);
    jsNumber(mx, sizeof(mx), (double) p.maxValue);
    jsNumber(df, sizeof(df), (double) p.defaultValue);
    jsNumber(sk, sizeof(sk), (double) p.skew);
    std::snprintf(st, sizeof(st), "%d", p.stepCount);
    if (i) js += ",";
    js += "{id:'" + escapeForJs(p.id) + "',label:'" + escapeForJs(p.label) + "',unit:'" +
          escapeForJs(p.unit) + "',min:";
    js += mn;
    js += ",max:";
    js += mx;
    js += ",default:";
    js += df;
    js += ",step:";
    js += st;
    js += ",skew:";
    js += sk;
    // A parameter the plugin REPORTS. The bridges drop a page's write to one
    // (uiMayWrite), and with nothing on the page saying so, a slider dragged
    // over it showed the dragged value until the plugin next changed it.
    js += p.readOnly ? ",readOnly:true" : ",readOnly:false";
    js += ",value:";
    if (values) {
      char now[40];
      jsNumber(now, sizeof(now), (double) values[i]);
      js += now;
    } else {
      js += df;
    }
    // The step names, so sonore.format() shows "Bandpass" where the host's
    // own readout does. Null where a step has no name.
    js += ",names:[";
    for (int n = 0; n < p.stepCount; ++n) {
      const char* name = paramValueName(p, n);
      if (n) js += ",";
      if (name) js += "'" + escapeForJs(name) + "'";
      else js += "null";
    }
    js += "]";
    // THE STUDIO'S SHAPE: { ..., options, curve, group }. A generated
    // interface is written, previewed and approved against the studio's
    // bridge, whose params carry `options` (a stepped control's positions, by
    // name), `curve` and `group`, and this bridge had none of the three. An
    // 1176 built its ratio buttons with `ratioParam.options.forEach(...)`,
    // which threw here on `undefined` and took the four knobs after it down
    // with it: the exported plugin showed a meter over an empty panel, while
    // the preview was complete.
    //
    // `options` is an array for EVERY stepped control, never holding a null:
    // a step with no name is labelled by its value, so a page that loops over
    // the options cannot meet an undefined one. Null for a continuous control.
    if (p.stepCount > 0) {
      js += ",options:[";
      for (int n = 0; n < p.stepCount; ++n) {
        const char* name = paramValueName(p, n);
        if (n) js += ",";
        if (name) {
          js += "'" + escapeForJs(name) + "'";
        } else {
          char sv[40];
          jsNumber(sv, sizeof(sv), (double) stepValueOf(p, n));
          js += "'";
          js += sv;
          js += "'";
        }
      }
      js += "]";
    } else {
      js += ",options:null";
    }
    // The studio turns a 'log' knob logarithmically. Here that is any skewed
    // parameter: a log control is written with the skew that puts its centre
    // at the geometric mean, which is where a log knob puts it.
    js += (p.skew != 1.0f && p.skew > 0.0f) ? ",curve:'log'" : ",curve:'linear'";
    js += ",group:";
    js += (p.group && *p.group) ? "'" + escapeForJs(p.group) + "'" : std::string("null");
    js += "}";
  }
  js += "];\n";
  // Preset NAMES only: the values live on the C++ side and are applied there,
  // so a page cannot drift from what the plugin would actually load.
  js += "var presets=[";
  for (int i = 0; i < desc.numPresets; ++i) {
    js += (i ? ",'" : "'");
    js += escapeForJs(desc.presets[i].name);
    js += "'";
  }
  js += "];\n";
  // ── How a page names a parameter ─────────────────────────────────────
  //
  // By index, by id, or by label -- the same three the studio's bridge
  // (PluginCanvas) and the render gate accept, normalised the same way.
  //
  // This bridge took an INDEX and nothing else, and that difference shipped
  // broken plugins. A generated interface iterates `sonore.params` and asks
  // for values back by name (`sonore.get(p.label)`), because that is what
  // the studio accepts and the studio is where the interface is written,
  // previewed, screenshotted and approved. `params["Time"]` is undefined, so
  // every readout showed 0 and every knob sat below its own minimum -- in
  // the DAW and in the standalone, never in the preview. Found by opening a
  // downloaded plugin, which is the only place it was visible.
  //
  // An explicit id wins over a positional guess: a plugin whose first
  // parameter is called "0" means that one, not index zero.
  //
  // NORMALISING IS A FALLBACK, NOT THE LOOKUP. `norm` strips everything that is
  // not a letter or a digit, and that includes the DECIMAL POINT -- so "1.25k"
  // and "12.5k" both come out "125k", as do "31.5"/"315" and "1.6k"/"16k".
  // Those are ordinary labels: they are what a graphic EQ calls its bands.
  //
  // The exact label was never a key. Only `p.id`, `norm(p.id)` and
  // `norm(p.label)` were -- so a page asking `sonore.get("1.25k")` missed every
  // exact key, fell through to `byKey["125k"]`, and got whichever colliding
  // parameter `reindex` happened to write LAST. Two faders, one parameter,
  // moving together; and the collision is silent, because both names resolve
  // to something.
  //
  // So the table is built in three passes, most specific first, and a later
  // pass never overwrites an earlier one: exact id, then exact label, then the
  // normalised aliases. An ambiguous normalised key now resolves to the first
  // parameter that claimed it rather than the last, which is at least
  // deterministic -- and neither of the two needs it, because both are reachable
  // by their exact labels.
  //
  // It is fixed HERE rather than by telling generated pages to ask by `p.id`,
  // because every plugin already shipped asks by label and cannot be
  // regenerated.
  //
  // AND THE TABLE HAS NO PROTOTYPE. `{}` inherits `constructor`, `toString` and
  // the rest, so `byKey["constructor"]` answered with a function and `P` handed
  // it back as a parameter. A control legitimately labelled "Constructor" is
  // unlikely; one labelled "Value Of" is not.
  js += "function norm(s){return String(s).toLowerCase().replace(/[^a-z0-9]/g,'');}\n";
  js += "var byKey=Object.create(null);\n";
  js += "function reindex(){byKey=Object.create(null);var i,p,a,b;"
        "for(i=0;i<params.length;i++)params[i].__i=i;"
        "for(i=0;i<params.length;i++)byKey[params[i].id]=params[i];"
        "for(i=0;i<params.length;i++){p=params[i];"
        "if(byKey[p.label]===undefined)byKey[p.label]=p;}"
        "for(i=0;i<params.length;i++){p=params[i];a=norm(p.id);b=norm(p.label);"
        "if(byKey[a]===undefined)byKey[a]=p;"
        "if(byKey[b]===undefined)byKey[b]=p;}}\n";
  js += "reindex();\n";
  js += "function P(r){if(r&&r.__i!==undefined&&r.id!==undefined)return r;"
        "if(typeof r==='number')return params[r]||null;"
        "var p=byKey[r];if(p)return p;p=byKey[norm(r)];if(p)return p;"
        "var n=Number(r);return (n===n&&params[n])?params[n]:null;}\n";
  // The id -> value map the studio hands its subscribers. Built fresh on
  // every broadcast rather than kept, because a page is free to keep the
  // object it was given and a shared one would change under it.
  js += "function vals(){var o={};for(var i=0;i<params.length;i++)o[params[i].id]=params[i].value;return o;}\n";
  // NOT `fire`: this script already has one, two hundred lines down, and it
  // dispatches `sonore:ready`. Overwriting it made every `set` re-run the
  // page's whole load handler, which called `set` again -- twenty-two thousand
  // writes before the LV2 bridge test caught it. One namespace, one long
  // generated script, and no compiler to tell you.
  js += "function fireChange(p){var v=vals();for(var i=0;i<changeCbs.length;i++){"
        "try{changeCbs[i](v,p?p.__i:-1,p?p.value:0);}catch(e){}}}\n";
  js += "var stateSeen=false;\n";
  js += "var changeCbs=[],levelCbs=[],vuCbs=[],grCbs=[],noteCbs=[],fileCbs=[],stateCbs=[],"
        "deviceCbs=[],cpuCbs=[];\n";
  // Every member is typed for a page in TypeScript by sonore.d.ts, beside this
  // header, and scripts/bridge-types.mjs fails the build when the two differ:
  // add a member here, type it there.
  js += "var api={\n";
  js += "  name:'" + escapeForJs(desc.name) + "',\n";
  js += "  vendor:'" + escapeForJs(desc.vendor) + "',\n";
  js += "  params:params,\n";
  js += "  get:function(r){var p=P(r);return p?p.value:undefined;},\n";
  // Only the licence overlay calls this, and only in a build that has one.
  // Defined unconditionally so the bridge script stays one static string --
  // a page that never activates simply never calls it.
  js += "  __activate:function(t){post({type:'activate',text:String(t||'')});},\n";
  // A stepped control lands ON a step, as plugin.h's clampToRange puts it in
  // every wrapper. The page kept what it was given: a switch dragged to 0.4
  // was Off in the plugin and 0.4 in sonore.get(), the knob stayed between
  // two positions, and a page indexing p.names by the value found nothing.
  js += "  set:function(r,v){var p=P(r);if(!p||p.readOnly)return;"
        "v=Math.max(p.min,Math.min(p.max,Number(v)||0));"
        "if(p.step>0){var sz=p.step>1?(p.max-p.min)/(p.step-1):0;"
        "var k=sz>0?Math.floor((v-p.min)/sz+0.5):0;k=Math.max(0,Math.min(p.step-1,k));"
        "v=k<=0?p.min:(k>=p.step-1?p.max:p.min+k*sz);}"
        "p.value=v;post({type:'set',index:p.__i,value:v});fireChange(p);},\n";
  js += "  begin:function(r){var p=P(r);if(p&&!p.readOnly)post({type:'begin',index:p.__i});},\n";
  js += "  end:function(r){var p=P(r);if(p&&!p.readOnly)post({type:'end',index:p.__i});},\n";
  js += "  on:function(cb){if(typeof cb==='function')changeCbs.push(cb);},\n";
  js += "  onLevel:function(cb){if(typeof cb==='function')levelCbs.push(cb);},\n";
  js += "  onVU:function(cb){if(typeof cb==='function')vuCbs.push(cb);},\n";
  // Gain reduction, in positive decibels, from a DSP that declares
  // gainReduction(). 0 from one that does not, which is also what a
  // compressor reads when it is not compressing, so a meter bound to this is
  // right either way.
  js += "  onGR:function(cb){if(typeof cb==='function')grCbs.push(cb);},\n";
  js += "  noteOn:function(n,v){post({type:'noteOn',note:n|0,velocity:v===undefined?100:v|0});},\n";
  js += "  noteOff:function(n){post({type:'noteOff',note:n|0});},\n";
  // Right-click on a control -> the HOST's menu, not ours. MIDI learn, "remove
  // automation", "show in mixer" are things only the host can offer, and a
  // plugin that swallows the right-click hides them all. Coordinates are
  // relative to the plugin window, which is what both formats expect for an
  // embedded UI. A page that wants its own menu simply does not call this.
  // A NATIVE file browser. The page cannot do this itself: <input type=file>
  // is disabled by default in an embedded WebView2, behaves differently under
  // WebKitGTK, and hands back a sandbox handle rather than a path a DSP can
  // open. The answer arrives asynchronously through onFile, because the
  // dialog is modal and the page is not running while it is up.
  // Whether the PAGE wants the keyboard. False is the default and the right
  // one: a webview with focus swallows every keystroke, so an open editor
  // eats the spacebar -- and the spacebar is transport in every DAW there is.
  // The user clicks the plugin, presses play, nothing happens, and nothing
  // looks broken.
  //
  // A page with a text field turns this on while the field has focus and off
  // again when it loses it. Anything else should never call it.
  js += "  captureKeys:function(on){keysPage=!!on;keysTell();},\n";
  js += "  chooseFile:function(purpose,mode){post({type:'file',"
        "purpose:String(purpose||'file'),mode:String(mode||'open')});},\n";
  js += "  onFile:function(cb){if(typeof cb==='function')fileCbs.push(cb);},\n";
  // A control the bridge cannot name is index -1, which every wrapper refuses:
  // no menu. It was 0, so a misspelt label opened the menu of the FIRST
  // parameter, and MIDI learn or "clear automation" landed on a control the
  // user never clicked.
  js += "  contextMenu:function(r,x,y){var p=P(r);post({type:'menu',index:p?p.__i:-1,"
        "x:Math.round(x)|0,y:Math.round(y)|0});},\n";
  // The audio devices this program can play through, and which one it is on.
  //
  // Empty in a PLUGIN, always, and that is the whole design: a plugin does not
  // own the audio device, the host does, and a plugin drawing a device picker
  // would be offering the user a control that cannot work. The standalone
  // fills this in; nothing else ever does.
  // Every field the standalone pushes, empty: a page written against the
  // standalone reads `sonore.audioDevices.inputs.length`, and in a plugin --
  // which is never pushed a list -- `inputs` was undefined and the page
  // threw on load. scripts/bridge-types.mjs holds the two to one shape.
  js += "  audioDevices:{system:[],asio:[],midi:[],inputs:[],currentInput:-1,testSignal:false,"
        "current:{backend:'',index:-1,name:''},currentMidi:-1,sampleRate:0,bufferFrames:0},\n";
  js += "  onAudioDevices:function(cb){if(typeof cb==='function')deviceCbs.push(cb);},\n";
  // `which` is an index for a system device and a NAME for an ASIO driver --
  // matching how each one is actually identified rather than forcing both
  // through a common shape that fits neither.
  js += "  selectMidiDevice:function(i){post({type:'device',backend:'midi',index:i|0});},\n";
  // The standalone's built-in test tone. A plugin ignores it -- the host feeds
  // a plugin -- and the standalone reads it as one more device switch.
  js += "  setTestSignal:function(on){post({type:'device',backend:'test',index:on?1:0});},\n";
  js += "  selectAudioDevice:function(backend,which){"
        "if(backend!=='system'&&backend!=='asio'&&backend!=='input')return;"
        "post(backend==='asio'?{type:'device',backend:'asio',name:String(which||'')}"
        ":{type:'device',backend:backend,index:which|0});},\n";
  js += "  presets:presets,\n";
  js += "  loadPreset:function(i){if(i>=0&&i<presets.length)post({type:'preset',index:i|0});},\n";
  // Display formatting, so a UI shows the same text the host does in its own
  // generic panel. MIRRORS formatParamValue() in plugin.h: the two are one
  // rule in two languages and must be changed together.
  // A stepped control: the nearest step by plugin.h's rule (min + k * size),
  // its name if it has one, else the whole number with its unit when every
  // step is whole, else the same decimals as any other value.
  js += "  format:function(r,v){var p=P(r);if(!p)return'';"
        "if(v===undefined)v=p.value;"
        "v=Math.max(p.min,Math.min(p.max,Number(v)||0));"
        "if(p.step>0){var sz=p.step>1?(p.max-p.min)/(p.step-1):0;"
        "var k=sz>0?Math.floor((v-p.min)/sz+0.5):0;k=Math.max(0,Math.min(p.step-1,k));"
        "if(p.names&&p.names[k])return p.names[k];"
        "v=k>=p.step-1?p.max:p.min+k*sz;"
        "var whole=Math.floor(p.min)===p.min&&(p.step<2||(sz>=1&&Math.floor(sz)===sz));"
        "if(whole)return String(Math.round(v))+(p.unit?' '+p.unit:'');}"
        // Within a millionth of the range of zero IS zero, as plugin.h has it:
        // a VST3 host hands 0 dB back as a single-precision position, and the
        // page printed -0.0000014 dB as "-0.000 dB" beside a host showing
        // "0.0 dB" -- the very case that rule was written for, left out here.
        "if(Math.abs(v)<=(p.max-p.min)*1e-6)v=0;"
        "if(p.unit==='Hz'&&v>=1000)return (v/1000).toFixed(2)+' kHz';"
        "var span=p.max-p.min,d=span>=100?0:(span>=10?1:2);"
        "var a=Math.abs(v),e=(a===0||a>=10)?0:(a>=1?1:(a>=0.1?2:3));if(e>d)d=e;"
        "return v.toFixed(d)+(p.unit?' '+p.unit:'');},\n";
  js += "  level:0, db:-100, vu:0, gr:0,\n";
  // How much of the audio deadline this plugin is using, 0..1 and beyond.
  // A generated DSP has nobody who profiled it; the page can show what it
  // costs, and an interface that never asks pays nothing.
  js += "  cpu:0, cpuPeak:0, xruns:0,\n";
  js += "  onCpu:function(cb){if(typeof cb==='function')cpuCbs.push(cb);},\n";
  // Which keys are sounding, low 64 and high 64. A page asks either way it
  // finds natural: a predicate for drawing one key, or a callback for
  // redrawing a whole keyboard when anything changes.
  js += "  notes:[0,0,0,0],\n";
  // The plugin's OWN state, as strings. Parameters travel on their own
  // channel; this is everything else a DSP keeps -- the sample it loaded, the
  // preset it is on, whatever it puts in its state bag.
  //
  // A UI needs it or it lies on reopen: the sampler still has the file, and
  // an editor that cannot see the bag draws an empty slot next to a plugin
  // that is playing a kit.
  js += "  state:{},\n";
  // The SAME curve the host's automation lane uses, so a knob drawn by the
  // page and a value drawn by the DAW agree about where something sits. A
  // page that did its own power law would look right until somebody opened
  // the generic editor beside it.
  //
  // And a stepped control is spread EVENLY whatever its skew, as plugin.h's
  // toNormalisedValue has it: a host offers its positions at k / (steps - 1).
  // The page bent them by the skew all the same, so a stepped knob with one
  // pointed somewhere else than the host's lane for every position between
  // the ends -- 5 of a 1..20 ratio in 20 steps at skew 0.5 sat at 0.46
  // against the host's 0.21.
  js += "  toNormalised:function(r,v){var p=P(r);if(!p)return 0;"
        "if(v===undefined)v=p.value;var s=p.max-p.min;if(s<=0)return 0;"
        "var n=Math.max(0,Math.min(1,(v-p.min)/s));"
        "return (!(p.step>0)&&p.skew&&p.skew!==1)?Math.pow(n,p.skew):n;},\n";
  js += "  fromNormalised:function(r,t){var p=P(r);if(!p)return 0;"
        "var n=Math.max(0,Math.min(1,Number(t)||0));"
        "if(!(p.step>0)&&p.skew&&p.skew!==1)n=Math.pow(n,1/p.skew);"
        "return p.min+n*(p.max-p.min);},\n";
  // A late subscriber is REPLAYED the state that already arrived. The bag is
  // pushed only when it is dirty and the flag clears on the push, so a page
  // that registers its handler after that tick used to miss the only one
  // there will ever be -- and then draw an empty slot beside a sampler that
  // is holding a file. It cost the Windows CI its GUI bridge test, where the
  // page subscribed a few milliseconds later than it does on a dev box.
  // Only a state that really arrived is replayed, never the empty default.
  js += "  onState:function(cb){if(typeof cb!=='function')return;stateCbs.push(cb);"
        "if(stateSeen){try{cb(api.state);}catch(e){}}},\n";
  // Shifts, not division: JS bitwise operators work on 32-bit signed values,
  // so bit 31 of a word comes back negative and `>>` would smear the sign
  // across everything above it. `>>>` is the unsigned one, and it is the
  // whole reason the words are 32 bits wide rather than 64.
  js += "  isNoteDown:function(n){n=n|0;if(n<0||n>127)return false;"
        "return ((api.notes[n>>>5]>>>(n&31))&1)===1;},\n";
  js += "  onNotes:function(cb){if(typeof cb==='function')noteCbs.push(cb);},\n";
  // The skin API in the SHAPE the studio's bridge gives it, even though this
  // build has no filmstrips to serve. A page written against the studio does
  // `sonore.skins.length`, `sonore.skin(name)` and spreads the style object
  // `skinFrame` returns; against `{}` and `null` those are a TypeError, and a
  // page that throws on load has NO interface -- strictly worse than the plain
  // one it would otherwise draw. An empty array, a null lookup and an empty
  // style object say "no skins here" in a language the page survives.
  js += "  accent:'#6e8bff', assets:{}, skins:[], skin:function(){return null;},\n";
  js += "  skinFrame:function(){return {};}\n";
  js += "};\n";
  // Host -> page. One entry point, so a backend only has to eval a call.
  // The host moved something: the page hears it the same way it hears its
  // own edits, and with the same payload -- the map first, because that is
  // what a studio-written page reads, then the index and value the older
  // native shape passed, so a page written either way keeps working.
  js += "api.__update=function(i,v){var p=params[i];if(!p)return;p.value=v;fireChange(p);};\n";
  // Sent only when it CHANGES, which for a keyboard is a few times a second
  // rather than thirty -- and a page that redraws 128 keys on every tick is a
  // page that makes a DAW feel slow.
  js += "api.__notes=function(a,b,c,d){api.notes=[a,b,c,d];"
        "noteCbs.forEach(function(cb){try{cb(api.notes);}catch(e){}});};\n";
  // An EMPTY path means cancelled, and every page must handle it: a user who
  // changes their mind is the common case, not an error.
  // Replaced wholesale rather than merged: the bag IS the state, so a key
  // that has stopped being in it has stopped being true, and merging would
  // leave a filename on screen for a sample that was unloaded.
  js += "api.__state=function(next){api.state=next||{};stateSeen=true;"
        "stateCbs.forEach(function(cb){try{cb(api.state);}catch(e){}});};\n";
  js += "api.__file=function(purpose,path){"
        "fileCbs.forEach(function(cb){try{cb(purpose,path);}catch(e){}});};\n";
  js += "api.__devices=function(next){api.audioDevices=next||api.audioDevices;"
        "deviceCbs.forEach(function(cb){try{cb(api.audioDevices);}catch(e){}});};\n";
  js += "api.__cpu=function(load,peak,xruns){api.cpu=load;api.cpuPeak=peak;"
        "api.xruns=xruns;cpuCbs.forEach(function(cb){try{cb(load,peak,xruns);}catch(e){}});};\n";
  // The fourth argument arrived later than the other three; a host build that
  // still calls it with three leaves gr at its last value rather than NaN.
  js += "api.__meter=function(level,db,vu,gr){api.level=level;api.db=db;api.vu=vu;"
        "if(typeof gr==='number')api.gr=gr;"
        "levelCbs.forEach(function(cb){try{cb(level,db);}catch(e){}});"
        "vuCbs.forEach(function(cb){try{cb(vu);}catch(e){}});"
        "grCbs.forEach(function(cb){try{cb(api.gr);}catch(e){}});};\n";
  js += "window.sonore=api;\n";
  // Asked for before the page is told it is ready, so a listener registered
  // in the ready handler is already in place when the answer arrives.
  // ── The window is not a stage ────────────────────────────────────────
  //
  // A generated faceplate is written for the STUDIO, where it sits on a dark
  // preview pane: `body{padding:24px}` for a mat around it, and
  // `width:min(1120px,96vw)` so it breathes on a narrow pane. Both are right
  // there and wrong here, where the page IS the window -- they came out as a
  // black border on all four sides of a plugin whose faceplate should reach
  // the frame. And the vw cap cannot be sized around: 96vw leaves 4% of
  // whatever window you give it, so a smaller window is a smaller gap and
  // never no gap.
  //
  // So the outer page is flattened and the panel is released from its cap.
  // The panel is the biggest top-level box; the cap is only lifted when it
  // is ALREADY within 12% of the window, which is what a vw-capped panel
  // looks like and what a deliberately narrow 700px panel in a wide window
  // does not -- that one is a design, and designs are left alone.
  //
  // Overflow is hidden because no host shows scrollbars in a plugin editor:
  // if a page ends up a few pixels over, clipping the edge of a mat is the
  // lesser wrong.
  js += "(function(){function fit(){try{"
        "var s=document.createElement('style');"
        "s.textContent='html,body{margin:0!important;padding:0!important;'"
        "+'overflow:hidden!important;width:100%!important;height:100%!important}';"
        "(document.head||document.documentElement).appendChild(s);"
        "var w=document.documentElement.clientWidth,w0=w,best=null,ba=0;"
        "var ch=document.body?document.body.children:[];"
        "for(var i=0;i<ch.length;i++){var r=ch[i].getBoundingClientRect();"
        "var a=r.width*r.height;if(a>ba){ba=a;best=ch[i];}}"
        "if(best){var bw=best.getBoundingClientRect().width;"
        "if(bw>=w*0.88&&bw<=w*1.02){"
        "best.style.setProperty('box-sizing','border-box','important');"
        // Capped at the size the window OPENED with, which is the size the
        // faceplate was measured to want. Released entirely, `width:100%` let
        // a user who dragged the window wider stretch the panel with it --
        // knobs drifting apart, a title lost in its own header. Filling the
        // window is the point at its own size; past that it should sit still.
        "best.style.setProperty('max-width',w0+'px','important');"
        "best.style.setProperty('width','100%','important');"
        "best.style.setProperty('margin','0','important');}}"
        "}catch(e){}}"
        "if(document.readyState==='loading')"
        "document.addEventListener('DOMContentLoaded',fit);else fit();})();\n";
  // 'ready' first: the host re-sends everything it has pushed so far, because
  // a push made while the page was still loading went to the blank document
  // the navigation replaced.
  js += "var fire=function(){post({type:'ready'});post({type:'devices'});"
        "window.dispatchEvent(new Event('sonore:ready'));};\n";
  js += "if(document.readyState==='loading')"
        "document.addEventListener('DOMContentLoaded',fire);else setTimeout(fire,0);\n";
  js += "})();\n";
  return js;
}

/**
 * The UI -> AUDIO channel: a single-producer/single-consumer ring.
 *
 * The main thread writes what the user did to the interface; the audio thread
 * drains it at the top of process(). Lock-free by construction: a mutex here
 * would put the UI thread's scheduling in the audio callback's critical path,
 * which is the classic way a plugin makes a DAW crackle.
 *
 * Overflow DROPS the newest event rather than blocking or growing. 256 slots is
 * dozens of frames of continuous knob dragging; if a host ever stalls
 * processing for that long, a stale gesture is worth less than a glitch.
 *
 * But not every event: parameter edits stop short of the last kReserved
 * slots, which only gestures and notes may take. Those come in pairs, and
 * the second half is the one that arrives after a long drag: a key held
 * while a knob was turned on a host that was not draining -- a track whose
 * processing it suspended -- lost its note-off to the knob's edits and
 * sounded for ever once the host drained, and a gesture lost its end and
 * stayed open. A refused edit loses only the host's report of it: every
 * wrapper writes the parameter on the main thread as well as queueing it.
 */
class UiEventQueue {
public:
  /** One thing the user did, copied by value into the ring. A parameter
   *  event carries the parameter index and its plain (not normalised) value;
   *  a note carries the MIDI note number and the MIDI velocity, 1..127. */
  struct Event {
    /** What the user did. */
    enum class Kind : uint8_t {
      ParamSet,     ///< parameter `index` set to the plain `value`
      GestureBegin, ///< the user took hold of parameter `index`
      GestureEnd,   ///< the user let go of parameter `index`
      NoteOn,       ///< note `index` pressed with velocity `value`
      NoteOff       ///< note `index` released
    };
    Kind kind = Kind::ParamSet; ///< which of the above; decides what index and value mean
    int32_t index = 0; ///< param index, or MIDI note 0..127
    float value = 0.0f; ///< plain param value, or velocity 1..127
  };

  /** [main thread] */
  bool push(const Event& e) {
    const size_t w = write_.load(std::memory_order_relaxed);
    const size_t next = (w + 1) % kCapacity;
    const size_t used = (w + kCapacity - read_.load(std::memory_order_acquire)) % kCapacity;
    const size_t room = e.kind == Event::Kind::ParamSet ? kCapacity - 1 - kReserved : kCapacity - 1;
    if (used >= room) return false; // full, for this kind
    const uint64_t serial = pushed_.load(std::memory_order_relaxed);
    slots_[w].event = e;
    slots_[w].serial = serial;
    pushed_.store(serial + 1, std::memory_order_relaxed);
    write_.store(next, std::memory_order_release);
    return true;
  }

  /** [audio thread] The next event, skipping parameter edits that
   *  supersedeParamEdits() said are out of date. At most one pass over the
   *  ring, never a wait. */
  bool pop(Event* out) {
    for (;;) {
      const size_t r = read_.load(std::memory_order_relaxed);
      if (r == write_.load(std::memory_order_acquire)) return false; // empty
      const Slot slot = slots_[r];
      read_.store((r + 1) % kCapacity, std::memory_order_release);
      if (slot.event.kind == Event::Kind::ParamSet &&
          slot.serial < staleBefore_.load(std::memory_order_acquire))
        continue;
      *out = slot.event;
      return true;
    }
  }

  /**
   * [main thread] Every parameter edit queued so far is older than the
   * parameters now: a state or a preset is about to write all of them.
   *
   * An edit is written into the parameter on the main thread AND queued, and
   * the audio side applies the queued value -- and reports it to the host as
   * a live edit -- when it drains: the next block, or the flush a stopped
   * host makes. A load in between was undone by that drain for every knob
   * touched before it: a preset arrived with one control left where the user
   * had it, marked modified at once, and the host recorded the old value as
   * a fresh edit. Gestures and notes still go through, so the host's gesture
   * pairs stay whole.
   */
  void supersedeParamEdits() {
    staleBefore_.store(pushed_.load(std::memory_order_relaxed), std::memory_order_release);
  }

private:
  /** An event and its place in the order of pushes, which is what
   *  supersedeParamEdits() is measured against. */
  struct Slot {
    Event event;         ///< the event as pushed
    uint64_t serial = 0; ///< how many pushes came before this one
  };
  static constexpr size_t kCapacity = 256;
  /** Slots only gestures and notes may fill: see the class comment. */
  static constexpr size_t kReserved = 64;
  Slot slots_[kCapacity]{};
  std::atomic<size_t> write_{0};
  std::atomic<size_t> read_{0};
  /** Pushes so far. Written only by the thread that pushes; atomic because
   *  a VST3 host may change the program -- a whole-set write, which
   *  supersedes -- from a thread of its own. */
  std::atomic<uint64_t> pushed_{0};
  std::atomic<uint64_t> staleBefore_{0};
};

/**
 * Which keys are sounding, so a UI can draw a keyboard that MOVES.
 *
 * An on-screen keyboard that lights only under the mouse is half a keyboard:
 * the notes the DAW is playing are the ones a user most wants to see, and
 * without this the plugin's own face is the one place in the session that
 * cannot show them.
 *
 * Counted, not flagged. Two note-ons on one key -- two MPE fingers, or a
 * sustained chord retriggered -- are two notes, and a single note-off must
 * not put the key out while the other is still held. The counts are touched
 * only by the audio thread; what crosses to the UI is the pair of masks, and
 * they are atomic because the editor reads them on its own clock and a torn
 * 64-bit read would light a key nobody pressed.
 */
class NoteState {
public:
  /** [audio-thread] One more note on MIDI key `key`, 0..127; any other key is
   *  ignored. No allocation, no lock. */
  void noteOn(int key) {
    if (key < 0 || key > 127) return;
    if (count_[key] < 255) ++count_[key];
    publish(key);
  }
  /** [audio-thread] One note fewer on `key`; the key goes dark when its last
   *  note ends. A note-off with none sounding is ignored. */
  void noteOff(int key) {
    if (key < 0 || key > 127) return;
    if (count_[key] > 0) --count_[key];
    publish(key);
  }
  /** Panic, and also what a host means by deactivating: nothing is held
   *  because nothing is running. */
  void allOff() {
    for (int i = 0; i < 128; ++i) count_[i] = 0;
    low_.store(0, std::memory_order_relaxed);
    high_.store(0, std::memory_order_relaxed);
  }

  /** [any thread] Whether at least one note is sounding on `key`; false for a
   *  key outside 0..127. */
  bool isDown(int key) const {
    if (key < 0 || key > 127) return false;
    const uint64_t word = (key < 64 ? low_ : high_).load(std::memory_order_relaxed);
    return (word >> (key & 63)) & 1u;
  }
  /** [any thread] Keys 0..63 as a mask: bit k set while key k sounds. */
  uint64_t low() const { return low_.load(std::memory_order_relaxed); }
  /** [any thread] Keys 64..127 as a mask: bit k set while key 64 + k sounds. */
  uint64_t high() const { return high_.load(std::memory_order_relaxed); }

  /**
   * One quarter of the keyboard: word 0 is keys 0-31, word 3 is 96-127.
   *
   * THIRTY-TWO bits, not sixty-four, and that is not tidiness. A JavaScript
   * number is a double, so it holds integers exactly only up to 2^53 -- a
   * 64-bit mask with a high key set is rounded on arrival, and keys 53 to 63
   * would light or fail to light at random. Every value here is below 2^32,
   * which a double represents exactly.
   */
  uint32_t word(int index) const {
    if (index < 0 || index > 3) return 0;
    const uint64_t half = (index < 2 ? low() : high());
    return (uint32_t) ((half >> ((index & 1) ? 32 : 0)) & 0xFFFFFFFFull);
  }

private:
  void publish(int key) {
    std::atomic<uint64_t>& word = key < 64 ? low_ : high_;
    const uint64_t bit = 1ull << (key & 63);
    uint64_t now = word.load(std::memory_order_relaxed);
    now = count_[key] ? (now | bit) : (now & ~bit);
    word.store(now, std::memory_order_relaxed);
  }

  unsigned char count_[128] = {};
  std::atomic<uint64_t> low_{0};
  std::atomic<uint64_t> high_{0};
};

/**
 * Note the keys going down and coming up, for whoever is drawing a keyboard.
 *
 * Read off the MidiBuffer rather than any format's own event list, which is
 * the point: every format funnels its notes into that buffer -- CLAP note
 * events, CLAP raw MIDI, VST3's separate event list, LV2 atoms, the
 * standalone's live input -- so counting them here counts all of them once,
 * instead of five decoders each remembering to keep a tally.
 *
 * [audio-thread], and cheap: one pass over events already in hand.
 */
inline void trackNotes(NoteState& notes, const MidiBuffer& midi) {
  for (const auto* it = midi.begin(); it != midi.end(); ++it) {
    const MidiMessage& m = it->getMessage();
    if (m.isNoteOn()) notes.noteOn(m.getNoteNumber());
    else if (m.isNoteOff()) notes.noteOff(m.getNoteNumber());
    else if (m.isController() &&
             (m.getControllerNumber() == 120 || m.getControllerNumber() == 123))
      notes.allOff();
  }
}

/**
 * A state bag as a JavaScript object literal.
 *
 * Every value is a STRING, which is what the bag stores: its own setters
 * write numbers as text so a session survives moving between machines of
 * different byte order. A page that wants a number calls Number() on it,
 * which is one honest conversion rather than a guess made here about which
 * keys are numeric.
 *
 * Binary entries are LEFT OUT, not mangled. setBytes() takes a wavetable or
 * an impulse response, and handing that to a page as if it were a string
 * produces a screenful of rubbish and a broken script.
 */
inline std::string bagToJson(const StateBag& bag) {
  std::string json = "{";
  bool first = true;
  for (size_t i = 0; i < bag.size(); ++i) {
    // The key too: it is the plugin's own string, but a page that receives
    // invalid UTF-8 in a script receives a script that may not parse.
    const std::string& key = bag.keyAt(i);
    if (!bag.valueIsText(i) || !StateBag::isUtf8Text((const uint8_t*) key.data(), key.size()))
      continue;
    if (!first) json += ",";
    first = false;
    json += "'";
    json += escapeForJs(bag.keyAt(i).c_str());
    json += "':'";
    json += escapeForJs(bag.valueAt(i).c_str());
    json += "'";
  }
  json += "}";
  return json;
}

/**
 * The call that tells a page which file was chosen.
 *
 * Built here rather than in each wrapper because the ESCAPING is the whole
 * risk and it was written three times. A Windows path is full of backslashes
 * and a backslash in a JavaScript string is an escape: an unescaped
 * "C:\\Samples\\new.wav" contains a newline, and one ending in a separator
 * swallows the closing quote and takes the rest of the call with it. Three
 * copies meant a test could prove one of them right.
 *
 * An EMPTY path is a real answer and is sent: the user cancelled, and a page
 * that is never told cannot put its own "loading" state back.
 */
inline std::string fileAnswerScript(const std::string& purpose, const std::string& path) {
  std::string js = "window.sonore.__file(\"";
  js += escapeForJs(purpose.c_str());
  js += "\",\"";
  js += escapeForJs(path.c_str());
  js += "\");";
  return js;
}

/** A message from the page, already parsed. */
struct BridgeMessage {
  /** Which message, and so which fields below it fills. */
  enum class Kind {
    None,         ///< not a message this parser knows, or a malformed one: do nothing
    Set,          ///< set parameter `index` to `value`, a plain value
    GestureBegin, ///< the page took hold of parameter `index`
    GestureEnd,   ///< the page let go of parameter `index`
    NoteOn,       ///< play `note` at `velocity` (100 when the page sent none)
    NoteOff,      ///< release `note`
    LoadPreset,   ///< load factory preset `index`, 0-based
    ContextMenu,  ///< the host's menu for parameter `index`, at `x`, `y` (0, 0 when not sent)
    ChooseFile,   ///< show a file dialog for `purpose`, in `mode`
    CaptureKeys,  ///< `value` 1 to keep the keyboard for the page, 0 to give it back
    SelectDevice, ///< use device `deviceName` or `index` (-1 when not sent) of `backend`
    ListDevices,  ///< the page asks, once, for the audio device lists
    Activate,     ///< an activation code in `text`, at most 4096 bytes
    Ready,        ///< the bridge is up in the page's real document
    /** A key the page did not use, for the host: index is the virtual-key
     *  code, value 1 for down and 0 for up. */
    Key
  };
  Kind kind = Kind::None; ///< None unless the message parsed completely
  int index = 0;          ///< parameter, preset, key code or device index, as `kind` says
  double value = 0.0;     ///< the plain value for Set; 1 or 0 for CaptureKeys and Key
  int note = 0;           ///< MIDI note for NoteOn and NoteOff, as the page sent it
  int velocity = 100;     ///< MIDI velocity for NoteOn, as the page sent it
  /** Where the click was, in the plugin window's own pixels. */
  int x = 0, y = 0; ///< x is across, y down, both from the window's top left
  /** What the page called this request, echoed back with the answer. A plugin
   *  with a sample slot AND an impulse response slot needs to know which one
   *  the user was filling in. */
  std::string purpose;
  /** "open", "save" or "folder". Anything else is treated as "open", because
   *  a typo should show a file browser rather than nothing. */
  std::string mode;
  /** Which audio backend a device belongs to: "system" or "asio". Its own
   *  field rather than a reused one -- `purpose` and `mode` mean something to
   *  the file dialog, and a struct where a field means two things depending on
   *  the message is a struct that gets read wrongly once. */
  std::string backend;
  /** Free text the page sent up. Today that is one thing: the activation
   *  code a user pasted into the licence overlay. Its own field because
   *  `purpose` and `mode` already mean something to the file dialog, and a
   *  struct whose field means two things is a struct that gets read wrongly
   *  exactly once. */
  std::string text;
  /** The device, by name. ASIO drivers are chosen this way because the
   *  registry order is not stable; system devices carry an index in `index`
   *  and set this only for the log. */
  std::string deviceName;
};

/** Minimal JSON field readers. The bridge is the ONLY writer of these messages
 *  and its shape is fixed, so a full parser would be weight without value,
 *  but everything is still range-checked by the caller before it reaches the
 *  audio state, because a compromised page is still a page. */
inline bool jsonField(const std::string& src, const char* key, double* out) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t at = src.find(needle);
  if (at == std::string::npos) return false;
  const char* start = src.c_str() + at + needle.size();
  char* end = nullptr;
  const double v = std::strtod(start, &end);
  if (end == start) return false;
  *out = v;
  return true;
}

/** Four hex digits at `at`, for a \\u escape. */
inline bool jsonHex4(const std::string& src, size_t at, unsigned* out) {
  if (at + 4 > src.size()) return false;
  unsigned v = 0;
  for (size_t i = at; i < at + 4; ++i) {
    const char c = src[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned) (c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned) (c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned) (c - 'A' + 10);
    else return false;
  }
  *out = v;
  return true;
}

/** Append code point `cp` to `out` as UTF-8, one to four bytes. Not checked:
 *  the caller passes a scalar value (jsonString has already turned a
 *  surrogate into U+FFFD). */
inline void appendUtf8(std::string& out, unsigned cp) {
  if (cp < 0x80) {
    out += (char) cp;
  } else if (cp < 0x800) {
    out += (char) (0xC0 | (cp >> 6));
    out += (char) (0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char) (0xE0 | (cp >> 12));
    out += (char) (0x80 | ((cp >> 6) & 0x3F));
    out += (char) (0x80 | (cp & 0x3F));
  } else {
    out += (char) (0xF0 | (cp >> 18));
    out += (char) (0x80 | ((cp >> 12) & 0x3F));
    out += (char) (0x80 | ((cp >> 6) & 0x3F));
    out += (char) (0x80 | (cp & 0x3F));
  }
}

/** One string field, unescaped. The page builds every message with
 *  JSON.stringify, which escapes a quote, a backslash and the control
 *  characters -- and this read up to the first quote, escaped or not: a
 *  purpose of `kick "A"` arrived as `kick \`, and a device name with a
 *  backslash in it never matched its device. \\uXXXX, surrogate pairs
 *  included, comes out as UTF-8; a lone surrogate as U+FFFD. A malformed
 *  escape fails the field: a message doing nothing, never something else.
 *
 *  So does a NUL, escaped or not. No field holds one, and every C string the
 *  value becomes ends there: the LV2 interface hands its plugin
 *  "purpose\0path", so a purpose of "sample\u0000C:\\any.wav" had the plugin
 *  load the PAGE's path in place of the file the user picked. */
inline bool jsonString(const std::string& src, const char* key, std::string* out) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const size_t at = src.find(needle);
  if (at == std::string::npos) return false;
  std::string value;
  for (size_t i = at + needle.size(); i < src.size(); ++i) {
    const char c = src[i];
    if (c == '"') {
      *out = std::move(value);
      return true;
    }
    if (c == '\0') return false;
    if (c != '\\') {
      value += c;
      continue;
    }
    if (++i >= src.size()) return false;
    switch (src[i]) {
      case '"': value += '"'; break;
      case '\\': value += '\\'; break;
      case '/': value += '/'; break;
      case 'b': value += '\b'; break;
      case 'f': value += '\f'; break;
      case 'n': value += '\n'; break;
      case 'r': value += '\r'; break;
      case 't': value += '\t'; break;
      case 'u': {
        unsigned cp = 0, low = 0;
        if (!jsonHex4(src, i + 1, &cp)) return false;
        i += 4;
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 2 < src.size() && src[i + 1] == '\\' &&
            src[i + 2] == 'u' && jsonHex4(src, i + 3, &low) && low >= 0xDC00 && low <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          i += 6;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
          cp = 0xFFFD;
        }
        if (cp == 0) return false;
        appendUtf8(value, cp);
        break;
      }
      default:
        return false;
    }
  }
  return false;
}

/** The message's "type" field, unescaped; false when it has none. */
inline bool jsonType(const std::string& src, std::string* out) {
  return jsonString(src, "type", out);
}

/** Parse one bridge message. Unknown shapes yield Kind::None rather than a
 *  guess: a malformed message must do nothing, never something arbitrary. */
inline BridgeMessage parseBridgeMessage(const std::string& json) {
  BridgeMessage msg;
  std::string type;
  if (!jsonType(json, &type)) return msg;

  double d = 0.0;
  if (type == "set") {
    if (!jsonField(json, "index", &d)) return msg;
    msg.index = (int) d;
    if (!jsonField(json, "value", &d)) return msg;
    msg.value = d;
    msg.kind = BridgeMessage::Kind::Set;
  } else if (type == "begin" || type == "end") {
    if (!jsonField(json, "index", &d)) return msg;
    msg.index = (int) d;
    msg.kind = type == "begin" ? BridgeMessage::Kind::GestureBegin
                               : BridgeMessage::Kind::GestureEnd;
  } else if (type == "preset") {
    if (!jsonField(json, "index", &d)) return msg;
    msg.index = (int) d;
    msg.kind = BridgeMessage::Kind::LoadPreset;
  } else if (type == "keys") {
    double on = 0.0;
    if (!jsonField(json, "capture", &on)) return msg;
    msg.value = on;
    msg.kind = BridgeMessage::Kind::CaptureKeys;
  } else if (type == "key") {
    double down = 0.0;
    if (!jsonField(json, "code", &d) || !jsonField(json, "down", &down)) return msg;
    msg.index = (int) d;
    msg.value = down;
    msg.kind = BridgeMessage::Kind::Key;
  } else if (type == "file") {
    // The purpose is required and the mode is not: a page that just wants a
    // file browser should not have to say so.
    if (!jsonString(json, "purpose", &msg.purpose)) return msg;
    jsonString(json, "mode", &msg.mode);
    msg.kind = BridgeMessage::Kind::ChooseFile;
  } else if (type == "menu") {
    if (!jsonField(json, "index", &d)) return msg;
    msg.index = (int) d;
    // Position is optional: a page that fires this from a keyboard shortcut
    // has no cursor to report, and a menu at the window origin is better than
    // no menu.
    if (jsonField(json, "x", &d)) msg.x = (int) d;
    if (jsonField(json, "y", &d)) msg.y = (int) d;
    msg.kind = BridgeMessage::Kind::ContextMenu;
  } else if (type == "ready") {
    // The bridge is up in a real document. Anything evaluated before this --
    // between the webview existing and the page's document being created --
    // ran in the blank document the navigation then replaced, and is gone.
    msg.kind = BridgeMessage::Kind::Ready;
  } else if (type == "devices") {
    // The page asking, once, when it is ready. A handshake rather than the
    // host pushing on a timer: enumerating audio devices is a COM call on
    // Windows, and doing it every frame to catch a page that may not have
    // loaded yet is paying forever for a race that happens once.
    msg.kind = BridgeMessage::Kind::ListDevices;
  } else if (type == "device") {
    // A backend is required. Everything else is optional, because "the ASIO
    // driver called X" and "system device number 2" are two different shapes
    // of the same request and neither needs the other's field.
    if (!jsonString(json, "backend", &msg.backend)) return msg;
    // "input" and "test" exist for the standalone's chrome; a plugin never
    // receives them because nothing in a plugin's page sends them.
    if (msg.backend != "system" && msg.backend != "asio" && msg.backend != "midi" &&
        msg.backend != "input" && msg.backend != "test")
      return msg;
    jsonString(json, "name", &msg.deviceName);
    msg.index = jsonField(json, "index", &d) ? (int) d : -1;
    msg.kind = BridgeMessage::Kind::SelectDevice;
  } else if (type == "noteOn" || type == "noteOff") {
    if (!jsonField(json, "note", &d)) return msg;
    msg.note = (int) d;
    if (jsonField(json, "velocity", &d)) msg.velocity = (int) d;
    msg.kind = type == "noteOn" ? BridgeMessage::Kind::NoteOn : BridgeMessage::Kind::NoteOff;
  } else if (type == "activate") {
    // An activation code the user pasted. Checked here only for length: what
    // it MEANS is decided by a signature check, which is the one opinion a
    // page cannot influence.
    if (!jsonString(json, "text", &msg.text)) return msg;
    if (msg.text.size() > 4096) return msg;
    msg.kind = BridgeMessage::Kind::Activate;
  }
  return msg;
}

/** The fallback face, shown when a plugin ships no `uihtml`. Deliberately plain
 *  and honest rather than clever: real generated plugins always carry their own
 *  interface, and a decorative placeholder would disguise a build that lost it. */
inline std::string fallbackHtml(const PluginDescriptor& desc) {
  std::string html;
  html += "<!doctype html><html><head><meta charset='utf-8'><style>";
  html += "body{margin:0;background:#0d1014;color:#dce5ee;font:13px system-ui,sans-serif;"
          "display:flex;flex-direction:column;height:100vh}";
  html += "header{padding:14px 18px;border-bottom:1px solid #1e2733}";
  html += "h1{margin:0;font-size:15px;letter-spacing:.02em}";
  html += "small{color:#7d8b9c}";
  html += ".rows{padding:10px 18px;overflow:auto;flex:1}";
  html += ".row{display:flex;align-items:center;gap:12px;padding:7px 0}";
  html += ".row label{flex:0 0 120px;color:#9fb0c3}";
  html += ".row input{flex:1}";
  html += ".row output{flex:0 0 90px;text-align:right;font-variant-numeric:tabular-nums}";
  html += "footer{padding:8px 18px 12px;border-top:1px solid #1e2733;display:none;"
          "align-items:center;gap:10px;flex-wrap:wrap}";
  html += "footer label{color:#7d8b9c;font-size:11px;text-transform:uppercase;"
          "letter-spacing:.06em}";
  html += "footer select{background:#131922;color:#dce5ee;border:1px solid #26313f;"
          "border-radius:4px;padding:4px 6px;max-width:260px}";
  html += "footer span{color:#7d8b9c;font-variant-numeric:tabular-nums}";
  html += "</style></head><body>";
  html += "<header><h1>" + escapeHtml(desc.name) + "</h1><small>" +
          escapeHtml(desc.vendor) + "</small></header><div class='rows' id='rows'></div>";
  if (desc.isInstrument) {
    // Two octaves of clickable keys. Pointer events (not click) so a key
    // sounds on press and stops on release, like an instrument and not a
    // button. The markup stays inline-styled: this page has no stylesheet
    // pipeline, and a keyboard that renders half-styled is worse than plain.
    html += "<div id='kb' style='display:flex;gap:2px;padding:10px 18px 16px'></div>";
  }
  // The device picker. Present in the markup always and SHOWN only when there
  // is something to pick -- which in a plugin is never, because the host owns
  // the device there and a picker that cannot work is worse than no picker.
  html += "<footer id='dev'><label>Audio</label><select id='devsel'></select>"
          "<span id='devinfo'></span>"
          "<label id='midilabel'>MIDI</label><select id='midisel'></select></footer>";
  html += "<script>window.addEventListener('sonore:ready',function(){";
  if (desc.isInstrument) {
    html += "var kb=document.getElementById('kb');";
    html += "for(var n=48;n<=72;n++){(function(note){";
    html += "var black=[1,3,6,8,10].indexOf(note%12)>=0;";
    html += "var k=document.createElement('button');";
    html += "k.style.cssText='flex:1;height:'+(black?'54px':'72px')+';border:1px solid #333;"
            "border-radius:0 0 4px 4px;background:'+(black?'#222':'#e8e8e8')+';"
            "align-self:flex-start;cursor:pointer';";
    html += "k.addEventListener('pointerdown',function(e){k.setPointerCapture(e.pointerId);"
            "sonore.noteOn(note,100);});";
    html += "var off=function(){sonore.noteOff(note);};";
    html += "k.addEventListener('pointerup',off);k.addEventListener('pointercancel',off);";
    html += "kb.appendChild(k);})(n);}";
  }
  // ── Devices ──
  //
  // One flat list across both backends rather than a backend chooser and then
  // a device chooser. A user picking where the sound comes out is answering
  // one question, and a panel that asks "device type" first is the part of it
  // people find confusing.
  html += "var foot=document.getElementById('dev'),sel=document.getElementById('devsel'),";
  html += "info=document.getElementById('devinfo');";
  html += "sonore.onAudioDevices(function(d){";
  html += "var opts=[];";
  html += "(d.system||[]).forEach(function(n,i){opts.push({b:'system',w:i,t:n});});";
  html += "(d.asio||[]).forEach(function(n){opts.push({b:'asio',w:n,t:n+'  (ASIO)'});});";
  // Nothing to choose between is not a picker worth drawing.
  html += "if(opts.length<2){foot.style.display='none';return;}";
  html += "foot.style.display='flex';";
  html += "sel.innerHTML='';";
  html += "opts.forEach(function(o,i){var e=document.createElement('option');";
  html += "e.value=String(i);e.textContent=o.t;";
  // Which one is playing, by the same identity each backend is chosen by: an
  // index for a system device, a name for a driver.
  html += "if(d.current&&d.current.backend===o.b&&"
          "(o.b==='asio'?d.current.name===o.w:d.current.index===o.w))e.selected=true;";
  html += "sel.appendChild(e);});";
  html += "info.textContent=d.sampleRate?(d.sampleRate+' Hz'+"
          "(d.bufferFrames?', '+d.bufferFrames+' frames':'')):'';";
  html += "sel.onchange=function(){var o=opts[sel.value|0];";
  html += "if(o)sonore.selectAudioDevice(o.b,o.w);};";
  // The MIDI input, in the same row. A machine with no MIDI hardware -- which
  // is most of them -- gets no control rather than an empty one.
  html += "var ml=document.getElementById('midilabel'),ms=document.getElementById('midisel');";
  html += "var ins=d.midi||[];";
  html += "var show=ins.length>0?'':'none';";
  html += "ml.style.display=show;ms.style.display=show;";
  html += "if(ins.length){ms.innerHTML='';";
  html += "ins.forEach(function(n,i){var e=document.createElement('option');";
  html += "e.value=String(i);e.textContent=n;";
  html += "if(d.currentMidi===i)e.selected=true;ms.appendChild(e);});";
  html += "ms.onchange=function(){sonore.selectMidiDevice(ms.value|0);};}";
  html += "});";
  html += "var rows=document.getElementById('rows');";
  html += "sonore.params.forEach(function(p,i){";
  html += "var d=document.createElement('div');d.className='row';";
  html += "var l=document.createElement('label');l.textContent=p.label;";
  html += "var s=document.createElement('input');s.type='range';s.min=p.min;s.max=p.max;";
  html += "s.step=(p.max-p.min)/1000;s.value=p.value;s.disabled=!!p.readOnly;";
  html += "var o=document.createElement('output');";
  html += "var show=function(v){o.textContent=sonore.format(i,v);};";
  html += "show(p.value);";
  html += "s.addEventListener('pointerdown',function(){sonore.begin(i);});";
  html += "s.addEventListener('pointerup',function(){sonore.end(i);});";
  html += "s.addEventListener('input',function(){sonore.set(i,parseFloat(s.value));"
          "show(parseFloat(s.value));});";
  // (values, index, value): the studio's order, and the bridge's. Read as
  // (index, value), j was the values object, never equal to i, and no slider
  // here moved when the host automated or loaded a preset.
  html += "sonore.on(function(_,j,v){if(j===i){s.value=v;show(v);}});";
  html += "d.appendChild(l);d.appendChild(s);d.appendChild(o);rows.appendChild(d);});";
  html += "});</script></body></html>";
  return html;
}

} // namespace sonore
