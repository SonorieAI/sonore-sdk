// SPDX-License-Identifier: Apache-2.0
// The showcase's native editor: a plugin's own face, built from the SDK's
// widgets and opened by the SDK's window in every format that has one.
//
// ── How it reaches the screen ───────────────────────────────────────────────
//
// SonoreDsp::createEditor() makes one of these whenever a host opens the
// editor; NativeEditor puts it in the host's window, ticks it, resizes it and
// drops its ComboBox menus down, and destroys it when the host closes the
// window. It is a Widget and nothing else, so the integration test makes one
// with four lambdas for a host and renders it into a Bitmap.
//
// ── How it talks to the parameters ──────────────────────────────────────────
//
// Through the DSP's AudioProcessorValueTreeState, connected to the host for as
// long as the editor lives. The attachments bind each widget to a parameter
// with the host's own curve and gestures; editorTick() reads the host back
// (syncFromHost), so automation and a preset loaded by the host move the
// knobs; the Edit menu's undo goes through the APVTS's history and tells the
// host what it changed, so the DAW's automation follows the undo.
//
// ── What is not a parameter ─────────────────────────────────────────────────
//
// The typed curve, the colour scheme and the accent are the plugin's state,
// saved with the session through the DSP's saveState(). The curve is compiled
// here on the message thread, as the user types, and handed to the audio
// thread by the DSP (curve.h says how) -- the editor never touches anything
// the audio thread reads.
//
// ── Presets ─────────────────────────────────────────────────────────────────
//
// The user's own as well as the factory three: a PresetBar (gfx/preset_bar.h)
// sits in the menu strip, right of the menus -- name, modified marker,
// previous and next -- and the Presets menu is the bar's menu with the curves
// after it. A preset is the plugin's whole state, so a saved one brings back
// the typed curve and the colours along with the knobs; loading one goes
// through the APVTS's editWholeState, which makes it ONE step for Edit > Undo.
// When the host gives no preset manager (a test's four lambdas) the bar is
// left out and the menu applies the factory presets as edits, as it always
// did.
#pragma once

#include <sonore/gfx/alert.h>
#include <sonore/gfx/code_editor.h>
#include <sonore/gfx/colour_selector.h>
#include <sonore/gfx/image.h>
#include <sonore/gfx/lookandfeel_versions.h>
#include <sonore/gfx/menu_bar.h>
#include <sonore/gfx/preset_bar.h>
#include <sonore/gfx/svg.h>
#include <sonore/gfx/system_font.h>
#include <sonore/gfx/value_tree_state_attachments.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "background.h"

namespace showcase {

namespace gfx = sonore::gfx;
using APVTS = sonore::AudioProcessorValueTreeState;

/** The logo: a badge, a curve and two lines of <text>. The text is not in the
 *  file as outlines -- the SDK lays it out in a system face (font_match.h),
 *  so it stays sharp at any editor size. currentColor is the accent. */
static const char* const kLogoSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 280 48">
  <rect x="2" y="2" width="44" height="44" rx="11" fill="currentColor"/>
  <path d="M9 33 C 19 33, 19 15, 24 15 S 29 33, 39 15" fill="none" stroke="#ffffff"
        stroke-width="3.2" stroke-linecap="round"/>
  <text x="58" y="27" font-family="Segoe UI, Helvetica Neue, Arial, sans-serif" font-size="23"
        font-weight="700" letter-spacing="3" fill="currentColor">SHOWCASE</text>
  <text x="59" y="43" font-family="Segoe UI, Helvetica Neue, Arial, sans-serif" font-size="10"
        letter-spacing="2" fill="#8d99a3">SONORE SDK - WAVESHAPER</text>
</svg>)SVG";

static const char* const kSchemeNames[4] = {"Dark", "Midnight", "Grey", "Light"};

inline gfx::LookAndFeel_V4::ColourScheme schemeAt(int index) {
  switch (index) {
    case 1: return gfx::LookAndFeel_V4::getMidnightColourScheme();
    case 2: return gfx::LookAndFeel_V4::getGreyColourScheme();
    case 3: return gfx::LookAndFeel_V4::getLightColourScheme();
    default: return gfx::LookAndFeel_V4::getDarkColourScheme();
  }
}

/** Curves worth hearing, for the Presets menu: each one is only text. */
struct ExampleCurve {
  const char* name;
  const char* text;
};
static const ExampleCurve kExampleCurves[] = {
    {"Tube (asymmetric)", "tanh(x + 0.3) - tanh(0.3)"},
    {"Cubic soft clip", "clamp(x, -1, 1) - pow(clamp(x, -1, 1), 3) / 3"},
    {"Rectified", "abs(tanh(x))"},
    {"Staircase", "round(tanh(x) * 4) / 4"},
    {"Wavefold", "sin(x * pi / 2)"},
};
static const int kNumExampleCurves = (int) (sizeof(kExampleCurves) / sizeof(kExampleCurves[0]));

/**
 * The curve the selected shape plays, drawn over the range a driven signal
 * actually covers, with a band showing where the signal sits on it right now.
 */
class CurveView : public gfx::Widget {
public:
  static constexpr float kViewX = 3.0f; // driven input, either side of zero
  static constexpr float kViewY = 1.5f; // output, either side of zero

  const CurveTable* curve = nullptr;
  /** How far out the driven signal reached lately: the lit band. */
  float signal = 0.0f;

  void paint(gfx::Graphics& g) override {
    gfx::LookAndFeel& look = lookAndFeel();
    const gfx::Rect r = localBounds();
    g.setColour(look.panel().withMultipliedAlpha(0.8f));
    g.fillRoundedRect(r, 10.0f);
    const gfx::Rect plot(r.x + 12.0f, r.y + 12.0f, r.w - 24.0f, r.h - 24.0f);
    auto px = [&](float x) { return plot.x + (x / kViewX * 0.5f + 0.5f) * plot.w; };
    auto py = [&](float y) { return plot.y + (0.5f - y / kViewY * 0.5f) * plot.h; };

    // Where the signal is: the input range it covered since the last frame.
    const float reach = (std::min)(signal, kViewX);
    if (reach > 0.01f) {
      g.setColour(look.accent().withMultipliedAlpha(0.16f));
      g.fillRect(gfx::Rect(px(-reach), plot.y, px(reach) - px(-reach), plot.h));
    }
    // The axes, and the straight line a curve with no shaping would be.
    g.setColour(look.outline().withMultipliedAlpha(0.5f));
    g.drawLine(plot.x, py(0.0f), plot.right(), py(0.0f), 1.0f);
    g.drawLine(px(0.0f), plot.y, px(0.0f), plot.bottom(), 1.0f);
    g.setColour(look.outline().withMultipliedAlpha(0.35f));
    g.drawLine(px(-kViewY), py(-kViewY), px(kViewY), py(kViewY), 1.0f);

    if (!curve) return;
    gfx::Path path;
    const int steps = (int) plot.w;
    for (int i = 0; i <= steps; ++i) {
      const float x = -kViewX + 2.0f * kViewX * (float) i / (float) steps;
      const float y = (std::max)(-kViewY, (std::min)(kViewY, curve->lookup(x)));
      if (i == 0) path.moveTo(px(x), py(y));
      else path.lineTo(px(x), py(y));
    }
    g.setColour(look.accent());
    g.strokePath(path, 2.5f);
  }
};

class ShowcaseEditor : public gfx::AudioProcessorEditor, public gfx::MenuBarModel {
public:
  static constexpr float kMenuHeight = 24.0f;
  /** The strip's height when the preset bar shares it: room for its buttons. */
  static constexpr float kMenuHeightWithPresets = 30.0f;
  /** How much of the strip the menu names take before the preset bar. */
  static constexpr float kMenuNamesWidth = 250.0f;

  ShowcaseEditor(SonoreDsp& dsp, const gfx::EditorHost& host)
      : dsp_(dsp), state_(dsp.parameters), presetBar_(host.presets) {
    if (auto face = gfx::systemTypeface()) setFont(gfx::Font(face, 13.0f));
    // Set once on the editor: every widget inside inherits it, so the scheme
    // switch below is one call.
    setLookAndFeel(&look_);

    // The host first, then the widgets: each attachment shows the APVTS's
    // value as it is made, and syncFromHost is what makes that the host's
    // value rather than whatever it was when the editor last closed.
    state_.connectToHost(gfx::hostLinkFrom(host));
    state_.syncFromHost();
    // Looked up once and kept: the lookup compares strings, the pointer is a
    // load. The same atomic a DSP would read on the audio thread.
    shapeValue_ = state_.getRawParameterValue("shape");

    // Decoded once per process, however many editors and instances there
    // are: the cache keys the bytes and hands every editor the same Bitmap.
    background_ = gfx::ImageCache::shared().getFromMemory(kBackgroundJpeg, sizeof(kBackgroundJpeg));
    logo_ = gfx::Drawable::parse(kLogoSvg);

    addChild(&menuBar_);
    if (presetBar_.manager()) {
      addChild(&presetBar_);
      // A preset load is one Edit > Undo step: the APVTS records the whole
      // state before it, and undoing puts all of it back -- curve and
      // colours included, which are not parameters.
      presetBar_.wrapLoad = [this](const std::string& label, const std::function<bool(std::string*)>& load,
                                   std::string* why) { return state_.editWholeState(label, load, why); };
    }

    static const char* const ids[4] = {"drive", "tone", "mix", "output"};
    static const char* const tips[4] = {
        "How hard the signal is pushed into the curve",
        "A lowpass after the curve, to tame the fizz",
        "Dry against shaped, delay-compensated",
        "Level after everything",
    };
    for (int i = 0; i < 4; ++i) {
      Knob& k = knobs_[i];
      k.name.setText(state_.getParameter(ids[i])->getName());
      k.name.setJustify(gfx::Justify::Centred);
      k.value.setJustify(gfx::Justify::Centred);
      k.slider.setTooltip(tips[i]);
      addChild(&k.name);
      addChild(&k.slider);
      addChild(&k.value);
      knobAttachments_[i].reset(new APVTS::SliderAttachment(state_, ids[i], k.slider));
      // The number under the knob is the attachment's own text, from the
      // same formatter the host's generic panel uses.
      gfx::Label* value = &k.value;
      if (gfx::SliderAttachment* binding = knobAttachments_[i]->binding()) {
        binding->onDisplayChanged = [value](const std::string& text) { value->setText(text); };
        binding->sync();
      }
    }

    shapeLabel_.setText("Shape");
    shapeLabel_.setJustify(gfx::Justify::Right);
    shape_.setTooltip("The curve: three built in, or the expression below");
    shape_.onOpenMenu = [this](gfx::ComboBox& box) { openComboMenu(box); };
    autoGainLabel_.setText("Auto gain");
    autoGainLabel_.setJustify(gfx::Justify::Right);
    autoGain_.setTooltip("Take half the drive back off the output");
    addChild(&shapeLabel_);
    addChild(&shape_);
    addChild(&autoGainLabel_);
    addChild(&autoGain_);
    shapeAttachment_.reset(new APVTS::ComboBoxAttachment(state_, "shape", shape_));
    autoGainAttachment_.reset(new APVTS::ButtonAttachment(state_, "autogain", autoGain_));

    exprLabel_.setText("Expression  f(x) =");
    code_.setLineNumbersShown(false);
    code_.setTooltip("Any function of x: tanh, sgn, clamp, sin, abs, pow, min, max... "
                     "It plays when Shape is Expression, and typing switches to it.");
    // Filled BEFORE the callback is set: opening the editor is not an edit.
    code_.loadContent(dsp_.expression);
    code_.onTextChange = [this]() { expressionEdited(); };
    addChild(&exprLabel_);
    addChild(&code_);

    curveView_.setTooltip("The curve the signal goes through; the band is where it is now");
    addChild(&curveView_);

    accentLabel_.setText("Accent");
    accent_.setTooltip("The colour the editor is drawn in");
    accent_.setCurrentColour(gfx::Colour::fromARGB(dsp_.accent), false);
    accent_.onChange = [this](gfx::Colour c) {
      dsp_.accent = c.toARGB();
      applyLook();
    };
    addChild(&accentLabel_);
    addChild(&accent_);

    addChild(&about_); // hidden until Help > About

    seenSerial_ = dsp_.stateSerial;
    curveView_.curve = &dsp_.displayCurve(currentShape());
    applyLook();
    setSize(780.0f, 500.0f); // what the wrapper replaces with the host's size
  }

  ~ShowcaseEditor() override {
    // The attachments are destroyed after this body, and any gesture they
    // still hold open is closed then: with no host to tell.
    state_.disconnectFromHost();
  }

  ShowcaseEditor(const ShowcaseEditor&) = delete;
  ShowcaseEditor& operator=(const ShowcaseEditor&) = delete;

  // ── A frame ─────────────────────────────────────────────────────────────

  void editorTick(double) override {
    state_.syncFromHost();
    presetBar_.tick();
    // A session the host loaded while the editor was open: its curve and
    // colours, not the ones on screen.
    if (dsp_.stateSerial != seenSerial_) {
      seenSerial_ = dsp_.stateSerial;
      loading_ = true;
      code_.loadContent(dsp_.expression);
      loading_ = false;
      statusOk_ = true;
      accent_.setCurrentColour(gfx::Colour::fromARGB(dsp_.accent), false);
      applyLook();
    }
    const CurveTable* playing = &dsp_.displayCurve(currentShape());
    if (playing != curveView_.curve) {
      curveView_.curve = playing;
      curveView_.repaint();
      repaint(statusArea_); // which says whether the typed curve is the one playing
    }
    // The audio thread's peak since the last frame, falling back slowly so
    // the band reads as a level rather than flicker.
    const float peak = dsp_.drivenPeak.exchange(0.0f, std::memory_order_relaxed);
    const float shown = (std::max)(peak, curveView_.signal * 0.85f);
    if (std::fabs(shown - curveView_.signal) > 0.01f) {
      curveView_.signal = shown;
      curveView_.repaint();
    }
  }

  // ── Drawing and layout ──────────────────────────────────────────────────

  void paint(gfx::Graphics& g) override {
    const gfx::Rect r = localBounds();
    g.fillAll(look_.background());
    if (background_) g.drawImage(*background_, r);
    // The backdrop under a wash of the scheme's own window colour, so a light
    // scheme is light and the photograph is only texture.
    g.fillAll(look_.background().withMultipliedAlpha(isDark() ? 0.35f : 0.82f));

    g.setColour(look_.panel().withMultipliedAlpha(0.72f));
    g.fillRoundedRect(knobPanel_, 10.0f);
    g.fillRoundedRect(exprPanel_, 10.0f);
    g.fillRoundedRect(sidePanel_, 10.0f);

    if (!logo_.isEmpty()) logo_.draw(g, logoArea_, look_.accent());

    // The compiler's verdict, in the colour of a verdict.
    g.setColour(statusOk_ ? look_.text().withMultipliedAlpha(0.7f) : gfx::Colour::fromRGB(0xff6b6b));
    statusFont().drawIn(g, statusText(), statusArea_, gfx::Justify::Left);
  }

  /** The status line's 12 px font, made once and kept, as font.h asks. A
   *  withHeight() in paint() is a Font with empty caches every time: each
   *  repaint -- every frame the curve's level band moved -- rasterised the
   *  whole line's glyphs again, a sixth of the frame, even where the line
   *  was clipped away. Remade if the editor's typeface changes (the window
   *  sets one when the constructor found none). */
  const gfx::Font& statusFont() {
    if (statusFont_.typeface() != font().typeface()) statusFont_ = font().withHeight(12.0f);
    return statusFont_;
  }

  void resized() override {
    const gfx::Rect r = localBounds();
    const float pad = 16.0f;
    const float strip = presetBar_.manager() ? kMenuHeightWithPresets : kMenuHeight;
    if (presetBar_.manager()) {
      const float names = (std::min)(kMenuNamesWidth, r.w * 0.4f);
      menuBar_.setBounds({0.0f, 0.0f, names, strip});
      presetBar_.setBounds({names, 0.0f, r.w - names, strip});
    } else {
      menuBar_.setBounds({0.0f, 0.0f, r.w, strip});
    }
    const float top = strip + 10.0f;
    logoArea_ = gfx::Rect(pad, top, 290.0f, 48.0f);

    // The header's right: the shape and the auto-gain switch.
    float x = r.w - pad - 64.0f;
    autoGain_.setBounds({x, top + 11.0f, 64.0f, 26.0f});
    autoGainLabel_.setBounds({x - 84.0f, top + 11.0f, 78.0f, 26.0f});
    x -= 84.0f + 12.0f + 140.0f;
    shape_.setBounds({x, top + 11.0f, 140.0f, 26.0f});
    shapeLabel_.setBounds({x - 56.0f, top + 11.0f, 50.0f, 26.0f});

    const float bodyTop = top + 62.0f;
    const float leftW = (r.w - pad * 3.0f) * 0.56f;
    // The knobs take what they need up to 170 and the code editor the rest,
    // so a host that makes the editor short squeezes the knobs a little
    // before the place to type disappears.
    const float knobsH = (std::max)(130.0f, (std::min)(170.0f, (r.h - bodyTop - pad * 2.0f) * 0.55f));
    knobPanel_ = gfx::Rect(pad, bodyTop, leftW, knobsH);
    const float cell = knobPanel_.w / 4.0f;
    const float knob = (std::min)(84.0f, knobsH - 58.0f);
    for (int i = 0; i < 4; ++i) {
      const float cx = knobPanel_.x + cell * (float) i;
      knobs_[i].name.setBounds({cx, knobPanel_.y + 8.0f, cell, 18.0f});
      knobs_[i].slider.setBounds({cx + (cell - knob) * 0.5f, knobPanel_.y + 30.0f, knob, knob});
      knobs_[i].value.setBounds({cx, knobPanel_.y + 34.0f + knob, cell, 18.0f});
    }

    exprPanel_ = gfx::Rect(pad, knobPanel_.bottom() + pad, leftW, r.h - knobPanel_.bottom() - pad * 2.0f);
    exprLabel_.setBounds({exprPanel_.x + 12.0f, exprPanel_.y + 8.0f, exprPanel_.w - 24.0f, 18.0f});
    code_.setBounds({exprPanel_.x + 12.0f, exprPanel_.y + 30.0f, exprPanel_.w - 24.0f, exprPanel_.h - 62.0f});
    statusArea_ = gfx::Rect(exprPanel_.x + 12.0f, exprPanel_.bottom() - 28.0f, exprPanel_.w - 24.0f, 20.0f);

    const float rx = pad * 2.0f + leftW, rw = r.w - rx - pad;
    curveView_.setBounds({rx, bodyTop, rw, knobPanel_.h});
    sidePanel_ = gfx::Rect(rx, exprPanel_.y, rw, exprPanel_.h);
    accentLabel_.setBounds({rx + 12.0f, sidePanel_.y + 8.0f, rw - 24.0f, 18.0f});
    accent_.setBounds({rx + 8.0f, sidePanel_.y + 28.0f, rw - 16.0f, sidePanel_.h - 36.0f});
    if (about_.isShowing()) about_.coverParent();
  }

  // ── The menus (MenuBarModel) ────────────────────────────────────────────

  enum MenuIds {
    kPresetBase = 100,
    kCurveBase = 200,
    kUndo = 300,
    kRedo,
    kSchemeBase = 400,
    kResetAccent = 410,
    kAbout = 500,
  };

  std::vector<std::string> getMenuBarNames() override { return {"&Presets", "&Edit", "&View", "&Help"}; }

  gfx::PopupMenu getMenuForIndex(int index, const std::string&) override {
    gfx::PopupMenu menu;
    if (index == 0) {
      // The preset bar's own menu -- save, rename, delete, both lists,
      // import, export, the folder -- when there is a manager behind it.
      if (presetBar_.manager()) {
        menu = presetBar_.buildMenu();
      } else {
        menu.addSectionHeader("Factory");
        for (int i = 0; i < kNumPresets; ++i) menu.addItem(kPresetBase + i, kPresets[i].name);
      }
      menu.addSeparator();
      menu.addSectionHeader("Curves");
      for (int i = 0; i < kNumExampleCurves; ++i)
        menu.addItem(kCurveBase + i, kExampleCurves[i].name, true, dsp_.expression == kExampleCurves[i].text);
    } else if (index == 1) {
      // Built when the menu opens, so the items say what there is to undo now.
      const sonore::UndoHistory* undo = state_.getUndoManager();
      menu.addItem(kUndo, state_.canUndo() ? "Undo " + undo->undoLabel() : std::string("Undo"), state_.canUndo());
      menu.addItem(kRedo, state_.canRedo() ? "Redo " + undo->redoLabel() : std::string("Redo"), state_.canRedo());
    } else if (index == 2) {
      for (int i = 0; i < 4; ++i) menu.addItem(kSchemeBase + i, kSchemeNames[i], true, dsp_.scheme == i);
      menu.addSeparator();
      menu.addItem(kResetAccent, "Reset accent colour");
    } else {
      menu.addItem(kAbout, "About Sonore Showcase...");
    }
    return menu;
  }

  void menuItemSelected(int id, int) override {
    if (presetBar_.perform(id)) return; // its ids are above all of these
    if (id >= kPresetBase && id < kPresetBase + kNumPresets) applyPreset(kPresets[id - kPresetBase]);
    else if (id >= kCurveBase && id < kCurveBase + kNumExampleCurves) loadCurve(kExampleCurves[id - kCurveBase].text);
    else if (id == kUndo) state_.undo();
    else if (id == kRedo) state_.redo();
    else if (id >= kSchemeBase && id < kSchemeBase + 4) {
      dsp_.scheme = id - kSchemeBase;
      applyLook();
    } else if (id == kResetAccent) {
      dsp_.accent = 0xff42a2c8;
      accent_.setCurrentColour(gfx::Colour::fromARGB(dsp_.accent), false);
      applyLook();
    } else if (id == kAbout) {
      about_.show("Sonore Showcase",
                  "An oversampled waveshaper whose curve you can type.\n"
                  "Native editor, parameters, menus and drawing: the Sonore SDK.",
                  {"OK"}, [](int) {});
    }
  }

  // ── For the integration test, which drives the editor as a user would ───

  gfx::MenuBarComponent& menuBar() { return menuBar_; }
  gfx::PresetBar& presetBar() { return presetBar_; }
  gfx::Slider& knob(int index) { return knobs_[index].slider; }
  gfx::CodeEditorComponent& codeEditor() { return code_; }
  gfx::ColourSelector& accentSelector() { return accent_; }
  CurveView& curveView() { return curveView_; }
  const gfx::LookAndFeel_V4& look() const { return look_; }
  bool statusIsOk() const { return statusOk_; }
  /** What the line under the code editor says. */
  std::string statusText() const {
    if (!statusOk_) return error_;
    return currentShape() == 3 ? "Compiled, and playing." : "Compiled. Set Shape to Expression to hear it.";
  }

private:
  struct Knob {
    gfx::Label name, value;
    gfx::Slider slider;
  };

  bool isDark() const { return look_.background().brightness() < 0.5f; }

  int currentShape() const {
    return shapeValue_ ? (int) std::lround(shapeValue_->load(std::memory_order_relaxed)) : 0;
  }

  /** The scheme from the View menu with the accent from the selector, and
   *  the code editor -- which has colours of its own -- told to match. */
  void applyLook() {
    using Scheme = gfx::LookAndFeel_V4::ColourScheme;
    Scheme scheme = schemeAt(dsp_.scheme);
    const gfx::Colour accent = gfx::Colour::fromARGB(dsp_.accent);
    scheme.setUIColour(Scheme::defaultFill, accent);
    scheme.setUIColour(Scheme::highlightedFill, accent.darkened(0.2f));
    look_.setColourScheme(scheme);

    const gfx::Colour text = scheme.getUIColour(Scheme::defaultText);
    gfx::CodeEditorComponent::Colours colours;
    // A shade off the panel it sits on, so the place to type can be found.
    const gfx::Colour field = scheme.getUIColour(Scheme::widgetBackground);
    colours.background = isDark() ? field.interpolatedTo(gfx::Colour::fromRGB(0x000000), 0.3f) : field;
    colours.defaultText = text;
    colours.highlight = accent.withMultipliedAlpha(0.35f);
    colours.caret = accent;
    code_.setColours(colours);
    gfx::CodeEditorComponent::ColourScheme tokens = tokeniser_.getDefaultColourScheme();
    const gfx::Colour quiet = text.withMultipliedAlpha(0.6f);
    tokens.set("Identifier", text);
    tokens.set("Integer", accent);
    tokens.set("Float", accent);
    tokens.set("Operator", quiet);
    tokens.set("Bracket", quiet);
    tokens.set("Punctuation", quiet);
    tokens.set("Error", gfx::Colour::fromRGB(0xff6b6b));
    code_.setColourScheme(tokens);
    repaint();
  }

  /** A factory preset, as ONE edit: every parameter's gesture is open while
   *  the values change, so the host records one automation edit and the
   *  APVTS one undo step, not six. */
  void applyPreset(const sonore::Preset& preset) {
    const int n = (std::min)(preset.numValues, state_.numParameters());
    for (int i = 0; i < n; ++i) state_.beginGesture(i);
    for (int i = 0; i < n; ++i) state_.setParameterValue(i, preset.values[i]);
    for (int i = n - 1; i >= 0; --i) state_.endGesture(i);
  }

  void loadCurve(const char* text) {
    code_.loadContent(text);
    expressionEdited(); // whether or not loading the text already said so
  }

  /** The user typed: compile, and on success make sure they HEAR it. */
  void expressionEdited() {
    const std::string text = code_.getDocument().getAllContent();
    std::string why;
    statusOk_ = dsp_.setExpression(text, &why);
    error_ = why;
    if (statusOk_ && !loading_ && currentShape() != 3) {
      // Typing a curve while another shape plays would be typing into
      // silence. One gesture, so the host and undo see one edit.
      state_.beginGesture(kShape);
      state_.setParameterValue(kShape, 3.0f);
      state_.endGesture(kShape);
    }
    curveView_.repaint();
    repaint(statusArea_);
  }

  SonoreDsp& dsp_;
  APVTS& state_;
  std::atomic<float>* shapeValue_ = nullptr;
  gfx::LookAndFeel_V4 look_;
  gfx::ImageCache::Image background_;
  gfx::Drawable logo_;
  gfx::Font statusFont_;

  gfx::MenuBarComponent menuBar_{this};
  gfx::PresetBar presetBar_;
  Knob knobs_[4];
  gfx::Label shapeLabel_, autoGainLabel_, exprLabel_, accentLabel_;
  gfx::ComboBox shape_;
  gfx::Button autoGain_;
  gfx::CodeDocument document_;
  gfx::CPlusPlusCodeTokeniser tokeniser_; // numbers, names, operators: arithmetic is C
  gfx::CodeEditorComponent code_{document_, &tokeniser_};
  CurveView curveView_;
  gfx::ColourSelector accent_{gfx::ColourSelector::showColourAtTop | gfx::ColourSelector::editableColour |
                              gfx::ColourSelector::showColourspace};
  gfx::AlertOverlay about_;

  // Declared after the widgets they bind, so they are destroyed first.
  std::unique_ptr<APVTS::SliderAttachment> knobAttachments_[4];
  std::unique_ptr<APVTS::ComboBoxAttachment> shapeAttachment_;
  std::unique_ptr<APVTS::ButtonAttachment> autoGainAttachment_;

  gfx::Rect logoArea_, knobPanel_, exprPanel_, sidePanel_, statusArea_;
  std::string error_;
  bool statusOk_ = true;
  bool loading_ = false;
  int seenSerial_ = 0;
};

} // namespace showcase
