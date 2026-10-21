// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: plugin state: AudioProcessorValueTreeState over the SDK's
// parameters, PluginHostType, the plugin list and its component, and LADSPA
// hosting.
//
//   plugin_state_test <path to the ladspa_gain_plugin library>
//
// The LADSPA library is built from tests/ladspa_gain_plugin.cpp -- a .dll on
// Windows, a .so elsewhere -- because no machine this runs on can be assumed
// to have a LADSPA plugin, and the ones a Linux machine has are not ones the
// test can know the arithmetic of.
#include <sonore/gfx/plugin_list.h>
#include <sonore/gfx/system_font.h>
#include <sonore/gfx/value_tree_state_attachments.h>
#include <sonore/host.h>
#include <sonore/plugin_cache.h>
#include <sonore/plugin_host_type.h>
#include <sonore/value_tree_state.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "temp_path.h"

using namespace sonore;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static bool approx(double a, double b, double tolerance = 1e-5) { return std::fabs(a - b) <= tolerance; }

// ── A layout of every kind ─────────────────────────────────────────────────────

static AudioProcessorValueTreeState::ParameterLayout makeLayout() {
  using Attributes = RangedAudioParameter::Attributes;
  AudioProcessorValueTreeState::ParameterLayout layout;
  layout.add(std::make_unique<AudioParameterFloat>(
      ParameterID{"gain", 1}, "Gain", NormalisableRange<float>(-60.0f, 12.0f, 0.1f), 0.0f,
      Attributes().withLabel("dB")));
  layout.add(std::make_unique<AudioParameterInt>("voices", "Voices", 1, 16, 8),
             std::make_unique<AudioParameterBool>("bypass", "Bypass", false),
             std::make_unique<AudioParameterChoice>("mode", "Mode",
                                                    std::vector<std::string>{"Clean", "Warm", "Hot"}, 1));
  NormalisableRange<float> cutoff(20.0f, 20000.0f);
  cutoff.setSkewForCentre(1000.0f);
  layout.add(std::make_unique<AudioProcessorParameterGroup>(
      "filter", "Filter", "|",
      std::make_unique<AudioParameterFloat>("cutoff", "Cutoff", cutoff, 1000.0f,
                                            Attributes().withLabel("Hz")),
      std::make_unique<AudioParameterFloat>("res", "Resonance", 0.0f, 1.0f, 0.25f)));
  return layout;
}

enum { kGain = 0, kVoices, kBypass, kMode, kCutoff, kRes, kCount };

/** A listener that remembers what it heard. */
struct Recorder : AudioProcessorValueTreeState::Listener {
  std::vector<std::pair<std::string, float>> heard;
  void parameterChanged(const std::string& id, float v) override { heard.push_back({id, v}); }
};

/** A stand-in for a wrapper: the real parameters, and everything sent to them. */
struct FakeHost {
  float values[kCount] = {};
  std::vector<std::string> log;
  AudioProcessorValueTreeState::HostLink link() {
    AudioProcessorValueTreeState::HostLink l;
    l.getParameter = [this](int i) { return values[i]; };
    l.setParameter = [this](int i, float v) {
      values[i] = v;
      char text[48];
      std::snprintf(text, sizeof(text), "S%d=%g", i, (double) v);
      log.push_back(text);
    };
    l.beginGesture = [this](int i) { log.push_back("B" + std::to_string(i)); };
    l.endGesture = [this](int i) { log.push_back("E" + std::to_string(i)); };
    return l;
  }
  int count(const std::string& entry) const {
    int n = 0;
    for (const auto& e : log) n += e == entry ? 1 : 0;
    return n;
  }
};

/** Shaped as a generated DSP is -- process(io, p), saveState, loadState -- and
 *  driven the way the wrappers drive one, to show the APVTS sits where a
 *  ported processor's would. */
struct PortedDsp {
  AudioProcessorValueTreeState apvts{nullptr, "PARAMETERS", makeLayout()};
  std::atomic<float>* gain = apvts.getRawParameterValue("gain");
  std::atomic<float>* bypass = apvts.getRawParameterValue("bypass");

  void process(AudioBlock<float>& io, const float* p) {
    apvts.updateFromHost(p, kCount); // [audio-thread] the one write it makes
    const float g = bypass->load() >= 0.5f ? 1.0f : std::pow(10.0f, gain->load() / 20.0f);
    for (size_t c = 0; c < io.getNumChannels(); ++c)
      for (size_t i = 0; i < io.getNumSamples(); ++i) io.getChannelPointer(c)[i] *= g;
  }
  void saveState(StateBag& bag) const { bag = apvts.copyState(); }
  void loadState(const StateBag& bag) { apvts.replaceState(bag); }
};

static void testValueTreeState() {
  std::printf("\n-- AudioProcessorValueTreeState --\n");
  char msg[256];
  UndoHistory history;
  AudioProcessorValueTreeState apvts(&history, "PARAMETERS", makeLayout());

  // ── Layout to parameters ──
  check(apvts.numParameters() == kCount, "six parameters, groups flattened in order");
  const ParamInfo* t = apvts.parameterInfos();
  check(t && std::strcmp(t[kGain].id, "gain") == 0 && std::strcmp(t[kRes].id, "res") == 0,
        "the table is in layout order: p[0] is gain, p[5] is the group's last");
  check(t[kGain].minValue == -60.0f && t[kGain].maxValue == 12.0f && t[kGain].defaultValue == 0.0f &&
            std::strcmp(t[kGain].unit, "dB") == 0 && t[kGain].stepCount == 0,
        "a float: range, default and unit carried, continuous");
  check(t[kVoices].stepCount == 16 && t[kVoices].minValue == 1.0f && t[kVoices].defaultValue == 8.0f,
        "an int: sixteen steps from 1, default 8");
  check(t[kBypass].stepCount == 2 && paramValueName(t[kBypass], 0) &&
            std::strcmp(paramValueName(t[kBypass], 1), "On") == 0,
        "a bool: two steps named Off and On");
  check(t[kMode].stepCount == 3 && isNamedChoice(t[kMode]) &&
            std::strcmp(paramValueName(t[kMode], 2), "Hot") == 0 && t[kMode].defaultValue == 1.0f,
        "a choice: three named steps, default the second");
  check(t[kCutoff].group && std::strcmp(t[kCutoff].group, "Filter") == 0 && !t[kGain].group,
        "the group's name becomes ParamInfo::group; ungrouped stays null");
  std::snprintf(msg, sizeof(msg), "skewForCentre carries over: 1 kHz at %.4f of the travel",
                toNormalisedValue(t[kCutoff], 1000.0));
  check(approx(toNormalisedValue(t[kCutoff], 1000.0), 0.5, 1e-4), msg);
  check(apvts.layoutProblem() == nullptr, "the layout passes every ParamInfo rule");
  check(apvts.tableMismatch(t, kCount) == nullptr, "and matches its own table");
  ParamInfo swapped[kCount];
  for (int i = 0; i < kCount; ++i) swapped[i] = t[i];
  std::swap(swapped[0], swapped[1]);
  check(apvts.tableMismatch(swapped, kCount) != nullptr, "a table in another order is refused");

  PluginDescriptor desc;
  desc.params = t;
  desc.numParams = apvts.numParameters();
  check(descriptorProblem(desc) == nullptr, "a PluginDescriptor pointing at the table is valid");

  {
    AudioProcessorValueTreeState::ParameterLayout bad;
    bad.add(std::make_unique<AudioParameterFloat>(
        "sym", "Sym", NormalisableRange<float>(-1.0f, 1.0f, 0.0f, 0.5f, true), 0.0f));
    AudioProcessorValueTreeState odd(nullptr, "X", std::move(bad));
    check(odd.layoutProblem() != nullptr, "a symmetric skew is reported, not drawn linear in silence");
    AudioProcessorValueTreeState::ParameterLayout dup(
        std::make_unique<AudioParameterBool>("a", "A", false),
        std::make_unique<AudioParameterBool>("a", "A again", true));
    AudioProcessorValueTreeState twice(nullptr, "X", std::move(dup));
    int which = -1;
    check(twice.layoutProblem(&which) != nullptr && which == 1, "a duplicate id is reported, at the second");
  }

  // ── Text conversion, through the functions the wrappers use ──
  RangedAudioParameter* gainParam = apvts.getParameter("gain");
  RangedAudioParameter* modeParam = apvts.getParameter("mode");
  RangedAudioParameter* bypassParam = apvts.getParameter("bypass");
  std::snprintf(msg, sizeof(msg), "gain reads \"%s\" at -6 dB", gainParam->getText(gainParam->convertTo0to1(-6.0f), 64).c_str());
  check(gainParam->getText(gainParam->convertTo0to1(-6.0f), 64) == "-6.0 dB", msg);
  check(modeParam->getCurrentValueAsText() == "Warm" && bypassParam->getCurrentValueAsText() == "Off",
        "a choice and a bool read as their names");
  {
    // A length cut never splits a UTF-8 character. "Gain é" is 7 bytes, the
    // é two of them; cut at 6 it kept the é's first byte, which no host can
    // show and the next decoder refuses. A choice's step names go through
    // the same cut: "−6" starts with a three-byte minus sign.
    AudioParameterBool accented("accented", "Gain \xC3\xA9", false);
    AudioParameterChoice minus("minus", "Minus", std::vector<std::string>{"\xE2\x88\x92" "6", "0"}, 0);
    const std::string six = accented.getName(6), seven = accented.getName(7);
    const std::string two = minus.getText(0.0f, 2), four = minus.getText(0.0f, 4);
    std::snprintf(msg, sizeof(msg),
                  "a cut ends before a character it would split (name at 6 is %u bytes, at 7 %u; "
                  "\"-6\" at 2 is %u bytes, at 4 %u)",
                  (unsigned) six.size(), (unsigned) seven.size(), (unsigned) two.size(), (unsigned) four.size());
    check(six == "Gain " && seven == "Gain \xC3\xA9" && two.empty() && four == "\xE2\x88\x92" "6", msg);
  }
  check(approx(modeParam->getValueForText("hot"), 1.0) &&
            approx(gainParam->convertFrom0to1(gainParam->getValueForText("-12 dB")), -12.0),
        "text parses back: a choice by name, a number with its unit");
  check(approx(gainParam->getValueForText("12abc"), gainParam->getValue()),
        "text that is not a value moves nothing");
  check(gainParam->getNumSteps() == 721 && apvts.getParameter("voices")->getNumSteps() == 16 &&
            apvts.getParameter("res")->getNumSteps() == 0x7fffffff,
        "getNumSteps: an interval's positions, an int's range, continuous");

  // ── Raw atomics follow setValue ──
  std::atomic<float>* raw = apvts.getRawParameterValue("gain");
  check(raw && raw->load() == 0.0f, "getRawParameterValue is the default before anything moves");
  gainParam->setValueNotifyingHost(gainParam->convertTo0to1(-6.04f));
  std::snprintf(msg, sizeof(msg), "setValueNotifyingHost(-6.04 dB) lands on the 0.1 interval: %.4f",
                (double) raw->load());
  check(approx(raw->load(), -6.0, 1e-4), msg);
  auto* voices = static_cast<AudioParameterInt*>(apvts.getParameter("voices"));
  *voices = 40;
  check(voices->get() == 16 && apvts.getRawParameterValue("voices")->load() == 16.0f,
        "an int assigned past its range is clamped, in the atomic too");
  auto* mode = static_cast<AudioParameterChoice*>(modeParam);
  *mode = 2;
  check(mode->getIndex() == 2 && mode->getCurrentChoiceName() == "Hot", "a choice assigned by index");
  check(apvts.getRawParameterValue("nope") == nullptr, "an unknown id has no atomic");

  // ── Listeners ──
  Recorder rec;
  apvts.addParameterListener("gain", &rec);
  gainParam->setValueNotifyingHost(gainParam->convertTo0to1(-3.0f));
  check(rec.heard.size() == 1 && rec.heard[0].first == "gain" && approx(rec.heard[0].second, -3.0, 1e-4),
        "a listener hears the plain value, once");
  gainParam->setValueNotifyingHost(gainParam->convertTo0to1(-3.0f));
  check(rec.heard.size() == 1, "setting the same value again is not a change");

  // From the audio side: stored at once, heard only when the message thread asks.
  float block[kCount] = {-9.0f, 4.0f, 1.0f, 0.0f, 440.0f, 0.5f};
  apvts.updateFromHost(block, kCount);
  check(approx(raw->load(), -9.0) && apvts.getRawParameterValue("bypass")->load() == 1.0f,
        "updateFromHost stores the block's values in the atomics");
  check(rec.heard.size() == 1, "and calls no listener on the audio thread");
  apvts.dispatchPendingChanges();
  check(rec.heard.size() == 2 && approx(rec.heard[1].second, -9.0),
        "dispatchPendingChanges delivers it on the message thread");
  apvts.dispatchPendingChanges();
  check(rec.heard.size() == 2, "and only once");

  // A real second thread writing while this one dispatches: the audio side
  // never waits, and the last value written is the one that stays.
  {
    std::atomic<bool> go{true};
    std::thread audio([&] {
      float p[kCount] = {0.0f, 4.0f, 0.0f, 0.0f, 440.0f, 0.5f};
      for (int n = 0; n < 20000; ++n) {
        p[kGain] = (float) (n % 600) * -0.1f;
        apvts.updateFromHost(p, kCount);
      }
      p[kGain] = -1.5f;
      apvts.updateFromHost(p, kCount);
      go = false;
    });
    while (go) apvts.dispatchPendingChanges();
    audio.join();
    apvts.dispatchPendingChanges();
    check(approx(raw->load(), -1.5) && approx(rec.heard.back().second, -1.5),
          "a concurrent audio thread: the last block's value is the one held and heard");
  }
  apvts.removeParameterListener("gain", &rec);
  const size_t heardBefore = rec.heard.size();
  gainParam->setValueNotifyingHost(gainParam->convertTo0to1(-20.0f));
  check(rec.heard.size() == heardBefore, "a removed listener hears nothing");

  // ── The host link ──
  FakeHost host;
  for (int i = 0; i < kCount; ++i) host.values[i] = apvts.parameterAt(i)->rawValue().load();
  apvts.connectToHost(host.link());
  gainParam->beginChangeGesture();
  gainParam->setValueNotifyingHost(gainParam->convertTo0to1(-10.0f));
  gainParam->endChangeGesture();
  check(host.log.size() == 3 && host.log[0] == "B0" && host.log[1] == "S0=-10" && host.log[2] == "E0",
        "an edit reaches the host bracketed: begin, set, end");
  host.values[kRes] = 0.75f;
  apvts.syncFromHost();
  check(approx(apvts.getRawParameterValue("res")->load(), 0.75), "syncFromHost reads automation back");

  // ── Attachments ──
  std::printf("\n-- attachments --\n");
  *mode = 2;
  static_cast<AudioParameterBool&>(*bypassParam) = false;
  history.reset("");
  apvts.replaceState(apvts.copyState()); // a clean undo history from here
  host.log.clear();
  gfx::Component root;
  root.setBounds({0.0f, 0.0f, 400.0f, 200.0f});
  gfx::Slider slider(gfx::Slider::Style::LinearHorizontal);
  slider.setBounds({0.0f, 0.0f, 214.0f, 30.0f}); // a 200-pixel track
  gfx::Button button;
  button.setBounds({0.0f, 40.0f, 80.0f, 30.0f});
  gfx::ComboBox combo;
  combo.setBounds({0.0f, 80.0f, 120.0f, 30.0f});
  root.addChild(&slider);
  root.addChild(&button);
  root.addChild(&combo);
  {
    AudioProcessorValueTreeState::SliderAttachment sa(apvts, "gain", slider);
    AudioProcessorValueTreeState::ButtonAttachment ba(apvts, "bypass", button);
    AudioProcessorValueTreeState::ComboBoxAttachment ca(apvts, "mode", combo);
    AudioProcessorValueTreeState::SliderAttachment missing(apvts, "no-such", slider);
    check(sa.isBound() && ba.isBound() && ca.isBound() && !missing.isBound(),
          "three attachments bound; an unknown id binds nothing");
    std::snprintf(msg, sizeof(msg), "the slider starts where gain is: %.4f", (double) slider.value());
    check(approx(slider.value(), gainParam->getValue(), 1e-5), msg);
    check(combo.selectedText() == "Hot" && button.isToggled() == false,
          "the combo shows the choice's name, the button the bool");

    // Parameters move widgets -- at once, through the listener, with no tick.
    gainParam->setValueNotifyingHost(gainParam->convertTo0to1(0.0f));
    check(approx(slider.value(), gainParam->convertTo0to1(0.0f)), "a parameter change moves the slider");
    *mode = 0;
    check(combo.selectedIndex() == 0, "and the combo");
    static_cast<AudioParameterBool&>(*bypassParam) = true;
    check(button.isToggled() && button.label() == "On", "and the toggle, labelled with the state's name");
    static_cast<AudioParameterBool&>(*bypassParam) = false;

    // Widgets move parameters, with gestures. A real drag through the router.
    const size_t stepsBefore = history.size();
    host.log.clear();
    gfx::MouseRouter router(root);
    router.mouseDown({107.0f, 15.0f}); // the middle of the track
    router.mouseMove({157.0f, 15.0f}); // three quarters
    router.mouseUp({157.0f, 15.0f});
    const float dragged = apvts.getRawParameterValue("gain")->load();
    std::snprintf(msg, sizeof(msg), "a drag to three quarters sets gain to %.2f dB", (double) dragged);
    check(approx(dragged, gainParam->convertFrom0to1(0.75f), 0.051), msg);
    check(host.count("B0") == 1 && host.count("E0") == 1 && host.log.front() == "B0" &&
              host.log.back() == "E0" && host.log.size() >= 4,
          "the drag reached the host as ONE gesture around its values");
    check(history.size() == stepsBefore + 1 && history.undoLabel() == "Gain",
          "and is one undo step, labelled with the parameter");

    router.mouseDown({40.0f, 55.0f});
    router.mouseUp({40.0f, 55.0f});
    check(bypassParam->getValue() == 1.0f && host.count("B2") == 1 && host.count("E2") == 1,
          "clicking the toggle sets the bool, bracketed");
    combo.setSelectedIndex(2);
    check(mode->getIndex() == 2 && host.count("B3") == 1, "choosing in the combo sets the choice, bracketed");

    const size_t stepsNow = history.size();
    apvts.beginGesture(kRes);
    apvts.endGesture(kRes);
    check(history.size() == stepsNow, "a gesture that changed nothing is not an undo step");

    // Undo walks back through the three, moving widgets and telling the host.
    host.log.clear();
    check(apvts.undo() && mode->getIndex() == 0 && combo.selectedIndex() == 0,
          "undo restores the choice, and the combo follows");
    check(host.count("B3") == 1 && host.count("E3") == 1 && host.values[kMode] == 0.0f,
          "the host hears the undo as an edit of its own");
    check(apvts.undo() && bypassParam->getValue() == 0.0f && !button.isToggled(), "undo again: the toggle");
    check(apvts.undo() && approx(gainParam->getValue(), gainParam->convertTo0to1(0.0f)) &&
              approx(slider.value(), gainParam->convertTo0to1(0.0f)),
          "and again: the drag, slider and all");
    check(apvts.redo() && approx(apvts.getRawParameterValue("gain")->load(), dragged), "redo puts the drag back");

    // Text typed into the SDK's own binding goes through the same gesture.
    host.log.clear();
    check(sa.binding() && sa.binding()->setFromText("-24 dB") &&
              approx(apvts.getRawParameterValue("gain")->load(), -24.0) && host.count("B0") == 1,
          "a typed value is one bracketed edit");

    // The non-parameter state, undoable as a step of its own.
    apvts.editState("load sample", [](StateBag& s) { s.setString("sample", "kick.wav"); });
    check(history.undoLabel() == "load sample", "editState records a labelled step");
    check(apvts.undo() && !apvts.state.has("sample"), "and undo takes the sample away again");
    apvts.redo();
  }
  check(!slider.onValueChange && !button.onToggle && !combo.onChange,
        "destroyed attachments leave the widgets inert, not dangling");

  // An attachment destroyed mid-drag -- the editor closed with the button
  // down. The release reaches nobody; the gesture used to stay open for good,
  // at the host and in the APVTS, and every later edit was folded into it:
  // no undo step was ever recorded again.
  {
    host.log.clear();
    const size_t stepsBefore = history.size();
    gfx::MouseRouter router(root);
    {
      AudioProcessorValueTreeState::SliderAttachment sa(apvts, "gain", slider);
      router.mouseDown({60.0f, 15.0f});
      router.mouseMove({90.0f, 15.0f});
    }
    router.mouseUp({90.0f, 15.0f});
    check(host.count("B0") == 1 && host.count("E0") == 1,
          "an attachment destroyed mid-drag closes the host's gesture it opened");
    check(apvts.canUndo() && history.size() == stepsBefore + 1, "and the drag is an undo step of its own");
    const float resBefore = apvts.getRawParameterValue("res")->load();
    apvts.setParameterValue(kRes, resBefore > 0.5f ? 0.1f : 0.9f);
    check(history.size() == stepsBefore + 2 && apvts.undo() &&
              apvts.getRawParameterValue("res")->load() == resBefore,
          "a later edit is still a step undo takes back");
  }

  // Undo takes back what the step changed, and nothing the host moved since:
  // automation on another control while the user turned this one, or a value
  // the host set. Each would otherwise go back to where it was when the step
  // was recorded, and reach the host as an edit -- which a DAW writing
  // automation records over the user's lane.
  {
    const float voicesBefore = apvts.getRawParameterValue("voices")->load();
    const float gainBefore = apvts.getRawParameterValue("gain")->load();
    const float gainAfter = gainBefore > -30.0f ? -40.0f : -20.0f;
    gainParam->beginChangeGesture();
    gainParam->setValueNotifyingHost(gainParam->convertTo0to1(gainAfter));
    host.values[kCutoff] = 5000.0f; // automation, mid-drag
    apvts.syncFromHost();
    gainParam->endChangeGesture();
    apvts.setParameterValue(kVoices, voicesBefore > 4.0f ? 2.0f : 12.0f); // a second step
    host.values[kCutoff] = 7000.0f; // and the host again
    host.values[kRes] = 0.125f;
    apvts.syncFromHost();
    host.log.clear();
    check(apvts.undo() && apvts.getRawParameterValue("voices")->load() == voicesBefore &&
              approx(apvts.getRawParameterValue("cutoff")->load(), 7000.0) &&
              approx(apvts.getRawParameterValue("res")->load(), 0.125) && host.count("B4") == 0 &&
              host.count("B5") == 0,
          "undoing Voices puts Voices back, leaves Cutoff and Resonance where the host put them, and "
          "sends the host nothing for them");
    host.values[kRes] = 0.375f;
    apvts.syncFromHost();
    check(apvts.undo() && approx(apvts.getRawParameterValue("gain")->load(), gainBefore) &&
              approx(apvts.getRawParameterValue("cutoff")->load(), 7000.0) &&
              approx(apvts.getRawParameterValue("res")->load(), 0.375) && host.count("B4") == 0,
          "undoing a drag during automation of another control: the drag only");
    check(apvts.redo() && approx(apvts.getRawParameterValue("gain")->load(), gainAfter) &&
              approx(apvts.getRawParameterValue("res")->load(), 0.375) && host.count("B5") == 0,
          "and redo puts the drag back, the drag only");
    apvts.redo();
  }
  apvts.disconnectFromHost();

  // Listeners that edit: resonance that follows the cutoff, a mode that
  // starts at -12 dB. What they do in answer to an edit is part of its step,
  // and an undo or redo records nothing. Before, the listener's edit during
  // an undo was pushed as a step of its own, which threw away the redo just
  // made -- with one such listener nothing could ever be redone; its edit
  // inside a plain edit was a second step; and the mode's reset, answering a
  // half-restored state, undid a mode switch to -12 dB rather than the
  // user's gain.
  {
    UndoHistory linkedHistory;
    AudioProcessorValueTreeState linked(&linkedHistory, "PARAMETERS", makeLayout());
    FakeHost linkedHost;
    linked.connectToHost(linkedHost.link());
    struct Rules : AudioProcessorValueTreeState::Listener {
      AudioProcessorValueTreeState* s = nullptr;
      bool undoRefused = false;
      void parameterChanged(const std::string& id, float v) override {
        if (id == "cutoff") s->setParameterValue(kRes, v / 20000.0f);
        if (id == "mode") s->setParameterValue(kGain, -12.0f);
        if (id == "voices") undoRefused = !s->undo();
      }
    } rules;
    rules.s = &linked;
    for (const char* id : {"cutoff", "mode", "voices"}) linked.addParameterListener(id, &rules);
    auto value = [&linked](const char* id) { return (double) linked.getRawParameterValue(id)->load(); };

    linked.beginGesture(kCutoff);
    linked.setParameterValue(kCutoff, 8000.0f);
    linked.endGesture(kCutoff);
    check(linkedHistory.size() == 1 && approx(value("res"), 0.4), "a drag the resonance follows is one step");
    check(linked.undo() && approx(value("cutoff"), 1000.0) && approx(value("res"), 0.25) &&
              linkedHistory.size() == 1 && linked.canRedo(),
          "undoing it restores both, records nothing, and can be redone");
    check(linked.redo() && approx(value("cutoff"), 8000.0) && approx(value("res"), 0.4),
          "redo puts both back");

    linked.setParameterValue(kCutoff, 2000.0f);
    check(linkedHistory.size() == 2 && approx(value("res"), 0.1),
          "an edit outside a gesture, and the listener's edit inside it, are one step");
    check(linked.undo() && approx(value("cutoff"), 8000.0) && approx(value("res"), 0.4) && linked.canRedo(),
          "which one undo takes back");
    linked.redo();

    linked.setParameterValue(kGain, -6.0f);
    linked.beginGesture(kMode);
    linked.setParameterValue(kMode, 2.0f);
    linked.endGesture(kMode);
    check(approx(value("gain"), -12.0) && linkedHistory.undoLabel() == "Mode",
          "choosing a mode resets the gain, in the mode's step");
    linkedHost.log.clear();
    check(linked.undo() && value("mode") == 1.0 && approx(value("gain"), -6.0) &&
              approx(linkedHost.values[kGain], -6.0) && linkedHost.count("S0=-12") == 0,
          "undoing the mode brings back the user's gain, not the reset one, and the host is never sent "
          "the reset");
    check(linked.redo() && value("mode") == 2.0 && approx(value("gain"), -12.0), "and redo the reset");

    const size_t steps = linkedHistory.size();
    linked.setParameterValue(kVoices, 3.0f);
    check(rules.undoRefused && value("voices") == 3.0 && linkedHistory.size() == steps + 1 &&
              value("mode") == 2.0,
          "an undo asked for by a listener while an edit is under way is refused");
  }

  // The non-parameter state written directly -- the tab the user moved to,
  // a sample the plugin loaded -- is not undoable, and undoing a knob turned
  // before it must not undo it either. Every parameter step put the whole
  // state back as it was before the knob: the tab jumped back, and the bag
  // named the old sample while the DSP played the new one.
  {
    UndoHistory stateHistory;
    AudioProcessorValueTreeState st(&stateHistory, "PARAMETERS", makeLayout());
    st.state.setString("sample", "kick.wav");
    st.setParameterValue(kGain, -6.0f);
    st.state.setString("tab", "filter");
    st.state.setString("sample", "snare.wav");
    check(st.undo() && st.getRawParameterValue("gain")->load() == 0.0f &&
              st.state.getString("tab") == "filter" && st.state.getString("sample") == "snare.wav",
          "undoing a knob leaves the state written since: the tab, the sample loaded");
    check(st.redo() && approx(st.getRawParameterValue("gain")->load(), -6.0) &&
              st.state.getString("sample") == "snare.wav",
          "and so does redo");
    st.editState("load sample", [](StateBag& s) { s.setString("sample", "hat.wav"); });
    check(st.undo() && st.state.getString("sample") == "snare.wav" && st.state.getString("tab") == "filter",
          "a step that edited the state undoes it");
    check(st.redo() && st.state.getString("sample") == "hat.wav", "and redoes it");
  }

  // A DSP whose loadState is replaceState -- a processor ported from the
  // reference -- under the preset bar, which loads a preset through
  // editWholeState and the plugin's own restore path. replaceState started
  // the history again every time: the preset load threw away every step
  // before it, and undoing it emptied the history mid-undo, so it could not
  // be redone and nothing before it could be undone.
  {
    UndoHistory presetHistory;
    AudioProcessorValueTreeState ported(&presetHistory, "PARAMETERS", makeLayout());
    FakeHost presetHost;
    AudioProcessorValueTreeState::HostLink link = presetHost.link();
    link.saveState = [&ported](std::vector<uint8_t>& out) {
      out.clear();
      ported.copyState().serialise(out);
      return true;
    };
    link.loadState = [&ported, &presetHost](const std::vector<uint8_t>& bytes, std::string*) {
      StateBag bag;
      if (!bag.deserialise(bytes.data(), bytes.size())) return false;
      ported.replaceState(bag); // the DSP's loadState
      for (int i = 0; i < kCount; ++i) presetHost.values[i] = ported.parameterAt(i)->rawValue().load();
      return true;
    };
    ported.connectToHost(link);
    for (int i = 0; i < kCount; ++i) presetHost.values[i] = ported.parameterAt(i)->rawValue().load();

    ported.setParameterValue(kGain, -6.0f);
    StateBag crunch;
    crunch.setDouble("param.gain", -20.0);
    crunch.setDouble("param.mode", 2.0);
    std::vector<uint8_t> crunchBytes;
    crunch.serialise(crunchBytes);
    check(ported.editWholeState("Crunch", [&](std::string* why) { return link.loadState(crunchBytes, why); }) &&
              approx(ported.getRawParameterValue("gain")->load(), -20.0) && presetHistory.size() == 2,
          "a preset loaded through replaceState is a step after the edits before it, not instead of them");
    check(ported.undo() && approx(ported.getRawParameterValue("gain")->load(), -6.0) &&
              ported.getRawParameterValue("mode")->load() == 1.0f && ported.canRedo() && ported.canUndo(),
          "undoing it brings back the state before it, and leaves redo and the earlier steps");
    check(ported.undo() && ported.getRawParameterValue("gain")->load() == 0.0f, "the edit before it undoes");
    check(ported.redo() && ported.redo() && approx(ported.getRawParameterValue("gain")->load(), -20.0) &&
              ported.getRawParameterValue("mode")->load() == 2.0f,
          "and both redo, the preset whole");
    StateBag session;
    session.setDouble("param.gain", -3.0);
    ported.replaceState(session);
    check(!ported.canUndo() && !ported.canRedo(), "a session the host loads still starts the history again");
  }

  // ── State round trip ──
  std::printf("\n-- state --\n");
  apvts.state.setString("sample", "snare.wav");
  StateBag saved = apvts.copyState();
  std::vector<uint8_t> bytes;
  saved.serialise(bytes);
  {
    AudioProcessorValueTreeState fresh(nullptr, "PARAMETERS", makeLayout());
    StateBag loaded;
    check(loaded.deserialise(bytes.data(), bytes.size()), "the saved state is a StateBag that reads back");
    fresh.replaceState(loaded);
    bool all = true;
    for (int i = 0; i < kCount; ++i)
      all = all && fresh.parameterAt(i)->rawValue().load() == apvts.parameterAt(i)->rawValue().load();
    check(all, "every parameter restored exactly");
    check(fresh.state.getString("sample") == "snare.wav" && fresh.state.size() == 1,
          "and the non-parameter state, without the APVTS's own keys");
    check(loaded.getString("@type") == "PARAMETERS", "the state is typed, as the reference's tree is");

    StateBag partial;
    partial.setDouble("param.gain", -30.0);
    partial.setString("param.voices", "nan");
    fresh.replaceState(partial);
    check(approx(fresh.getRawParameterValue("gain")->load(), -30.0) &&
              fresh.getRawParameterValue("voices")->load() == 8.0f &&
              fresh.getRawParameterValue("mode")->load() == 1.0f && fresh.state.empty(),
          "a bag missing a parameter, or naming it NaN, restores the default");
  }

  // The same, through a DSP shaped as the wrappers expect, driven as they do.
  {
    PortedDsp a;
    std::vector<float> l(64, 1.0f), r(64, 1.0f);
    float* ch[2] = {l.data(), r.data()};
    AudioBlock<float> io(ch, 2, 64);
    const float p[kCount] = {-6.0f, 8.0f, 0.0f, 1.0f, 1000.0f, 0.25f};
    a.process(io, p);
    check(approx(l[10], std::pow(10.0, -6.0 / 20.0), 1e-5),
          "a ported DSP reads p[] through getRawParameterValue and processes with it");
    a.apvts.state.setString("ir", "hall.wav");
    StateBag bag;
    a.saveState(bag);
    std::vector<uint8_t> blob;
    bag.serialise(blob);
    PortedDsp b;
    StateBag back;
    back.deserialise(blob.data(), blob.size());
    b.loadState(back);
    check(approx(b.gain->load(), -6.0) && b.apvts.state.getString("ir") == "hall.wav",
          "its saveState/loadState carry the APVTS through a session");
  }
}

// ── PluginHostType ──────────────────────────────────────────────────────────

static void testHostType() {
  std::printf("\n-- PluginHostType --\n");
  char msg[512];
  using H = PluginHostType;
  struct Case {
    const char* path;
    const char* bundle;
    const char* reported;
    H::HostType expect;
    int major;
  };
  const Case cases[] = {
      {"C:\\Program Files\\REAPER (x64)\\reaper.exe", "", "", H::Reaper, 0},
      {"C:\\Program Files\\REAPER (x64)\\reaper_host64.exe", "", "", H::Reaper, 64},
      {"C:\\ProgramData\\Ableton\\Live 11 Suite\\Program\\Ableton Live 11 Suite.exe", "", "", H::AbletonLive, 11},
      {"/Applications/Ableton Live 12 Suite.app/Contents/MacOS/Live", "com.ableton.live", "", H::AbletonLive, 0},
      {"C:\\Program Files\\Bitwig Studio\\5.1\\bin\\BitwigPluginHost-X64-SSE41.exe", "", "", H::BitwigStudio, 64},
      {"/opt/bitwig-studio/bin/BitwigAudioEngine-X64-AVX2", "", "", H::BitwigStudio, 64},
      {"C:\\Program Files\\Steinberg\\Cubase 13\\Cubase13.exe", "", "", H::SteinbergCubase, 13},
      {"C:\\Program Files\\Steinberg\\Nuendo 12\\Nuendo12.exe", "", "", H::SteinbergNuendo, 12},
      {"C:\\Program Files\\Steinberg\\WaveLab 6\\WaveLab6.exe", "", "", H::SteinbergWavelab, 6},
      {"C:\\Program Files\\Steinberg\\VST3PluginTestHost\\VST3PluginTestHost.exe", "", "", H::SteinbergTestHost, 3},
      {"/Applications/Logic Pro X.app/Contents/MacOS/Logic Pro X", "com.apple.logic10", "", H::AppleLogic, 0},
      {"/Applications/GarageBand.app/Contents/MacOS/GarageBand", "com.apple.garageband10", "", H::AppleGarageBand, 0},
      {"/Applications/MainStage 3.app/Contents/MacOS/MainStage 3", "com.apple.mainstage3", "", H::AppleMainStage, 3},
      {"C:\\Program Files\\Avid\\Pro Tools\\ProTools.exe", "", "", H::AvidProTools, 0},
      {"/Applications/Pro Tools.app/Contents/MacOS/Pro Tools", "com.avid.ProTools", "", H::AvidProTools, 0},
      {"C:\\Program Files\\Image-Line\\FL Studio 21\\FL64.exe", "", "", H::FruityLoops, 64},
      {"C:\\Program Files\\Image-Line\\Shared\\ilbridge.exe", "", "", H::FruityLoops, 0},
      {"C:\\Program Files\\PreSonus\\Studio One 6\\Studio One.exe", "", "", H::StudioOne, 0},
      {"/Applications/Studio One 6.app/Contents/MacOS/Studio One 6", "com.presonus.studioone6", "", H::StudioOne, 6},
      {"/usr/bin/ardour8", "", "", H::Ardour, 8},
      {"/opt/Mixbus10/bin/mixbus10", "", "", H::Mixbus, 10},
      {"C:\\Program Files\\Tracktion\\Waveform 13\\Waveform 13.exe", "", "", H::TracktionWaveform, 13},
      {"C:\\Program Files\\Cakewalk\\Cakewalk Core\\Cakewalk.exe", "", "", H::CakewalkByBandlab, 0},
      {"C:\\Program Files\\Renoise 3.4.3\\Renoise.exe", "", "", H::Renoise, 0},
      {"C:\\Program Files\\Propellerhead\\Reason 12\\Reason.exe", "", "", H::Reason, 0},
      {"C:\\Program Files\\Blackmagic Design\\DaVinci Resolve\\Resolve.exe", "", "", H::DaVinciResolve, 0},
      {"C:\\Program Files\\Adobe\\Adobe Audition 2024\\Adobe Audition.exe", "", "", H::AdobeAudition, 0},
      {"C:\\Program Files\\Adobe\\Adobe Premiere Pro 2024\\Adobe Premiere Pro.exe", "", "", H::AdobePremierePro, 0},
      {"C:\\Program Files\\Native Instruments\\Maschine 2\\Maschine 2.exe", "", "", H::Maschine, 2},
      {"C:\\Program Files\\Vienna Ensemble Pro\\Vienna Ensemble Pro.exe", "", "", H::ViennaEnsemblePro, 0},
      {"/usr/bin/pluginval", "", "", H::Pluginval, 0},
      {"/usr/local/bin/clap-validator", "", "", H::ClapValidator, 0},
      {"C:\\Program Files\\Audacity\\Audacity.exe", "", "", H::Audacity, 0},
      {"/usr/bin/carla", "", "", H::Carla, 0},
      {"C:\\Users\\me\\Desktop\\MySynth.exe", "", "Sonore Standalone", H::SonoreStandalone, 0},
      // The standalone's executable is named after its PLUGIN, so the name
      // can be anything -- a host's name included. Its own report wins.
      {"C:\\Users\\me\\Desktop\\Sonar Delay.exe", "", "Sonore Standalone", H::SonoreStandalone, 0},
      {"/home/me/bin/Reason Compressor", "", "Sonore Standalone", H::SonoreStandalone, 0},
      {"C:\\Apps\\Waveform Shaper.exe", "", "Sonore Standalone", H::SonoreStandalone, 0},
      {"/Applications/Resolver.app/Contents/MacOS/Resolver", "com.example.resolver", "Sonore Standalone",
       H::SonoreStandalone, 0},
      {"/usr/bin/some-host", "", "REAPER", H::Reaper, 0},
      {"/usr/bin/some-host", "", "Bitwig Studio", H::BitwigStudio, 0},
      {"C:\\tools\\mystery.exe", "", "", H::UnknownHost, 0},
  };
  int wrong = 0;
  for (const Case& c : cases) {
    // Classified as given, not read from disk -- these paths are not on this
    // machine, and a test of the rules must not depend on what is installed.
    bool bridged = false;
    const H::HostType got = H::classify(c.path, c.bundle, c.reported, &bridged);
    const int major = H::majorVersionOf(std::string(c.path).substr(std::string(c.path).find_last_of("/\\") + 1));
    if (got != c.expect || (c.major && major != c.major)) {
      ++wrong;
      std::snprintf(msg, sizeof(msg), "%s -> %s (want %s), major %d", c.path, H::describe(got),
                    H::describe(c.expect), major);
      check(false, msg);
    }
  }
  std::snprintf(msg, sizeof(msg), "%d executables, bundles and reported names classified as expected",
                (int) (sizeof(cases) / sizeof(cases[0])) - wrong);
  check(wrong == 0, msg);

  const H cubase = H::fromProcess("C:\\nowhere\\Steinberg\\Cubase 13\\Cubase13.exe");
  check(cubase.isCubase() && cubase.isSteinberg() && cubase.isCubase7orLater() &&
            cubase.getMajorVersion() == 13 && std::strcmp(cubase.getHostDescription(), "Cubase") == 0,
        "Cubase 13: the checks, the version from its name, the description");
  const H bridge = H::fromProcess("C:\\nowhere\\Steinberg\\Cubase 5\\VSTBridgeApp.exe");
  check(bridge.isCubaseBridged(), "Cubase's VSTBridgeApp is Cubase, bridged");
  const H wavelab = H::fromProcess("C:\\nowhere\\WaveLab 6\\WaveLab6.exe");
  check(wavelab.isWavelab() && wavelab.isWavelabLegacy(), "WaveLab 6 is legacy");
  const H mixbus = H::fromProcess("/opt/Mixbus10/bin/mixbus10");
  check(mixbus.isArdour() && mixbus.isMixbus(), "Mixbus is Ardour as far as quirks go");
  const H waveform = H::fromProcess("C:\\x\\Waveform 13.exe");
  check(waveform.isTracktion() && waveform.type == H::TracktionWaveform, "Waveform is Tracktion");
  const H reported = H::fromProcess("/usr/bin/mystery", "", "Some Host", "2.1");
  check(std::strcmp(reported.getHostDescription(), "Some Host") == 0 && reported.getHostVersion() == "2.1",
        "an unknown host is described by the name and version it reported");

#if defined(_WIN32)
  // A real executable's version resource, read and not run.
  const char* reaper = "C:\\Program Files\\REAPER (x64)\\reaper.exe";
  if (std::filesystem::exists(reaper)) {
    const H real = H::fromProcess(reaper);
    std::snprintf(msg, sizeof(msg), "the installed REAPER: %s %s (major %d)", real.getHostDescription(),
                  real.getHostVersion().c_str(), real.getMajorVersion());
    check(real.isReaper() && !real.getHostVersion().empty() && real.getMajorVersion() >= 1, msg);
  } else {
    std::printf("  --   REAPER is not installed here; its version resource was not read\n");
  }
  char system[MAX_PATH] = {};
  GetSystemDirectoryA(system, MAX_PATH);
  const std::string notepad = std::string(system) + "\\notepad.exe";
  const H unknown = H::fromProcess(notepad);
  std::snprintf(msg, sizeof(msg), "an unknown executable keeps its product name: \"%s\" %s",
                unknown.getHostDescription(), unknown.getHostVersion().c_str());
  check(unknown.type == H::UnknownHost && std::strcmp(unknown.getHostDescription(), "Unknown") != 0 &&
            !unknown.getHostVersion().empty(),
        msg);
#endif

  // The plugin format, from the binary the code was loaded from.
  struct FormatCase {
    const char* module;
    const char* exe;
    H::PluginFormat expect;
  };
  const FormatCase formats[] = {
      {"C:\\Program Files\\Common Files\\VST3\\Gain.vst3\\Contents\\x86_64-win\\Gain.vst3",
       "C:\\REAPER\\reaper.exe", H::PluginFormat::Vst3},
      {"/home/u/.vst3/Gain.vst3/Contents/x86_64-linux/Gain.so", "/usr/bin/reaper", H::PluginFormat::Vst3},
      {"C:\\Program Files\\Common Files\\CLAP\\Gain.clap", "C:\\REAPER\\reaper.exe", H::PluginFormat::Clap},
      {"/Users/u/Library/Audio/Plug-Ins/CLAP/Gain.clap/Contents/MacOS/Gain", "/Applications/REAPER.app/Contents/MacOS/REAPER",
       H::PluginFormat::Clap},
      {"/Library/Audio/Plug-Ins/Components/Gain.component/Contents/MacOS/Gain",
       "/Applications/Logic Pro X.app/Contents/MacOS/Logic Pro X", H::PluginFormat::AudioUnit},
      {"/Applications/Gain.app/Contents/PlugIns/Gain.appex/Contents/MacOS/Gain", "/x", H::PluginFormat::AudioUnitV3},
      {"/usr/lib/lv2/gain.lv2/gain.so", "/usr/bin/ardour8", H::PluginFormat::Lv2},
      {"C:\\Apps\\Gain.exe", "C:\\Apps\\Gain.exe", H::PluginFormat::Standalone},
      {"/usr/lib/libsomething.so", "/usr/bin/host", H::PluginFormat::Undefined},
  };
  int formatWrong = 0;
  for (const FormatCase& f : formats)
    if (H::pluginFormatFromModulePath(f.module, f.exe) != f.expect) {
      ++formatWrong;
      std::snprintf(msg, sizeof(msg), "%s -> %s", f.module,
                    H::getPluginFormatName(H::pluginFormatFromModulePath(f.module, f.exe)));
      check(false, msg);
    }
  check(formatWrong == 0, "VST3, CLAP (file and bundle), AU, AUv3, LV2, standalone and none, from the module path");

  // This process: an executable that is no host anybody named.
  const H self;
  std::snprintf(msg, sizeof(msg), "this process is \"%s\" at %s, loaded as %s", self.getHostDescription(),
                self.getHostPath().c_str(), H::getPluginFormatName(H::getPluginLoadedAs()));
  check(!self.getHostPath().empty() && self.getHostPath().find("plugin_state") != std::string::npos &&
            H::getPluginLoadedAs() == H::PluginFormat::Standalone,
        msg);
  HostInfo info;
  info.name = "Sonore Standalone";
  info.version = "1.0.0";
  const H fromInfo(info);
  check(fromInfo.isSonoreStandalone(), "a HostInfo naming the standalone classifies as it");
}

// ── LADSPA ──────────────────────────────────────────────────────────────────

static void testLadspa(const std::string& library) {
  std::printf("\n-- LADSPA --\n");
  char msg[512];
  using namespace sonore::host;

  // The hint rules, without a plugin.
  {
    ladspa::PortRangeHint low{ladspa::kHintBoundedBelow | ladspa::kHintBoundedAbove |
                                  ladspa::kHintLogarithmic | ladspa::kHintDefaultLow,
                              10.0f, 1000.0f};
    const HostedParam p = LadspaPlugin::describePort(low, 48000.0);
    std::snprintf(msg, sizeof(msg), "a logarithmic LOW default is geometric: %.4f (10^1.5 = 31.6228)",
                  p.defaultValue);
    check(approx(p.defaultValue, std::pow(10.0, 1.5), 1e-3), msg);
    ladspa::PortRangeHint mid{ladspa::kHintBoundedBelow | ladspa::kHintBoundedAbove | ladspa::kHintDefaultMiddle,
                              -1.0f, 3.0f};
    check(approx(LadspaPlugin::describePort(mid, 48000.0).defaultValue, 1.0), "a linear MIDDLE is the mean");
    ladspa::PortRangeHint rate{ladspa::kHintBoundedBelow | ladspa::kHintBoundedAbove |
                                   ladspa::kHintSampleRate | ladspa::kHintDefaultMaximum,
                               0.0f, 0.5f};
    const HostedParam r = LadspaPlugin::describePort(rate, 44100.0);
    check(approx(r.maxValue, 22050.0) && approx(r.defaultValue, 22050.0),
          "rate-relative bounds are multiplied by the rate");
    ladspa::PortRangeHint toggle{ladspa::kHintToggled | ladspa::kHintDefault1, 0.0f, 0.0f};
    const HostedParam tg = LadspaPlugin::describePort(toggle, 48000.0);
    check(tg.stepped && tg.minValue == 0.0 && tg.maxValue == 1.0 && tg.defaultValue == 1.0,
          "a toggle is 0..1, stepped, defaulting on");
    ladspa::PortRangeHint open{ladspa::kHintBoundedBelow | ladspa::kHintDefault440, 20.0f, 0.0f};
    const HostedParam o = LadspaPlugin::describePort(open, 48000.0);
    check(o.minValue == 20.0 && o.maxValue > o.minValue && o.defaultValue == o.maxValue,
          "a range bounded on one side gets the other, and the default stays inside it");
  }

  check(LADSPAPluginFormat::fileMightContainThisPluginType(library), "the test library has a LADSPA extension");
  const std::vector<PluginDescription> found = describeFile(library);
  std::snprintf(msg, sizeof(msg), "describeFile finds the library's %d plugins", (int) found.size());
  check(found.size() == 2, msg);
  if (found.size() != 2) return;
  check(found[0].format == "LADSPA" && found[0].id == "4242001" && found[0].name == "Sonore Test Gain" &&
            found[0].vendor == "Sonore SDK tests" &&
            found[0].features.find("label=sonore_test_gain") != std::string::npos,
        "described: format, unique id, name, maker, label");

  // ── The gain, loaded and processing ──
  {
    std::unique_ptr<HostedPlugin> gain = loadPlugin(found[0]);
    check(gain && gain->isValid(), "loadPlugin loads a LADSPA description");
    if (!gain) return;
    check(gain->numParameters() == 2 && gain->parameter(0).name == "Gain (dB)" &&
              gain->parameter(0).minValue == -60.0 && gain->parameter(0).maxValue == 24.0 &&
              gain->parameter(1).stepped,
          "two control inputs: the gain's bounds, the invert a switch");
    check(gain->hasAudioInput() && !gain->acceptsMidi() && !gain->hasEditor() && !gain->hasBypass() &&
              gain->numPresets() == 0,
          "audio in, and none of what LADSPA does not have");
    check(gain->prepare(48000.0, 256, 2), "prepare instantiates and activates");
    gain->setParameterValue(0, -6.0);
    std::vector<float> l(256), r(256);
    for (int i = 0; i < 256; ++i) {
      l[(size_t) i] = std::sin((float) i * 0.1f);
      r[(size_t) i] = 0.25f;
    }
    const std::vector<float> dry = l;
    float* ch[2] = {l.data(), r.data()};
    AudioBlock<float> io(ch, 2, 256);
    gain->process(io);
    const double g = std::pow(10.0, -6.0 / 20.0);
    double worst = 0.0;
    for (int i = 0; i < 256; ++i) worst = (std::max)(worst, std::fabs((double) l[(size_t) i] - dry[(size_t) i] * g));
    std::snprintf(msg, sizeof(msg), "-6 dB through the plugin: worst error %.2e", worst);
    check(worst < 1e-6, msg);
    check(approx(r[100], l[100]), "a mono plugin on a stereo bus is heard on both sides");
    gain->setParameterValue(1, 1.0);
    gain->setParameterValue(0, 0.0);
    std::fill(l.begin(), l.end(), 0.5f);
    gain->process(io);
    check(approx(l[7], -0.5), "the toggle inverts");

    std::vector<uint8_t> state;
    check(gain->saveState(state) && !state.empty(), "the control values save");
    std::unique_ptr<HostedPlugin> again = loadPlugin(found[0]);
    check(again && again->loadState(state.data(), state.size()) && again->parameterValue(1) == 1.0 &&
              again->parameterValue(0) == 0.0,
          "and restore into another instance");
    std::vector<uint8_t> broken = state;
    broken.pop_back();
    check(!again->loadState(broken.data(), broken.size()), "a truncated state is refused");
    std::vector<std::pair<uint32_t, float>> decoded;
    check(LadspaPlugin::decodeState(state.data(), state.size(), &decoded) && decoded.size() == 2 &&
              decoded[0].first == 0 && decoded[1].first == 1,
          "the blob is keyed by port index");
    gain->setParameterValue(0, 1000.0);
    check(gain->parameterValue(0) == 24.0, "a value past the bound is clamped");
    gain->release();
  }

  // ── The stereo one: latency, integers, rate-relative bounds ──
  {
    std::unique_ptr<HostedPlugin> st = loadPlugin(found[1]);
    check(st && st->numParameters() == 2, "the second plugin of the library loads by its own id");
    if (!st) return;
    check(st->prepare(44100.0, 128, 2), "prepared at 44.1 kHz");
    std::snprintf(msg, sizeof(msg), "the \"latency\" output port is the latency: %u", st->latencySamples());
    check(st->latencySamples() == 64, msg);
    const HostedParam& corner = st->parameter(1);
    std::snprintf(msg, sizeof(msg), "the corner's bounds follow the rate: %.2f..%.1f Hz, default %.2f",
                  corner.minValue, corner.maxValue, corner.defaultValue);
    check(approx(corner.maxValue, 0.25 * 44100.0, 0.01) && approx(corner.minValue, 0.0001 * 44100.0, 1e-3) &&
              corner.defaultValue > corner.minValue && corner.defaultValue < corner.maxValue / 10.0,
          msg);
    check(st->parameter(0).stepped && st->parameterValue(0) == 5.0 && st->parameterText(0, 5.0) == "5",
          "an integer control: stepped, MIDDLE of 1..8 rounded, printed whole");
    std::vector<float> l(128, 0.3f), r(128, -0.7f);
    float* ch[2] = {l.data(), r.data()};
    AudioBlock<float> io(ch, 2, 128);
    st->process(io);
    check(approx(l[50], 0.3) && approx(r[50], -0.7), "stereo in, stereo out, channels kept apart");
    std::vector<float> big(512);
    float* bch[2] = {big.data(), big.data()};
    AudioBlock<float> tooLong(bch, 2, 512);
    big[0] = 9.0f;
    st->process(tooLong);
    check(big[0] == 9.0f, "a block longer than prepared is left alone, not overrun");
  }

  // ── A port that is audio AND control ──
  // The library offers it only while this variable is set. Loaded, the port
  // was filed as audio and then connected to one control float, and the
  // plugin's first run wrote its whole block past it, into the host's heap.
  {
    auto offerMalformed = [](bool on) {
#if defined(_WIN32)
      SetEnvironmentVariableA("SONORE_LADSPA_TEST_MALFORMED", on ? "1" : nullptr);
#else
      if (on) setenv("SONORE_LADSPA_TEST_MALFORMED", "1", 1);
      else unsetenv("SONORE_LADSPA_TEST_MALFORMED");
#endif
    };
    offerMalformed(true);
    const std::vector<PluginDescription> withBad = describeFile(library);
    std::unique_ptr<HostedPlugin> bad = withBad.size() == 3 ? loadPlugin(withBad[2]) : nullptr;
    offerMalformed(false);
    check(withBad.size() == 3 && withBad[2].id == "4242003", "the malformed plugin is offered on request");
    check(withBad.size() == 3 && !bad,
          "a port declared both audio and control is refused, not connected to one float");
  }
}

// ── The plugin list ─────────────────────────────────────────────────────────

static void testPluginList(const std::string& library) {
  std::printf("\n-- plugin list --\n");
  namespace fs = std::filesystem;
  using namespace sonore::host;
  char msg[512];

  const fs::path dir = fs::path(sonoretest::tempDir()) / "ladspa-folder";
  std::error_code ec;
  fs::create_directories(dir, ec);
#if defined(_WIN32)
  const char* ext = ".dll";
#else
  const char* ext = ".so";
#endif
  const std::string good = (dir / (std::string("a_gain") + ext)).string();
  const std::string crasher = (dir / (std::string("crasher") + ext)).string();
  const std::string junk = (dir / (std::string("notaplugin") + ext)).string();
  fs::copy_file(library, good, fs::copy_options::overwrite_existing, ec);
  { std::ofstream(crasher, std::ios::binary) << "this file pretends to crash whoever loads it"; }
  { std::ofstream(junk, std::ios::binary) << "MZ but not really"; }
  const std::string cachePath = (fs::path(sonoretest::tempDir()) / "plugins.cache").string();

  // The run before this one died inside crasher: its dead man's mark is on disk.
  {
    std::ofstream out(cachePath, std::ios::binary);
    out << "SONORE-PLUGIN-CACHE-1\nSCANNING\t" << crasher << "\t0\n";
  }
  PluginCache cache;
  check(cache.load(cachePath.c_str()) && cache.crashedOn() == crasher,
        "a cache whose last scan never finished names the file it died in");
  const std::vector<std::string> files = LADSPAPluginFormat::filesIn(dir.string());
  check(files.size() == 3, "LADSPAPluginFormat::filesIn lists the folder's three libraries");
  const std::vector<PluginDescription> found = scanFilesCached(files, cache, cachePath.c_str());
  std::snprintf(msg, sizeof(msg), "the scan finds %d plugins and blacklists the crasher without opening it",
                (int) found.size());
  check(found.size() == 2 && cache.isBlacklisted(crasher) && cache.plugins().size() == 2, msg);
  check(cache.isUpToDate(junk) && cache.knownFor(junk).empty(),
        "a library that is not a plugin is remembered as empty, so it is not reopened");
  // A plugin named with a tab -- legal outside Windows -- dies mid-scan. The
  // mark is written with the tab as a space, and matched nothing: the plugin
  // was loaded again, and killed the host again, on every start.
  {
    const std::string tabbedCache = (fs::path(sonoretest::tempDir()) / "tabbed.cache").string();
    const std::string tabbed = "/plugins/evil\tname.so";
    PluginCache dying;
    dying.beginScanning(tabbedCache.c_str(), tabbed);
    PluginCache next;
    next.load(tabbedCache.c_str());
    scanFilesCached({tabbed}, next, tabbedCache.c_str());
    check(next.isBlacklisted(tabbed), "a plugin with a tab in its path is blacklisted after it kills a scan");
  }

#if defined(_WIN32)
  // The junk library again, with the process allowing Windows' hard-error
  // boxes, as a process started by ctest (or a DAW) may. LoadLibrary raised
  // "Bad Image" -- modal, invisible from a service or a test runner -- and
  // the scan waited on it for half an hour. openLib turns them off for its
  // own call; on a thread with a deadline, so a regression fails, not hangs.
  {
    const UINT previous = SetErrorMode(0);
    std::atomic<int> state{0}; // 0 running, 1 returned null, 2 returned a handle
    std::thread loader([&] {
      host::LibHandle h = host::openLib(junk.c_str());
      state = h ? 2 : 1;
      if (h) host::closeLib(h);
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (state == 0 && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool returned = state != 0;
    check(returned && state == 1, "a corrupt library fails to open at once, with no hard-error box to wait on");
    SetErrorMode(previous);
    if (returned) loader.join();
    else {
      loader.detach();
      std::printf("  (the loader is still inside LoadLibrary; exiting)\n");
      std::fflush(stdout);
      std::_Exit(1);
    }
  }
#endif

  // Saved after the scan, and read back by somebody else.
  PluginCache reread;
  check(reread.load(cachePath.c_str()) && reread.plugins().size() == 2 && reread.isBlacklisted(crasher) &&
            reread.crashedOn().empty(),
        "the list saves and restores: plugins, blacklist, and no stale crash mark");

  // ── The component over it ──
  // Three of other formats, described by hand, so the columns have something
  // to sort: they need not exist to be listed.
  PluginDescription zeta;
  zeta.path = "C:/plugins/Zeta.clap";
  zeta.format = "CLAP";
  zeta.id = "com.acme.zeta";
  zeta.name = "Zeta Verb";
  zeta.vendor = "Acme";
  zeta.features = "audio-effect;reverb;stereo";
  PluginDescription alpha;
  alpha.path = "C:/plugins/Alpha.vst3";
  alpha.format = "VST3";
  alpha.id = "00112233445566778899AABBCCDDEEFF";
  alpha.name = "alpha synth";
  alpha.vendor = "Zed";
  alpha.features = "Instrument|Synth";
  alpha.isInstrument = true;
  PluginDescription mid;
  mid.path = "/usr/lib/lv2/mid.lv2";
  mid.format = "LV2";
  mid.id = "urn:mid";
  mid.name = "Mid Delay";
  mid.vendor = "Beta";
  mid.features = "http://lv2plug.in/ns/lv2core#DelayPlugin";
  reread.remember(zeta.path, {zeta});
  reread.remember(alpha.path, {alpha});
  reread.remember(mid.path, {mid});

  gfx::PluginListComponent list(reread, cachePath);
  list.setBounds({0.0f, 0.0f, 600.0f, 260.0f});
  check(list.numRows() == 6, "five plugins and one blacklisted file, one row each");
  auto column = [&](int c) {
    std::string s;
    for (int r = 0; r < list.numRows(); ++r) s += list.cellText(r, c) + (r + 1 < list.numRows() ? "," : "");
    return s;
  };
  std::snprintf(msg, sizeof(msg), "sorted by name, case-blind, blacklisted last: %s", column(0).c_str());
  check(column(0) == "alpha synth,Mid Delay,Sonore Test Gain,Sonore Test Stereo,Zeta Verb,crasher" +
                         std::string(ext),
        msg);
  std::snprintf(msg, sizeof(msg), "categories from each format's words: %s", column(2).c_str());
  check(column(2) == "Instrument,Delay,Effect,Effect,Reverb,Blacklisted", msg);
  check(column(1) == "VST3,LV2,LADSPA,LADSPA,CLAP,", "the format column");

  gfx::Component root;
  root.setBounds({0.0f, 0.0f, 600.0f, 260.0f});
  root.addChild(&list);
  gfx::MouseRouter router(root);
  const float manufacturerX = list.table().columnX(gfx::PluginListComponent::kManufacturerColumn) + 20.0f;
  router.mouseDown({manufacturerX, 10.0f});
  router.mouseUp({manufacturerX, 10.0f});
  std::snprintf(msg, sizeof(msg), "clicking Manufacturer sorts by it: %s", column(3).c_str());
  check(column(3) == "Acme,Beta,Sonore SDK tests,Sonore SDK tests,Zed,", msg);
  router.mouseDown({manufacturerX, 10.0f});
  router.mouseUp({manufacturerX, 10.0f});
  check(column(3) == "Zed,Sonore SDK tests,Sonore SDK tests,Beta,Acme,", "clicking it again reverses it");
  check(list.cellText(5, 0) == "crasher" + std::string(ext), "and the blacklisted file stays at the bottom");

  // ── Painted ──
  {
    auto face = gfx::systemTypeface();
    if (face) list.setFont(gfx::Font(face, 13.0f));
    list.table().setFont(face ? gfx::Font(face, 13.0f) : gfx::Font());
    gfx::Bitmap bmp(600, 260);
    bmp.clear(gfx::Colour::fromRGB(0x000000));
    gfx::Graphics g(bmp);
    root.paintTree(g);
    auto inkIn = [&](int x0, int y0, int x1, int y1) {
      // Pixels that differ from the row's own background, sampled at its
      // left edge where no text is drawn.
      const gfx::PremulColour bg = bmp.pixelAt(x0, y0);
      int n = 0;
      for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
          const gfx::PremulColour p = bmp.pixelAt(x, y);
          if (std::abs((int) p.r - (int) bg.r) + std::abs((int) p.g - (int) bg.g) + std::abs((int) p.b - (int) bg.b) > 60) ++n;
        }
      return n;
    };
    const float header = list.table().headerHeight();
    const float row = list.table().rowHeight();
    const int nameInk = inkIn(1, (int) (header + 1), 200, (int) (header + row - 1));
    const int emptyFormatInk = inkIn((int) list.table().columnX(1) + 2, (int) (header + 5 * row + 1),
                                     (int) list.table().columnX(2) - 2, (int) (header + 6 * row - 1));
    std::snprintf(msg, sizeof(msg), "painted: the first row's name has ink (%d px), the blacklisted row's empty format cell none (%d px)",
                  nameInk, emptyFormatInk);
    check(face == nullptr || (nameInk > 30 && emptyFormatInk == 0), msg);
    const int buttonInk = inkIn(7, 260 - 27, 96, 260 - 3);
    check(buttonInk > 0, "the Scan and Remove buttons are drawn below the table");
  }

  // ── Remove, forgive, rescan ──
  list.table().setSelectedRow(4); // in reverse-manufacturer order: Acme's Zeta Verb is last of the plugins
  check(list.pluginForRow(4) && list.pluginForRow(4)->name == "Zeta Verb", "row 4 is Zeta Verb");
  check(list.removeSelected() && list.numRows() == 5, "Remove takes the selected plugin off the list");
  PluginCache afterRemove;
  afterRemove.load(cachePath.c_str());
  bool zetaGone = true;
  for (const auto& d : afterRemove.plugins()) zetaGone = zetaGone && d.name != "Zeta Verb";
  check(zetaGone, "and the change is on disk at once");

  // A LADSPA plugin removed comes back when its file is scanned again.
  int gainRow = -1;
  for (int r = 0; r < list.numRows(); ++r)
    if (list.cellText(r, 0) == "Sonore Test Gain") gainRow = r;
  check(gainRow >= 0 && list.removeRow(gainRow) && list.numRows() == 4, "removing the gain");
  list.scanFor({dir.string()});
  int back = 0;
  for (int r = 0; r < list.numRows(); ++r) back += list.cellText(r, 0) == "Sonore Test Gain" ? 1 : 0;
  check(back == 1 && list.numRows() == 5, "a rescan brings it back, once, without reopening the crasher");

  const int crasherRow = list.numRows() - 1;
  check(list.rowIsBlacklisted(crasherRow) && list.removeRow(crasherRow) && !reread.isBlacklisted(crasher),
        "removing a blacklisted row forgives the file");
  list.clearList();
  check(list.numRows() == 0 && reread.plugins().empty(), "clear list empties it");

  // Stale files: a plugin whose library has gone is found and removed.
  list.scanFor({dir.string()});
  fs::remove(junk, ec);
  PluginDescription ghost;
  ghost.path = (dir / "ghost.clap").string();
  ghost.format = "CLAP";
  ghost.id = "ghost";
  ghost.name = "Ghost";
  reread.remember(ghost.path, {ghost});
  list.refresh();
  check(list.removeMissing() == 1 && list.numRows() == 2, "removeMissing drops the plugin whose file is gone");
}

int main(int argc, char** argv) {
  std::printf("plugin_state_test\n");
  testValueTreeState();
  testHostType();
  if (argc < 2) {
    check(false, "no LADSPA test library given: plugin_state_test <ladspa_gain_plugin library>");
  } else {
    testLadspa(argv[1]);
    testPluginList(argv[1]);
  }
  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  std::printf(g_failures == 0 ? "PLUGIN STATE TESTS PASSED\n" : "PLUGIN STATE TESTS FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
