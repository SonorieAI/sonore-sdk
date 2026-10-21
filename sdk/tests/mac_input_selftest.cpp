// SPDX-License-Identifier: Apache-2.0
// What the Cocoa peer exposes to VoiceOver and to input methods, on THIS Mac.
//
// gfx/accessibility_mac.h and the NSTextInputClient half of gfx/window_cocoa.h
// were written and compiled on a machine with no Mac (tests/
// mac_syntax_mac_input.cpp), and have never run. This is the program a Mac
// developer runs to find out whether they work:
//
//   mac_input_selftest               checks, prints the tree, exits
//   mac_input_selftest --wait 30     then keeps the window up 30 s, to try
//                                    VoiceOver (Cmd-F5) and an input method
//   mac_input_selftest --bluetooth   also opens the Bluetooth MIDI window
//
// It asks AppKit what the view exposes through the ATTRIBUTE API --
// accessibilityAttributeValue:, accessibilityPerformAction: -- which is the
// path the accessibility server takes on VoiceOver's behalf, rather than
// calling the bridge's own methods; and it drives NSTextInputClient through
// the view as NSTextInputContext would. Every expected rectangle is computed
// with AppKit's own conversions (convertRect:toView:, convertRectToScreen:),
// not with the bridge's arithmetic, so a wrong flip shows up as a failure
// rather than agreeing with itself.
//
// Exit code: non-zero when a check fails.
#if !defined(__APPLE__)
#error "mac_input_selftest is for macOS"
#endif

#include <sonore/bluetooth_midi.h>
#include <sonore/gfx/accessibility.h>
#include <sonore/gfx/system_font.h>
#include <sonore/gfx/text_editor.h>
#include <sonore/gfx/widgets.h>
#include <sonore/gfx/window_cocoa.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace gfx = sonore::gfx;
using gfx::nsax::send;
using gfx::nsax::sendRect;
using gfx::nsax::sel;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++failures;
}

id cls(const char* name) { return (id) objc_getClass(name); }
id str(const char* utf8) { return gfx::nsax::string(utf8); }

std::string text(id value) {
  if (!value) return "(nil)";
  if (send<BOOL>(value, sel("isKindOfClass:"), cls("NSString"))) return "\"" + gfx::nsax::utf8Of(value) + "\"";
  if (send<BOOL>(value, sel("isKindOfClass:"), cls("NSNumber"))) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", send<double>(value, sel("doubleValue")));
    return buffer;
  }
  id description = send<id>(value, sel("description"));
  return gfx::nsax::utf8Of(description);
}

id attribute(id element, const char* name) {
  return send<id>(element, sel("accessibilityAttributeValue:"), str(name));
}

double numberOf(id value) {
  return value && send<BOOL>(value, sel("isKindOfClass:"), cls("NSNumber")) ? send<double>(value, sel("doubleValue"))
                                                                           : NAN;
}

std::vector<id> items(id array) {
  std::vector<id> out;
  if (!array) return out;
  const unsigned long n = send<unsigned long>(array, sel("count"));
  for (unsigned long i = 0; i < n; ++i) out.push_back(send<id>(array, sel("objectAtIndex:"), i));
  return out;
}

bool hasAction(id element, const char* action) {
  for (id name : items(send<id>(element, sel("accessibilityActionNames"))))
    if (gfx::nsax::utf8Of(name) == action) return true;
  return false;
}

void runFor(double seconds) {
  id date = send<id>(cls("NSDate"), sel("dateWithTimeIntervalSinceNow:"), seconds);
  id loop = send<id>(cls("NSRunLoop"), sel("currentRunLoop"));
  send<void>(loop, sel("runUntilDate:"), date);
}

/** A component's rectangle on the screen, through AppKit's conversions. */
gfx::nsax::AxRect expectedFrame(id view, const gfx::Rect& inView) {
  const gfx::nsax::AxRect r{{inView.x, inView.y}, {inView.w, inView.h}};
  const gfx::nsax::AxRect inWindow = sendRect<gfx::nsax::AxRect>(view, sel("convertRect:toView:"), r, (id) nullptr);
  id window = send<id>(view, sel("window"));
  return sendRect<gfx::nsax::AxRect>(window, sel("convertRectToScreen:"), inWindow);
}

bool near(const gfx::nsax::AxRect& a, const gfx::nsax::AxRect& b) {
  return std::fabs(a.origin.x - b.origin.x) < 1.0 && std::fabs(a.origin.y - b.origin.y) < 1.0 &&
         std::fabs(a.size.width - b.size.width) < 1.0 && std::fabs(a.size.height - b.size.height) < 1.0;
}

std::string rectText(const gfx::nsax::AxRect& r) {
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), "{%.1f, %.1f, %.1f x %.1f}", r.origin.x, r.origin.y, r.size.width,
                r.size.height);
  return buffer;
}

void printElement(id element, int depth) {
  std::printf("  %*s%s", depth * 2, "", gfx::nsax::utf8Of(attribute(element, "AXRole")).c_str());
  const std::string subrole = gfx::nsax::utf8Of(attribute(element, "AXSubrole"));
  if (!subrole.empty()) std::printf("/%s", subrole.c_str());
  std::printf(" [%s]", gfx::nsax::utf8Of(attribute(element, "AXRoleDescription")).c_str());
  const std::string title = gfx::nsax::utf8Of(attribute(element, "AXTitle"));
  const std::string label = gfx::nsax::utf8Of(attribute(element, "AXDescription"));
  if (!title.empty()) std::printf(" title \"%s\"", title.c_str());
  if (!label.empty()) std::printf(" label \"%s\"", label.c_str());
  id value = attribute(element, "AXValue");
  if (value) std::printf(" value %s", text(value).c_str());
  const std::string valueText = gfx::nsax::utf8Of(attribute(element, "AXValueDescription"));
  if (!valueText.empty()) std::printf(" (\"%s\")", valueText.c_str());
  std::printf(" frame %s", rectText(sendRect<gfx::nsax::AxRect>(element, sel("accessibilityFrame"))).c_str());
  std::string actions;
  for (id name : items(send<id>(element, sel("accessibilityActionNames"))))
    actions += " " + gfx::nsax::utf8Of(name);
  if (!actions.empty()) std::printf(" actions:%s", actions.c_str());
  std::printf("\n");
  for (id child : items(attribute(element, "AXChildren"))) printElement(child, depth + 1);
}

id findElement(id parent, const char* role, const std::string& name) {
  for (id child : items(attribute(parent, "AXChildren"))) {
    const bool roleMatches = gfx::nsax::utf8Of(attribute(child, "AXRole")) == role;
    const std::string label = gfx::nsax::utf8Of(attribute(child, "AXDescription"));
    const std::string title = gfx::nsax::utf8Of(attribute(child, "AXTitle"));
    if (roleMatches && (label == name || title == name)) return child;
    if (id deeper = findElement(child, role, name)) return deeper;
  }
  return nullptr;
}

} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  double wait = 0.0;
  bool bluetooth = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--wait") == 0 && i + 1 < argc) wait = std::atof(argv[++i]);
    if (std::strcmp(argv[i], "--bluetooth") == 0) bluetooth = true;
  }
  std::printf("MAC INPUT SELF-TEST\n");

  id app = send<id>(cls("NSApplication"), sel("sharedApplication"));
  send<BOOL>(app, sel("setActivationPolicy:"), (long) 0); // NSApplicationActivationPolicyRegular
  check(gfx::NativeWindowCocoa::isAvailable(), "the Cocoa peer is available");
  check(gfx::nsax::MacAccessibilityRoot::isAvailable(),
        std::string("NSAccessibilityElement is available ") + gfx::nsax::MacAccessibilityRoot::unavailableReason());
  std::printf("  input methods: %s\n", gfx::NativeWindowCocoa::inputMethodStatus());

  // ── A window with one of each control ──
  const gfx::nsax::AxRect contentRect{{200.0, 300.0}, {480.0, 220.0}};
  using InitWindow = id (*)(id, SEL, gfx::nsax::AxRect, unsigned long, unsigned long, BOOL);
  id window = send<id>(cls("NSWindow"), sel("alloc"));
  window = reinterpret_cast<InitWindow>(objc_msgSend)(window, sel("initWithContentRect:styleMask:backing:defer:"),
                                                      contentRect, 3ul /*titled|closable*/, 2ul, NO);
  send<void>(window, sel("setTitle:"), str("sonore mac input self-test"));
  send<void>(window, sel("makeKeyAndOrderFront:"), (id) nullptr);
  send<void>(app, sel("activateIgnoringOtherApps:"), (BOOL) YES);
  id contentView = send<id>(window, sel("contentView"));

  auto face = gfx::systemTypeface();
  gfx::Widget root;
  root.setFont(face ? gfx::Font(face, 13.0f) : gfx::Font());
  root.setBounds({0.0f, 0.0f, 480.0f, 220.0f});
  gfx::Slider gain;
  gain.setAccessibleName("Gain");
  gain.setAccessibleValueText("-6.0 dB");
  gain.setValue(0.5f, false);
  gain.setBounds({20.0f, 20.0f, 80.0f, 80.0f});
  gfx::Button bypass("Bypass");
  bypass.setToggleable(true);
  bypass.setBounds({120.0f, 20.0f, 100.0f, 28.0f});
  gfx::ComboBox shape;
  shape.setAccessibleName("Shape");
  shape.setItems({"Sine", "Saw", "Square"});
  shape.setSelectedIndex(1, false);
  shape.setBounds({240.0f, 20.0f, 120.0f, 28.0f});
  gfx::Label caption("Preset name");
  caption.setBounds({20.0f, 120.0f, 120.0f, 24.0f});
  gfx::TextEditor field;
  field.setAccessibleName("Name");
  field.setBounds({150.0f, 116.0f, 300.0f, 32.0f});
  for (gfx::Component* c : std::vector<gfx::Component*>{&gain, &bypass, &shape, &caption, &field}) root.addChild(c);

  gfx::NativeWindowCocoa peer;
  peer.setAccessibleWindowName("Self-test editor");
  check(peer.open(contentView, root, 480, 220), "the peer opens into the window's content view");
  runFor(0.3);
  id view = (id) peer.handle();

  // ── Accessibility, through AppKit's attribute API ──
  std::printf("\n-- accessibility --\n");
  std::printf("  the model says:\n%s", gfx::describeAccessibleTree(&root).c_str());
  check(gfx::nsax::utf8Of(attribute(view, "AXRole")) == "AXGroup", "the view is an AXGroup");
  check(gfx::nsax::utf8Of(attribute(view, "AXDescription")) == "Self-test editor", "named after the editor");
  const std::vector<id> top = items(attribute(view, "AXChildren"));
  check(!top.empty(), "the view has children: " + std::to_string(top.size()));
  std::printf("  AppKit says:\n");
  printElement(view, 1);

  id slider = findElement(view, "AXSlider", "Gain");
  check(slider != nullptr, "a slider named Gain");
  if (slider) {
    check(std::fabs(numberOf(attribute(slider, "AXValue")) - 0.5) < 1e-6, "its AXValue is 0.5");
    check(gfx::nsax::utf8Of(attribute(slider, "AXValueDescription")) == "-6.0 dB", "read as -6.0 dB");
    check(hasAction(slider, "AXIncrement") && hasAction(slider, "AXDecrement"), "it offers AXIncrement and AXDecrement");
    send<void>(slider, sel("accessibilityPerformAction:"), str("AXIncrement"));
    check(std::fabs(gain.value() - 0.51f) < 1e-4f, "AXIncrement steps it to 0.51: " + std::to_string(gain.value()));
    check(send<BOOL>(slider, sel("accessibilityIsAttributeSettable:"), str("AXValue")) != NO, "AXValue is settable");
    send<void>(slider, sel("accessibilitySetValue:forAttribute:"), gfx::nsax::number(0.25), str("AXValue"));
    check(std::fabs(gain.value() - 0.25f) < 1e-4f, "setting AXValue to 0.25 moves it there");
    const gfx::nsax::AxRect frame = sendRect<gfx::nsax::AxRect>(slider, sel("accessibilityFrame"));
    const gfx::nsax::AxRect expected = expectedFrame(view, gain.bounds());
    check(near(frame, expected), "its frame " + rectText(frame) + " is AppKit's own conversion " + rectText(expected));
    const gfx::nsax::AxPoint centre{expected.origin.x + expected.size.width * 0.5,
                                    expected.origin.y + expected.size.height * 0.5};
    check(send<id>(view, sel("accessibilityHitTest:"), centre) == slider, "a hit test at its centre finds it");
  }

  id toggle = findElement(view, "AXCheckBox", "Bypass");
  check(toggle != nullptr, "a toggle titled Bypass (AXCheckBox/AXToggle)");
  if (toggle) {
    check(gfx::nsax::utf8Of(attribute(toggle, "AXSubrole")) == "AXToggle", "subrole AXToggle");
    check(numberOf(attribute(toggle, "AXValue")) == 0.0, "reads 0 while off");
    send<void>(toggle, sel("accessibilityPerformAction:"), str("AXPress"));
    check(bypass.isToggled() && numberOf(attribute(toggle, "AXValue")) == 1.0, "AXPress turns it on, and it reads 1");
  }

  id popup = findElement(view, "AXPopUpButton", "Shape");
  check(popup != nullptr && gfx::nsax::utf8Of(attribute(popup, "AXValue")) == "Saw",
        "the combo box is a pop-up button reading \"Saw\"");

  id label = nullptr;
  for (id child : top)
    if (gfx::nsax::utf8Of(attribute(child, "AXRole")) == "AXStaticText") label = child;
  check(label && gfx::nsax::utf8Of(attribute(label, "AXValue")) == "Preset name", "the label is static text whose value is its text");

  id textField = findElement(view, "AXTextField", "Name");
  check(textField != nullptr, "a text field named Name");
  if (textField) {
    send<void>(textField, sel("accessibilitySetValue:forAttribute:"), str("Lead 1"), str("AXValue"));
    check(field.getText() == "Lead 1", "setting its AXValue types \"Lead 1\": \"" + field.getText() + "\"");
    send<void>(textField, sel("accessibilitySetValue:forAttribute:"), send<id>(cls("NSNumber"), sel("numberWithBool:"), (BOOL) YES), str("AXFocused"));
    check(peer.router()->focused() == &field, "setting AXFocused gives it the keyboard focus");
  }

  // ── Input methods, through NSTextInputClient as NSTextInputContext drives it ──
  std::printf("\n-- input methods --\n");
  id protocol = (id) objc_getProtocol("NSTextInputClient");
  check(protocol && send<BOOL>(view, sel("conformsToProtocol:"), protocol), "the view conforms to NSTextInputClient");
  check(send<id>(view, sel("inputContext")) != nullptr, "and therefore has an input context");
  peer.router()->setFocus(&field);
  field.setText("");
  using SetMarked = void (*)(id, SEL, id, gfx::cocoa::NSRange2, gfx::cocoa::NSRange2);
  reinterpret_cast<SetMarked>(objc_msgSend)(view, sel("setMarkedText:selectedRange:replacementRange:"),
                                            str("にほん"), gfx::cocoa::NSRange2{3, 0},
                                            gfx::cocoa::NSRange2{gfx::cocoa::kNSNotFound, 0});
  check(send<BOOL>(view, sel("hasMarkedText")) != NO, "setMarkedText: leaves marked text");
  const gfx::cocoa::NSRange2 marked = send<gfx::cocoa::NSRange2>(view, sel("markedRange"));
  check(marked.location == 0 && marked.length == 3, "markedRange is {0, 3}");
  const gfx::cocoa::NSRange2 selected = send<gfx::cocoa::NSRange2>(view, sel("selectedRange"));
  check(selected.location == 3 && selected.length == 0, "selectedRange is {3, 0}");
  check(field.getText().empty(), "the field's value does not include it");
  using FirstRect = gfx::nsax::AxRect (*)(id, SEL, gfx::cocoa::NSRange2, gfx::cocoa::NSRange2*);
  gfx::cocoa::NSRange2 actual{0, 0};
#if defined(__x86_64__)
  const gfx::nsax::AxRect first = reinterpret_cast<FirstRect>(objc_msgSend_stret)(
      view, sel("firstRectForCharacterRange:actualRange:"), gfx::cocoa::NSRange2{0, 1}, &actual);
#else
  const gfx::nsax::AxRect first = reinterpret_cast<FirstRect>(objc_msgSend)(
      view, sel("firstRectForCharacterRange:actualRange:"), gfx::cocoa::NSRange2{0, 1}, &actual);
#endif
  const gfx::nsax::AxRect fieldOnScreen = expectedFrame(view, field.bounds());
  check(first.size.width > 0.0 && first.origin.x >= fieldOnScreen.origin.x &&
            first.origin.x < fieldOnScreen.origin.x + fieldOnScreen.size.width &&
            first.origin.y >= fieldOnScreen.origin.y - 1.0 &&
            first.origin.y + first.size.height <= fieldOnScreen.origin.y + fieldOnScreen.size.height + 1.0,
        "firstRect " + rectText(first) + " lies inside the field on screen " + rectText(fieldOnScreen));
  check(actual.location == 0 && actual.length == 1, "actualRange is {0, 1}");
  using Substring = id (*)(id, SEL, gfx::cocoa::NSRange2, gfx::cocoa::NSRange2*);
  id sub = reinterpret_cast<Substring>(objc_msgSend)(view, sel("attributedSubstringForProposedRange:actualRange:"),
                                                     gfx::cocoa::NSRange2{0, 3}, &actual);
  check(sub && gfx::nsax::utf8Of(send<id>(sub, sel("string"))) == "にほん", "the substring reads back the composition");
  using Insert = void (*)(id, SEL, id, gfx::cocoa::NSRange2);
  reinterpret_cast<Insert>(objc_msgSend)(view, sel("insertText:replacementRange:"), str("日本"),
                                         gfx::cocoa::NSRange2{gfx::cocoa::kNSNotFound, 0});
  check(field.getText() == "日本" && send<BOOL>(view, sel("hasMarkedText")) == NO,
        "insertText: replaces the composition: \"" + field.getText() + "\"");

  // A real key, through keyDown: and interpretKeyEvents:. What it types
  // depends on the input source selected right now, so it is printed rather
  // than judged.
  using KeyEvent = id (*)(id, SEL, unsigned long, gfx::nsax::AxPoint, unsigned long, double, long, id, id, id, BOOL,
                          unsigned short);
  id event = reinterpret_cast<KeyEvent>(objc_msgSend)(
      cls("NSEvent"), sel("keyEventWithType:location:modifierFlags:timestamp:windowNumber:context:characters:"
                          "charactersIgnoringModifiers:isARepeat:keyCode:"),
      10ul /*NSEventTypeKeyDown*/, gfx::nsax::AxPoint{0.0, 0.0}, 0ul, 0.0,
      send<long>(window, sel("windowNumber")), (id) nullptr, str("a"), str("a"), (BOOL) NO, (unsigned short) 0);
  send<void>(view, sel("keyDown:"), event);
  std::printf("  'a' through keyDown: with the current input source gave: \"%s\"%s\n", field.getText().c_str(),
              field.composition().isActive() ? " (composing)" : "");

  // ── Bluetooth MIDI ──
  std::printf("\n-- Bluetooth MIDI --\n");
  std::printf("  available: %s %s\n", sonore::BluetoothMidiDevicePairingDialogue::isAvailable() ? "yes" : "no",
              sonore::BluetoothMidiDevicePairingDialogue::unavailableReason().c_str());
  std::printf("  shows: %s\n", sonore::BluetoothMidiDevicePairingDialogue::description());
  if (bluetooth) {
    bool closed = false;
    const bool opened = sonore::BluetoothMidiDevicePairingDialogue::open([&](bool) { closed = true; });
    check(opened, "the Bluetooth MIDI window opens");
    for (int i = 0; opened && !closed && i < 300; ++i) runFor(0.1);
    std::printf("  closed: %s\n", closed ? "yes" : "no (30 s passed)");
  }

  if (wait > 0.0) {
    std::printf("\n  %.0f s to try VoiceOver (Cmd-F5) and an input method in the window...\n", wait);
    runFor(wait);
  }
  peer.close();
  send<void>(window, sel("close"));
  std::printf(failures ? "\nMAC INPUT SELF-TEST FAILED (%d)\n" : "\nMAC INPUT SELF-TEST PASSED\n", failures);
  return failures ? 1 : 0;
}
