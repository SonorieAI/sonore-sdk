// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: CLAP note-off and choke events reach the notes they address.
//
// A CLAP note event addresses notes by note_id, key and channel, any of which
// may be -1: a wildcard, or "unspecified" for the id. The wrapper turned
// channel -1 into channel 0 -- so on an MPE instrument, whose notes sit on
// channels 1-15, a wildcard-channel note-off released nothing and the note
// hung -- and treated key -1 as "every note" even when a note_id named ONE
// voice: a choke meant for a single note silenced the whole chord.
//
// So does a note expression that names its note by note_id alone: passed on
// with key and channel -1, it reached no voice.
//
// The probe instrument records every MIDI message it is given, and where in
// the host's block it was given it.
//
// Event times. The host's were passed on as they came: a note-off timed at the
// end of the block or past it sat where a DSP that stops at its last frame
// never reached it, and the note hung.
//
// And notes that arrive with no block to play in: a zero-length process()
// call, and the editor's keyboard drained by params.flush(). Both were kept
// only until the next process() call cleared them unplayed.
//
// And a block longer than activate() allowed, which went to the DSP whole:
// past the end of everything the DSP had sized to the promised maximum.
#define SONORE_NUM_PARAMS 1

#include <cstdio>
#include <vector>

#include <sonore/dsp.h>
#include <sonore/transport.h>

struct Received {
  int status, data1;
  int at; // in the host's block
  int data2;
};
static std::vector<Received> g_received;
static std::vector<size_t> g_blockSizes; // as the DSP was given them
static size_t g_offset = 0;              // where in the host's block this one starts
static float g_counter = 0.0f;
struct ReceivedExpression {
  int key, channel;
  float value;
};
static std::vector<ReceivedExpression> g_expressions;

static sonore::TransportInfo g_transport;

static int g_prepares = 0, g_resets = 0;

struct SonoreDsp {
  void prepare(const sonore::ProcessSpec&) { ++g_prepares; }
  void reset() { ++g_resets; }
  void setTransport(const sonore::TransportInfo& t) { g_transport = t; }
  void process(sonore::ProcessContext& ctx, const float*) {
    sonore::AudioBlock<float>& io = ctx.main;
    const sonore::MidiBuffer& midi = ctx.midi;
    if (ctx.expression)
      for (const auto& x : *ctx.expression) g_expressions.push_back({x.key, x.channel, x.value});
    for (const auto& e : midi)
      g_received.push_back({e.getMessage().getRawStatus(), e.getMessage().getRawData1(),
                            (int) g_offset + e.samplePosition, e.getMessage().getRawData2()});
    io.clear();
    // A count that runs on from one call to the next, so the host's buffer
    // shows whether each piece of it was written where it belongs.
    float* out = io.getChannelPointer(0);
    for (size_t i = 0; i < io.getNumSamples(); ++i) out[i] = g_counter++;
    g_blockSizes.push_back(io.getNumSamples());
    g_offset += io.getNumSamples();
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {{"x", "X", "", 0.0f, 1.0f, 0.0f, 0}};
static sonore::PluginDescriptor makeDesc() {
  sonore::PluginDescriptor d;
  d.id = "com.sonorie.test.clapnotes";
  d.name = "Sonore CLAP Notes Probe";
  d.description = "Records every MIDI message.";
  d.url = "https://sonorie.com";
  d.isInstrument = true;
  d.params = kParamTable;
  d.numParams = SONORE_NUM_PARAMS;
  d.supportsMpe = true; // so note expressions reach the DSP
  return d;
}
static const sonore::PluginDescriptor kDesc = makeDesc();

#include <sonore/clap_wrapper.h>

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

namespace {
const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }
void hostRequest(const clap_host_t*) {}
const clap_host_t kHost = {CLAP_VERSION_INIT, nullptr, "com.sonorie.clap-notes-test", "Sonorie",
                           "",                "1.0",   hostGetExtension,            hostRequest,
                           hostRequest,       hostRequest};

std::vector<clap_event_note_t> g_events;
uint32_t inSize(const clap_input_events_t*) { return (uint32_t) g_events.size(); }
const clap_event_header_t* inGet(const clap_input_events_t*, uint32_t i) {
  return i < g_events.size() ? &g_events[i].header : nullptr;
}
const clap_input_events_t kIn = {nullptr, inSize, inGet};
bool outPush(const clap_output_events_t*, const clap_event_header_t*) { return true; }
const clap_output_events_t kOut = {nullptr, outPush};

clap_event_note_t note(uint16_t type, uint32_t time, int32_t id, int16_t channel, int16_t key) {
  clap_event_note_t e{};
  e.header.size = sizeof(e);
  e.header.time = time;
  e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  e.header.type = type;
  e.note_id = id;
  e.port_index = 0;
  e.channel = channel;
  e.key = key;
  e.velocity = 0.8;
  return e;
}

float g_left[1024], g_right[1024];

void runBlock(const clap_plugin_t* plugin, uint32_t frames = 64) {
  float* out[2] = {g_left, g_right};
  clap_audio_buffer_t outBuf{};
  outBuf.data32 = out;
  outBuf.channel_count = 2;
  clap_process_t p{};
  p.frames_count = frames;
  p.steady_time = -1;
  p.audio_outputs = &outBuf;
  p.audio_outputs_count = 1;
  p.in_events = &kIn;
  p.out_events = &kOut;
  g_received.clear();
  g_expressions.clear();
  g_blockSizes.clear();
  g_offset = 0;
  plugin->process(plugin, &p);
}

int receivedAt(int status, int data1) {
  for (const Received& r : g_received)
    if (r.status == status && r.data1 == data1) return r.at;
  return -1000;
}

bool has(int status, int data1) {
  for (const Received& r : g_received)
    if (r.status == status && r.data1 == data1) return true;
  return false;
}
} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── CLAP note-off and choke addressing ──\n");
  g_received.reserve(256);
  clap_entry.init("");
  const auto* factory =
      static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
  const clap_plugin_t* plugin =
      factory->create_plugin(factory, &kHost, factory->get_plugin_descriptor(factory, 0)->id);
  check(plugin && plugin->init(plugin) && plugin->activate(plugin, 48000.0, 1, 64) &&
            plugin->start_processing(plugin),
        "the probe instrument activates");
  if (!plugin) return 1;
  // No dry ring for an instrument: its bypass fades to silence and never
  // reads one, and one sized like an effect's was kept for nothing.
  check(sonore::clapwrap::self(plugin)->bypass.ring[0].capacity() == 0,
        "an instrument is given no bypass ring");

  // Two notes on two channels, as an MPE controller plays them.
  g_events = {note(CLAP_EVENT_NOTE_ON, 0, 7, 3, 60), note(CLAP_EVENT_NOTE_ON, 0, 8, 5, 64)};
  runBlock(plugin);

  // A note-off for key 60 on ANY channel.
  g_events = {note(CLAP_EVENT_NOTE_OFF, 0, -1, -1, 60)};
  runBlock(plugin);
  check(has(0x83, 60), "a wildcard-channel note-off releases the note on the channel it plays on");

  // A choke for ONE voice, named by its note_id, key and channel left open.
  g_events = {note(CLAP_EVENT_NOTE_CHOKE, 0, 8, -1, -1)};
  runBlock(plugin);
  check(has(0x85, 64), "a choke naming a note_id ends that note");
  bool silencedAll = false;
  for (const Received& r : g_received)
    if ((r.status & 0xF0) == 0xB0 && (r.data1 == 120 || r.data1 == 123)) silencedAll = true;
  check(!silencedAll, "...and only that note, not the whole chord");

  // Every note on every channel.
  g_events = {note(CLAP_EVENT_NOTE_OFF, 0, -1, -1, -1)};
  runBlock(plugin);
  check(has(0xB3, 123) && has(0xB5, 123), "a note-off with every field open reaches every channel");

  // A tuning expression for note 9 (key 67 on channel 2), named by its id
  // alone; and one for an id nobody played.
  {
    g_events = {note(CLAP_EVENT_NOTE_ON, 0, 9, 2, 67)};
    runBlock(plugin);
    g_events.clear();
    std::vector<clap_event_note_expression_t> expressions(2);
    for (size_t k = 0; k < expressions.size(); ++k) {
      clap_event_note_expression_t& x = expressions[k];
      x.header.size = sizeof(x);
      x.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      x.header.type = CLAP_EVENT_NOTE_EXPRESSION;
      x.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
      x.note_id = k == 0 ? 9 : 4242;
      x.port_index = -1;
      x.channel = -1;
      x.key = -1;
      x.value = 0.5;
    }
    struct ExpressionList {
      static uint32_t size(const clap_input_events_t* l) {
        return (uint32_t) static_cast<std::vector<clap_event_note_expression_t>*>(l->ctx)->size();
      }
      static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i) {
        return &(*static_cast<std::vector<clap_event_note_expression_t>*>(l->ctx))[i].header;
      }
    };
    const clap_input_events_t list = {&expressions, ExpressionList::size, ExpressionList::get};
    float* out[2] = {g_left, g_right};
    clap_audio_buffer_t outBuf{};
    outBuf.data32 = out;
    outBuf.channel_count = 2;
    clap_process_t p{};
    p.frames_count = 64;
    p.steady_time = -1;
    p.audio_outputs = &outBuf;
    p.audio_outputs_count = 1;
    p.in_events = &list;
    p.out_events = &kOut;
    g_expressions.clear();
    plugin->process(plugin, &p);
    char line[160];
    std::snprintf(line, sizeof(line),
                  "a note expression named by note_id alone reaches that note (%zu, key %d, channel %d)",
                  g_expressions.size(), g_expressions.empty() ? -9 : g_expressions[0].key,
                  g_expressions.empty() ? -9 : g_expressions[0].channel);
    check(g_expressions.size() == 1 && g_expressions[0].key == 67 && g_expressions[0].channel == 2,
          line);

    // Per-note pressure, which a host speaking CLAP's own dialect -- the one
    // this plugin prefers -- sends as a pressure expression: by note_id, and
    // by key and channel. MIDI has a message for it, poly aftertouch, and the
    // VST3 build hands its poly pressure over in both forms; here only a DSP
    // that declared MPE heard anything, and only as the expression, so a DSP
    // reading aftertouch from its MIDI heard none in this format alone.
    for (size_t k = 0; k < expressions.size(); ++k) {
      clap_event_note_expression_t& x = expressions[k];
      x.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
      x.header.time = k == 0 ? 5 : 9;
      x.note_id = k == 0 ? 9 : -1;
      x.channel = k == 0 ? -1 : 2;
      x.key = k == 0 ? -1 : 67;
      x.value = k == 0 ? 0.5 : 1.0;
    }
    g_received.clear();
    g_expressions.clear();
    g_offset = 0;
    plugin->process(plugin, &p);
    int at = -1000, value = -1, at2 = -1000, value2 = -1;
    for (const Received& r : g_received)
      if (r.status == 0xA2 && r.data1 == 67) {
        if (at == -1000) at = r.at, value = r.data2;
        else at2 = r.at, value2 = r.data2;
      }
    std::snprintf(line, sizeof(line),
                  "a pressure expression reaches the DSP as poly aftertouch too (%d@%d, %d@%d), and as "
                  "the expression (%zu)",
                  value, at, value2, at2, g_expressions.size());
    check(at == 5 && value == 64 && at2 == 9 && value2 == 127 && g_expressions.size() == 2, line);
    g_events = {note(CLAP_EVENT_NOTE_OFF, 0, 9, 2, 67)};
    runBlock(plugin);
  }

  // Note-offs timed at the end of a 64-frame block, and past anything an int
  // holds.
  g_events = {note(CLAP_EVENT_NOTE_ON, 0, -1, 0, 60), note(CLAP_EVENT_NOTE_OFF, 64, -1, 0, 60),
              note(CLAP_EVENT_NOTE_OFF, 0x80000000u, -1, 0, 61)};
  runBlock(plugin);
  {
    char line[160];
    std::snprintf(line, sizeof(line),
                  "note-offs timed at the block's end and at 2^31 land on its last frame (%d, %d)",
                  receivedAt(0x80, 60), receivedAt(0x80, 61));
    check(receivedAt(0x80, 60) == 63 && receivedAt(0x80, 61) == 63, line);
  }

  // A note-off in a zero-length call.
  g_events = {note(CLAP_EVENT_NOTE_ON, 0, -1, 0, 62)};
  runBlock(plugin);
  g_events = {note(CLAP_EVENT_NOTE_OFF, 0, -1, 0, 62)};
  runBlock(plugin, 0);
  g_events.clear();
  runBlock(plugin);
  check(has(0x80, 62), "a note-off sent in a zero-length process() call reaches the next block");

  // The editor's keyboard, drained by flush rather than by process().
  {
    const auto* params =
        static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    auto* inst = static_cast<sonore::clapwrap::Instance*>(plugin->plugin_data);
    sonore::UiEventQueue::Event on{};
    on.kind = sonore::UiEventQueue::Event::Kind::NoteOn;
    on.index = 63;
    on.value = 100.0f;
    inst->uiEvents.push(on);
    params->flush(plugin, &kIn, &kOut);
    runBlock(plugin);
    const bool played = has(0x90, 63);
    sonore::UiEventQueue::Event off{};
    off.kind = sonore::UiEventQueue::Event::Kind::NoteOff;
    off.index = 63;
    inst->uiEvents.push(off);
    params->flush(plugin, &kIn, &kOut);
    runBlock(plugin);
    check(played && has(0x80, 63), "the editor's keyboard drained by params.flush() plays in the next block");

    // A key held while a knob is dragged on a host that is not draining --
    // a track whose processing the host suspended: the knob's edits fill the
    // ring, and the release must still get in, or the note sounds for ever
    // once the host drains.
    on.index = 64;
    inst->uiEvents.push(on);
    sonore::UiEventQueue::Event edit{};
    edit.kind = sonore::UiEventQueue::Event::Kind::ParamSet;
    for (int i = 0; i < 400; ++i) {
      edit.value = (float) (i % 2);
      inst->uiEvents.push(edit);
    }
    off.index = 64;
    const bool released = inst->uiEvents.push(off);
    params->flush(plugin, &kIn, &kOut);
    runBlock(plugin);
    check(released && has(0x90, 64) && has(0x80, 64),
          "a note-off still gets in after knob edits filled the editor's queue");
    // The same for a gesture's end, which a host otherwise keeps open.
    sonore::UiEventQueue queue;
    sonore::UiEventQueue::Event begin{}, end{};
    begin.kind = sonore::UiEventQueue::Event::Kind::GestureBegin;
    end.kind = sonore::UiEventQueue::Event::Kind::GestureEnd;
    queue.push(begin);
    for (int i = 0; i < 400; ++i) queue.push(edit);
    check(queue.push(end), "and so does a gesture's end");
  }

  // Four output channels for a stereo instrument: the two it does not write
  // are silence, not what the host's buffer held.
  {
    float c2[64], c3[64];
    for (int i = 0; i < 64; ++i) c2[i] = c3[i] = -1.0f;
    float* out[4] = {g_left, g_right, c2, c3};
    clap_audio_buffer_t outBuf{};
    outBuf.data32 = out;
    outBuf.channel_count = 4;
    clap_process_t p{};
    p.frames_count = 64;
    p.steady_time = -1;
    p.audio_outputs = &outBuf;
    p.audio_outputs_count = 1;
    g_events.clear();
    p.in_events = &kIn;
    p.out_events = &kOut;
    plugin->process(plugin, &p);
    bool silent = true;
    for (int i = 0; i < 64; ++i) silent = silent && c2[i] == 0.0f && c3[i] == 0.0f;
    char line[120];
    std::snprintf(line, sizeof(line), "the two channels past a stereo DSP are silence (%.0f, %.0f)", c2[0], c3[0]);
    check(silent, line);
  }

  // A host's reset() is on the audio thread, so it calls the DSP's reset()
  // rather than prepare(), which is where a DSP allocates.
  {
    const int prepares = g_prepares, resets = g_resets;
    plugin->reset(plugin);
    char line[160];
    std::snprintf(line, sizeof(line), "reset() calls the DSP's reset(), not prepare() (%d reset, %d prepare)",
                  g_resets - resets, g_prepares - prepares);
    check(g_resets - resets == 1 && g_prepares == prepares, line);
  }

  // A CLAP host's loop reaches the DSP: it was read as "looping" with no ends.
  {
    clap_event_transport_t tr{};
    tr.header.size = sizeof(tr);
    tr.header.type = CLAP_EVENT_TRANSPORT;
    tr.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    tr.flags = CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_IS_LOOP_ACTIVE |
               CLAP_TRANSPORT_HAS_TEMPO;
    tr.tempo = 120.0;
    tr.song_pos_beats = 10 * CLAP_BEATTIME_FACTOR;
    tr.loop_start_beats = 8 * CLAP_BEATTIME_FACTOR;
    tr.loop_end_beats = 16 * CLAP_BEATTIME_FACTOR;
    float* out[2] = {g_left, g_right};
    clap_audio_buffer_t outBuf{};
    outBuf.data32 = out;
    outBuf.channel_count = 2;
    clap_process_t p{};
    p.frames_count = 64;
    p.steady_time = -1;
    p.audio_outputs = &outBuf;
    p.audio_outputs_count = 1;
    p.transport = &tr;
    g_events.clear();
    p.in_events = &kIn;
    p.out_events = &kOut;
    plugin->process(plugin, &p);
    char line[160];
    std::snprintf(line, sizeof(line), "a CLAP host's loop reaches the DSP (%.1f to %.1f)", g_transport.loopStartBeats,
                  g_transport.loopEndBeats);
    check(g_transport.isLooping && g_transport.hasLoop && g_transport.loopStartBeats == 8.0 &&
              g_transport.loopEndBeats == 16.0,
          line);
  }

  // 200 frames after activating for at most 64.
  g_events = {note(CLAP_EVENT_NOTE_ON, 10, -1, 0, 70), note(CLAP_EVENT_NOTE_OFF, 130, -1, 0, 70),
              note(CLAP_EVENT_NOTE_ON, 199, -1, 0, 71)};
  runBlock(plugin, 200);
  {
    size_t longest = 0, total = 0;
    for (size_t n : g_blockSizes) {
      longest = n > longest ? n : longest;
      total += n;
    }
    bool inPlace = true;
    for (int i = 1; i < 200; ++i) inPlace = inPlace && g_left[i] == g_left[i - 1] + 1.0f;
    char line[200];
    std::snprintf(line, sizeof(line),
                  "a 200-frame block reaches a DSP activated for 64 as %zu blocks, the longest %zu",
                  g_blockSizes.size(), longest);
    check(longest <= 64 && total == 200, line);
    check(inPlace, "...each written where it belongs in the host's buffer");
    std::snprintf(line, sizeof(line), "...and every event at its own time in it (%d, %d, %d)",
                  receivedAt(0x90, 70), receivedAt(0x80, 70), receivedAt(0x90, 71));
    check(receivedAt(0x90, 70) == 10 && receivedAt(0x80, 70) == 130 && receivedAt(0x90, 71) == 199,
          line);
  }

  plugin->stop_processing(plugin);
  plugin->deactivate(plugin);
  plugin->destroy(plugin);
  clap_entry.deinit();
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE CLAP NOTES PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
