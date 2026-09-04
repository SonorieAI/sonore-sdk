// SPDX-License-Identifier: Apache-2.0
// Sonore SDK example: a stereo saturator built exactly the way a GENERATED
// plugin is: a `SonoreDsp` against the toolkit, a parameter table, a descriptor,
// then the format wrapper. Nothing here is special-cased for being an example.
#define SONORE_NUM_PARAMS 4

#include <sonore/dsp.h>

struct SonoreDsp {
  sonore::Smooth driveSm, toneSm, mixSm, outSm;
  sonore::Biquad toneL, toneR;
  sonore::DcBlocker dcL, dcR;
  sonore::Oversampler2x osL, osR;
  // The nonlinearity runs oversampled, so the wet path comes out late. At the
  // default 100% mix that is invisible; anywhere in between it combs the dry
  // against the wet. The reverb example is where this was found.
  sonore::CompensatedDryWetMixer<2, 64> mixer;
  float lastTone = -1.0f;
  float sampleRate = 48000.0f;

  void prepare(const sonore::ProcessSpec& spec) {
    const float sr = (float) spec.sampleRate;
    sampleRate = sr;
    driveSm.setup(sr, 12.0f); driveSm.snap(1.0f);
    toneSm.setup(sr, 20.0f);  toneSm.snap(6000.0f);
    mixSm.setup(sr, 12.0f);   mixSm.snap(1.0f);
    outSm.setup(sr, 12.0f);   outSm.snap(1.0f);
    toneL.setSampleRate(sr); toneR.setSampleRate(sr);
    toneL.reset(); toneR.reset();
    dcL.setSampleRate(sr); dcR.setSampleRate(sr);
    dcL.reset(); dcR.reset();
    osL.reset(); osR.reset();
    mixer.setWetLatency(sonore::Oversampler2x::latencySamples());
    lastTone = -1.0f;
  }

  /** The oversampler delays the signal, so the plugin says so. Without this
   *  the host cannot time-align it and every parallel mix it sits in is
   *  smeared: silently, which is the worst kind. (A saturator that would
   *  rather have none can use sonore::OversamplerIir instead: minimum phase,
   *  no latency to report, and a quarter of the multiplies.) */
  int latencySamples() const { return sonore::Oversampler2x::latencySamples(); }

  /** This plugin declared NO tail until the host test stopped assuming and
   *  started measuring: it found 2964 samples still coming out after the input
   *  went silent. The DC blocker is the reason: a 10 Hz corner takes over a
   *  hundred milliseconds to settle, and everything else in the chain is fast.
   *  A host told there is no tail cuts all of it on stop and on export. */
  int tailSamples() const { return sonore::DcBlocker::tailSamples(sampleRate); }

  void process(sonore::AudioBlock<float>& io, const float* p) {
    const size_t n = io.getNumSamples();
    float* L = io.getChannelPointer(0);
    float* R = io.getNumChannels() > 1 ? io.getChannelPointer(1) : L;

    // Coefficients are rebuilt only when the control actually moved: a filter
    // factory called every block is what the real-time lint flags.
    const float toneTarget = p[1];
    if (std::fabs(toneTarget - lastTone) > 0.5f) {
      toneL.lowpass(toneTarget, 0.707f);
      toneR.lowpass(toneTarget, 0.707f);
      lastTone = toneTarget;
    }
    mixer.setMix(p[2] * 0.01f); // the control is a percentage

    for (size_t i = 0; i < n; ++i) {
      const float drive = driveSm.next(p[0]);
      const float outGain = outSm.next(sonore::dbToGain(p[3]));
      const float dryL = L[i], dryR = R[i];

      // The nonlinearity runs oversampled, that is what keeps the aliasing
      // probe clean at high drive.
      float wetL = osL.process(dryL, [drive](float x) { return std::tanh(x * drive); });
      float wetR = osR.process(dryR, [drive](float x) { return std::tanh(x * drive); });

      wetL = dcL.process(toneL.process(wetL));
      wetR = dcR.process(toneR.process(wetR));

      L[i] = mixer.process(0, dryL, wetL) * outGain;
      R[i] = mixer.process(1, dryR, wetR) * outGain;
    }
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"drive", "Drive", "x", 1.0f, 20.0f, 2.0f, 0},
    {"tone", "Tone", "Hz", 500.0f, 18000.0f, 6000.0f, 0},
    {"mix", "Mix", "%", 0.0f, 100.0f, 100.0f, 0},
    {"output", "Output", "dB", -24.0f, 12.0f, 0.0f, 0},
};

// Factory presets: drive, tone, mix, output: in the contract's index order.
// Compiled in, so they can never go missing or drift from the parameter set
// that shipped with them.
#include <sonore/presets.h>

static const float kPresetWarm[SONORE_NUM_PARAMS] = {2.5f, 5000.0f, 60.0f, 0.0f};
static const float kPresetCrunch[SONORE_NUM_PARAMS] = {8.0f, 8000.0f, 100.0f, -3.0f};
static const float kPresetDestroy[SONORE_NUM_PARAMS] = {18.0f, 12000.0f, 100.0f, -6.0f};

static const sonore::Preset kPresets[] = {
    {"Warm Glue", kPresetWarm, SONORE_NUM_PARAMS},
    {"Crunch", kPresetCrunch, SONORE_NUM_PARAMS},
    {"Destroy", kPresetDestroy, SONORE_NUM_PARAMS},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.example.saturator",
    "Sonore Saturator",
    "Sonorie",
    "1.0.0",
    "Oversampled stereo saturation with tone and mix.",
    "https://sonorie.com",
    false, // effect
    kParamTable,
    SONORE_NUM_PARAMS,
    kPresets,
    (int) (sizeof(kPresets) / sizeof(kPresets[0])),
    "distortion", // -> lv2:DistortionPlugin and friends per format
};

// The faceplate. This is the shape a GENERATED plugin ships: one self-contained
// HTML document, bound to the parameters through `window.sonore`: the same
// bridge, the same markup, as the studio preview.
#define SONORE_UI_WIDTH 620
#define SONORE_UI_HEIGHT 300
#define SONORE_UI_HTML                                                                     \
  R"HTML(<!doctype html><html><head><meta charset="utf-8"><style>
  :root{--bg:#12100c;--panel:#1c1913;--edge:#2e2a20;--ink:#e8dcc4;--dim:#9c8f76;--hot:#d8a24a}
  *{box-sizing:border-box}
  body{margin:0;height:100vh;background:radial-gradient(120% 100% at 50% 0%,#1e1a13,#0b0a08);
       color:var(--ink);font:13px/1.4 "Segoe UI",system-ui,sans-serif;
       display:flex;align-items:center;justify-content:center;user-select:none}
  .unit{width:100%;max-width:600px;background:linear-gradient(180deg,#221e17,#15120d);
        border:1px solid var(--edge);border-radius:10px;padding:18px 22px;
        box-shadow:0 18px 40px rgba(0,0,0,.55),inset 0 1px 0 rgba(255,255,255,.05)}
  header{display:flex;align-items:baseline;justify-content:space-between;
         border-bottom:1px solid var(--edge);padding-bottom:10px;margin-bottom:16px}
  h1{margin:0;font-size:17px;letter-spacing:.14em;text-transform:uppercase;color:var(--hot)}
  .sub{font-size:10px;letter-spacing:.2em;color:var(--dim);text-transform:uppercase}
  .row{display:flex;gap:18px;align-items:flex-end}
  .knob{flex:1;text-align:center}
  .dial{width:64px;height:64px;margin:0 auto;border-radius:50%;position:relative;cursor:ns-resize;
        background:conic-gradient(from 220deg,#3a3125,#0f0d0a 60%);
        border:1px solid #3d3629;box-shadow:0 4px 10px rgba(0,0,0,.5),inset 0 1px 0 rgba(255,255,255,.07)}
  .dial i{position:absolute;left:50%;top:6px;width:2px;height:22px;background:var(--hot);
          transform-origin:50% 26px;border-radius:1px;margin-left:-1px}
  .name{margin-top:9px;font-size:11px;letter-spacing:.1em;text-transform:uppercase;color:var(--dim)}
  .val{font-size:12px;font-variant-numeric:tabular-nums;color:var(--ink);margin-top:2px}
  .meter{width:64px;height:6px;margin:0 auto;background:#0d0b08;border:1px solid #322c22;
         border-radius:3px;overflow:hidden}
  .meter>span{display:block;height:100%;width:0%;background:linear-gradient(90deg,#6a8f4a,#d8a24a,#c04b32)}
  </style></head><body>
  <div class="unit">
    <header><h1>Saturator</h1><span class="sub">Sonorie &middot; oversampled</span></header>
    <div class="row" id="row"></div>
    <div style="margin-top:16px"><div class="meter"><span id="lvl"></span></div></div>
  </div>
  <script>
  window.addEventListener('sonore:ready', function () {
    var row = document.getElementById('row');
    sonore.params.forEach(function (p, i) {
      var wrap = document.createElement('div'); wrap.className = 'knob';
      var dial = document.createElement('div'); dial.className = 'dial';
      var pointer = document.createElement('i'); dial.appendChild(pointer);
      var name = document.createElement('div'); name.className = 'name'; name.textContent = p.label;
      var val = document.createElement('div'); val.className = 'val';
      wrap.appendChild(dial); wrap.appendChild(name); wrap.appendChild(val);
      row.appendChild(wrap);

      var norm = function (v) { return (v - p.min) / (p.max - p.min); };
      var show = function (v) {
        pointer.style.transform = 'rotate(' + (-140 + norm(v) * 280) + 'deg)';
        val.textContent = sonore.format(i, v); // same text the host shows
      };
      show(p.value);

      // Vertical drag, the hardware convention. Shift = fine.
      var dragging = false, lastY = 0;
      dial.addEventListener('pointerdown', function (e) {
        dragging = true; lastY = e.clientY; dial.setPointerCapture(e.pointerId);
        sonore.begin(i);
      });
      dial.addEventListener('pointermove', function (e) {
        if (!dragging) return;
        var span = p.max - p.min;
        var step = span / (e.shiftKey ? 900 : 180);
        var next = Math.max(p.min, Math.min(p.max, sonore.get(i) + (lastY - e.clientY) * step));
        lastY = e.clientY;
        sonore.set(i, next); show(next);
      });
      var release = function (e) {
        if (!dragging) return;
        dragging = false; sonore.end(i);
        try { dial.releasePointerCapture(e.pointerId); } catch (err) {}
      };
      dial.addEventListener('pointerup', release);
      dial.addEventListener('pointercancel', release);
      dial.addEventListener('dblclick', function () { sonore.set(i, p.default); show(p.default); });

      sonore.on(function (j, v) { if (j === i) show(v); });
    });

    var lvl = document.getElementById('lvl');
    sonore.onLevel(function (level) { lvl.style.width = Math.min(100, level * 100) + '%'; });
  });
  </script></body></html>)HTML"

#include <sonore/clap_wrapper.h>

// The same source builds a VST3 as well: the CLAP wrapper owns the shared
// machinery and the VST3 one adapts it, so the two formats cannot drift.
#if defined(SONORE_BUILD_VST3)
#include <sonore/vst3_wrapper.h>
#endif

// …and an Audio Unit on macOS, from the same sources again.
#if defined(SONORE_BUILD_AU)
#include <sonore/au_wrapper.h>
#include <sonore/au_view.h>
#endif

// …and an LV2 bundle for the Linux-native hosts (the same file also builds
// the TTL generator under SONORE_LV2_TTLGEN).
#if defined(SONORE_BUILD_LV2)
#include <sonore/lv2_wrapper.h>
#endif

// …and a runnable application, no DAW required. standalone.h defines main().
#if defined(SONORE_BUILD_STANDALONE)
#include <sonore/standalone.h>
#endif
