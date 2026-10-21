// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: sonore/host.h hosts a VST3 plugin whose controller is its own
// object: the halves are connected, and the controller's edits reach the
// processor.
//
// The fixture's controller edits a visible parameter (100) and a hidden one
// (200) as soon as it has a component handler, as its editor would; the
// processor records what process() delivered for each, and both halves count
// their live IConnectionPoint connections in the process environment.
//
// The processor also moves a read-only meter (300) through its output
// parameter changes, which must reach the controller -- the plugin's own
// editor reads it there -- and the caller, as an edit.
//
// And the halves talk as a plugin built on Steinberg's SDK has them talk:
// through IMessages made by the host application initialize() is given, sent
// over the connection -- on the main thread, never from process().

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <sonore/host.h>

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static int readEnv(const char* name) {
  char text[32] = {0};
#if defined(_WIN32)
  GetEnvironmentVariableA(name, text, (DWORD) sizeof(text));
#else
  if (const char* v = std::getenv(name)) std::snprintf(text, sizeof(text), "%s", v);
#endif
  return std::atoi(text);
}

static void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
  SetEnvironmentVariableA(name, value);
#else
  setenv(name, value, 1);
#endif
}

static void clearEnv(const char* name) { setEnv(name, "0"); }

static std::string envText(const char* name) {
  char text[256] = {0};
#if defined(_WIN32)
  GetEnvironmentVariableA(name, text, (DWORD) sizeof(text));
#else
  if (const char* v = std::getenv(name)) std::snprintf(text, sizeof(text), "%s", v);
#endif
  return text;
}

/** A half that refuses initialize() -- a plugin missing a licence, or a
 *  host feature it requires -- and what host.h then does with it. It went
 *  on: a refused controller was used regardless, uninitialised, and either
 *  half was sent terminate() for an initialize() that never succeeded. */
static void checkRefusedInitialize(const sonore::host::PluginDescription& description) {
  std::printf("── A half that refuses initialize() ──\n");
  char line[200];
  for (const char* which : {"SONORE_SPLIT_REFUSE_INIT", "SONORE_SPLIT_REFUSE_CTRL_INIT"}) {
    clearEnv("SONORE_SPLIT_TERMINATED_UNINIT");
    clearEnv("SONORE_SPLIT_CONNECTED");
    setEnv(which, "1");
    bool loaded = false;
    {
      auto refused = sonore::host::loadPlugin(description);
      loaded = refused != nullptr;
    }
    clearEnv(which);
    const bool processor = std::string(which) == "SONORE_SPLIT_REFUSE_INIT";
    std::snprintf(line, sizeof(line), "a %s refusing initialize() is not loaded (%s), nor sent "
                  "terminate() for it (%d), nor left connected (%d)",
                  processor ? "processor" : "controller", loaded ? "loaded" : "refused",
                  readEnv("SONORE_SPLIT_TERMINATED_UNINIT"), readEnv("SONORE_SPLIT_CONNECTED"));
    check(!loaded && readEnv("SONORE_SPLIT_TERMINATED_UNINIT") == 0 &&
              readEnv("SONORE_SPLIT_CONNECTED") == 0,
          line);
  }
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: vst3_split_test <SonoreVst3SplitFixture.vst3>\n");
    return 2;
  }
  std::printf("── VST3 with a separate controller: %s ──\n", argv[1]);
  for (const char* name :
       {"SONORE_SPLIT_CONNECTED", "SONORE_SPLIT_P100", "SONORE_SPLIT_P200", "SONORE_SPLIT_HOST_APP",
        "SONORE_SPLIT_SUPPORT", "SONORE_SPLIT_HELLO_SENT", "SONORE_SPLIT_HELLO_HEARD",
        "SONORE_SPLIT_HELLO_INTACT", "SONORE_SPLIT_CTRL_HEARD", "SONORE_SPLIT_RT_HEARD",
        "SONORE_SPLIT_RT_DELIVERED", "SONORE_SPLIT_TERMINATED_UNINIT"})
    clearEnv(name);
  setEnv("SONORE_SPLIT_HELLO_LEFT", "-1");
  setEnv("SONORE_SPLIT_HOST_NAME", "(unset)");
  sonore::host::setHostName("Split Test Host");

  const auto found = sonore::host::describeFile(argv[1]);
  check(found.size() == 1, "the processor class is described, the controller class is not");
  if (found.empty()) return 1;
  auto plugin = sonore::host::loadPlugin(found.front());
  check(plugin != nullptr, "the plugin loads, controller and all");
  if (!plugin) return 1;

  char line[200];
  std::snprintf(line, sizeof(line), "the two halves are connected to each other (%d connections)",
                readEnv("SONORE_SPLIT_CONNECTED"));
  check(readEnv("SONORE_SPLIT_CONNECTED") == 2, line);
  const bool dirty = plugin->takeStateDirty(), again = plugin->takeStateDirty();
  check(dirty && !again, "the controller's setDirty marks the session unsaved, once");

  // ── The host context ─────────────────────────────────────────────────────
  //
  // initialize() was given null, so a plugin built on Steinberg's SDK had no
  // IHostApplication: no host name, and no way to make the IMessage its
  // halves talk in -- allocateMessage() answers null. Connected, and silent.
  check(readEnv("SONORE_SPLIT_HOST_APP") == 1,
        "initialize() is given a context holding an IHostApplication");
  std::snprintf(line, sizeof(line), "...which goes by the name the caller set (\"%s\")",
                envText("SONORE_SPLIT_HOST_NAME").c_str());
  check(envText("SONORE_SPLIT_HOST_NAME") == "Split Test Host", line);
  std::snprintf(line, sizeof(line), "...and says through IPlugInterfaceSupport which plugin "
                "interfaces it uses (%d of 7)", readEnv("SONORE_SPLIT_SUPPORT"));
  check(readEnv("SONORE_SPLIT_SUPPORT") == 7, line);
  check(readEnv("SONORE_SPLIT_CTRL_HEARD") == 1,
        "a message the controller sends on the main thread reaches the processor");

  check(plugin->prepare(48000.0, 64, 2), "it prepares");
  std::snprintf(line, sizeof(line), "the processor's message, made in initialize(), reaches the "
                "controller when activated (sent %d, heard %d)", readEnv("SONORE_SPLIT_HELLO_SENT"),
                readEnv("SONORE_SPLIT_HELLO_HEARD"));
  check(readEnv("SONORE_SPLIT_HELLO_SENT") == 1 && readEnv("SONORE_SPLIT_HELLO_HEARD") == 1, line);
  std::snprintf(line, sizeof(line), "...its int, float, string and binary attributes as they were "
                "set, copied from buffers since overwritten (%d of 31)",
                readEnv("SONORE_SPLIT_HELLO_INTACT"));
  check(readEnv("SONORE_SPLIT_HELLO_INTACT") == 31, line);
  std::snprintf(line, sizeof(line), "...and the plugin's own release is its last: the host keeps "
                "no reference to it (%d left)", readEnv("SONORE_SPLIT_HELLO_LEFT"));
  check(readEnv("SONORE_SPLIT_HELLO_LEFT") == 0, line);

  std::vector<float> l(64, 0.0f), r(64, 0.0f);
  float* ch[2] = {l.data(), r.data()};
  sonore::AudioBlock<float> io(ch, 2, 64);
  plugin->process(io, nullptr);
  std::snprintf(line, sizeof(line), "an edit made in the controller reaches the processor (0.%03d)",
                readEnv("SONORE_SPLIT_P100"));
  check(readEnv("SONORE_SPLIT_P100") == 750, line);
  std::snprintf(line, sizeof(line), "...hidden parameters included, which the host does not list "
                "(0.%03d)", readEnv("SONORE_SPLIT_P200"));
  check(readEnv("SONORE_SPLIT_P200") == 250, line);

  int meter = -1;
  for (int i = 0; i < plugin->numParameters(); ++i)
    if (plugin->parameter(i).name == "Meter") meter = i;
  check(meter >= 0, "the processor's read-only meter is listed");
  std::vector<sonore::host::ParamEdit> edits;
  plugin->drainParameterEdits(edits);
  double reported = -1.0;
  for (const auto& e : edits)
    if (e.kind == sonore::host::ParamEdit::Kind::kValue && e.index == meter) reported = e.value;
  std::snprintf(line, sizeof(line), "a value the processor reports reaches the caller as an edit, "
                "at the block's last point (%.3f)", reported);
  check(meter >= 0 && std::fabs(reported - 0.625) < 1e-9, line);
  const double shown = meter >= 0 ? plugin->parameterValue(meter) : -1.0;
  std::snprintf(line, sizeof(line), "...and the controller, where the plugin's editor reads it "
                "(%.3f)", shown);
  check(std::fabs(shown - 0.625) < 1e-9, line);

  // Two hundred changes with no block between them -- a knob dragged while
  // no audio runs. The queue holds 64, and when it was full the NEWEST was
  // the one dropped: the processor stopped at some step of the drag while
  // the controller showed where it ended.
  bool allTaken = true;
  for (int i = 1; i <= 200; ++i)
    if (!plugin->setParameterValue(0, (double) i / 1000.0)) allTaken = false;
  plugin->process(io, nullptr);
  std::snprintf(line, sizeof(line), "after 200 changes between two blocks the processor has the "
                "last one (0.%03d)", readEnv("SONORE_SPLIT_P100"));
  check(allTaken && readEnv("SONORE_SPLIT_P100") == 200, line);

  // A meter reporting on every block, 1,200 blocks with nobody draining --
  // a UI asleep for a while, or blocks far smaller than its tick. The
  // reports went through one queue of 1,024, and a full queue refused the
  // NEWEST: the meter, and anything else the DSP moved, stayed at the value
  // of the 1,024th block. What matters is the latest, and that it arrives.
  for (int i = 0; i < 1200; ++i) plugin->process(io, nullptr);
  edits.clear();
  plugin->drainParameterEdits(edits);
  reported = -1.0;
  for (const auto& e : edits)
    if (e.kind == sonore::host::ParamEdit::Kind::kValue && e.index == meter) reported = e.value;
  const double expected = (double) (1202 % 997) / 1000.0; // the 1,202nd block in all
  std::snprintf(line, sizeof(line), "after 1,200 undrained blocks the newest meter value arrives "
                "(%.3f, expected %.3f; controller %.3f)", reported, expected,
                meter >= 0 ? plugin->parameterValue(meter) : -1.0);
  check(meter >= 0 && std::fabs(reported - expected) < 1e-9 &&
            std::fabs(plugin->parameterValue(meter) - expected) < 1e-9,
        line);

  // A processor sending from process(): the controller's notify() is UI
  // code -- it allocates, locks, redraws -- and must not run on the audio
  // thread. Refused from the thread that loaded the plugin when it is the
  // one inside process(), as in an offline render, and from any other.
  setEnv("SONORE_SPLIT_RT_MSG", "1");
  plugin->process(io, nullptr);
  {
    std::thread audio([&] {
      std::vector<float> al(64, 0.0f), ar(64, 0.0f);
      float* ach[2] = {al.data(), ar.data()};
      for (int i = 0; i < 64; ++i) {
        sonore::AudioBlock<float> block(ach, 2, 64);
        plugin->process(block, nullptr);
      }
    });
    audio.join();
  }
  clearEnv("SONORE_SPLIT_RT_MSG");
  std::snprintf(line, sizeof(line), "a message the processor sends from process() never reaches "
                "the controller (%d heard, %d accepted, of 65)", readEnv("SONORE_SPLIT_RT_HEARD"),
                readEnv("SONORE_SPLIT_RT_DELIVERED"));
  check(readEnv("SONORE_SPLIT_RT_HEARD") == 0 && readEnv("SONORE_SPLIT_RT_DELIVERED") == 0, line);

  // A caller's MidiBuffer may hold a position at or past the end of the
  // block, or before its start; MidiBuffer does not know how long the block
  // is. CLAP moved such an event inside; VST3 handed the plugin a
  // sampleOffset past numSamples, which a plugin rendering up to its next
  // event's offset renders past its buffers for.
  clearEnv("SONORE_SPLIT_LATE_EVENTS");
  sonore::MidiBuffer stray;
  stray.addEvent(sonore::MidiMessage::noteOn(0, 60, 100), 63);
  stray.addEvent(sonore::MidiMessage::noteOff(0, 60), 64);
  stray.addEvent(sonore::MidiMessage::noteOn(0, 62, 100), 1000);
  check(plugin->acceptsMidi(), "the fixture takes notes");
  plugin->process(io, &stray);
  std::snprintf(line, sizeof(line), "every event reaches the plugin inside its 64-frame block "
                "(%d outside)", readEnv("SONORE_SPLIT_LATE_EVENTS"));
  check(readEnv("SONORE_SPLIT_LATE_EVENTS") == 0, line);

  // IComponentHandler belongs to the UI thread, and plugins call it from
  // process() anyway: a gesture and a value from the audio thread, and now
  // and then a restart. The host kept those edits in a plain vector that
  // drainParameterEdits() copies and clears on the main thread, so the two
  // threads met inside one vector -- a heap-use-after-free under ASan --
  // and the audio thread allocated whenever it grew. Here the audio thread
  // runs 20,000 blocks while the main thread drains.
  {
    setEnv("SONORE_SPLIT_RT_EDITS", "1");
    std::atomic<bool> done{false};
    std::thread audio([&] {
      std::vector<float> al(64, 0.0f), ar(64, 0.0f);
      float* ach[2] = {al.data(), ar.data()};
      for (int i = 0; i < 20000; ++i) {
        sonore::AudioBlock<float> block(ach, 2, 64);
        plugin->process(block, nullptr);
      }
      done.store(true);
    });
    // One gesture a block: more than 20,000 is an edit read twice.
    size_t begins = 0, ends = 0, values = 0, misplaced = 0;
    std::vector<sonore::host::ParamEdit> got;
    for (bool last = false; !last;) {
      last = done.load();
      got.clear();
      plugin->drainParameterEdits(got);
      for (const auto& e : got) {
        if (e.index < 0 || e.index >= plugin->numParameters()) ++misplaced;
        if (e.kind == sonore::host::ParamEdit::Kind::kGestureBegin) ++begins;
        if (e.kind == sonore::host::ParamEdit::Kind::kGestureEnd) ++ends;
        if (e.kind == sonore::host::ParamEdit::Kind::kValue) ++values;
      }
    }
    audio.join();
    clearEnv("SONORE_SPLIT_RT_EDITS");
    std::snprintf(line, sizeof(line), "edits a plugin makes from inside process() reach the main "
                  "thread's drain, each once (%zu gestures begun, %zu ended, %zu values, %zu "
                  "misplaced, in 20000 blocks)", begins, ends, values, misplaced);
    check(begins > 0 && values > 0 && begins <= 20000 && ends <= 20000 && misplaced == 0, line);
  }

  // An editor that resizes itself -- its corner dragged, its zoom changed --
  // asks through IPlugFrame::resizeView, and the frame said no to every
  // request: a view that waits for onSize(), as Steinberg's own do, kept its
  // size, editorSize() never moved, and the caller, told to follow it, had
  // nothing to follow. The fixture's view asks for 640x480 as it is attached.
  {
    int notAWindow = 0; // the fixture's view never touches the window
    const bool opened = plugin->hasEditor() && plugin->openEditor(&notAWindow);
    check(opened, "the fixture's editor opens");
    if (opened) {
      uint32_t w = 0, h = 0;
      const bool sized = plugin->editorSize(w, h);
      std::snprintf(line, sizeof(line), "an editor that asks for 640x480 is that size, for the "
                    "caller to follow (%ux%u; resizeView %s, onSize %s)", (unsigned) w,
                    (unsigned) h, readEnv("SONORE_SPLIT_RESIZE_ANSWER") == 1 ? "accepted" : "refused",
                    envText("SONORE_SPLIT_RESIZE_ONSIZE").empty()
                        ? "never"
                        : envText("SONORE_SPLIT_RESIZE_ONSIZE").c_str());
      check(sized && w == 640 && h == 480 && readEnv("SONORE_SPLIT_RESIZE_ANSWER") == 1, line);
      // And the caller's window dragged to 800x600: canResize(),
      // checkSizeConstraint() and onSize(), as a Steinberg host does.
      const bool took = plugin->setEditorSize(800, 600);
      const bool resized = plugin->editorSize(w, h);
      std::snprintf(line, sizeof(line), "the caller resizes the editor to 800x600 (%s, %ux%u, onSize %s)",
                    took ? "taken" : "refused", (unsigned) w, (unsigned) h,
                    envText("SONORE_SPLIT_RESIZE_ONSIZE").c_str());
      check(took && resized && w == 800 && h == 600 && envText("SONORE_SPLIT_RESIZE_ONSIZE") == "800x600",
            line);
      plugin->closeEditor();
    }
  }

  plugin->release();
  plugin.reset();
  std::snprintf(line, sizeof(line), "and unloading disconnects both before terminating them "
                "(%d left)", readEnv("SONORE_SPLIT_CONNECTED"));
  check(readEnv("SONORE_SPLIT_CONNECTED") == 0, line);

  checkRefusedInitialize(found.front());

  // ── The controller's own state ───────────────────────────────────────────
  //
  // A plugin keeps some state in its controller alone -- an editor's open
  // tab, a zoom -- and hands it over through IEditController::getState. The
  // host saved the component's state only, so a session reopened with the
  // sound restored and the editor back at its defaults.
  {
    setEnv("SONORE_SPLIT_TAB", "7");
    auto saved = sonore::host::loadPlugin(found.front());
    std::vector<uint8_t> blob;
    const bool wrote = saved && saved->saveState(blob);
    saved.reset();
    setEnv("SONORE_SPLIT_TAB", "0");
    setEnv("SONORE_SPLIT_RESTORED_TAB", "-1");
    auto reopened = sonore::host::loadPlugin(found.front());
    const bool read = reopened && reopened->loadState(blob.data(), blob.size());
    std::snprintf(line, sizeof(line), "the controller's own state is saved with the session and "
                  "given back to it (tab %d, 7 saved)", readEnv("SONORE_SPLIT_RESTORED_TAB"));
    check(wrote && read && readEnv("SONORE_SPLIT_RESTORED_TAB") == 7, line);
    // A session saved before -- the component's bytes and nothing else --
    // still loads, and leaves the controller's state alone.
    setEnv("SONORE_SPLIT_RESTORED_TAB", "-1");
    const uint8_t none = 0;
    check(reopened && reopened->loadState(&none, 0) && readEnv("SONORE_SPLIT_RESTORED_TAB") == -1,
          "...and a session saved with the component's state alone still loads");
    reopened.reset();
    clearEnv("SONORE_SPLIT_TAB");
  }

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE VST3 SPLIT PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
