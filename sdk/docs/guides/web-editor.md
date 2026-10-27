# The web editor

A plugin's face as an HTML page, shown in the operating system's webview and
wired to the plugin through `window.sonore`. The bridge is the same one the
browser preview in [Sonorie](https://sonorie.com)'s studio provides, so a page
approved there opens unchanged in a DAW.

## Supplying the page

Before the last include, define `SONORE_UI_HTML` as the whole HTML document,
and `SONORE_UI_WIDTH` and `SONORE_UI_HEIGHT` as its size (700 by 420 when left
out). A plugin with a page opens it in the webview, as long as
`PluginDescriptor::editor` is left at `sonore::EditorKind::Auto`.

```cpp
// test-run
#define SONORE_NUM_PARAMS 2
#include <sonore/dsp.h>

struct SonoreDsp {
  sonore::Smooth driveSm;

  void prepare(const sonore::ProcessSpec& spec) {
    driveSm.setup((float) spec.sampleRate, 12.0f);
    driveSm.snap(1.0f);
  }

  void process(sonore::AudioBlock<float>& io, const float* p) {
    const float out = sonore::dbToGain(p[1]);
    for (size_t i = 0; i < io.getNumSamples(); ++i) {
      const float drive = driveSm.next(p[0]);
      for (size_t c = 0; c < io.getNumChannels(); ++c) {
        float& x = io.getChannelPointer(c)[i];
        x = std::tanh(x * drive) * out;
      }
    }
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"drive", "Drive", "x", 1.0f, 20.0f, 2.0f},
    {"output", "Output", "dB", -24.0f, 12.0f, 0.0f},
};

static const sonore::PluginDescriptor kDesc = {
    "com.mycompany.webdrive", "Web Drive", "My Company", "1.0.0",
    "A drive with a page for a face.", "",
    false, kParamTable, SONORE_NUM_PARAMS,
};

#define SONORE_UI_WIDTH 420
#define SONORE_UI_HEIGHT 160
#define SONORE_UI_HTML R"HTML(<!doctype html><html><head><meta charset="utf-8">
<style>body{margin:0;padding:16px;font:13px system-ui,sans-serif;background:#16181c;color:#e6e6e6}
label{display:flex;gap:12px;align-items:center;margin:8px 0}input{flex:1}</style></head>
<body><div id="rows"></div><script>
window.addEventListener('sonore:ready', function () {
  var rows = document.getElementById('rows');
  sonore.params.forEach(function (p, i) {
    var label = document.createElement('label');
    var name = document.createElement('span'); name.textContent = p.label;
    var input = document.createElement('input');
    input.type = 'range'; input.min = 0; input.max = 1; input.step = 0.001;
    var text = document.createElement('span');
    var show = function (v) {
      input.value = sonore.toNormalised(i, v);
      text.textContent = sonore.format(i, v); // the host's own text
    };
    // A drag is one gesture: one automation pass, one undo step in the host.
    input.addEventListener('pointerdown', function () { sonore.begin(i); });
    input.addEventListener('input', function () {
      sonore.set(i, sonore.fromNormalised(i, Number(input.value)));
      text.textContent = sonore.format(i);
    });
    input.addEventListener('change', function () { sonore.end(i); });
    label.appendChild(name); label.appendChild(input); label.appendChild(text);
    rows.appendChild(label);
    show(p.value);
    // Every change, from this page or from the host's automation.
    sonore.on(function (values, index, value) { if (index === i) show(value); });
  });
});
</script></body></html>)HTML"

#include <sonore/formats.h>
```

The page starts work on the `sonore:ready` event, which fires once the bridge
exists. A knob turned on the page sends the same parameter and gesture events a
host's own control does, so the host records it as automation. Values reach the
audio thread through a lock-free queue.

## What the page gets

| Member | |
| --- | --- |
| `sonore.name`, `sonore.vendor`, `sonore.params` | identity, and every parameter in the plugin's order: `id`, `label`, `unit`, `min`, `max`, `default`, `step`, `skew`, `readOnly`, `value`, `names`, `options` (a stepped control's positions; null for a continuous one), `curve` (`'log'` or `'linear'`), `group` |
| `get(p)`, `set(p, v)` | plain values; a parameter is named by index, id, label or the object itself |
| `begin(p)`, `end(p)` | a gesture around a drag |
| `on(cb)` | `cb(values, index, value)` on any change |
| `format(p, v)`, `toNormalised(p, v)`, `fromNormalised(p, n)` | the host's text, and its automation curve |
| `onLevel(cb)`, `onVU(cb)` | meters, up to about thirty times a second while the level moves (not a clock) |
| `onGR(cb)`, `gr` | gain reduction in positive dB, from a DSP that declares `float gainReduction() const`; 0 from one that does not |
| `onSpectrum(cb, {input})`, `spectrumFrame`, `sampleRate` | the output's spectrum, and the input's when `input` is true: `cb(out, input, sampleRate)`, 256 dBFS values each (`input` is null unless asked for) on a fixed log grid from 20 Hz to 20 kHz, up to about thirty times a second; see *A spectrum on the page* below |
| `noteOn(note, velocity)`, `noteOff(note)`, `onNotes(cb)` | on-screen keyboards for an instrument |
| `presets`, `loadPreset(i)` | factory presets by name, applied in the plugin |
| `chooseFile(purpose)`, `onFile(cb)` | a native file dialog; the path also reaches the DSP's `loadFile(purpose, path)` |
| `state`, `onState(cb)` | the plugin's state beyond its parameters, as strings |
| `contextMenu(p, x, y)` | the HOST's menu for a control: MIDI learn, automation |
| `captureKeys(on)` | take the keyboard for something that reads keys and is not a text field; a focused text field takes it by itself |

In an Audio Unit, `chooseFile` and `contextMenu` do nothing: no dialog opens,
`onFile` is never called, and AU gives a plugin no way to ask its host for a
menu. CLAP and VST3 do both.

The complete list, with types, is `sonore/sonore.d.ts`, installed with the
headers: reference it from TypeScript and the compiler checks the page against
it. `npm run verify:bridge-types` checks that file against the bridge `gui.h`
generates, member for member.

## A spectrum on the page

The plugin measures its spectrum only after the page asks. The first
`sonore.onSpectrum(cb)` turns on the output's feed (`sonore::SpectrumFeed` in
`spectrum_feed.h`), and `{input: true}` turns on the input's as well. A page
that never calls it costs the audio thread nothing.

Each frame has 256 values in dBFS. Point `i` is at
`20 * 1000^(i / 255)` Hz, from 20 Hz to 20 kHz. A full-scale sine that sits on a
bin reads 0 dBFS, and silence reads -140. The rule is fixed: the mono average
of the channels, a Blackman window about 170 ms long at every rate (8192
samples at 44.1 or 48 kHz), and each point shows the loudest bin inside its
band. The Sonorie studio's browser preview measures by the same rule, so an
analyzer looks the same there and in a DAW.

```js
const c = document.querySelector('canvas'), g = c.getContext('2d');
sonore.onSpectrum((out) => {
  g.clearRect(0, 0, c.width, c.height);
  g.beginPath();
  out.forEach((db, i) => {
    const x = i / 255 * c.width;
    const y = (12 - Math.max(-96, Math.min(12, db))) / 108 * c.height;
    i ? g.lineTo(x, y) : g.moveTo(x, y);
  });
  g.stroke();
});
```

The frames arrive only when the spectrum changes, so do not use them as a
clock. `sonore.spectrumFrame` holds the newest frame as `{out, in, sr}`. LV2 does not feed a
spectrum yet, because its interface is a separate module that sees only ports.

## Per platform

- **Windows**: Edge WebView2. Its SDK (BSD-3-Clause) is fetched from nuget.org
  when CMake configures, and linked statically. `SONORE_WEBVIEW2_DIR` (a CMake
  or environment variable) keeps the download where it survives the build, and
  `-DSONORE_GUI=OFF` leaves the webview out. On a machine without the Edge
  runtime, a CLAP or VST3 opens the native editor instead.
- **Linux**: GTK3 and WebKitGTK, loaded at run time, newest WebKit first. The
  plugin loads on distributions with either version, and builds with no GTK
  development packages installed.
- **macOS**: WKWebView, driven through the Objective-C runtime, so a project
  stays plain C++.

On Windows the page is served from memory at an address of the plugin's own,
`https://<plugin id>-<hash>.sonore.invalid/editor.html`, which never leaves the
process. It reloads like any page, and `localStorage` and IndexedDB work: kept
per plugin (shared by its instances, not by other plugins) and across sessions.
On Linux and macOS the page is loaded as a string with no address of its own,
so a page that has to run on every platform should not depend on `localStorage`
or on reloading itself.

Either way, keep anything the session must restore in a parameter or in the
plugin's own state (what its `saveState` writes; the page reads it as
`sonore.state` and cannot write it), not in the page's storage: a project moved
to another machine does not carry that.

Only embedded windows are supported: the page lives inside the host's plugin
window, never in a floating one.
