// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the Audio Unit editor.
//
// Compiled on macOS since 2026-09-01 (the SDK workflow builds it beside the
// AU), but never OPENED: auval validates the unit and does not instantiate its
// CocoaUI, and no macOS test attaches an editor yet. Treat the view as unrun
// until a host on a Mac has shown it.
//
// An AU host asks for `kAudioUnitProperty_CocoaUI`, gets the NAME of an
// Objective-C class, instantiates it, and calls `uiViewForAudioUnit:withSize:`
// expecting an NSView back. There is no such class in any framework, so one is
// REGISTERED AT RUNTIME here: the same technique webview_cocoa.h uses for its
// script-message handler, and the reason this SDK needs no .mm file anywhere.
//
// The view it returns is the same WKWebView every other format shows, wired to
// the same `window.sonore` bridge.
#pragma once

#if !defined(__APPLE__) && !defined(SONORE_APPLE_SYNTAX_CHECK)
#error "au_view.h is macOS only"
#endif

#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

#include "au_wrapper.h"
#include "gui.h"
#include "webview_cocoa.h"

namespace sonore {
namespace au {

/** The Unit behind a host's AudioUnit handle. The handle is an OPAQUE wrapper
 *  owned by the AudioComponent framework: casting it (an earlier version did)
 *  dereferences framework internals and crashes. The sanctioned route from a
 *  Cocoa view back to its instance is a property read, which the framework
 *  routes to our own getProperty. */
inline Unit* unitFromAudioUnit(void* audioUnit) {
  if (!audioUnit) return nullptr;
  Unit* unit = nullptr;
  UInt32 size = sizeof(unit);
  if (AudioUnitGetProperty((AudioUnit) audioUnit, kSonorePropertyUnitPointer,
                           kAudioUnitScope_Global, 0, &unit, &size) != noErr)
    return nullptr;
  return unit;
}

/**
 * A UI edit, on the main thread.
 *
 * AU's equivalent of VST3's IComponentHandler is AUParameterSet, which both
 * applies the value and posts the notification hosts listen to for automation
 * and undo. Sending only the queue message would move the sound without the
 * host ever knowing.
 */
inline void viewOnMessage(Unit* unit, const BridgeMessage& message) {
  if (!unit) return;

  auto notify = [unit](int index, float plain) {
    AudioUnitParameter p{};
    p.mAudioUnit = unit->hostInstance; // the HOST's handle, never our pointer
    p.mParameterID = (AudioUnitParameterID) index;
    p.mScope = kAudioUnitScope_Global;
    p.mElement = 0;
    AUParameterSet(nullptr, nullptr, &p, plain, 0);
  };

  UiEventQueue::Event e;
  switch (message.kind) {
    case BridgeMessage::Kind::Set: {
      if (message.index < 0 || message.index >= SONORE_NUM_PARAMS) return;
      const float plain = clampToRange(kDesc.params[message.index], (float) message.value);
      e.kind = UiEventQueue::Event::Kind::ParamSet;
      e.index = message.index;
      e.value = plain;
      unit->shared.uiEcho[message.index] = plain;
      notify(message.index, plain);
      break;
    }
    case BridgeMessage::Kind::GestureBegin:
    case BridgeMessage::Kind::GestureEnd: {
      // AU has no begin/end pair; hosts infer a gesture from the notification
      // stream, so there is nothing to send and nothing to queue.
      return;
    }
    case BridgeMessage::Kind::LoadPreset: {
      if (message.index < 0 || message.index >= kDesc.numPresets || !kDesc.presets) return;
      const Preset& preset = kDesc.presets[message.index];
      if (preset.numValues != kDesc.numParams) return; // stale: refuse, never half-apply
      for (int i = 0; i < kDesc.numParams; ++i) {
        const float plain = clampToRange(kDesc.params[i], preset.values[i]);
        UiEventQueue::Event pe;
        pe.kind = UiEventQueue::Event::Kind::ParamSet;
        pe.index = i;
        pe.value = plain;
        unit->shared.uiEvents.push(pe);
        unit->shared.uiEcho[i] = plain;
        notify(i, plain);
      }
      return;
    }
    case BridgeMessage::Kind::ChooseFile: {
      // The dialog exists on every platform except the one this file is for.
      // file_dialog.h declares a Cocoa backend and does not yet define it.
      // A page asking for a file here gets nothing rather than a wrong
      // answer.
      return;
    }
    case BridgeMessage::Kind::ContextMenu: {
      // AudioUnit has no way for a plugin to ask its host for a menu. CLAP has
      // an extension and VST3 has IComponentHandler3; AU v2 simply never grew
      // one, so a right-click here does nothing rather than doing something
      // invented. Saying so in a case of its own instead of letting it fall
      // through the default, which would read as an oversight.
      return;
    }
    case BridgeMessage::Kind::NoteOn:
      if (message.note < 0 || message.note > 127) return;
      e.kind = UiEventQueue::Event::Kind::NoteOn;
      e.index = message.note;
      e.value =
          (float) (message.velocity < 1 ? 1 : (message.velocity > 127 ? 127 : message.velocity));
      break;
    case BridgeMessage::Kind::NoteOff:
      if (message.note < 0 || message.note > 127) return;
      e.kind = UiEventQueue::Event::Kind::NoteOff;
      e.index = message.note;
      break;
    default:
      return;
  }
  unit->shared.uiEvents.push(e);
}

/** The ~30 Hz editor clock: push what the page does not know yet. */
inline void viewTick(Unit* unit) {
  if (!unit) return;
  if (unit->shared.guiIsNative) {
    unit->shared.nativeEditor.tick();
    return;
  }
  if (!unit->shared.webview.ready()) return;
  unit->shared.webview.eval(clapwrap::uiTickScript(unit->shared));
}

/** `uiViewForAudioUnit:withSize:`: the one method AUCocoaUIBase requires. */
inline id viewForAudioUnit(id /*self*/, SEL, void* audioUnit, cocoa::CGSizeStruct size) {
  Unit* unit = unitFromAudioUnit(audioUnit);
  if (!unit) return nullptr;

  uint32_t width = size.width > 100.0 ? (uint32_t) size.width : SONORE_UI_WIDTH;
  uint32_t height = size.height > 100.0 ? (uint32_t) size.height : SONORE_UI_HEIGHT;
  // Through the SAME function as every other format. This was the one size
  // path that did not go through it, so a plugin declaring a minimum was
  // honoured everywhere except here -- the class of inconsistency the shared
  // clamp exists to prevent, quietly reintroduced by a format added later.
  clapwrap::clampEditorSize(&width, &height);

  // A plain NSView to host the webview, which the AU host then owns.
  id container = cocoa::msg<id>(cocoa::cls("NSView"), sel_registerName("alloc"));
  const cocoa::CGRectStruct frame{{0.0, 0.0}, {(double) width, (double) height}};
  using InitFrame = id (*)(id, SEL, cocoa::CGRectStruct);
  container = reinterpret_cast<InitFrame>(objc_msgSend)(
      container, sel_registerName("initWithFrame:"), frame);
  if (!container) return nullptr;

  unit->shared.uiEchoValid = false;

  // The same choice CLAP and VST3 make, from the same function. On macOS
  // today it always answers Web, because there is no Cocoa window backend and
  // chooseEditorBackend says exactly that rather than leaving a blank view --
  // but the branch is here so the day one lands, AU is not the format that
  // forgot.
  const EditorChoice choice = clapwrap::editorChoice();
  unit->shared.guiIsNative = choice.backend == EditorBackend::Native;
  if (unit->shared.guiIsNative) {
    if (unit->shared.nativeEditor.open(container, kDesc.params, (int) kDesc.numParams,
                                       clapwrap::makeEditorHost(&unit->shared), (int) width,
                                       (int) height))
      return container;
    unit->shared.guiIsNative = false; // fall through to the page
  }

  unit->shared.webview.onMessage = [unit](const BridgeMessage& m) { viewOnMessage(unit, m); };
  unit->shared.webview.onTick = [unit]() { viewTick(unit); };
  unit->shared.webview.create(container, width, height, clapwrap::uiHtml(), bridgeScript(kDesc),
                              kDesc.id);
  return container;
}

/** Register the factory class once. The name must match the string returned for
 *  kAudioUnitProperty_CocoaUI, or the host looks up a class that isn't there
 *  and silently shows its generic sliders instead. */
inline Class registerViewFactory() {
  static Class klass = [] {
    const char* name = auViewFactoryClassName(); // the same string the property returns
    if (Class existing = objc_getClass(name)) return existing;
    Class c = objc_allocateClassPair((Class) cocoa::cls("NSObject"), name, 0);
    if (!c) return (Class) nullptr;
    // Signature: id (id self, SEL _cmd, void* au, NSSize size)
    class_addMethod(c, sel_registerName("uiViewForAudioUnit:withSize:"), (IMP) viewForAudioUnit,
                    "@@:^v{CGSize=dd}");
    // AUCocoaUIBase also asks for an interface version.
    class_addMethod(c, sel_registerName("interfaceVersion"),
                    (IMP) +[](id, SEL) -> unsigned { return 0; }, "I@:");
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

/** Registered from a static initialiser, so the class exists before any host
 *  can ask for it: a lazily-registered class loses the race with a fast host. */
struct ViewFactoryRegistrar {
  ViewFactoryRegistrar() { registerViewFactory(); }
};
static ViewFactoryRegistrar g_sonoreViewFactoryRegistrar;

} // namespace au
} // namespace sonore
