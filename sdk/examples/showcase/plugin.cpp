// SPDX-License-Identifier: Apache-2.0
// Sonore SDK example -- the showcase: a waveshaper with a face of its own.
//
// What it is: drive into a transfer curve, 4x oversampled, then a tone
// control, a dry/wet mix and an output trim. The curve is one of three
// built-in shapes or ANY function of x the user types -- "tanh(x + 0.3) -
// tanh(0.3)" for a tube-ish asymmetry, "round(tanh(x) * 4) / 4" for steps --
// compiled off the audio thread into a table (curve.h).
//
// What it shows, and where:
//
//   plugin.cpp  the parameters as an AudioProcessorValueTreeState layout,
//               which is also the table the descriptor hands every format;
//               the DSP; factory presets; state beyond the parameters.
//   curve.h     sonore::Expression, sampled into a lookup table and handed
//               to the audio thread without a lock.
//   editor.h    a NATIVE editor built from the SDK's widgets: APVTS
//               attachments, a menu bar, a LookAndFeel_V4 scheme switch, a
//               ColourSelector for the accent, an SVG logo whose text the SDK
//               draws, a JPEG backdrop through ImageCache, a code editor for
//               the curve, tooltips.
//
// The DSP reads its parameters from p[], the block-consistent copy every
// wrapper hands it. The APVTS lives here too, as the reference keeps it in the
// processor, but it is the EDITOR's model: the editor connects it to the host
// while it is open, and the widgets, undo and the menus go through it.
//
// Where the editor appears: CLAP, VST3 and AU, inside the host's window. The
// standalone build (SonoreShowcaseApp) still shows the webview's generated
// panel -- standalone.h keeps the webview for its device pickers -- and an LV2
// UI is the webview too. tests/showcase_test.cpp runs all of this headless.
#define SONORE_NUM_PARAMS 6

#include <sonore/dsp.h>
#include <sonore/state_bag.h>
#include <sonore/value_tree_state.h>
#include <sonore/gfx/plugin_editor.h>

#include <atomic>
#include <memory>
#include <string>

#include "curve.h"

namespace showcase {

/** Positions in the layout below, which are the DSP's p[] indices. */
enum Param { kDrive, kShape, kTone, kMix, kOutput, kAutoGain, kNumParams };
static_assert(kNumParams == SONORE_NUM_PARAMS, "the layout and SONORE_NUM_PARAMS must agree");

/** The built-in shapes are expressions too: one path from text to table, so
 *  the expression compiler is exercised by every instance, typed into or not. */
static const char* const kBuiltInCurves[3] = {"tanh(x)", "clamp(x, -1, 1)", "sin(x)"};
static const char* const kExpressionShape = "Expression";
static const char* const kDefaultExpression = "tanh(x + 0.3) - tanh(0.3)";

inline sonore::AudioProcessorValueTreeState::ParameterLayout createParameterLayout() {
  using namespace sonore;
  using Attributes = RangedAudioParameter::Attributes;
  NormalisableRange<float> tone(400.0f, 20000.0f);
  tone.setSkewForCentre(3000.0f); // so the knob's middle is where tone controls live
  return AudioProcessorValueTreeState::ParameterLayout(
      std::make_unique<AudioParameterFloat>("drive", "Drive", NormalisableRange<float>(0.0f, 36.0f), 12.0f,
                                            Attributes().withLabel("dB")),
      std::make_unique<AudioParameterChoice>(
          "shape", "Shape", std::vector<std::string>{"Soft", "Hard", "Fold", kExpressionShape}, 0),
      std::make_unique<AudioParameterFloat>("tone", "Tone", tone, 8000.0f, Attributes().withLabel("Hz")),
      std::make_unique<AudioParameterFloat>("mix", "Mix", NormalisableRange<float>(0.0f, 100.0f), 100.0f,
                                            Attributes().withLabel("%")),
      std::make_unique<AudioParameterFloat>("output", "Output", NormalisableRange<float>(-24.0f, 12.0f), 0.0f,
                                            Attributes().withLabel("dB")),
      std::make_unique<AudioParameterBool>("autogain", "Auto Gain", true));
}

/** The table the descriptor points at: one APVTS from the same layout, alive
 *  as long as the module. The DSP's own APVTS cannot be it -- the descriptor
 *  exists before any instance does. */
inline const sonore::AudioProcessorValueTreeState& parameterTable() {
  static const sonore::AudioProcessorValueTreeState table(nullptr, "Showcase", createParameterLayout());
  return table;
}

/** The three fixed curves, compiled once for the module and read by every
 *  instance. They are the same tables in each and never written after they
 *  are made, so each instance compiling its own copy was 24 KB apiece. Made
 *  by the first instance's constructor, on the main thread; the audio thread
 *  only reads them, through the pointer its Curves holds. */
inline const CurveTable* builtInCurves() {
  struct Tables {
    CurveTable table[3];
    Tables() {
      std::string error;
      for (int i = 0; i < 3; ++i) compileCurve(kBuiltInCurves[i], table[i], error);
    }
  };
  static const Tables tables;
  return tables.table;
}

/** Every curve an instance can play: the three fixed ones, the exchange that
 *  carries the typed one to the audio thread, and the message thread's own
 *  copy of it for the editor to draw. On the heap, once: 33 kB of tables is
 *  no business of whatever stack the wrapper builds a DSP on. */
struct Curves {
  const CurveTable* builtIn = builtInCurves(); // the module's, shared
  CurveTable shown; // [message thread] what the exchange last published
  CurveExchange exchange;
  explicit Curves(const CurveTable& initial) : shown(initial), exchange(initial) {}
};

} // namespace showcase

struct SonoreDsp {
  // ── The message thread's half ───────────────────────────────────────────
  sonore::UndoHistory undo;
  sonore::AudioProcessorValueTreeState parameters{&undo, "Showcase", showcase::createParameterLayout()};

  SonoreDsp() {
    showcase::CurveTable table;
    std::string error;
    showcase::compileCurve(showcase::kDefaultExpression, table, error);
    curves.reset(new showcase::Curves(table));
    curves->shown = table;
    expression = showcase::kDefaultExpression;
  }

  /**
   * [message thread] Make `text` the Expression shape's curve. False, with
   * the reason, when it does not compile -- and then the old curve keeps
   * playing, so a half-typed formula is never heard.
   */
  bool setExpression(const std::string& text, std::string* why = nullptr) {
    std::string error;
    if (!showcase::compileCurve(text, curves->exchange.back(), error)) {
      if (why) *why = error;
      return false;
    }
    curves->shown = curves->exchange.back();
    curves->exchange.publish();
    expression = text;
    return true;
  }

  /** [message thread] The curve shape `index` plays, for drawing. */
  const showcase::CurveTable& displayCurve(int index) const {
    return index >= 0 && index < 3 ? curves->builtIn[index] : curves->shown;
  }

  // The editor's choices, saved with the session so the plugin reopens the
  // way the user left it. Not parameters: nobody automates a colour scheme.
  std::string expression;
  int scheme = 0;                // LookAndFeel_V4: dark, midnight, grey, light
  uint32_t accent = 0xff42a2c8;  // ARGB
  /** Bumped by loadState, so an open editor sees a session it did not make. */
  int stateSerial = 0;

  void saveState(sonore::StateBag& bag) const {
    bag.setString("expression", expression);
    bag.setInt("scheme", scheme);
    bag.setInt("accent", (int64_t) accent);
  }
  void loadState(const sonore::StateBag& bag) {
    if (!setExpression(bag.getString("expression", showcase::kDefaultExpression)))
      setExpression(showcase::kDefaultExpression);
    scheme = (int) bag.getInt("scheme", 0);
    accent = (uint32_t) bag.getInt("accent", (int64_t) 0xff42a2c8);
    ++stateSerial;
  }

  /** [message thread] The native editor; defined after editor.h. */
  std::unique_ptr<sonore::gfx::AudioProcessorEditor> createEditor(const sonore::gfx::EditorHost& host);

  // ── The audio thread's half ─────────────────────────────────────────────
  std::unique_ptr<showcase::Curves> curves;
  sonore::Smooth driveSm, outSm, autoSm;
  /** False until the first block after prepare(), which starts the smoothers
   *  AT the controls' values: gliding there from a guess would be a 20 ms
   *  swell on every transport start. */
  bool primed = false;
  sonore::Biquad toneL, toneR;
  sonore::DcBlocker dcL, dcR; // a typed curve need not pass through zero
  sonore::Oversampler4x osL, osR;
  // The oversampler delays the wet path; a mix in between would comb without
  // the dry being delayed to match.
  sonore::CompensatedDryWetMixer<2, 64> mixer;
  float lastTone = -1.0f;
  float sampleRate = 48000.0f;
  /** The loudest DRIVEN sample since the editor last looked: where on the
   *  curve the signal is sitting. Written here, read and cleared by the UI. */
  std::atomic<float> drivenPeak{0.0f};

  void prepare(const sonore::ProcessSpec& spec) {
    sampleRate = (float) spec.sampleRate;
    driveSm.setup(sampleRate, 20.0f);
    outSm.setup(sampleRate, 20.0f);
    autoSm.setup(sampleRate, 20.0f);
    primed = false; // the first block snaps them to where the controls are
    toneL.setSampleRate(sampleRate);
    toneR.setSampleRate(sampleRate);
    toneL.reset();
    toneR.reset();
    dcL.setSampleRate(sampleRate);
    dcR.setSampleRate(sampleRate);
    dcL.reset();
    dcR.reset();
    osL.reset();
    osR.reset();
    mixer.setSampleRate(sampleRate);
    mixer.setWetLatency(sonore::Oversampler4x::latencySamples());
    lastTone = -1.0f;
  }

  /** Reported, or every parallel mix the host builds around it is smeared. */
  int latencySamples() const { return sonore::Oversampler4x::latencySamples(); }
  /** The DC blocker's settle time: the longest thing in the chain. */
  int tailSamples() const { return sonore::DcBlocker::tailSamples(sampleRate); }

  /** [audio thread] */
  void process(sonore::AudioBlock<float>& io, const float* p) {
    using namespace showcase;
    const size_t n = io.getNumSamples();
    float* L = io.getChannelPointer(0);
    float* R = io.getNumChannels() > 1 ? io.getChannelPointer(1) : L;

    // Picked up every block whichever shape is playing, so switching to the
    // Expression shape plays the newest curve and not one from minutes ago.
    const CurveTable& typed = curves->exchange.acquire();
    const int shape = (int) (p[kShape] + 0.5f);
    const CurveTable& curve = shape >= 0 && shape < 3 ? curves->builtIn[shape] : typed;

    // Filter coefficients only when the control moved: a filter designed
    // every block is what the real-time audit flags.
    if (std::fabs(p[kTone] - lastTone) > 0.5f) {
      toneL.lowpass(p[kTone], 0.707f);
      toneR.lowpass(p[kTone], 0.707f);
      lastTone = p[kTone];
    }
    mixer.setMix(p[kMix] * 0.01f);
    const float driveTarget = sonore::dbToGain(p[kDrive]);
    // A rough loudness match: half the drive taken back off. Rough on
    // purpose -- a curve the user types can have any gain at all, and a
    // matched level that was right for tanh would be wrong for theirs.
    const float autoTarget = p[kAutoGain] >= 0.5f ? sonore::dbToGain(-0.5f * p[kDrive]) : 1.0f;
    const float outTarget = sonore::dbToGain(p[kOutput]);
    if (!primed) {
      driveSm.snap(driveTarget);
      autoSm.snap(autoTarget);
      outSm.snap(outTarget);
      primed = true;
    }

    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i) {
      const float drive = driveSm.next(driveTarget);
      const float makeup = autoSm.next(autoTarget) * outSm.next(outTarget);
      const float dryL = L[i], dryR = R[i];
      const float inPeak = std::fabs(dryL) > std::fabs(dryR) ? std::fabs(dryL) : std::fabs(dryR);
      if (inPeak * drive > peak) peak = inPeak * drive;

      // The curve runs at four times the rate: a sharp corner in it makes
      // harmonics far above Nyquist, and at the base rate they fold back down
      // as inharmonic hash.
      auto shaper = [&curve, drive](float x) { return curve.lookup(x * drive); };
      float wetL = osL.process(dryL, shaper);
      float wetR = osR.process(dryR, shaper);
      wetL = dcL.process(toneL.process(wetL));
      wetR = dcR.process(toneR.process(wetR));
      L[i] = mixer.process(0, dryL, wetL) * makeup;
      R[i] = mixer.process(1, dryR, wetR) * makeup;
    }
    if (peak > drivenPeak.load(std::memory_order_relaxed)) drivenPeak.store(peak, std::memory_order_relaxed);
  }
};

#include <sonore/presets.h>

namespace showcase {

// Factory presets, in the layout's order: drive, shape, tone, mix, output,
// auto gain. The host lists them (kDesc, below); the editor's Presets menu
// applies the same ones through the APVTS, as an edit the host hears and undo
// can take back -- which is why they come before the editor.
static const float kPresetWarm[SONORE_NUM_PARAMS] = {6.0f, 0.0f, 9000.0f, 60.0f, 0.0f, 1.0f};
static const float kPresetCrunch[SONORE_NUM_PARAMS] = {20.0f, 1.0f, 7000.0f, 100.0f, -2.0f, 1.0f};
static const float kPresetFolded[SONORE_NUM_PARAMS] = {15.0f, 2.0f, 12000.0f, 80.0f, -3.0f, 1.0f};

static const sonore::Preset kPresets[] = {
    {"Warm Glue", kPresetWarm, SONORE_NUM_PARAMS},
    {"Crunch", kPresetCrunch, SONORE_NUM_PARAMS},
    {"Folded", kPresetFolded, SONORE_NUM_PARAMS},
};
static const int kNumPresets = (int) (sizeof(kPresets) / sizeof(kPresets[0]));

} // namespace showcase

#include "editor.h"

inline std::unique_ptr<sonore::gfx::AudioProcessorEditor> SonoreDsp::createEditor(
    const sonore::gfx::EditorHost& host) {
  return std::unique_ptr<sonore::gfx::AudioProcessorEditor>(new showcase::ShowcaseEditor(*this, host));
}

#include <sonore/plugin.h>

static const sonore::PluginDescriptor kDesc = [] {
  sonore::PluginDescriptor d;
  d.id = "com.sonorie.example.showcase";
  d.name = "Sonore Showcase";
  d.vendor = "Sonorie";
  d.version = "1.0.0";
  d.description = "Oversampled waveshaper whose curve you can type, with a native editor.";
  d.url = "https://sonorie.com";
  d.params = showcase::parameterTable().parameterInfos();
  d.numParams = showcase::parameterTable().numParameters();
  d.presets = showcase::kPresets;
  d.numPresets = showcase::kNumPresets;
  d.category = "distortion";
  // Resizable within reason: the layout is proportional, and below this the
  // code editor stops having room for a formula.
  d.editorLimits.minWidth = 600;
  d.editorLimits.minHeight = 380;
  d.editorLimits.maxWidth = 1600;
  d.editorLimits.maxHeight = 1000;
  return d;
}();

#define SONORE_UI_WIDTH 780
#define SONORE_UI_HEIGHT 500

#include <sonore/formats.h> // CLAP, and every format this target is built as
