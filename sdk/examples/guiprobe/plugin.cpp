// SPDX-License-Identifier: Apache-2.0
// Sonore SDK test fixture: a plugin whose interface TALKS the moment it loads.
//
// It exists to prove the whole GUI chain end to end, which no amount of
// extension-contract checking can: page loaded -> bridge injected before page
// script -> JS ran -> message posted -> C++ parsed it -> lock-free queue ->
// audio thread applied it. If any link is broken the parameter never moves, and
// the host test says so.
//
// The magic values are deliberately odd numbers no default could produce.
#define SONORE_NUM_PARAMS 3

#include <sonore/dsp.h>
// The bag the host reads back, which is how a test sees what the plugin was
// told about its surroundings.
#include <sonore/state_bag.h>

struct SonoreDsp {
  sonore::Smooth gain;
  // The fixture's second job: report what the host said about musical time, so
  // a test can prove the transport actually arrived. The CLAP beat position is
  // FIXED POINT, and getting that conversion wrong is silent: the plugin still
  // runs, it is just wrong about time forever.
  sonore::TransportInfo transport;
  /** The fixture's fourth job: prove the OFFLINE flag reaches a DSP that uses
   *  the simple process() signature.
   *
   *  That is the harder half of the feature. A DSP taking a ProcessContext can
   *  read ctx.offline every block; one taking process(block, params) has no
   *  context at all, and only learns the render mode because the wrapper
   *  re-runs prepare() when it changes. Reading it HERE, in prepare, is what
   *  makes the test prove that re-prepare actually happens. */
  bool offline = false;

  void prepare(const sonore::ProcessSpec& spec) {
    gain.setup((float) spec.sampleRate, 5.0f);
    gain.snap(1.0f);
    offline = spec.offline;
  }

  /** Optional hook: the wrapper calls this before every block when present. */
  void setTransport(const sonore::TransportInfo& t) { transport = t; }

  /** The fixture's sixth job: prove the host INTRODUCED ITSELF.
   *
   *  Told once at creation rather than per block, because a host does not
   *  change identity mid-session. Written into the state bag so a test can
   *  read it back out of a saved blob -- which is the only channel a host has
   *  for seeing what a plugin was told, short of listening to the audio. */
  void setHostInfo(const sonore::HostInfo& info) { host = info; }
  sonore::HostInfo host;

  /**
   * A file the user picked. [main-thread] -- see WantsFile in the wrapper.
   *
   * Declared here for a reason beyond the fixture: without ONE DSP in the
   * build that declares it, the true branch of sendFile() is never
   * instantiated in any format, and a compile error in it would wait for the
   * first plugin that wanted a file browser.
   *
   * The dialog itself is never opened by the probe's page. It is modal, and a
   * modal dialog inside the test suite is a suite that never finishes.
   */
  void loadFile(const char* purpose, const char* path) {
    lastFilePurpose = purpose ? purpose : "";
    lastFilePath = path ? path : "";
  }
  std::string lastFilePurpose, lastFilePath;

  /** And which track it landed on, which unlike the host CHANGES: a user
   *  renames a track, or recolours it, without reloading anything. */
  void setTrackInfo(const sonore::TrackInfo& info) { track = info; }
  sonore::TrackInfo track;

  /** The probe's only way of telling a test what it knows. Everything a
   *  wrapper hands it goes into the bag, so the saved bytes ARE the evidence
   *  -- a plugin's state is the one channel a host can inspect without
   *  listening to the audio. */
  void saveState(sonore::StateBag& bag) const {
    bag.setString("host", host.name);
    bag.setString("track", track.hasName ? track.name : std::string());
    bag.setString("trackColour", track.colourHex());
    bag.setString("file", lastFilePath);
  }
  void loadState(const sonore::StateBag&) {}

  /** The fixture's third job. A DSP that delays its signal: a look-ahead
   *  limiter, a convolver: must report it or every parallel mix it sits in is
   *  silently smeared. Declaring it here proves the wrapper asks. */
  int latencySamples() const { return extraLatency > 0.5f ? 512 : 64; }

  /**
   * And it DELAYS BY THAT MUCH, which it did not use to.
   *
   * A fixture that declares a latency it does not have is a fixture nobody
   * can check latency compensation against. The LV2 host test measures the
   * delay through the plugin and compares it to the published number; this
   * probe published 64 and delayed by 0, which is the mismatch that test
   * exists to catch.
   *
   * So the audio is really late now, and this is the one plugin in the suite
   * where a host's delay compensation can be verified rather than assumed.
   *
   * 1024 rather than 512: read() clamps to MaxSamples-2, so a line exactly as
   * long as the longest delay would quietly hand back 510 samples when asked
   * for 512 -- a two-sample lie in the one place that must not have one.
   */
  sonore::DelayLine<1024> delayL;
  /** Read from the parameter every block, so the reported latency CHANGES.
   *
   *  That is the harder half of the feature. Latency that is constant for the
   *  life of a plugin is reported once and never thought about again; a
   *  plugin whose oversampling or look-ahead is a switch has to tell the host
   *  every time it moves, or the host's delay compensation stays on the old
   *  number and everything running in parallel with it is quietly early. */
  float extraLatency = 0.0f;

  /** The fixture's fifth job: state the host has no way to see.
   *
   *  A sampler that loads a file through its own browser changes what it
   *  would save, and the host has no idea -- no parameter moved, no preset
   *  was chosen, nothing the host did caused it. If nobody says so the
   *  session is never marked dirty, the DAW closes without asking, and the
   *  work is gone.
   *
   *  The switch stands in for that here: toggling it is this fixture's
   *  version of "the user just loaded a sample". Returns true ONCE and clears
   *  itself, so a host is told each time and not on every block. */
  bool consumeStateDirty() {
    if (!dirty) return false;
    dirty = false;
    return true;
  }
  bool dirty = false;
  float lastSwitch = -1.0f;
  /** Moves with the same switch, so one fixture exercises three
   *  notifications that a host is told about by three different rules:
   *  latency needs a restart, dirty state needs a main-thread callback, and
   *  the tail is announced from the audio thread itself. */
  int tailSamples() const { return extraLatency > 0.5f ? 9999 : 4321; }

  void process(sonore::AudioBlock<float>& io, const float* p) {
    float* L = io.getChannelPointer(0);
    float* R = io.getNumChannels() > 1 ? io.getChannelPointer(1) : L;
    for (size_t i = 0; i < io.getNumSamples(); ++i) {
      if (p[2] != lastSwitch) {
        lastSwitch = p[2];
        dirty = true;
      }
      extraLatency = p[2];
      const float g = gain.next(p[0]);
      // Written into the line and read back `latencySamples()` behind, so the
      // number the host is told is the number the audio is actually late by.
      // READ before WRITE. The other way round advances the write cursor
      // first, so asking for 64 returns the sample from 63 writes ago -- a
      // one-sample lie, tolerated by the test's margin and still a lie.
      const float delayed = delayL.read((float) latencySamples());
      delayL.write(L[i] * g);
      L[i] = delayed;
      // Right channel carries the observed tempo and beat as DC, scaled so a
      // test can read them straight out of the buffer.
      // 0.125 is a value none of the other terms can produce or cancel: the
      // tempo term is a multiple of 0.001, the beat term of 0.01, and the
      // playing flag is 0.5. A test can therefore read the offline flag out of
      // the sum by subtraction alone.
      // NOT delayed, deliberately. The left channel is the audio path and is
      // honestly late; this one is the fixture's instrument panel, and its
      // whole documented purpose is that a test can read the transport
      // straight out of the buffer. Delaying a measurement channel measures
      // the delay instead of the thing being measured -- which is exactly
      // what happened when it was: two transport tests started reading the
      // PREVIOUS block's tempo.
      R[i] = (float) (transport.tempo * 0.001) +
             (float) (transport.positionBeats * 0.01) +
             (transport.isPlaying ? 0.5f : 0.0f) + (offline ? 0.125f : 0.0f);
    }
  }
};

#include <sonore/plugin.h>

static const char* const kLatencyNames[] = {"Short", "Long"};

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"gain", "Gain", "x", 0.0f, 2.0f, 1.0f, 0},
    // HIDDEN, and honestly so: this exists for the page to drive on load so a
    // test can prove the bridge works end to end. It means nothing to anybody
    // reading a host's generic panel, which is the whole category the flag is
    // for -- a value that must be in the parameter list because the host has
    // to save it, and has no business being shown.
    //
    // Hidden is not private. It is still saved, still automatable, still
    // readable and writable by a host that asks. Only the generic UI is told
    // to leave it out.
    {"probe", "Probe", "", 0.0f, 1.0f, 0.0f, 0, nullptr, nullptr, 0, true, true},
    // A switch, so a host test can move the plugin's latency and watch
    // whether it is told about it. Its steps are NAMED, which is the
    // difference between an automation lane reading "Long" and one reading
    // "1" -- and automation is where a user works on a control whose face is
    // closed.
    // NOT automatable, and honestly so rather than as a test fixture: moving
    // this switch changes the plugin's latency, and a host asked to record
    // that as automation would be re-planning its delay compensation on every
    // automation point. Which is the whole category the flag is for -- a
    // control whose change is a reconfiguration rather than a movement.
    {"biglatency", "Big Latency", "", 0.0f, 1.0f, 0.0f, 2, nullptr, kLatencyNames, 2, false},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.test.guiprobe",
    "Sonore GUI Probe",
    "Sonorie",
    "1.0.0",
    "Test fixture: its UI exercises the bridge on load.",
    "https://sonorie.com",
    false,
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr, 0,           // no factory presets
    nullptr,              // no category
    nullptr,              // licence falls back to the vendor URL
    nullptr,              // no maintainer email
    2, 2,                 // fixed stereo
    nullptr, 0,           // no aux buses
    false,                // emits no MIDI
    false,                // not expressive
    sonore::EditorKind::Auto,
    // The fixture's fifth job: prove a DECLARED editor size reaches the host.
    //
    // Every wrapper used to answer 320x200 to 8192x8192 for every plugin, so
    // "the host was told a minimum" and "the host was told the SDK's default"
    // looked identical from outside. These are deliberately odd numbers no
    // default could produce -- the same trick the parameter values in this
    // file use, and for the same reason: a test that passes against a default
    // is a test that would pass against a wrapper ignoring the descriptor.
    {517, 341, 1200, 800},
};

// The interface. On `sonore:ready` it reports what the bridge handed it, then
// drives a real parameter, so the assertion downstream is about the whole
// chain, not about this markup.
#define SONORE_UI_HTML                                                                    \
  R"HTML(<!doctype html><html><head><meta charset="utf-8"></head>
<body style="margin:0;background:#0d1014;color:#cfe;font:13px system-ui">
<div id="log">waiting for the bridge...</div>
<script>
window.addEventListener('sonore:ready', function () {
  var ok = !!(window.sonore && sonore.params && sonore.params.length === 3 &&
              typeof sonore.set === 'function' && typeof sonore.get === 'function' &&
              typeof sonore.onLevel === 'function' && typeof sonore.onVU === 'function' &&
              typeof sonore.noteOn === 'function' &&
              typeof sonore.contextMenu === 'function' &&
              typeof sonore.isNoteDown === 'function' &&
              typeof sonore.onState === 'function' &&
              sonore.name === 'Sonore GUI Probe');
  document.getElementById('log').textContent =
    'bridge ' + (ok ? 'ok' : 'BROKEN') + ' - ' + sonore.params.length + ' params';
  if (!ok) return;
  // The keyboard the page is handed comes as four 32-bit words, and both
  // hazards live in this one line: bit 31 of a word is NEGATIVE under JS's
  // signed bitwise operators, and a 64-bit mask would have been rounded by
  // the double it travelled in. Checking the top bit of a word and a key past
  // 64 catches both. A failure here stops the parameter below from moving,
  // which is what the host test is watching.
  sonore.__notes(0, 1 << 31, 0, 1 << 4);
  if (!(sonore.isNoteDown(63) && sonore.isNoteDown(100) &&
        !sonore.isNoteDown(62) && !sonore.isNoteDown(127))) {
    document.getElementById('log').textContent = 'bridge BROKEN - note mask';
    return;
  }
  // A gesture around the edit, exactly as a real knob would: the host must see
  // begin -> value -> end so it can coalesce the drag into one undo step.
  // A gesture around the edit, exactly as a real knob would: the host must see
  // begin -> value -> end so it can coalesce the drag into one undo step.
  // BY NAME, not by index, and that is the point of writing it this way: the
  // studio's bridge resolves a parameter by id or label, so a generated
  // interface asks for its values that way, and this bridge understood only
  // an array subscript. Every such plugin shipped with its controls reading 0
  // -- correct in the preview, wrong in the DAW. Driving the probe by its id
  // means the index-only bridge cannot come back without this test failing.
  sonore.begin('probe');
  sonore.set('probe', 0.777);
  sonore.end('probe');

  // The plugin's own state, which parameters do not carry. The probe writes
  // the host's name into its bag, so seeing it HERE proves the whole channel:
  // DSP bag -> wrapper -> editor tick -> page.
  //
  // Reported on a SEPARATE parameter rather than by gating the one above,
  // because not every format can do this. An LV2 UI is a different module
  // from the plugin and talks to it only through ports -- it never sees the
  // bag, by design. Gating 0.777 on the state made the LV2 bridge test fail
  // for a reason that was not a bug, which is how this ended up written down.
  sonore.onState(function (state) {
    if (!state || typeof state.host !== 'string' || state.host.length === 0) {
      document.getElementById('log').textContent = 'bridge BROKEN - no state';
      return;
    }
    sonore.begin(0);
    sonore.set(0, 1.25);
    sonore.end(0);
  });
  // A right-click on parameter 2, at a position the host can recognise. The
  // host's own menu is what carries MIDI learn and "remove automation", so a
  // page that handles the click itself takes those away from the user.
  sonore.contextMenu(2, 12, 34);
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

// …and an LV2, which is the format the probe was missing. It is here for the
// same reason the probe exists at all: it REPORTS what the host told it, and
// lv2:freeWheeling cannot be shown to reach a DSP by any other means.
#if defined(SONORE_BUILD_LV2)
#include <sonore/lv2_wrapper.h>
#endif
