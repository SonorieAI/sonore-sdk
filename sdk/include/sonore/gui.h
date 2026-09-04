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
// the only shared state, and it is a pair of atomics: never a lock, never an
// allocation, on the audio thread.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <type_traits>

#include "plugin.h"
#include "presets.h" // the bridge lists preset names, so it needs the full type
#include "state_bag.h" // the bridge shows the bag, so it needs the full type
#include "simd.h"      // measureBlock is on the audio thread, so it vectorises

namespace sonore {

/** What the audio thread publishes for the meters, and the UI thread reads. */
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
  float peak = 0.0f;
  float rms = 0.0f;
};

template <typename Sample>
inline BlockLevel measureBlock(const Sample* samples, size_t frames) {
  BlockLevel out;
  if (!samples || frames == 0) return out;
  if constexpr (std::is_same<Sample, float>::value) {
    // The vectorised pair, which three of the four copies already used. The
    // fourth -- the 64-bit path -- had a hand-written scalar loop, so the two
    // could have disagreed about what a peak is and nothing would have said
    // so.
    out.peak = simd::peakAbs(samples, frames);
    out.rms = (float) std::sqrt(simd::sumSquares(samples, frames) / (double) frames);
  } else {
    double sum = 0.0;
    for (size_t i = 0; i < frames; ++i) {
      const double v = (double) samples[i];
      const float a = (float) (v < 0.0 ? -v : v);
      if (a > out.peak) out.peak = a;
      sum += v * v;
    }
    out.rms = (float) std::sqrt(sum / (double) frames);
  }
  return out;
}

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

  /** [main thread] Advance the ballistics and read the display values.
   *  `dt` is the seconds since the last call. */
  void tick(double dt) {
    const float peak = peak_.load(std::memory_order_relaxed);
    const float rms = rms_.load(std::memory_order_relaxed);

    // Peak meter: instant attack, exponential decay: the standard behaviour
    // that lets a transient show and then fall back visibly.
    const float decay = (float) std::exp(-dt / 0.6);
    display_ = peak > display_ ? peak : display_ * decay;

    // VU: a CALIBRATED analog needle, matching PluginCanvas.tsx sample for
    // sample: +3.0103 dB sine calibration, 0 VU at -9 dBFS (below full scale
    // and tuned to the preview's level, so a hot signal swings PAST 0), and
    // critically damped T99 ~= 0.35 s so it never overshoots like a peak meter.
    const float rmsDb = 20.0f * std::log10(rms > 1e-9f ? rms : 1e-9f) + 3.0103f;
    const float targetVu = (rmsDb - kVuReferenceDb - kVuScaleMin) / (kVuScaleMax - kVuScaleMin);
    const float clamped = targetVu < 0.0f ? 0.0f : (targetVu > 1.2f ? 1.2f : targetVu);
    // One-pole toward the target; 4.6 time constants reach 99% in T99.
    const float k = (float) (1.0 - std::exp(-4.6 * dt / 0.35));
    vu_ += (clamped - vu_) * k;
  }

  /** Peak-equivalent level, 0..1, for `sonore.level`. */
  float level() const { return display_; }
  /** Peak in dBFS, for `sonore.db`. */
  float db() const {
    return 20.0f * std::log10(display_ > 1e-9f ? display_ : 1e-9f);
  }
  /** Calibrated VU needle position, 0..1 (>1 = past 0 VU), for `sonore.vu`. */
  float vu() const { return vu_; }

private:
  // -20..+3 VU scale with 0 VU referenced to -9 dBFS. Keep these three in step
  // with PluginCanvas.tsx or the native needle and the preview's disagree.
  static constexpr float kVuReferenceDb = -9.0f;
  static constexpr float kVuScaleMin = -20.0f;
  static constexpr float kVuScaleMax = 3.0f;

  std::atomic<float> peak_{0.0f};
  std::atomic<float> rms_{0.0f};
  float display_ = 0.0f;
  float vu_ = 0.0f;
};

/** Format a number for a JAVASCRIPT context, immune to the C locale.
 *
 *  Hosts really do call setlocale(), a German DAW makes %g print "0,5", and a
 *  comma inside generated JS is a syntax error that kills the entire bridge.
 *  The fix is one pass over the digits, because only the decimal separator can
 *  legally vary in %g output. */
inline void jsNumber(char* out, size_t capacity, double v) {
  std::snprintf(out, capacity, "%g", v);
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

/**
 * The `window.sonore` bridge, injected before any page script runs.
 *
 * Deliberately API-identical to the preview's injected bridge: the SAME uihtml
 * must work in both, so a UI authored against the studio preview cannot
 * discover at export time that a method it used doesn't exist here.
 *
 *   sonore.name                  plugin name
 *   sonore.params                [{id,label,min,max,default,unit,step}]
 *   sonore.get(i) / set(i, v)    plain-value parameter access
 *   sonore.on(cb)                cb(index, value) on any change (host or UI)
 *   sonore.onLevel(cb)           cb(level, db) ~30 Hz
 *   sonore.onVU(cb)              cb(vu) ~30 Hz, calibrated needle
 *   sonore.noteOn/noteOff        instruments with an on-screen keyboard
 *   'sonore:ready' event         fired once the bridge is populated
 */
inline std::string bridgeScript(const PluginDescriptor& desc) {
  std::string js;
  js.reserve(4096);
  js += "(function(){\n";
  js += "var post=function(m){try{window.chrome.webview.postMessage(JSON.stringify(m));}"
        "catch(e){try{window.webkit.messageHandlers.sonore.postMessage(JSON.stringify(m));}"
        "catch(e2){}}};\n";
  js += "var params=[";
  for (int i = 0; i < desc.numParams; ++i) {
    const ParamInfo& p = desc.params[i];
    // Strings are appended, never snprintf'd into a fixed buffer: a long label
    // truncated MID-JAVASCRIPT is a syntax error that silently kills the whole
    // bridge: the least debuggable failure an interface can have. Only the
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
    js += ",value:";
    js += df;
    // The step names, so sonore.format() shows "Bandpass" where the host's
    // own readout does. Null where a step has no name.
    js += ",names:[";
    for (int n = 0; n < p.stepCount; ++n) {
      const char* name = paramValueName(p, n);
      if (n) js += ",";
      if (name) js += "'" + escapeForJs(name) + "'";
      else js += "null";
    }
    js += "]}";
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
  js += "function norm(s){return String(s).toLowerCase().replace(/[^a-z0-9]/g,'');}\n";
  js += "var byKey={};\n";
  js += "function reindex(){byKey={};for(var i=0;i<params.length;i++){var p=params[i];"
        "p.__i=i;byKey[p.id]=p;byKey[norm(p.id)]=p;byKey[norm(p.label)]=p;}}\n";
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
  js += "var changeCbs=[],levelCbs=[],vuCbs=[],noteCbs=[],fileCbs=[],stateCbs=[],"
        "deviceCbs=[],cpuCbs=[];\n";
  js += "var api={\n";
  js += "  name:'" + escapeForJs(desc.name) + "',\n";
  js += "  vendor:'" + escapeForJs(desc.vendor) + "',\n";
  js += "  params:params,\n";
  js += "  get:function(r){var p=P(r);return p?p.value:undefined;},\n";
  // Only the licence overlay calls this, and only in a build that has one.
  // Defined unconditionally so the bridge script stays one static string --
  // a page that never activates simply never calls it.
  js += "  __activate:function(t){post({type:'activate',text:String(t||'')});},\n";
  js += "  set:function(r,v){var p=P(r);if(!p)return;"
        "v=Math.max(p.min,Math.min(p.max,Number(v)||0));p.value=v;"
        "post({type:'set',index:p.__i,value:v});fireChange(p);},\n";
  js += "  begin:function(r){var p=P(r);if(p)post({type:'begin',index:p.__i});},\n";
  js += "  end:function(r){var p=P(r);if(p)post({type:'end',index:p.__i});},\n";
  js += "  on:function(cb){if(typeof cb==='function')changeCbs.push(cb);},\n";
  js += "  onLevel:function(cb){if(typeof cb==='function')levelCbs.push(cb);},\n";
  js += "  onVU:function(cb){if(typeof cb==='function')vuCbs.push(cb);},\n";
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
  js += "  captureKeys:function(on){post({type:'keys',capture:on?1:0});},\n";
  js += "  chooseFile:function(purpose,mode){post({type:'file',"
        "purpose:String(purpose||'file'),mode:String(mode||'open')});},\n";
  js += "  onFile:function(cb){if(typeof cb==='function')fileCbs.push(cb);},\n";
  js += "  contextMenu:function(r,x,y){var p=P(r);post({type:'menu',index:p?p.__i:0,"
        "x:Math.round(x)|0,y:Math.round(y)|0});},\n";
  // The audio devices this program can play through, and which one it is on.
  //
  // Empty in a PLUGIN, always, and that is the whole design: a plugin does not
  // own the audio device, the host does, and a plugin drawing a device picker
  // would be offering the user a control that cannot work. The standalone
  // fills this in; nothing else ever does.
  js += "  audioDevices:{system:[],asio:[],midi:[],current:{backend:'',index:-1,name:''},"
        "currentMidi:-1,sampleRate:0,bufferFrames:0},\n";
  js += "  onAudioDevices:function(cb){if(typeof cb==='function')deviceCbs.push(cb);},\n";
  // `which` is an index for a system device and a NAME for an ASIO driver --
  // matching how each one is actually identified rather than forcing both
  // through a common shape that fits neither.
  js += "  selectMidiDevice:function(i){post({type:'device',backend:'midi',index:i|0});},\n";
  js += "  selectAudioDevice:function(backend,which){"
        "if(backend!=='system'&&backend!=='asio')return;"
        "post(backend==='asio'?{type:'device',backend:'asio',name:String(which||'')}"
        ":{type:'device',backend:'system',index:which|0});},\n";
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
        "if(p.unit==='Hz'&&v>=1000)return (v/1000).toFixed(2)+' kHz';"
        "var span=p.max-p.min,d=span>=100?0:(span>=10?1:2);"
        "return v.toFixed(d)+(p.unit?' '+p.unit:'');},\n";
  js += "  level:0, db:-100, vu:0,\n";
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
  js += "  toNormalised:function(r,v){var p=P(r);if(!p)return 0;"
        "if(v===undefined)v=p.value;var s=p.max-p.min;if(s<=0)return 0;"
        "var n=Math.max(0,Math.min(1,(v-p.min)/s));"
        "return (p.skew&&p.skew!==1)?Math.pow(n,p.skew):n;},\n";
  js += "  fromNormalised:function(r,t){var p=P(r);if(!p)return 0;"
        "var n=Math.max(0,Math.min(1,Number(t)||0));"
        "if(p.skew&&p.skew!==1)n=Math.pow(n,1/p.skew);"
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
  js += "api.__meter=function(level,db,vu){api.level=level;api.db=db;api.vu=vu;"
        "levelCbs.forEach(function(cb){try{cb(level,db);}catch(e){}});"
        "vuCbs.forEach(function(cb){try{cb(vu);}catch(e){}});};\n";
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
  js += "var fire=function(){post({type:'devices'});window.dispatchEvent(new Event('sonore:ready'));};\n";
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
 */
class UiEventQueue {
public:
  struct Event {
    enum class Kind : uint8_t { ParamSet, GestureBegin, GestureEnd, NoteOn, NoteOff };
    Kind kind = Kind::ParamSet;
    int32_t index = 0; // param index, or MIDI note
    float value = 0.0f; // param value, or velocity
  };

  /** [main thread] */
  bool push(const Event& e) {
    const size_t w = write_.load(std::memory_order_relaxed);
    const size_t next = (w + 1) % kCapacity;
    if (next == read_.load(std::memory_order_acquire)) return false; // full
    slots_[w] = e;
    write_.store(next, std::memory_order_release);
    return true;
  }

  /** [audio thread] */
  bool pop(Event* out) {
    const size_t r = read_.load(std::memory_order_relaxed);
    if (r == write_.load(std::memory_order_acquire)) return false; // empty
    *out = slots_[r];
    read_.store((r + 1) % kCapacity, std::memory_order_release);
    return true;
  }

private:
  static constexpr size_t kCapacity = 256;
  Event slots_[kCapacity]{};
  std::atomic<size_t> write_{0};
  std::atomic<size_t> read_{0};
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
  void noteOn(int key) {
    if (key < 0 || key > 127) return;
    if (count_[key] < 255) ++count_[key];
    publish(key);
  }
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

  bool isDown(int key) const {
    if (key < 0 || key > 127) return false;
    const uint64_t word = (key < 64 ? low_ : high_).load(std::memory_order_relaxed);
    return (word >> (key & 63)) & 1u;
  }
  uint64_t low() const { return low_.load(std::memory_order_relaxed); }
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
    if (!bag.valueIsText(i)) continue;
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
  enum class Kind {
    None, Set, GestureBegin, GestureEnd, NoteOn, NoteOff, LoadPreset, ContextMenu,
    ChooseFile, CaptureKeys, SelectDevice, ListDevices, Activate
  };
  Kind kind = Kind::None;
  int index = 0;
  double value = 0.0;
  int note = 0;
  int velocity = 100;
  /** Where the click was, in the plugin window's own pixels. */
  int x = 0, y = 0;
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

/** One string field. No unescaping: the bridge is the only writer of these
 *  messages, it never emits an escape, and a parser that handled them would
 *  be code with no input. A value containing a quote simply ends early, which
 *  is a malformed message doing nothing rather than something arbitrary. */
inline bool jsonString(const std::string& src, const char* key, std::string* out) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const size_t at = src.find(needle);
  if (at == std::string::npos) return false;
  const size_t begin = at + needle.size();
  const size_t end = src.find('"', begin);
  if (end == std::string::npos) return false;
  *out = src.substr(begin, end - begin);
  return true;
}

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
    if (msg.backend != "system" && msg.backend != "asio" && msg.backend != "midi") return msg;
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
  html += "s.step=(p.max-p.min)/1000;s.value=p.value;";
  html += "var o=document.createElement('output');";
  html += "var show=function(v){o.textContent=sonore.format(i,v);};";
  html += "show(p.value);";
  html += "s.addEventListener('pointerdown',function(){sonore.begin(i);});";
  html += "s.addEventListener('pointerup',function(){sonore.end(i);});";
  html += "s.addEventListener('input',function(){sonore.set(i,parseFloat(s.value));"
          "show(parseFloat(s.value));});";
  html += "sonore.on(function(j,v){if(j===i){s.value=v;show(v);}});";
  html += "d.appendChild(l);d.appendChild(s);d.appendChild(o);rows.appendChild(d);});";
  html += "});</script></body></html>";
  return html;
}

} // namespace sonore
