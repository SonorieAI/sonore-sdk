# The native editor

The interface the SDK draws itself, with no browser behind it, so it looks and
behaves the same on every machine.

## The one you get for free

A plugin that supplies no web page (no `SONORE_UI_HTML`) opens the generic
native editor: a row per parameter, folded into sections by each parameter's
`group`, a keyboard along the bottom for an instrument, and the preset bar
along the top where the host gives the plugin its presets. Nothing to write.

`PluginDescriptor::editor` overrides that choice: `sonore::EditorKind::Native`
uses the native editor even when the plugin has a page, and
`sonore::EditorKind::Web` asks for a webview without one. `PluginDescriptor::editorLimits` sets the smallest and largest
size (`minWidth`, `minHeight`, `maxWidth`, `maxHeight`) and whether either
direction resizes.

On Linux the native editor needs the X11 development headers at build time
(`libx11-dev` or your distribution's equivalent); a build without them uses
the webview instead.

## One of your own

Declare `createEditor` in `SonoreDsp` and return a
`sonore::gfx::AudioProcessorEditor`. Its life:

- `createEditor` is called on the main thread each time the host opens the
  editor.
- The window that holds the editor runs a 33 ms clock, and calls
  `editorTick()` before each repaint.
- When the host closes the window, the editor is destroyed.

The DSP outlives every editor it makes, so an editor may keep a reference to
it. Anything the editor hands to the audio thread still goes through atomics.

The widgets work in 0..1 and know nothing about parameters. An attachment binds
one to a parameter, through the `sonore::gfx::EditorHost` the wrapper passes
in:

- `sonore::gfx::SliderAttachment` binds a slider or a knob,
- `sonore::gfx::ButtonAttachment` binds a button to a two-state parameter,
- `sonore::gfx::ComboAttachment` binds a combo box to a stepped one.

A drag is one gesture: one automation edit and one undo step in the host.
`sync()` pulls the host's value back, so the knob follows automation and preset
loads.

```cpp
// test-run
#define SONORE_NUM_PARAMS 2
#include <sonore/dsp.h>
#include <sonore/gfx/lookandfeel_versions.h>
#include <sonore/gfx/plugin_editor.h>
#include <sonore/gfx/preset_bar.h>

#include <memory>
#include <string>

namespace gfx = sonore::gfx;

struct SonoreDsp {
  sonore::Smooth gainSm, panSm;

  void prepare(const sonore::ProcessSpec& spec) {
    gainSm.setup((float) spec.sampleRate, 20.0f);
    panSm.setup((float) spec.sampleRate, 20.0f);
    gainSm.snap(1.0f);
    panSm.snap(0.0f);
  }

  void process(sonore::AudioBlock<float>& io, const float* p) {
    if (io.getNumChannels() < 2) return;
    float* L = io.getChannelPointer(0);
    float* R = io.getChannelPointer(1);
    const float gain = sonore::dbToGain(p[0]);
    for (size_t i = 0; i < io.getNumSamples(); ++i) {
      const float g = gainSm.next(gain), pan = panSm.next(p[1]);
      L[i] *= g * (pan > 0.0f ? 1.0f - pan : 1.0f);
      R[i] *= g * (pan < 0.0f ? 1.0f + pan : 1.0f);
    }
  }

  // [main thread] The editor; defined below, once the parameter table exists.
  std::unique_ptr<gfx::AudioProcessorEditor> createEditor(const gfx::EditorHost& host);
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"gain", "Gain", "dB", -24.0f, 12.0f, 0.0f},
    {"pan", "Pan", "", -1.0f, 1.0f, 0.0f},
};

class TrimEditor : public gfx::AudioProcessorEditor {
public:
  explicit TrimEditor(const gfx::EditorHost& host) : presets_(host.presets) {
    // Set once here, inherited by every child: a scheme change is one call.
    look_.setColourScheme(gfx::LookAndFeel_V4::getMidnightColourScheme());
    setLookAndFeel(&look_);

    for (int i = 0; i < SONORE_NUM_PARAMS; ++i) {
      Knob& k = knobs_[i];
      k.name.setText(kParamTable[i].label);
      k.name.setJustify(gfx::Justify::Centred);
      k.value.setJustify(gfx::Justify::Centred);
      addChild(&k.name);
      addChild(&k.slider);
      addChild(&k.value);
      k.binding.reset(new gfx::SliderAttachment(k.slider, kParamTable[i], i, host));
      // The text under the knob is the host's own text for the value.
      gfx::Label* value = &k.value;
      k.binding->onDisplayChanged = [value](const std::string& text) { value->setText(text); };
      k.binding->sync();
    }
    // Null where the format offers no presets: then there is no bar.
    if (presets_.manager()) addChild(&presets_);
    setSize(360.0f, 240.0f); // the wrapper then gives it the host's size
  }

  void editorTick(double) override {
    for (Knob& k : knobs_) k.binding->sync(); // automation, a preset the host loaded
    presets_.tick();
  }

  void paint(gfx::Graphics& g) override { g.fillAll(lookAndFeel().background()); }

  void resized() override {
    const gfx::Rect r = localBounds();
    float top = 0.0f;
    if (presets_.manager()) {
      presets_.setBounds({0.0f, 0.0f, r.w, gfx::PresetBar::kHeight});
      top = gfx::PresetBar::kHeight;
    }
    const float w = r.w / SONORE_NUM_PARAMS;
    for (int i = 0; i < SONORE_NUM_PARAMS; ++i) {
      const float x = w * (float) i;
      knobs_[i].name.setBounds({x, top + 8.0f, w, 20.0f});
      knobs_[i].slider.setBounds({x + 16.0f, top + 32.0f, w - 32.0f, r.h - top - 72.0f});
      knobs_[i].value.setBounds({x, r.h - 36.0f, w, 20.0f});
    }
  }

private:
  struct Knob {
    gfx::Label name, value;
    gfx::Slider slider{gfx::Slider::Style::Rotary};
    std::unique_ptr<gfx::SliderAttachment> binding;
  };
  // Declared first, destroyed last: the children draw with it.
  gfx::LookAndFeel_V4 look_;
  gfx::PresetBar presets_;
  Knob knobs_[SONORE_NUM_PARAMS];
};

inline std::unique_ptr<gfx::AudioProcessorEditor> SonoreDsp::createEditor(const gfx::EditorHost& host) {
  return std::unique_ptr<gfx::AudioProcessorEditor>(new TrimEditor(host));
}

static const sonore::PluginDescriptor kDesc = {
    "com.mycompany.trimeditor", "Trim Editor", "My Company", "1.0.0",
    "A gain and pan with an editor of its own.", "",
    false, kParamTable, SONORE_NUM_PARAMS,
};

#include <sonore/formats.h>
```

## Widgets

`gfx/widgets.h` has the basics: `sonore::gfx::Slider` (rotary, linear and bar
styles), `sonore::gfx::Button`, `sonore::gfx::Label` and
`sonore::gfx::ComboBox`. The other headers under `gfx/` add:

- text fields, tables and tree views,
- menus, a menu bar and toolbars,
- a colour selector, a code editor and an on-screen keyboard,
- images: PNG, JPEG, GIF, and SVG through `sonore::gfx::Drawable`,
- layout helpers: `sonore::gfx::FlexBox` and `sonore::gfx::Grid`.

Each header's comment says what it is for, and the
[API reference](../index.md) lists them under *Native GUI*.

A `ComboBox` opens its list through the editor: set
`box.onOpenMenu = [this](gfx::ComboBox& b) { openComboMenu(b); };`, and the
window drops it down as a real menu.

## LookAndFeel

Appearance lives in a `sonore::gfx::LookAndFeel`, separately from behaviour.
`setLookAndFeel()` on a widget applies to it and everything inside it.
`sonore::gfx::LookAndFeel_V4` draws from nine colours, with four built-in
schemes: `getDarkColourScheme()`, `getMidnightColourScheme()`,
`getGreyColourScheme()` and `getLightColourScheme()`. For a look of your own,
derive from `LookAndFeel` and override the `draw...` methods you want, such as
`drawRotarySlider`, `drawButton` and `drawLinearSlider`.

## The preset bar

`sonore::gfx::PresetBar` is the strip the generic editor shows: the preset's
name (marked when the sound has changed since it was loaded), previous and
next, Save, and a menu with Save As, Rename, Delete, Import, Export and Show
Folder. Construct it with `host.presets` and call `tick()` each frame, as above.
An editor with an undo history can make each load one undo step through
`wrapLoad`. [Presets](presets.md) covers what it manages.

## Where it appears

- **CLAP, VST3 and AU** open the editor inside the host's window.
- **The standalone application** shows the webview instead.
- **LV2** shows the webview on Windows. On Linux and macOS an LV2 build
  promises no interface of its own, and the host shows its generic controls.

The showcase example (`sdk/examples/showcase/`) is a larger editor: menus, a
code editor, an SVG logo, a colour selector and the APVTS attachments.
`sdk/tests/showcase_test.cpp` drives it with no window at all: an editor is a
widget, and it renders into a bitmap.
