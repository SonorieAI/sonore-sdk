// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the LV2 user interface.
//
// The same WebView page that CLAP, VST3 and AU show, presented the way LV2
// asks for it. Without this an LV2 host draws a column of generic sliders and
// the plugin the seller designed simply does not appear.
//
// LV2 keeps the interface and the DSP FURTHER APART than any other format
// here. They are separate objects with separate lifetimes, they may be in
// separate binaries, and a host is entitled to run them in separate
// PROCESSES. So this shares no state with the plugin instance: everything the
// interface knows arrives through port_event(), and everything it does leaves
// through the host's write_function. That is more restrictive than the CLAP
// path, which reaches into the instance directly, and it is the reason this
// is a separate file rather than a branch inside the wrapper.
//
// WHAT IS HERE, and what is not:
//
//   Windows (ui:WindowsUI): complete, and tested by embedding it in a real
//       window and driving a real message loop, exactly as the CLAP and VST3
//       GUI tests do.
//   X11 (ui:X11UI): NOT built, and the reason is worth stating rather than
//       leaving as an absence. The GTK backend this SDK carries embeds a
//       GtkPlug INTO a parent window and never exposes an id of its own;
//       LV2's X11 UI works the other way round, handing the host a window it
//       will reparent. Bridging the two is a small change and an untestable
//       one: WSL has no display.
//   Cocoa (ui:CocoaUI): macOS, same argument.
//
// SONORE_LV2_UI is the one switch. Where it is undefined the plugin builds
// exactly as it did before this file existed and its manifest promises no
// interface, which is better than promising one that will not open.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>

// clap_wrapper.h is where the shared GUI machinery lives -- the per-platform
// WebView alias, the page source and the bridge script. That is the same
// arrangement the VST3 and AU wrappers use: the CLAP wrapper owns what is
// common and the others adapt it, so the formats cannot drift apart.
#include "clap_wrapper.h"
#include "lv2_ports.h" // the numbering this module must agree with
#include "gui.h"
#include "plugin.h"

#if defined(SONORE_HAS_WEBVIEW_BACKEND) && defined(_WIN32)
#define SONORE_LV2_UI 1
#endif

#if defined(SONORE_LV2_UI)

namespace sonore {
namespace lv2ui {

using clapwrap::PlatformWebView;
using clapwrap::uiHtml;

// ── The LV2 UI ABI ───────────────────────────────────────────────────────────
//
// Declared here rather than vendored. The extension's own header is not part
// of the core LV2 package this SDK carries, and these five types are the
// entire surface a host uses: transcribing a whole upstream file to reach
// them would add more to go wrong than it removes.

#define SONORE_LV2_UI_URI "http://lv2plug.in/ns/extensions/ui"
#define SONORE_LV2_UI__parent SONORE_LV2_UI_URI "#parent"
#define SONORE_LV2_UI__idleInterface SONORE_LV2_UI_URI "#idleInterface"
#define SONORE_LV2_UI__resize SONORE_LV2_UI_URI "#resize"

using Lv2UiWidget = void*;
using Lv2UiHandle = void*;
using Lv2UiController = void*;

/** How the interface tells the host a control moved. `format` 0 means the
 *  buffer is a single float and the port is an ordinary control port. */
using Lv2UiWriteFunction = void (*)(Lv2UiController controller, uint32_t portIndex,
                                    uint32_t bufferSize, uint32_t format, const void* buffer);

struct Lv2Feature {
  const char* URI;
  void* data;
};

struct Lv2UiDescriptor {
  const char* URI;
  Lv2UiHandle (*instantiate)(const struct Lv2UiDescriptor* descriptor, const char* pluginUri,
                             const char* bundlePath, Lv2UiWriteFunction writeFunction,
                             Lv2UiController controller, Lv2UiWidget* widget,
                             const Lv2Feature* const* features);
  void (*cleanup)(Lv2UiHandle ui);
  void (*port_event)(Lv2UiHandle ui, uint32_t portIndex, uint32_t bufferSize, uint32_t format,
                     const void* buffer);
  const void* (*extension_data)(const char* uri);
};

/** The host calls idle() on the main thread, which is where a WebView's
 *  message loop has to be pumped and where anything the page said gets turned
 *  into a port write. Returning non-zero asks the host to close the UI. */
struct Lv2UiIdleInterface {
  int (*idle)(Lv2UiHandle ui);
};

struct Lv2UiResize {
  void* handle;
  int (*ui_resize)(void* handle, int width, int height);
};

// ── One open interface ───────────────────────────────────────────────────────

struct UiInstance {
  PlatformWebView webview;
  Lv2UiWriteFunction write = nullptr;
  Lv2UiController controller = nullptr;
  /** What the page said, waiting to be handed to the host on the next idle().
   *  The WebView delivers its messages on its own callback and the host owns
   *  the thread that may call write_function, so the two are kept apart by
   *  the same lock-free queue the other wrappers use. */
  UiEventQueue queue;
  /** The last value this interface pushed at each control, so a port_event
   *  that merely repeats what the page already shows does not fight the user's
   *  mouse. A host sends a port_event for every change INCLUDING the ones the
   *  page just made. */
  float echoed[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1]{};
  bool echoValid = false;
  /** The level, and the same ballistics every other format's editor runs.
   *  Owned HERE rather than in the plugin: this module has the clock a needle
   *  should be driven by, and the plugin has only the audio thread. */
  MeterState meter;
  float meterPeak = 0.0f, meterRms = 0.0f;

  /** The URIDs this interface needs to say anything structured. Zero means
   *  the host gave no urid:map, which is legal and leaves the atom half of
   *  this interface switched off rather than crashing. */
  LV2_URID uridAtomSequence = 0;
  LV2_URID uridEventTransfer = 0;
  LV2_URID uridStateRequest = 0;
  LV2_URID uridStateJson = 0;
  LV2_URID uridLoadFile = 0;
  LV2_URID uridNotes = 0;
  /** Still to ask. An interface can open long after the plugin did, so it
   *  cannot wait to be told -- it asks, and keeps asking until an answer
   *  arrives, because the first request can land in a run() the host has
   *  already prepared the buffers for. */
  bool needState = true;
  int requestsSent = 0;
};

/**
 * The page asked for a file.
 *
 * The dialog opens HERE, in the interface, because the interface is the thing
 * with a window and a main thread -- the plugin has neither. The path then
 * goes to the plugin as an atom, and the plugin hands it to the host's worker
 * thread, which is the only thread in LV2 that is allowed to read it.
 *
 * [main-thread], and modal: this does not return until the user is done.
 */
inline void chooseFileForPage(UiInstance* ui, const BridgeMessage& m) {
  void* parent = ui->webview.nativeWindow();
  const std::string path = FileDialog::byMode(m.mode, parent);

  // The plugin first, then the page -- the same order the other formats use,
  // and for the same reason: a filename on screen for a sample that has not
  // been accepted yet is a lie the user acts on.
  if (ui->write && ui->uridLoadFile && !path.empty()) {
    // "purpose\0path", two C strings in one blob. A struct for two strings
    // would mean agreeing on padding across a module boundary that does not
    // need it.
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), m.purpose.begin(), m.purpose.end());
    payload.push_back(0);
    payload.insert(payload.end(), path.begin(), path.end());
    payload.push_back(0);

    std::vector<uint8_t> buffer(sizeof(LV2_Atom_Sequence) + sizeof(LV2_Atom_Event) +
                                lv2::atomPad((uint32_t) payload.size()));
    auto* seq = (LV2_Atom_Sequence*) buffer.data();
    seq->atom.type = ui->uridAtomSequence;
    seq->body.unit = 0;
    seq->body.pad = 0;
    auto* ev = (LV2_Atom_Event*) ((uint8_t*) &seq->body + sizeof(LV2_Atom_Sequence_Body));
    ev->time.frames = 0;
    ev->body.size = (uint32_t) payload.size();
    ev->body.type = ui->uridLoadFile;
    std::memcpy((uint8_t*) ev + sizeof(LV2_Atom_Event), payload.data(), payload.size());
    seq->atom.size = (uint32_t) (sizeof(LV2_Atom_Sequence_Body) + sizeof(LV2_Atom_Event) +
                                 lv2::atomPad((uint32_t) payload.size()));
    ui->write(ui->controller, (uint32_t) lv2::portUiControl(),
              (uint32_t) (sizeof(LV2_Atom) + seq->atom.size), ui->uridEventTransfer, seq);
    // The plugin's state has moved, so ask for it again -- the answer is what
    // puts the filename in front of the user.
    ui->needState = true;
    ui->requestsSent = 0;
  }

  ui->webview.eval(fileAnswerScript(m.purpose, path));
}

inline void onBridgeMessage(UiInstance* ui, const BridgeMessage& m) {
  UiEventQueue::Event e{};
  switch (m.kind) {
    case BridgeMessage::Kind::ChooseFile:
      // On this thread and blocking, which is what a modal dialog is. The
      // audio thread is the host's and keeps running throughout.
      chooseFileForPage(ui, m);
      return;
    case BridgeMessage::Kind::Set:
      e.kind = UiEventQueue::Event::Kind::ParamSet;
      e.index = m.index;
      e.value = (float) m.value;
      break;
    case BridgeMessage::Kind::GestureBegin:
      e.kind = UiEventQueue::Event::Kind::GestureBegin;
      e.index = m.index;
      break;
    case BridgeMessage::Kind::GestureEnd:
      e.kind = UiEventQueue::Event::Kind::GestureEnd;
      e.index = m.index;
      break;
    default:
      // Notes and preset loads have no port to write to: LV2 carries notes on
      // an atom port the interface has no handle for, and a preset is
      // something the HOST applies. Dropped rather than half-delivered.
      return;
  }
  ui->queue.push(e);
}

inline Lv2UiHandle instantiate(const Lv2UiDescriptor*, const char* /*pluginUri*/,
                               const char* /*bundlePath*/, Lv2UiWriteFunction writeFunction,
                               Lv2UiController controller, Lv2UiWidget* widget,
                               const Lv2Feature* const* features) {
  if (!widget || !writeFunction) return nullptr;
  *widget = nullptr;

  void* parent = nullptr;
  for (const Lv2Feature* const* f = features; f && *f; ++f) {
    if (std::strcmp((*f)->URI, SONORE_LV2_UI__parent) == 0) parent = (*f)->data;
  }
  // ui:parent is a REQUIRED feature in the TTL, so a host arriving without it
  // is a host that ignored what the bundle asked for. Refusing is better than
  // creating a window nobody owns and that nothing will ever close.
  if (!parent) return nullptr;

  LV2_URID_Map* map = nullptr;
  for (const Lv2Feature* const* f = features; f && *f; ++f) {
    if (std::strcmp((*f)->URI, LV2_URID__map) == 0) map = (LV2_URID_Map*) (*f)->data;
  }

  auto* ui = new UiInstance();
  ui->write = writeFunction;
  ui->controller = controller;
  if (map && map->map) {
    ui->uridAtomSequence = map->map(map->handle, LV2_ATOM__Sequence);
    ui->uridEventTransfer = map->map(map->handle, LV2_ATOM__eventTransfer);
    ui->uridStateRequest = map->map(map->handle, "urn:sonorie:ui:stateRequest");
    ui->uridStateJson = map->map(map->handle, "urn:sonorie:ui:stateJson");
    ui->uridLoadFile = map->map(map->handle, "urn:sonorie:ui:loadFile");
    ui->uridNotes = map->map(map->handle, "urn:sonorie:ui:notes");
  } else {
    // No map, no atoms. The knobs and the meters still work, because those
    // travel on control ports that need no vocabulary.
    ui->needState = false;
  }
  ui->webview.onMessage = [ui](const BridgeMessage& m) { onBridgeMessage(ui, m); };

  const uint32_t width = 720, height = 480;
  const bool created =
      ui->webview.create((HWND) parent, width, height, uiHtml(), bridgeScript(kDesc), kDesc.id);
  if (!created) {
    delete ui;
    return nullptr;
  }

  // Ask the host for the size the bundle wants, if it offered to listen. A
  // host that did not is left to size the widget itself, which is legal.
  for (const Lv2Feature* const* f = features; f && *f; ++f) {
    if (std::strcmp((*f)->URI, SONORE_LV2_UI__resize) == 0 && (*f)->data) {
      auto* resize = (Lv2UiResize*) (*f)->data;
      if (resize->ui_resize) resize->ui_resize(resize->handle, (int) width, (int) height);
    }
  }

  *widget = (Lv2UiWidget) ui->webview.hwnd();
  return (Lv2UiHandle) ui;
}

inline void cleanup(Lv2UiHandle handle) {
  auto* ui = (UiInstance*) handle;
  if (!ui) return;
  ui->webview.destroy();
  delete ui;
}

/** The host telling the interface a control moved. Only float control ports;
 *  anything else is a protocol the page has no way to render. */
inline void portEvent(Lv2UiHandle handle, uint32_t portIndex, uint32_t bufferSize,
                      uint32_t format, const void* buffer) {
  auto* ui = (UiInstance*) handle;
  if (!ui || !buffer) return;

  // The meter ports, which are the only way a level reaches an LV2 interface:
  // this module is a separate object from the plugin and shares no memory
  // with it. Raw peak and RMS arrive here and the BALLISTICS are done on this
  // side, by the same MeterState every other format uses -- so an LV2 meter
  // falls at the same rate as a VST3 one instead of at a rate invented here.
  if (portIndex == (uint32_t) lv2::portUiNotify()) {
    // The plugin's answer. Anything that is not our own message type is
    // somebody else's business and is left alone rather than guessed at.
    if (format != ui->uridEventTransfer || !ui->uridStateJson) return;
    const auto* seq = (const LV2_Atom_Sequence*) buffer;
    if (seq->atom.type != ui->uridAtomSequence) return;
    lv2::forEachAtomEvent(seq, [ui](const LV2_Atom_Event* ev) {
      if (ev->body.type == ui->uridNotes && ev->body.size == 4 * sizeof(uint32_t)) {
        // Four 32-bit words, straight through to the page. Not one 128-bit
        // number and not two 64-bit ones: a JavaScript number is a double and
        // holds integers exactly only to 2^53, so anything wider arrives
        // rounded and keys light at random.
        uint32_t words[4];
        std::memcpy(words, (const uint8_t*) ev + sizeof(LV2_Atom_Event), sizeof(words));
        char call[128];
        std::snprintf(call, sizeof(call), "window.sonore.__notes(%u,%u,%u,%u);", words[0],
                      words[1], words[2], words[3]);
        ui->webview.eval(call);
      } else if (ev->body.type == ui->uridStateJson) {
        const char* json = (const char*) ev + sizeof(LV2_Atom_Event);
        std::string call = "window.sonore.__state(";
        call.append(json, ev->body.size);
        call += ");";
        ui->webview.eval(call);
        ui->needState = false;
      }
    });
    return;
  }

  if (portIndex == (uint32_t) lv2::portMeterPeak() || portIndex == (uint32_t) lv2::portMeterRms()) {
    if (portIndex == (uint32_t) lv2::portMeterPeak()) ui->meterPeak = *(const float*) buffer;
    else ui->meterRms = *(const float*) buffer;
    // Pushed once both halves have arrived. A host delivers them as separate
    // events, and pushing on each would feed the meter one block's peak
    // against the previous block's RMS.
    if (portIndex == (uint32_t) lv2::portMeterRms()) ui->meter.push(ui->meterPeak, ui->meterRms);
    return;
  }

  // Everything below is a float control port. An atom arriving here would be
  // read as a number, which is the shape of bug that shows up as a knob
  // jumping to a value nobody set.
  if (format != 0 || bufferSize != sizeof(float)) return;
  if (portIndex >= (uint32_t) kDesc.numParams) return; // not one of ours

  const float value = *(const float*) buffer;
  // A host echoes back every change including the one the page just made.
  // Re-sending it would move the control under the mouse that is dragging it.
  if (ui->echoValid && ui->echoed[portIndex] == value) return;
  ui->echoed[portIndex] = value;

  char number[40], call[96];
  jsNumber(number, sizeof(number), (double) value);
  std::snprintf(call, sizeof(call), "window.sonore.__update(%u,%s);", (unsigned) portIndex,
                number);
  ui->webview.eval(call);
}

inline int idle(Lv2UiHandle handle) {
  auto* ui = (UiInstance*) handle;
  if (!ui) return 0;

  // Ask what the plugin has, until it says. An interface opened after the
  // plugin has missed every change, so being told is not enough on its own --
  // and the first request can land in a run() whose buffers the host has
  // already prepared, so one is not enough either. Capped, because a host
  // that never forwards the reply must not have this repeat for ever.
  if (ui->needState && ui->write && ui->uridStateRequest && ui->requestsSent < 40) {
    ++ui->requestsSent;
    struct {
      LV2_Atom_Sequence seq;
      LV2_Atom_Event ev;
    } message{};
    message.seq.atom.type = ui->uridAtomSequence;
    message.seq.atom.size = (uint32_t) (sizeof(LV2_Atom_Sequence_Body) + sizeof(LV2_Atom_Event));
    message.ev.time.frames = 0;
    message.ev.body.size = 0;
    message.ev.body.type = ui->uridStateRequest;
    ui->write(ui->controller, (uint32_t) lv2::portUiControl(), (uint32_t) sizeof(message),
              ui->uridEventTransfer, &message);
  }

  // The needle, on the interface's own clock. A host calls idle() at roughly
  // the rate it repaints, which is what the ballistics are written against.
  if (ui->webview.ready()) {
    ui->meter.tick(0.033);
    char lv[40], db[40], vu[40], call[160];
    jsNumber(lv, sizeof(lv), (double) ui->meter.level());
    jsNumber(db, sizeof(db), (double) ui->meter.db());
    jsNumber(vu, sizeof(vu), (double) ui->meter.vu());
    std::snprintf(call, sizeof(call), "window.sonore.__meter(%s,%s,%s);", lv, db, vu);
    ui->webview.eval(call);
  }
  // Nothing is pumped here. The WebView's messages are dispatched by the
  // host's own message loop -- the same loop that calls idle() -- and a
  // second loop inside a callback is how a plugin deadlocks a DAW.
  UiEventQueue::Event e{};
  while (ui->queue.pop(&e)) {
    if (e.kind != UiEventQueue::Event::Kind::ParamSet) {
      // Gestures have no LV2 spelling on an ordinary control port: touch is a
      // separate extension a host may not implement. Dropped rather than sent
      // as a value the host would apply.
      continue;
    }
    if (e.index < 0 || e.index >= kDesc.numParams) continue;
    const float value = clampToRange(kDesc.params[e.index], (float) e.value);
    // Remembered BEFORE the write, because the host will echo it straight
    // back and portEvent() has to recognise it as our own.
    ui->echoed[e.index] = value;
    ui->echoValid = true;
    // The write function is mandatory in the LV2 UI contract; the check costs
    // nothing and turns a host that broke the contract into a UI that does
    // not move instead of one that crashes.
    if (ui->write) ui->write(ui->controller, (uint32_t) e.index, sizeof(float), 0, &value);
  }
  return 0;
}

inline const void* extensionData(const char* uri) {
  if (uri && std::strcmp(uri, SONORE_LV2_UI__idleInterface) == 0) {
    static const Lv2UiIdleInterface iface = {idle};
    return &iface;
  }
  return nullptr;
}

inline const char* uiUri() {
  // Built in the initialiser, not filled in afterwards. A static that is
  // constructed empty and then assigned on first use is a data race between
  // two threads arriving together; a static built by a lambda is initialised
  // exactly once and every other caller waits, which C++ has guaranteed since
  // it started guaranteeing anything about these.
  static const std::string uri = [] {
    return std::string("urn:sonorie:") + kDesc.id + "#ui";
  }();
  return uri.c_str();
}

inline const Lv2UiDescriptor* descriptor(uint32_t index) {
  if (index != 0) return nullptr;
  static Lv2UiDescriptor d = {};
  d.URI = uiUri();
  d.instantiate = instantiate;
  d.cleanup = cleanup;
  d.port_event = portEvent;
  d.extension_data = extensionData;
  return &d;
}

} // namespace lv2ui
} // namespace sonore

#endif // SONORE_LV2_UI
