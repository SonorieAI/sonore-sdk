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
//       one: WSL has no display, so it would ship having never been opened.
//       The rest of this SDK does not claim what it has not run, and neither
//       does this.
//   Cocoa (ui:CocoaUI): macOS, same argument, and macOS is unverified here
//       for the separate reason that CI cannot run at all.
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
// entire surface a host uses; transcribing a whole upstream file to reach
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

/** LV2_Feature, transcribed: one capability the host offers, named by URI,
 *  with data whose meaning that URI defines. A host hands instantiate() a
 *  null-terminated array of pointers to these, valid for that call; anything
 *  kept must be copied. The layout is the C ABI's and must not change. */
struct Lv2Feature {
  const char* URI;
  void* data;
};

/** LV2UI_Descriptor, transcribed: what lv2ui_descriptor() returns to the host
 *  -- the UI's URI and the four entry points. The layout is the C ABI's and
 *  must not change; this module's one instance is static and lives as long as
 *  the library. */
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

/** LV2UI_Resize, transcribed: the ui:resize feature. A host that offers it
 *  is told the interface's size through ui_resize(handle, width, height); a
 *  host that does not simply takes the window at the size it is made. */
struct Lv2UiResize {
  void* handle;
  int (*ui_resize)(void* handle, int width, int height);
};

#define SONORE_LV2_OPTIONS__options "http://lv2plug.in/ns/ext/options#options"
#define SONORE_LV2_UI__scaleFactor SONORE_LV2_UI_URI "#scaleFactor"

namespace detail {
/** One entry of the options feature's array (LV2_Options_Option), ended by
 *  an entry whose key is 0 and value null. The context is a C enum, which is
 *  an int's size on every compiler this builds with. */
struct Lv2Option {
  uint32_t context;
  uint32_t subject;
  LV2_URID key;
  uint32_t size;
  LV2_URID type;
  const void* value;
};

/** The host's ui:scaleFactor -- device pixels per logical pixel -- from the
 *  options it handed instantiate(), or 1 when it gave none. It is how LV2
 *  says what CLAP's set_scale and VST3's setContentScaleFactor say. */
inline double hostScaleFactor(const Lv2Feature* const* features, LV2_URID_Map* map) {
  if (!map || !map->map) return 1.0;
  const LV2_URID key = map->map(map->handle, SONORE_LV2_UI__scaleFactor);
  const LV2_URID atomFloat = map->map(map->handle, LV2_ATOM__Float);
  const LV2_URID atomDouble = map->map(map->handle, LV2_ATOM__Double);
  for (const Lv2Feature* const* f = features; f && *f; ++f) {
    if (std::strcmp((*f)->URI, SONORE_LV2_OPTIONS__options) != 0 || !(*f)->data) continue;
    // Bounded: a host that forgot the terminator must not walk us off the end.
    const auto* o = (const Lv2Option*) (*f)->data;
    for (int i = 0; i < 256 && (o[i].key != 0 || o[i].value != nullptr); ++i) {
      if (o[i].key != key || !o[i].value) continue;
      double v = 0.0;
      if (o[i].type == atomFloat && o[i].size == sizeof(float)) v = *(const float*) o[i].value;
      else if (o[i].type == atomDouble && o[i].size == sizeof(double)) v = *(const double*) o[i].value;
      if (v > 0.0 && v <= 8.0) return v; // the range guiSetScale accepts
    }
  }
  return 1.0;
}
} // namespace detail

// ── One open interface ───────────────────────────────────────────────────────

/**
 * One open LV2 interface, the handle instantiate() returns to the host: the
 * webview, the host's write_function, and what the page and the plugin have
 * said to each other that has not been delivered yet.
 *
 * Shares nothing with the plugin instance (see the file comment): parameters
 * arrive through port_event and leave through write_function, the state and
 * notes as atoms. Every call on it is the host's UI thread -- instantiate,
 * port_event, idle, cleanup -- and the host deletes it through cleanup(),
 * except while a file dialog is open (see dialogsOpen).
 */
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
  float meterPeak = 0.0f, meterRms = 0.0f, meterGr = 0.0f;

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

  /** The page's document is up: its bridge said "ready". Before that a
   *  script runs in the blank document the navigation then replaces, and is
   *  gone -- and a host sends every control's value right after
   *  instantiate(), and the plugin answers the first state request within a
   *  run() or two, both long before WebView2 has started. So what arrives
   *  early is held here and pushed on ready. */
  bool pageUp = false;
  float latest[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1]{};
  bool haveLatest[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1]{};
  std::string pendingState;
  std::string pendingNotes;
  /** The plugin's state as it last arrived, JSON. The plugin sends it again
   *  every second of audio (lv2_wrapper.h's writeUiNotify), so the page is
   *  handed a state only when it differs from this one or when the page is
   *  new -- a document after a reload gets it from here, with no request. */
  std::string lastState;
  /** The meter call the page last ran: a plugin at rest is not news, and
   *  each call wakes the renderer (see Instance::uiLastMeter). */
  std::string lastMeter;

  /** File dialogs open on the stack. The dialog is modal and runs a message
   *  loop of its own, and the host's messages are dispatched by it -- so the
   *  host can call cleanup() before the dialog returns. While one is open,
   *  cleanup() closes the page and leaves the instance to be deleted by the
   *  dialog's caller, which would otherwise come back to freed memory. */
  int dialogsOpen = 0;
  bool cleanedUp = false;
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
  ++ui->dialogsOpen;
  const std::string path = FileDialog::byMode(m.mode, parent);
  --ui->dialogsOpen;
  if (ui->cleanedUp) {
    // The host closed the interface while the dialog was up: there is no
    // page to answer and no host to write to. The last dialog out deletes
    // it; nothing between here and the webview's own handler touches it.
    if (ui->dialogsOpen == 0) delete ui;
    return;
  }

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

    // ONE atom, which is what atom:eventTransfer carries: the host makes it
    // an event in the plugin's input sequence. A whole Sequence written here
    // arrived as an event whose body was a Sequence, and the plugin, looking
    // for a load-file event, never saw one.
    std::vector<uint8_t> buffer(sizeof(LV2_Atom) + payload.size());
    auto* atom = (LV2_Atom*) buffer.data();
    atom->size = (uint32_t) payload.size();
    atom->type = ui->uridLoadFile;
    std::memcpy(buffer.data() + sizeof(LV2_Atom), payload.data(), payload.size());
    ui->write(ui->controller, (uint32_t) lv2::portUiControl(), (uint32_t) buffer.size(),
              ui->uridEventTransfer, atom);
    // The plugin's state has moved, so ask for it again -- the answer is what
    // puts the filename in front of the user.
    ui->needState = true;
    ui->requestsSent = 0;
  }

  ui->webview.eval(fileAnswerScript(m.purpose, path));
}

/** The page's bridge came up in a real document: everything it missed. On a
 *  reload too, which is why the state is asked for again once the held
 *  answer has been spent. */
inline void pageUp(UiInstance* ui) {
  ui->pageUp = true;
  ui->lastMeter.clear(); // a new document has seen none
  std::string js;
  for (int i = 0; i < kDesc.numParams; ++i) {
    if (!ui->haveLatest[i]) continue;
    char number[40], call[96];
    jsNumber(number, sizeof(number), (double) ui->latest[i]);
    std::snprintf(call, sizeof(call), "window.sonore.__update(%d,%s);", i, number);
    js += call;
    ui->echoed[i] = ui->latest[i];
  }
  js += ui->pendingNotes;
  if (!ui->pendingState.empty()) {
    js += ui->pendingState;
    ui->pendingState.clear();
  } else if (!ui->lastState.empty()) {
    // A new document (a reload) and a state already here: its own, now.
    js += "window.sonore.__state(" + ui->lastState + ");";
  } else if (ui->uridStateRequest) {
    ui->needState = true;
    ui->requestsSent = 0;
  }
  if (!js.empty()) ui->webview.eval(js);
}

inline void onBridgeMessage(UiInstance* ui, const BridgeMessage& m) {
  // Nothing an interface may not write: out of range, or a parameter the
  // plugin REPORTS (readOnly). See uiMayWrite.
  if ((m.kind == BridgeMessage::Kind::Set || m.kind == BridgeMessage::Kind::GestureBegin ||
       m.kind == BridgeMessage::Kind::GestureEnd) &&
      !uiMayWrite(kDesc, m.index))
    return;
  UiEventQueue::Event e{};
  switch (m.kind) {
    case BridgeMessage::Kind::Ready:
      pageUp(ui);
      return;
    case BridgeMessage::Kind::CaptureKeys:
      // Straight onto the webview, as CLAP and VST3 do. Dropped here, the
      // flag stayed false, so every key WebView2 reports as an accelerator
      // -- Backspace, Delete, the arrows, Ctrl+C and Ctrl+V -- went to the
      // host even while a text field in the page had focus and had asked
      // for the keyboard.
      ui->webview.captureKeys = m.value >= 0.5;
      return;
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

  // The size every other format opens at: the plugin's own (SONORE_UI_WIDTH
  // x SONORE_UI_HEIGHT, inside its editorLimits) in logical pixels, times
  // the host's scale -- the webview draws each CSS pixel as that many device
  // pixels, so the window must be that much bigger (guiSetScale in
  // clap_wrapper.h). It was a fixed 720x480 device pixels for every plugin:
  // in REAPER at 125% the showcase opened at 780x500 as CLAP and VST3 and at
  // 576x384 as LV2, and a plugin with a larger minimum opened below it.
  uint32_t width = SONORE_UI_WIDTH, height = SONORE_UI_HEIGHT;
  clapwrap::clampEditorSize(&width, &height);
  const double scale = detail::hostScaleFactor(features, map);
  width = (uint32_t) ((double) width * scale + 0.5);
  height = (uint32_t) ((double) height * scale + 0.5);
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
  if (ui->dialogsOpen > 0) {
    // Called from inside the file dialog's message loop: chooseFileForPage()
    // is still on the stack below us and deletes the instance when it
    // returns. The host's write function is not to be used after this.
    ui->write = nullptr;
    ui->cleanedUp = true;
    return;
  }
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
    // The host says how many bytes it handed over, and the atom's own size
    // has to fit inside them: everything below trusts that one number.
    if (bufferSize < sizeof(LV2_Atom)) return;
    const auto* atom = (const LV2_Atom*) buffer;
    if (atom->size > bufferSize - sizeof(LV2_Atom)) return;
    // One message: its type, its size and its bytes.
    auto deliver = [ui](uint32_t type, uint32_t size, const uint8_t* body) {
      if (type == ui->uridNotes && size == 4 * sizeof(uint32_t)) {
        // Four 32-bit words, straight through to the page. Not one 128-bit
        // number and not two 64-bit ones: a JavaScript number is a double and
        // holds integers exactly only to 2^53, so anything wider arrives
        // rounded and keys light at random.
        uint32_t words[4];
        std::memcpy(words, body, sizeof(words));
        char call[128];
        std::snprintf(call, sizeof(call), "window.sonore.__notes(%u,%u,%u,%u);", words[0],
                      words[1], words[2], words[3]);
        ui->pendingNotes = call; // the latest, for a page that is not up yet
        if (ui->pageUp) ui->webview.eval(call);
      } else if (type == ui->uridStateJson) {
        ui->needState = false;
        // The second-by-second repeat of a state the page has: nothing new.
        if (ui->lastState.size() == size && std::memcmp(ui->lastState.data(), body, size) == 0)
          return;
        ui->lastState.assign((const char*) body, size);
        std::string call = "window.sonore.__state(";
        call += ui->lastState;
        call += ");";
        if (ui->pageUp) ui->webview.eval(call);
        else ui->pendingState = std::move(call);
      }
    };
    // eventTransfer hands over ONE event's atom at a time -- jalv, Carla,
    // Ardour and REAPER all deliver it so, and the state arriving that way
    // was dropped here, because only a whole Sequence was read: the page
    // never had the plugin's state. A Sequence is still read, event by
    // event, for a host that forwards the port's whole buffer.
    if (atom->type == ui->uridAtomSequence) {
      if (bufferSize < sizeof(LV2_Atom_Sequence)) return;
      lv2::forEachAtomEvent((const LV2_Atom_Sequence*) buffer, [&deliver](const LV2_Atom_Event* ev) {
        deliver(ev->body.type, ev->body.size, (const uint8_t*) ev + sizeof(LV2_Atom_Event));
      });
    } else {
      deliver(atom->type, atom->size, (const uint8_t*) buffer + sizeof(LV2_Atom));
    }
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
  // Gain reduction is not measured from the audio, so it has no second half to
  // wait for: the plugin wrote what it took off and the interface shows it.
  if (lv2::grPorts() && portIndex == (uint32_t) lv2::portMeterGr()) {
    ui->meterGr = *(const float*) buffer;
    return;
  }

  // Everything below is a float control port. An atom arriving here would be
  // read as a number, which is the shape of bug that shows up as a knob
  // jumping to a value nobody set.
  if (format != 0 || bufferSize != sizeof(float)) return;
  if (portIndex >= (uint32_t) kDesc.numParams) return; // not one of ours

  const float value = *(const float*) buffer;
  ui->latest[portIndex] = value;
  ui->haveLatest[portIndex] = true;
  if (!ui->pageUp) return; // pushed by pageUp() when the document is there
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
    // One bare atom, as eventTransfer is specified and as every host delivers
    // it: wrapped in a Sequence it reached the plugin as an event of type
    // Sequence and was never taken for a request. In REAPER the page asked
    // 88 times and was answered none.
    // With a body: the request's number, which the plugin does not read. An
    // atom with an empty body is legal, and REAPER dropped every one -- the
    // page asked fourteen times and the plugin heard nothing.
    struct {
      LV2_Atom atom;
      uint64_t number;
    } request{};
    request.atom.size = (uint32_t) sizeof(request.number);
    request.atom.type = ui->uridStateRequest;
    request.number = (uint64_t) ui->requestsSent;
    ui->write(ui->controller, (uint32_t) lv2::portUiControl(), (uint32_t) sizeof(request),
              ui->uridEventTransfer, &request);
  }

  // The needle, on the interface's own clock. A host calls idle() at the
  // rate it likes, so the ballistics are advanced by the time that passed
  // rather than by a 30 Hz tick the host never promised.
  if (ui->webview.ready()) {
    ui->meter.pushGr(ui->meterGr);
    ui->meter.tickNow();
    char lv[40], db[40], vu[40], gr[40], call[200];
    jsNumber(lv, sizeof(lv), (double) ui->meter.level());
    jsNumber(db, sizeof(db), (double) ui->meter.db());
    jsNumber(vu, sizeof(vu), (double) ui->meter.vu());
    jsNumber(gr, sizeof(gr), (double) ui->meter.gr());
    std::snprintf(call, sizeof(call), "window.sonore.__meter(%s,%s,%s,%s);", lv, db, vu, gr);
    if (ui->lastMeter != call) {
      ui->lastMeter = call;
      ui->webview.eval(call);
    }
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

/** ui:resize as the interface's own: the host tells it the size its window
 *  now is -- a plugin window dragged larger -- and the page follows, within
 *  the plugin's editor limits. A plugin whose editor resizes on neither axis
 *  refuses. Without it the page stayed the size it opened at inside a host
 *  window of any size. The handle field is the host's to ignore here: the
 *  interface's own handle is the first argument. */
inline int uiResize(void* handle, int width, int height) {
  auto* ui = (UiInstance*) handle;
  if (!ui || width <= 0 || height <= 0) return 1;
  if (!kDesc.editorLimits.resizableHorizontally && !kDesc.editorLimits.resizableVertically) return 1;
  uint32_t w = (uint32_t) width, h = (uint32_t) height;
  clapwrap::clampEditorSize(&w, &h);
  ui->webview.setSize(w, h);
  return 0;
}

inline const void* extensionData(const char* uri) {
  if (uri && std::strcmp(uri, SONORE_LV2_UI__idleInterface) == 0) {
    static const Lv2UiIdleInterface iface = {idle};
    return &iface;
  }
  if (uri && std::strcmp(uri, SONORE_LV2_UI__resize) == 0) {
    static const Lv2UiResize iface = {nullptr, uiResize};
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
