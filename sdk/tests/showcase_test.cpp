// SPDX-License-Identifier: Apache-2.0
// The showcase example, exercised the way a user and a host would -- headless.
//
// The example is documentation, and documentation that does not run is the
// kind that rots. So this compiles the plugin's own source into itself (as
// rt_safety_test does) and checks what the example claims, in numbers and in
// pixels:
//
//   - the APVTS layout IS the table the descriptor hands every format;
//   - a typed curve reaches the audio thread, and only once it compiles;
//   - the latency the plugin reports is the delay it has;
//   - the editor renders: the JPEG backdrop, the SVG logo in the accent, the
//     curve, the knobs -- and the View menu, the ColourSelector, a drag, the
//     undo menu and a preset all do what a user would see them do;
//   - automation and a session the host loads reach an open editor.
//
//   showcase_test [--bmp <prefix>]   writes <prefix>_dark.bmp and _light.bmp
#include "../examples/showcase/plugin.cpp"

#include <sonore/gfx/bitmap.h>
#include <sonore/gfx/component.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_checks = 0, g_failures = 0;
void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

using namespace sonore;
using namespace sonore::gfx;

const float kPi = 3.14159265358979f;

/** The default parameter values, from the descriptor's own table. */
std::vector<float> defaults() {
  std::vector<float> v;
  for (int i = 0; i < kDesc.numParams; ++i) v.push_back(kDesc.params[i].defaultValue);
  return v;
}

/** Run `dsp` over a sine; `out` gets the output's left channel. */
void render(SonoreDsp& dsp, const std::vector<float>& p, int frames, float amplitude, std::vector<float>& in,
            std::vector<float>& out, int phaseStart = 0) {
  const int block = 256;
  std::vector<float> l((size_t) block), r((size_t) block);
  in.clear();
  out.clear();
  for (int done = 0; done < frames; done += block) {
    for (int i = 0; i < block; ++i) {
      const float s = amplitude * std::sin(2.0f * kPi * 1000.0f * (float) (phaseStart + done + i) / 48000.0f);
      l[(size_t) i] = r[(size_t) i] = s;
      in.push_back(s);
    }
    float* ch[2] = {l.data(), r.data()};
    AudioBlock<float> io(ch, 2, (size_t) block);
    dsp.process(io, p.data());
    out.insert(out.end(), l.begin(), l.end());
  }
}

/** The largest |out[i] - scale * in[i - delay]| past the settling time. */
float worstError(const std::vector<float>& in, const std::vector<float>& out, int delay, float scale) {
  float worst = 0.0f;
  for (size_t i = 4000; i < out.size(); ++i) {
    const float e = std::fabs(out[i] - scale * in[i - (size_t) delay]);
    if (e > worst) worst = e;
  }
  return worst;
}

bool similar(Colour a, Colour b, int tolerance) {
  return std::abs((int) a.r - (int) b.r) <= tolerance && std::abs((int) a.g - (int) b.g) <= tolerance &&
         std::abs((int) a.b - (int) b.b) <= tolerance;
}

Colour pixel(const Bitmap& bmp, float x, float y) {
  const PremulColour p = bmp.pixelAt((int) x, (int) y);
  return Colour(p.r, p.g, p.b, p.a); // opaque everywhere the editor paints
}

/** Pixels in `area` within `tolerance` of `colour`. */
int count(const Bitmap& bmp, const Rect& area, Colour colour, int tolerance) {
  int n = 0;
  for (int y = (int) area.y; y < (int) area.bottom(); ++y)
    for (int x = (int) area.x; x < (int) area.right(); ++x)
      if (similar(pixel(bmp, (float) x, (float) y), colour, tolerance)) ++n;
  return n;
}

Bitmap renderEditor(Component& editor) {
  Bitmap bmp((int) editor.bounds().w, (int) editor.bounds().h);
  Graphics g(bmp);
  editor.paintTree(g);
  return bmp;
}

void writeBmp(const Bitmap& bmp, const std::string& path) {
  const int w = bmp.width(), h = bmp.height();
  std::vector<unsigned char> body((size_t) w * (size_t) h * 4u);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const PremulColour p = bmp.pixelAt(x, y);
      unsigned char* d = &body[((size_t) (h - 1 - y) * (size_t) w + (size_t) x) * 4u];
      d[0] = p.b;
      d[1] = p.g;
      d[2] = p.r;
      d[3] = 255;
    }
  unsigned char header[54] = {'B', 'M'};
  auto put32 = [&header](int at, uint32_t v) {
    for (int i = 0; i < 4; ++i) header[at + i] = (unsigned char) (v >> (8 * i));
  };
  put32(2, 54u + (uint32_t) body.size());
  put32(10, 54);
  put32(14, 40);
  put32(18, (uint32_t) w);
  put32(22, (uint32_t) h);
  header[26] = 1;
  header[28] = 32;
  put32(34, (uint32_t) body.size());
  if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(header, 1, sizeof(header), f);
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
  }
}

} // namespace

int main(int argc, char** argv) {
  std::string bmpPrefix;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--bmp") == 0) bmpPrefix = argv[i + 1];

  std::printf("showcase: the parameters\n");
  {
    check(kDesc.params == showcase::parameterTable().parameterInfos() && kDesc.numParams == SONORE_NUM_PARAMS,
          "the descriptor's table is the APVTS layout's");
    check(showcase::parameterTable().layoutProblem() == nullptr, "and the layout has nothing ParamInfo cannot carry");
    SonoreDsp dsp;
    check(dsp.parameters.tableMismatch(kDesc.params, kDesc.numParams) == nullptr,
          "the DSP's own APVTS agrees with the descriptor, id for id");
    check(std::strcmp(kDesc.params[showcase::kShape].valueNames[3], "Expression") == 0 &&
              kDesc.params[showcase::kShape].stepCount == 4,
          "the shape choice reaches the host as four named steps");
    check(std::fabs(sonore::toPlainValue(kDesc.params[showcase::kTone], 0.5) - 3000.0) < 1.0,
          "the tone knob's middle is 3 kHz, the skew the layout set");
  }

  std::printf("showcase: the curve compiler\n");
  {
    showcase::CurveTable t;
    std::string error;
    check(showcase::compileCurve("x", t, error) && std::fabs(t.lookup(0.5f) - 0.5f) < 1e-4f,
          "f(x) = x samples to the identity");
    check(t.lookup(100.0f) == 4.0f && t.lookup(-100.0f) == -4.0f, "held at its ends, and limited to +-4");
    const float before = t.lookup(0.25f);
    check(!showcase::compileCurve("tanh(", t, error) && !error.empty() && t.lookup(0.25f) == before,
          "a half-typed curve is refused with a reason, and the table kept");
    check(!showcase::compileCurve("y * 2", t, error) && error.find("y") != std::string::npos,
          "an unknown name is named");
    check(!showcase::compileCurve("log(x)", t, error), "a curve that is not a number somewhere is refused");
    check(showcase::compileCurve("clamp(x, -1, 1) + sgn(x) * 0", t, error) && t.lookup(3.0f) == 1.0f,
          "clamp and sgn, the functions the scope adds");

    showcase::CurveExchange exchange(t);
    const showcase::CurveTable* first = &exchange.acquire();
    showcase::compileCurve("-x", exchange.back(), error);
    check(&exchange.back() != first, "the writer never fills the table the reader holds");
    exchange.publish();
    const showcase::CurveTable& next = exchange.acquire();
    check(&next != first && std::fabs(next.lookup(0.5f) + 0.5f) < 1e-4f, "a published curve is the next one read");
    check(&exchange.acquire() == &next, "and stays until another is published");
  }

  std::printf("showcase: the DSP\n");
  {
    SonoreDsp dsp;
    ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 256;
    spec.numChannels = 2;
    dsp.prepare(spec);
    std::vector<float> p = defaults(), in, out;
    p[showcase::kDrive] = 0.0f;
    p[showcase::kShape] = 3.0f;
    p[showcase::kTone] = 20000.0f;
    p[showcase::kMix] = 100.0f;
    p[showcase::kOutput] = 0.0f;
    p[showcase::kAutoGain] = 0.0f;
    check(dsp.setExpression("x"), "f(x) = x compiles");
    render(dsp, p, 9600, 0.25f, in, out);
    const int latency = dsp.latencySamples();
    // The tone filter and the DC blocker add a little phase of their own at
    // 1 kHz, so "late by exactly the reported latency" is asked as: the
    // input lines up best at that delay, and closely.
    const float atLatency = worstError(in, out, latency, 1.0f);
    char msg[200];
    std::snprintf(msg, sizeof(msg),
                  "through f(x) = x the output is the input %d samples late (error %.4f; %.4f a sample either side)",
                  latency, atLatency, (std::min)(worstError(in, out, latency - 1, 1.0f), worstError(in, out, latency + 1, 1.0f)));
    check(latency == 24 && atLatency < 0.02f && atLatency < worstError(in, out, latency - 1, 1.0f) &&
              atLatency < worstError(in, out, latency + 1, 1.0f),
          msg);
    check(dsp.drivenPeak.load() > 0.2f && dsp.drivenPeak.load() < 0.3f, "the driven peak is published for the editor");

    check(dsp.setExpression("-x"), "a new curve compiles while audio runs");
    render(dsp, p, 9600, 0.25f, in, out, 9600);
    check(worstError(in, out, latency, -1.0f) < 0.02f, "and the next blocks play it: f(x) = -x inverts");
    check(!dsp.setExpression("-x +"), "a broken one does not compile");
    render(dsp, p, 9600, 0.25f, in, out, 19200);
    check(worstError(in, out, latency, -1.0f) < 0.02f, "and the curve that was playing keeps playing");

    p[showcase::kMix] = 0.0f;
    render(dsp, p, 9600, 0.25f, in, out, 28800);
    check(worstError(in, out, latency, 1.0f) < 0.01f, "mix 0 is the dry signal, delayed to line up with the wet");

    p[showcase::kMix] = 100.0f;
    p[showcase::kDrive] = 24.0f;
    float peak = 0.0f, softPeak = 0.0f;
    const char* names[3] = {"Soft", "Hard", "Fold"};
    for (int shape = 0; shape < 3; ++shape) {
      p[showcase::kShape] = (float) shape;
      render(dsp, p, 9600, 0.5f, in, out, 38400);
      peak = 0.0f;
      bool finite = true;
      for (size_t i = 4000; i < out.size(); ++i) {
        peak = (std::max)(peak, std::fabs(out[i]));
        finite = finite && std::isfinite(out[i]);
      }
      std::snprintf(msg, sizeof(msg), "%s at +24 dB drive stays bounded (peak %.3f)", names[shape], peak);
      check(finite && peak < 1.2f && peak > 0.3f, msg);
      if (shape == 0) softPeak = peak;
    }
    p[showcase::kAutoGain] = 1.0f;
    p[showcase::kShape] = 0.0f;
    render(dsp, p, 9600, 0.5f, in, out, 48000);
    float autoPeak = 0.0f;
    for (size_t i = 4000; i < out.size(); ++i) autoPeak = (std::max)(autoPeak, std::fabs(out[i]));
    std::snprintf(msg, sizeof(msg), "auto gain takes half the drive back off: 12 dB at +24 (%.3f against %.3f)",
                  autoPeak, softPeak);
    check(std::fabs(autoPeak / softPeak - sonore::dbToGain(-12.0f)) < 0.02f, msg);
  }

  std::printf("showcase: state beyond the parameters\n");
  {
    SonoreDsp a;
    a.setExpression("abs(x)");
    a.scheme = 2;
    a.accent = 0xffe0457b;
    StateBag bag;
    a.saveState(bag);
    SonoreDsp b;
    b.loadState(bag);
    check(b.expression == "abs(x)" && b.scheme == 2 && b.accent == 0xffe0457b && b.stateSerial == 1,
          "the curve, the scheme and the accent come back with the session");
    check(std::fabs(b.displayCurve(3).lookup(-0.5f) - 0.5f) < 1e-4f, "and the curve is compiled on the way in");
    // The fixed shapes are the module's, not each instance's: the same three
    // tables, compiled once and only ever read.
    check(&a.displayCurve(0) == &b.displayCurve(0) && &a.displayCurve(2) == &b.displayCurve(2) &&
              std::fabs(a.displayCurve(2).lookup(0.5f) - std::sin(0.5f)) < 1e-3f,
          "two instances play one copy of the built-in curves");
    StateBag broken;
    broken.setString("expression", "((((");
    b.loadState(broken);
    check(b.expression == showcase::kDefaultExpression, "a session with a curve that does not compile gets the default");
  }

  std::printf("showcase: the native editor, headless\n");
  {
    SonoreDsp dsp;
    std::vector<float> values = defaults();
    int sets = 0, begins = 0, ends = 0;
    EditorHost host;
    host.getParameter = [&](int i) { return values[(size_t) i]; };
    host.setParameter = [&](int i, float v) {
      values[(size_t) i] = v;
      ++sets;
    };
    host.beginGesture = [&](int) { ++begins; };
    host.endGesture = [&](int) { ++ends; };

    std::unique_ptr<AudioProcessorEditor> made = dsp.createEditor(host);
    auto* editor = static_cast<showcase::ShowcaseEditor*>(made.get());
    check(editor != nullptr && dsp.parameters.isConnectedToHost(), "createEditor makes one, connected to the host");
    editor->setBounds({0.0f, 0.0f, 780.0f, 500.0f});
    MouseRouter router(*editor);
    const bool text = editor->font().isValid();
    std::printf("  ---- text is %s ----\n", text ? "being drawn" : "ABSENT: no system typeface found");

    Bitmap dark = renderEditor(*editor);
    if (!bmpPrefix.empty()) writeBmp(dark, bmpPrefix + "_dark.bmp");
    const Colour accent = Colour::fromARGB(0xff42a2c8);
    check(similar(pixel(dark, 34.0f, 42.0f), accent, 40),
          "the SVG logo's badge is drawn in the accent");
    check(count(dark, Rect(80.0f, 44.0f, 170.0f, 18.0f), accent, 60) > 60 || !text,
          "and its <text> is laid out and drawn by the SDK, in the accent");
    const Rect curve = editor->curveView().bounds();
    check(count(dark, curve, accent, 50) > 150, "the curve view draws the playing curve");
    check(count(dark, Rect(16.0f, 96.0f, 410.0f, 170.0f), accent, 50) > 200, "the knobs' arcs are in the accent");
    // The backdrop shows between the panels: its lit top-left corner and its
    // dark bottom are different colours under the same wash.
    const Colour lit = pixel(dark, 6.0f, 30.0f), shade = pixel(dark, 774.0f, 494.0f);
    check(lit.brightness() > shade.brightness() + 0.02f, "the JPEG backdrop, through ImageCache, is under it all");

    // ── The View menu, through the menu bar, as a click would choose it ──
    editor->menuBar().showMenu(2);
    check(editor->menuBar().isMenuOpen(), "the View menu opens in the editor");
    editor->menuBar().menuOverlay().dismiss(showcase::ShowcaseEditor::kSchemeBase + 3);
    check(dsp.scheme == 3 && editor->look().background().brightness() > 0.5f,
          "choosing Light switches the LookAndFeel_V4 scheme");

    // ── The accent, from the ColourSelector ──
    editor->accentSelector().setCurrentColour(Colour::fromRGB(0xe0457b));
    check(dsp.accent == 0xffe0457b, "the ColourSelector's colour becomes the accent, kept in the state");
    Bitmap light = renderEditor(*editor);
    if (!bmpPrefix.empty()) writeBmp(light, bmpPrefix + "_light.bmp");
    check(pixel(light, 400.0f, 494.0f).brightness() > pixel(dark, 400.0f, 494.0f).brightness() + 0.3f,
          "and the editor is redrawn light");
    check(count(light, curve, Colour::fromRGB(0xe0457b), 50) > 150, "with the curve in the new accent");

    // ── A drag on the Drive knob: the attachment, the gesture, the host ──
    const Rect knob = editor->knob(0).bounds();
    const Point centre(knob.x + knob.w * 0.5f, knob.y + knob.h * 0.5f);
    const float driveBefore = values[showcase::kDrive];
    sets = begins = ends = 0;
    router.mouseDown(centre);
    router.mouseMove(Point(centre.x, centre.y - 60.0f));
    router.mouseUp(Point(centre.x, centre.y - 60.0f));
    char msg[200];
    std::snprintf(msg, sizeof(msg), "dragging Drive up raises it at the host (%.2f -> %.2f dB), in one gesture",
                  driveBefore, values[showcase::kDrive]);
    check(values[showcase::kDrive] > driveBefore + 1.0f && begins == 1 && ends == 1 && sets > 0, msg);

    // ── Undo, from the Edit menu ──
    check(dsp.parameters.canUndo(), "the drag is an undo step");
    editor->menuItemSelected(showcase::ShowcaseEditor::kUndo, 1);
    check(std::fabs(values[showcase::kDrive] - driveBefore) < 1e-3f, "Edit > Undo puts the host's value back");

    // ── A factory preset from the Presets menu: one edit ──
    begins = ends = 0;
    editor->menuItemSelected(showcase::ShowcaseEditor::kPresetBase + 1, 0);
    bool matches = true;
    for (int i = 0; i < SONORE_NUM_PARAMS; ++i)
      matches = matches && std::fabs(values[(size_t) i] - showcase::kPresetCrunch[i]) < 1e-3f;
    check(matches && begins == SONORE_NUM_PARAMS && ends == SONORE_NUM_PARAMS,
          "Presets > Crunch sets every parameter at the host, inside one gesture each");
    editor->menuItemSelected(showcase::ShowcaseEditor::kUndo, 1);
    check(std::fabs(values[showcase::kDrive] - driveBefore) < 1e-3f, "and is one undo step");

    // ── A curve from the menu, then one typed wrong ──
    editor->menuItemSelected(showcase::ShowcaseEditor::kCurveBase + 3, 0);
    check(dsp.expression == showcase::kExampleCurves[3].text && editor->statusIsOk(),
          "a curve from the menu is compiled and playing");
    check(std::lround(values[showcase::kShape]) == 3, "and switches the shape to Expression at the host");
    editor->codeEditor().loadContent("round(tanh(x) * 4 / 4");
    check(!editor->statusIsOk() && dsp.expression == showcase::kExampleCurves[3].text,
          "a typing mistake is reported and the old curve keeps playing");

    // ── The host moves things while the editor is open ──
    values[showcase::kDrive] = 30.0f;
    editor->editorTick(0.033);
    check(std::fabs(editor->knob(0).value() - (float) toNormalisedValue(kDesc.params[0], 30.0f)) < 1e-4f,
          "automation reaches the knob on the next frame");
    StateBag session;
    session.setString("expression", "abs(x)");
    session.setInt("scheme", 1);
    dsp.loadState(session);
    editor->editorTick(0.066);
    check(editor->codeEditor().getDocument().getAllContent() == "abs(x)" && editor->statusIsOk(),
          "a session the host loads replaces the curve on screen");
    check(!editor->knob(0).tooltip().empty() && !editor->codeEditor().tooltip().empty(), "the controls carry tooltips");

    // At the smallest size the descriptor allows -- which is what the host
    // tests resize to -- the knobs give way before the place to type does.
    editor->setBounds({0.0f, 0.0f, (float) kDesc.editorLimits.minWidth, (float) kDesc.editorLimits.minHeight});
    const Rect code = editor->codeEditor().bounds(), knob0 = editor->knob(0).bounds();
    std::snprintf(msg, sizeof(msg), "at %ux%u the code editor keeps %.0f px and the knobs %.0f",
                  kDesc.editorLimits.minWidth, kDesc.editorLimits.minHeight, code.h, knob0.h);
    check(code.h >= 40.0f && knob0.h >= 60.0f && code.y > knob0.bottom(), msg);
    if (!bmpPrefix.empty()) writeBmp(renderEditor(*editor), bmpPrefix + "_small.bmp");

    made.reset();
    check(!dsp.parameters.isConnectedToHost(), "closing the editor disconnects the APVTS from the host");
  }

  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  std::printf(g_failures == 0 ? "SHOWCASE TESTS PASSED\n" : "SHOWCASE TESTS FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
