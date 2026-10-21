// SPDX-License-Identifier: Apache-2.0
// The parts of the macOS and X11 input work that do not need a Mac or an X
// server, run for real.
//
// The NSAccessibility bridge, NSTextInputClient and XIM are only COMPILED here
// (tests/mac_syntax_mac_input.cpp, and the X11 headers in CI). What they stand
// on is platform-neutral and is run here, on every platform:
//
//  - the role table both accessibility bridges read -- every role has a row,
//    UIA's ids are the ones the Windows bridge always returned, and the AX
//    roles are the ones the Mac bridge must return;
//  - the actions a reader performs (press, increment, decrement, show menu,
//    set a value), through real widgets, checking that they go through the
//    controls' own key paths -- one bracketed edit each;
//  - the Mac bridge's bookkeeping: which element survives a rebuild, which
//    announcements a change produces and how often, and the y-up flip;
//  - the composition model all input-method paths share: NSTextInputClient's
//    UTF-16 range arithmetic and XIM's preedit edits, driven against real
//    TextEditor and TextArea instances, including the field's own rules on
//    commit and what happens when focus or a click interrupts a composition;
//  - the Bluetooth MIDI dialogue's availability logic, and on Windows its
//    launch path with the launch itself disabled (SONORE_BLUETOOTH_MIDI_NO_LAUNCH).
#include <sonore/bluetooth_midi.h>
#include <sonore/gfx/accessibility.h>
#include <sonore/gfx/system_font.h>
#include <sonore/gfx/text_area.h>
#include <sonore/gfx/text_editor.h>
#include <sonore/gfx/text_input.h>
#include <sonore/gfx/widgets.h>

#if defined(_WIN32)
#include <sonore/gfx/uia_win32.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace sonore::gfx;

namespace {

int g_failures = 0;
int g_checks = 0;
// Whether a typeface was found: the checks that need glyph WIDTHS are skipped,
// loudly, without one, rather than failing on a machine with no fonts.
bool g_measured = false;

void check(bool ok, const char* what) {
  ++g_checks;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

void check(bool ok, const std::string& what) { check(ok, what.c_str()); }

std::shared_ptr<Typeface> loadAnyTypeface() {
  if (auto system = systemTypeface()) return system;
  const char* candidates[] = {
#if defined(_WIN32)
      "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/segoeui.ttf",
#endif
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/System/Library/Fonts/Helvetica.ttc",
  };
  for (const char* path : candidates) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) continue;
    std::vector<uint8_t> bytes;
    uint8_t chunk[65536];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) bytes.insert(bytes.end(), chunk, chunk + got);
    std::fclose(f);
    auto face = std::make_shared<Typeface>();
    if (face->load(std::move(bytes))) return face;
  }
  return nullptr;
}

std::string shown(const TextInputTarget& t) {
  const std::vector<uint32_t> s = TextInputSession::shownText(t);
  return detail::textInputEncode(s.data(), s.data() + s.size());
}

std::string composed(const TextInputTarget& t) {
  const TextComposition& c = t.composition();
  return detail::textInputEncode(c.text.data(), c.text.data() + c.text.size());
}

std::vector<uint32_t> cps(const char* utf8) { return detail::textInputDecode(utf8); }

bool rangeIs(TextRange16 r, size_t location, size_t length) { return r.location == location && r.length == length; }

const AccessibleRole kAllRoles[] = {
    AccessibleRole::Unknown,   AccessibleRole::Group,     AccessibleRole::Window,
    AccessibleRole::Dialog,    AccessibleRole::Button,    AccessibleRole::ToggleButton,
    AccessibleRole::Slider,    AccessibleRole::Label,     AccessibleRole::TextField,
    AccessibleRole::ComboBox,  AccessibleRole::List,      AccessibleRole::ListItem,
    AccessibleRole::Table,     AccessibleRole::ScrollBar, AccessibleRole::ProgressBar,
    AccessibleRole::MusicalKeyboard,
};

// ── The role table ─────────────────────────────────────────────────────────

void testRoleTable() {
  std::printf("\n-- the role table both bridges read --\n");
  // What uia::controlTypeFor returned before the table existed (its old
  // switch, transcribed): the Windows bridge, verified against a real UIA
  // client, must not have changed by moving.
  struct Uia {
    AccessibleRole role;
    long id;
  };
  const Uia uia[] = {
      {AccessibleRole::Button, 50000},       {AccessibleRole::ToggleButton, 50000},
      {AccessibleRole::Slider, 50015},       {AccessibleRole::Label, 50020},
      {AccessibleRole::TextField, 50004},    {AccessibleRole::ComboBox, 50003},
      {AccessibleRole::List, 50008},         {AccessibleRole::ListItem, 50007},
      {AccessibleRole::Table, 50036},        {AccessibleRole::ScrollBar, 50014},
      {AccessibleRole::ProgressBar, 50012},  {AccessibleRole::Dialog, 50032},
      {AccessibleRole::Window, 50032},       {AccessibleRole::Group, 50026},
      {AccessibleRole::MusicalKeyboard, 50026}, {AccessibleRole::Unknown, 50033},
  };
  int rows = 0, uiaSame = 0;
  for (AccessibleRole role : kAllRoles) {
    const AccessibleRoleMapping& m = accessibleRoleMapping(role);
    if (m.role == role && m.axRole && m.axDescribeAsRole && std::strncmp(m.axRole, "AX", 2) == 0) ++rows;
  }
  check(rows == (int) (sizeof(kAllRoles) / sizeof(kAllRoles[0])), "every role has its own row, with an AX role");
  for (const Uia& u : uia)
    if (accessibleRoleMapping(u.role).uiaControlType == u.id) ++uiaSame;
  check(uiaSame == (int) (sizeof(uia) / sizeof(uia[0])), "the UIA control types are the ones the Windows bridge returned");
#if defined(_WIN32)
  int bridgeSame = 0;
  for (const Uia& u : uia)
    if (uia::controlTypeFor(u.role) == u.id) ++bridgeSame;
  check(bridgeSame == (int) (sizeof(uia) / sizeof(uia[0])), "and uia::controlTypeFor reads them from the table");
  check(uia::controlTypeFor(AccessibleRole::Slider) == UIA_SliderControlTypeId &&
            uia::controlTypeFor(AccessibleRole::ComboBox) == UIA_ComboBoxControlTypeId &&
            uia::controlTypeFor(AccessibleRole::Unknown) == UIA_PaneControlTypeId,
        "the numbers are UIAutomationClient.h's constants");
#endif

  // What the Mac bridge must say, as known answers.
  struct Ax {
    AccessibleRole role;
    const char* axRole;
    const char* subrole;
    AccessibleValueKind kind;
    bool title;
  };
  const Ax ax[] = {
      {AccessibleRole::Slider, "AXSlider", nullptr, AccessibleValueKind::Number, false},
      {AccessibleRole::ToggleButton, "AXCheckBox", "AXToggle", AccessibleValueKind::Toggle, true},
      {AccessibleRole::Button, "AXButton", nullptr, AccessibleValueKind::Text, true},
      {AccessibleRole::ComboBox, "AXPopUpButton", nullptr, AccessibleValueKind::Text, false},
      {AccessibleRole::Label, "AXStaticText", nullptr, AccessibleValueKind::NameAsValue, false},
      {AccessibleRole::TextField, "AXTextField", nullptr, AccessibleValueKind::Text, false},
      {AccessibleRole::Table, "AXList", nullptr, AccessibleValueKind::Text, false},
      {AccessibleRole::ScrollBar, "AXScrollBar", nullptr, AccessibleValueKind::Number, false},
      {AccessibleRole::ProgressBar, "AXProgressIndicator", nullptr, AccessibleValueKind::Number, false},
      {AccessibleRole::Dialog, "AXGroup", nullptr, AccessibleValueKind::Text, true},
      {AccessibleRole::MusicalKeyboard, "AXGroup", nullptr, AccessibleValueKind::Text, false},
  };
  int axSame = 0;
  for (const Ax& a : ax) {
    const AccessibleRoleMapping& m = accessibleRoleMapping(a.role);
    const bool subroleSame = (a.subrole == nullptr) == (m.axSubrole == nullptr) &&
                             (!a.subrole || std::strcmp(a.subrole, m.axSubrole) == 0);
    if (std::strcmp(m.axRole, a.axRole) == 0 && subroleSame && m.axValue == a.kind && m.axNameIsTitle == a.title)
      ++axSame;
    else
      std::printf("       %s: %s/%s\n", accessibleRoleName(a.role), m.axRole, m.axSubrole ? m.axSubrole : "-");
  }
  check(axSame == (int) (sizeof(ax) / sizeof(ax[0])), "the AX roles, subroles and value kinds are the expected ones");
  const AccessibleRoleMapping& dialog = accessibleRoleMapping(AccessibleRole::Dialog);
  check(std::strcmp(dialog.axDescribeAsRole, "AXWindow") == 0 && dialog.axDescribeAsSubrole &&
            std::strcmp(dialog.axDescribeAsSubrole, "AXDialog") == 0,
        "a dialog drawn in the editor is an AXGroup still CALLED a dialog");

  // Every widget that reports a range reads as a number on the Mac.
  Slider slider;
  ComboBox combo;
  combo.setItems({"a", "b"});
  check(accessibleRoleMapping(slider.accessibleInfo().role).axValue == AccessibleValueKind::Number,
        "a slider's range is read as a number");
  check(accessibleRoleMapping(combo.accessibleInfo().role).axValue == AccessibleValueKind::Text,
        "a choice is read by its NAME, not its index");
}

// ── Actions ────────────────────────────────────────────────────────────────

void testActions(const Font& font) {
  std::printf("\n-- what a reader can make a control do --\n");
  Widget root;
  root.setFont(font);
  root.setBounds({0.0f, 0.0f, 400.0f, 300.0f});

  Slider gain;
  gain.setBounds({100.0f, 50.0f, 60.0f, 60.0f});
  root.addChild(&gain);
  int starts = 0, ends = 0, changes = 0;
  gain.onDragStart = [&] { ++starts; };
  gain.onDragEnd = [&] { ++ends; };
  gain.onValueChange = [&](float) { ++changes; };
  gain.setValue(0.5f, false);
  AccessibleInfo info = gain.accessibleInfo();
  check(info.offers(AccessibleAction::Increment) && info.offers(AccessibleAction::Decrement) &&
            !info.offers(AccessibleAction::Press) && info.valueSettable,
        "a slider offers increment and decrement and a settable value, and no press");
  check(!info.offers(AccessibleAction::ShowMenu), "and no menu while nobody supplies one");
  check(gain.performAccessibleAction(AccessibleAction::Increment) && std::fabs(gain.value() - 0.51f) < 1e-5f,
        "increment is the arrow key's step: 0.50 -> 0.51");
  check(gain.performAccessibleAction(AccessibleAction::Decrement) && std::fabs(gain.value() - 0.50f) < 1e-5f,
        "decrement steps back");
  check(starts == 2 && ends == 2, "each one bracketed as its own gesture, as a key press is");
  check(gain.setAccessibleValue(0.25) && std::fabs(gain.value() - 0.25f) < 1e-6f && starts == 3 && ends == 3,
        "a value set outright lands, bracketed once");
  check(!gain.setAccessibleValue(std::nan("")) && gain.value() == 0.25f, "NaN is not a position");
  check(!gain.performAccessibleAction(AccessibleAction::Press), "press does nothing to a slider, and says so");
  Point menuAt{-1.0f, -1.0f}, menuRoot{-1.0f, -1.0f};
  bool right = false;
  gain.onContextMenu = [&](const MouseEvent& e) {
    menuAt = e.position;
    menuRoot = e.rootPosition;
    right = e.rightButton;
  };
  check(gain.accessibleInfo().offers(AccessibleAction::ShowMenu), "with a menu supplied, show-menu is offered");
  check(gain.performAccessibleAction(AccessibleAction::ShowMenu) && menuAt.x == 30.0f && menuAt.y == 30.0f &&
            menuRoot.x == 130.0f && menuRoot.y == 80.0f && right,
        "and opens it as a right click in the control's centre would (30,30 local, 130,80 in the root)");
  gain.setEnabled(false);
  check(gain.accessibleInfo().actions == 0 && !gain.accessibleInfo().valueSettable,
        "a disabled slider offers nothing");
  check(!gain.performAccessibleAction(AccessibleAction::Increment) && !gain.setAccessibleValue(0.9) &&
            gain.value() == 0.25f,
        "and does nothing when asked anyway");
  gain.setEnabled(true);
  (void) changes;

  Button push("Go");
  int clicks = 0;
  push.onClick = [&] { ++clicks; };
  check(push.accessibleInfo().offers(AccessibleAction::Press) &&
            push.accessibleInfo().toggle == AccessibleToggle::NotToggle,
        "a button offers press and has no state");
  check(push.performAccessibleAction(AccessibleAction::Press) && clicks == 1, "press clicks it once");
  Button bypass("Bypass");
  bypass.setToggleable(true);
  check(bypass.accessibleInfo().toggle == AccessibleToggle::Off, "a toggle reads off");
  check(bypass.performAccessibleAction(AccessibleAction::Press) && bypass.isToggled() &&
            bypass.accessibleInfo().toggle == AccessibleToggle::On,
        "press toggles it, and it reads on");

  ComboBox shape;
  shape.setItems({"Sine", "Saw", "Square"});
  shape.setSelectedIndex(0, false);
  check(shape.accessibleInfo().offers(AccessibleAction::Press) && shape.accessibleInfo().valueSettable,
        "a choice offers press and a settable value");
  check(shape.performAccessibleAction(AccessibleAction::Press) && shape.selectedIndex() == 1,
        "press with no menu supplied cycles, as a click does");
  int menus = 0;
  shape.onOpenMenu = [&](ComboBox&) { ++menus; };
  check(shape.performAccessibleAction(AccessibleAction::Press) && menus == 1 && shape.selectedIndex() == 1,
        "press with a menu supplied opens it, as a click does");
  check(shape.performAccessibleAction(AccessibleAction::Increment) && shape.selectedIndex() == 2,
        "increment is the next item");
  check(shape.performAccessibleAction(AccessibleAction::Decrement) && shape.selectedIndex() == 1,
        "decrement the previous");
  check(shape.setAccessibleValue(2.4) && shape.selectedIndex() == 2, "a value 2.4 picks item 2");
  check(shape.setAccessibleText("Sine") && shape.selectedIndex() == 0, "a NAME picks that item");
  check(!shape.setAccessibleText("Triangle") && shape.selectedIndex() == 0, "an unknown name changes nothing");

  TextEditor number;
  number.setFont(font);
  number.setBounds({0.0f, 0.0f, 200.0f, 30.0f});
  number.setAllowedCharacters("0123456789.");
  number.setMaxLength(5);
  std::string reported;
  number.onTextChange = [&](const std::string& t) { reported = t; };
  check(number.accessibleInfo().valueSettable, "a text field's value is settable");
  // The call on its own line: the message reads the result, and the order a
  // compiler evaluates arguments in is unspecified.
  const bool typed = number.setAccessibleText("12ab3.45678");
  check(typed && number.getText() == "123.4" && reported == "123.4",
        "typed by a reader, through the field's filter and length limit: \"" + number.getText() + "\"");
  number.setReadOnly(true);
  check(!number.accessibleInfo().valueSettable && !number.setAccessibleText("9") && number.getText() == "123.4",
        "a read-only field refuses");

  TextArea notes;
  notes.setFont(font);
  notes.setBounds({0.0f, 0.0f, 200.0f, 100.0f});
  check(notes.setAccessibleText("one\r\ntwo\rthree") && notes.getText() == "one\ntwo\nthree",
        "a text area takes line breaks as LF");
  root.removeChild(&gain);
}

// ── The Mac bridge's bookkeeping ───────────────────────────────────────────

AccessibleNode node(Component* c, int parent, AccessibleRole role, float x = 0.0f) {
  AccessibleNode n;
  n.component = c;
  n.parent = parent;
  n.info.role = role;
  n.info.bounds = Rect(x, 0.0f, 10.0f, 10.0f);
  return n;
}

void testBookkeeping() {
  std::printf("\n-- which element is which across a rebuild --\n");
  Component a, b, c, d;
  std::vector<AccessibleNode> before = {node(&a, -1, AccessibleRole::Group), node(&b, 0, AccessibleRole::Slider),
                                        node(&c, 0, AccessibleRole::Button)};
  std::vector<AccessibleNode> after = before;
  std::vector<int> m = detail::matchAccessibleNodes(before, after);
  check(m == std::vector<int>({0, 1, 2}), "nothing changed: every element kept in place");
  after = {node(&a, -1, AccessibleRole::Group), node(&d, 0, AccessibleRole::Label),
           node(&b, 0, AccessibleRole::Slider), node(&c, 0, AccessibleRole::Button)};
  m = detail::matchAccessibleNodes(before, after);
  check(m == std::vector<int>({0, -1, 1, 2}), "a label inserted before the slider: the slider keeps its element");
  after = {node(&a, -1, AccessibleRole::Group), node(&c, 0, AccessibleRole::Button)};
  m = detail::matchAccessibleNodes(before, after);
  check(m == std::vector<int>({0, 2}), "the slider removed: the button keeps its element");
  after = {node(&a, -1, AccessibleRole::Group), node(&b, 0, AccessibleRole::Slider),
           node(&c, 0, AccessibleRole::ToggleButton)};
  m = detail::matchAccessibleNodes(before, after);
  check(m == std::vector<int>({0, 1, -1}), "a button that became a toggle is a new element");

  std::printf("\n-- what is announced, and how often --\n");
  detail::AccessibilityAnnouncer announcer;
  std::vector<AccessibleNode> nodes = before;
  auto kinds = [](const std::vector<detail::AccessibilityEvent>& events) {
    std::string out;
    for (const detail::AccessibilityEvent& e : events) {
      out += e.kind == detail::AccessibilityEvent::Kind::FocusChanged
                 ? "focus"
                 : (e.kind == detail::AccessibilityEvent::Kind::ValueChanged ? "value" : "layout");
      out += "@" + std::to_string(e.index) + " ";
    }
    return out;
  };
  check(announcer.update(nodes, 1000).empty(), "the first look is a baseline, not news");
  nodes[1].info.focused = true;
  nodes[1].info.value = "-6.0 dB";
  check(kinds(announcer.update(nodes, 1010)) == "focus@1 ", "focus moving to the slider is announced");
  nodes[1].info.value = "-5.0 dB";
  check(announcer.update(nodes, 1100).empty(), "a value change 90 ms later waits");
  nodes[1].info.value = "-4.0 dB";
  check(announcer.update(nodes, 1200).empty(), "and another, still inside the quarter second");
  check(kinds(announcer.update(nodes, 1300)) == "value@1 ", "then the LAST value is said, once");
  check(announcer.update(nodes, 1600).empty(), "and nothing more while nothing changes");
  nodes[1].info.name = "Gain (renamed)";
  check(announcer.update(nodes, 1700).empty(), "a renamed control is still the same control: no focus event");
  nodes[2].info.bounds.x = 40.0f;
  check(kinds(announcer.update(nodes, 1750)) == "layout@-1 ", "a control moving is a layout change");
  nodes[2].info.bounds.x = 41.0f;
  check(announcer.update(nodes, 1800).empty(), "another within a quarter second waits");
  check(kinds(announcer.update(nodes, 2000)) == "layout@-1 ", "then is said");
  nodes[1].info.focused = false;
  check(announcer.update(nodes, 2100).empty(), "focus leaving the editor says nothing");
  nodes[1].info.focused = true;
  check(kinds(announcer.update(nodes, 2200)) == "focus@1 ", "and coming back to the same control is news again");

  std::printf("\n-- the y-up screen --\n");
  // A view whose bounds sit on the screen at x 100, from y 200 (its bottom)
  // to 500 (its top). An element 20 points from the view's TOP is 50 points
  // below 500 at its bottom edge.
  const Rect view(100.0f, 200.0f, 400.0f, 300.0f);
  const Rect onScreen = detail::flippedViewRectToScreen(view, Rect(10.0f, 20.0f, 50.0f, 30.0f));
  check(onScreen.x == 110.0f && onScreen.y == 450.0f && onScreen.w == 50.0f && onScreen.h == 30.0f,
        "a rectangle at (10,20) in the flipped view is at (110,450) on the screen");
  const Point back = detail::screenPointToFlippedView(view, {110.0f, 480.0f});
  check(back.x == 10.0f && back.y == 20.0f, "and its top-left corner on the screen maps back to (10,20)");
}

// ── The composition model ──────────────────────────────────────────────────

void testComposition(const Font& font) {
  std::printf("\n-- composing in a TextEditor (NSTextInputClient's arithmetic) --\n");
  Widget root;
  root.setFont(font);
  root.setBounds({0.0f, 0.0f, 400.0f, 200.0f});
  Component panel;
  panel.setBounds({50.0f, 40.0f, 320.0f, 100.0f});
  root.addChild(&panel);
  TextEditor field;
  field.setBounds({10.0f, 10.0f, 300.0f, 30.0f});
  panel.addChild(&field);
  MouseRouter router(root);
  router.setFocus(&field);
  int changes = 0;
  field.onTextChange = [&](const std::string&) { ++changes; };

  check(focusedTextInputTarget(&router) == &field, "the focused field is the input target");
  field.setText("abcd");
  field.setInputSelection(2, 2);
  Rect before;
  focusedCaretInRoot(&router, &before);
  Rect local;
  field.caretBounds(&local);
  check(before.x == local.x + 60.0f && before.y == local.y + 50.0f,
        "the caret in the root is the field's caret plus both offsets (50+10, 40+10)");

  check(TextInputSession::setMarkedText(field, "にほん", TextRange16{3, 0}, TextRange16{}),
        "marked text is accepted");
  check(field.composition().isActive() && composed(field) == "にほん", "and shown as a composition");
  check(field.getText() == "abcd" && changes == 0, "without becoming the value, or reporting a change");
  check(shown(field) == "abにほんcd", "shown at the caret: \"" + shown(field) + "\"");
  check(rangeIs(TextInputSession::markedRange(field), 2, 3), "markedRange {2, 3}");
  check(rangeIs(TextInputSession::selectedRange(field), 5, 0), "selectedRange {5, 0}: its caret, after it");
  Rect during;
  focusedCaretInRoot(&router, &during);
  if (g_measured) check(during.x > before.x, "the caret the candidate list follows moved into the composition");
  TextRange16 actual;
  check(TextInputSession::substring(field, TextRange16{2, 3}, &actual) == "にほん" && rangeIs(actual, 2, 3),
        "the substring reads the composition back");

  TextInputSession::setMarkedText(field, "日本", TextRange16{0, 2}, TextRange16{});
  check(composed(field) == "日本" && field.composition().clauseStart == 0 && field.composition().clauseEnd == 2,
        "a new marked string replaces the old, with its clause");
  check(rangeIs(TextInputSession::selectedRange(field), 2, 2), "and selectedRange is the clause {2, 2}");

  check(TextInputSession::insertText(field, "日本", TextRange16{}), "insertText commits");
  check(field.getText() == "ab日本cd" && !field.composition().isActive() && changes == 1,
        "replacing the composition: \"" + field.getText() + "\", one change");
  check(field.caretPosition() == 4, "the caret after what was committed");

  field.setText("hello");
  changes = 0;
  field.setInputSelection(1, 4);
  TextInputSession::setMarkedText(field, "x", TextRange16{1, 0}, TextRange16{});
  check(field.getText() == "ho" && changes == 1 && shown(field) == "hxo",
        "composing over a selection deletes it at once: \"" + shown(field) + "\"");
  TextInputSession::setMarkedText(field, "", TextRange16{0, 0}, TextRange16{});
  check(!field.composition().isActive() && field.getText() == "ho", "empty marked text ends it, typing nothing");
  TextInputSession::setMarkedText(field, "y", TextRange16{1, 0}, TextRange16{});
  TextInputSession::unmarkText(field);
  check(field.getText() == "hyo" && !field.composition().isActive(), "unmarkText commits what is there");

  std::printf("\n-- UTF-16, as NSString counts --\n");
  field.setText("a\xF0\x9F\x98\x80" "b"); // a, U+1F600, b
  check(rangeIs(TextInputSession::selectedRange(field), 4, 0), "a, an emoji and b are four UTF-16 units");
  check(TextInputSession::substring(field, TextRange16{1, 2}, &actual) == "\xF0\x9F\x98\x80" && rangeIs(actual, 1, 2),
        "the emoji is units 1..3");
  const std::string inside = TextInputSession::substring(field, TextRange16{2, 1}, &actual);
  check(inside == "\xF0\x9F\x98\x80" && rangeIs(actual, 1, 2),
        "a range starting inside the pair widens to the whole emoji ({1, 2}), never half of it");
  const std::string half = TextInputSession::substring(field, TextRange16{1, 1}, &actual);
  check(half.empty() && rangeIs(actual, 1, 0), "one unit of the pair rounds down to nothing, not to a lone surrogate");
  TextInputSession::setMarkedText(field, "\xC3\xA9", TextRange16{1, 0}, TextRange16{});
  check(rangeIs(TextInputSession::markedRange(field), 4, 1), "marked after it: {4, 1}");
  TextInputSession::insertText(field, "e", TextRange16{1, 2});
  check(field.getText() == "aeb" && !field.composition().isActive(),
        "a replacement range in the shown text replaces the emoji and drops the composition: \"" + field.getText() + "\"");

  field.setText("abc");
  TextInputSession::setMarkedText(field, "X", TextRange16{1, 0}, TextRange16{1, 1});
  check(field.getText() == "ac" && shown(field) == "aXc", "setMarkedText with a replacement range replaces it");
  TextInputSession::insertText(field, "B", TextRange16{1, 1});
  check(field.getText() == "aBc", "insertText over the composition's own range commits in its place");

  std::printf("\n-- where it is drawn --\n");
  if (!g_measured) std::printf("  skip no typeface here: the rectangle checks need glyph widths\n");
  if (g_measured) {
    field.setText("");
    TextInputSession::setMarkedText(field, "kana", TextRange16{4, 0}, TextRange16{});
    Rect r0, r2;
    const bool first = TextInputSession::firstRect(field, TextRange16{0, 1}, &r0, &actual);
    check(first && rangeIs(actual, 0, 1) && r0.w > 0.0f, "firstRect of the first composed character is a real box");
    TextInputSession::firstRect(field, TextRange16{0, 4}, &r2, &actual);
    check(rangeIs(actual, 0, 4) && r2.w > r0.w && r2.x == r0.x, "of all four, one wider box from the same start");
    Rect caret;
    TextInputSession::firstRect(field, TextRange16{4, 0}, &caret, &actual);
    check(caret.w == 0.0f && caret.x >= r2.right() - 0.5f, "an empty range is the caret, after the text");
    const size_t at = TextInputSession::characterIndexAt(field, {r0.x + r0.w * 0.9f, r0.y + 2.0f});
    check(at == 1, "characterIndexForPoint near the first character's right edge is 1: " + std::to_string(at));
    Bitmap bitmap(400, 200);
    Graphics g(bitmap);
    root.paintTree(g);
    check(true, "a field with a composition paints");
  }

  std::printf("\n-- the field's rules apply on commit, and interruptions commit --\n");
  field.setText("");
  field.setAllowedCharacters("0123456789");
  field.setMaxLength(3);
  TextInputSession::insertText(field, "12ab345", TextRange16{});
  check(field.getText() == "123", "committed text meets the filter and the length limit: \"" + field.getText() + "\"");
  field.setAllowedCharacters("");
  field.setMaxLength(0);
  field.setReadOnly(true);
  check(!TextInputSession::setMarkedText(field, "x", TextRange16{1, 0}, TextRange16{}) &&
            !TextInputSession::insertText(field, "x", TextRange16{}) && field.getText() == "123",
        "a read-only field takes neither");
  field.setReadOnly(false);

  TextEditor other;
  other.setBounds({10.0f, 50.0f, 300.0f, 30.0f});
  panel.addChild(&other);
  std::string lost;
  field.onFocusLost = [&](const std::string& t) { lost = t; };
  field.setText("");
  TextInputSession::setMarkedText(field, "abc", TextRange16{3, 0}, TextRange16{});
  router.setFocus(&other);
  check(field.getText() == "abc" && lost == "abc" && !field.composition().isActive(),
        "losing the focus commits the composition before the value is reported");
  router.setFocus(&field);
  TextInputSession::setMarkedText(field, "de", TextRange16{2, 0}, TextRange16{});
  MouseEvent click;
  field.mouseDown(click);
  check(field.getText() == "abcde" && !field.composition().isActive(), "a click in the field commits it first");
  TextInputSession::setMarkedText(field, "zz", TextRange16{2, 0}, TextRange16{});
  field.setText("new");
  check(field.getText() == "new" && !field.composition().isActive(), "setText drops a composition");
  TextInputSession::setMarkedText(field, "q", TextRange16{1, 0}, TextRange16{});
  KeyPress left;
  left.keyCode = KeyPress::Left;
  field.keyPressed(left);
  check(field.getText() == "newq" && field.caretPosition() == 3, "a key that reaches the field commits, then acts");
  panel.removeChild(&other);

  std::printf("\n-- XIM's preedit, in code points --\n");
  field.setText("[]");
  field.setInputSelection(1, 1);
  TextInputSession::preeditStart(field);
  std::vector<uint32_t> add = cps("にほ");
  TextInputSession::preeditDraw(field, 0, 0, &add, 2);
  check(composed(field) == "にほ" && field.composition().caret == 2, "draw inserts at 0");
  add = cps("ん");
  TextInputSession::preeditDraw(field, 2, 0, &add, 3);
  check(composed(field) == "にほん" && shown(field) == "[にほん]", "draw appends: \"" + shown(field) + "\"");
  add = cps("日本");
  TextInputSession::preeditDraw(field, 0, 3, &add, 2);
  check(composed(field) == "日本", "draw replaces all three with two");
  TextInputSession::preeditDraw(field, 1, 1, nullptr, 1);
  check(composed(field) == "日" && field.composition().caret == 1, "a draw with no text deletes");
  add = cps("abc");
  TextInputSession::preeditDraw(field, 99, 99, &add, 99);
  check(composed(field) == "日abc" && field.composition().caret == 4, "positions from another process are clamped");
  check(TextInputSession::preeditCaret(field, TextInputSession::CaretMove::BackwardChar, 0) == 3 &&
            TextInputSession::preeditCaret(field, TextInputSession::CaretMove::LineStart, 0) == 0 &&
            TextInputSession::preeditCaret(field, TextInputSession::CaretMove::ForwardChar, 0) == 1 &&
            TextInputSession::preeditCaret(field, TextInputSession::CaretMove::LineEnd, 0) == 4 &&
            TextInputSession::preeditCaret(field, TextInputSession::CaretMove::Absolute, 2) == 2 &&
            TextInputSession::preeditCaret(field, TextInputSession::CaretMove::Absolute, 40) == 4 &&
            TextInputSession::preeditCaret(field, TextInputSession::CaretMove::Up, 0) == 4,
        "the caret moves as XIMCaretDirection says, clamped");
  TextInputSession::setClause(field, 1, 3);
  check(rangeIs(TextInputSession::selectedRange(field), 2, 2), "a highlighted clause becomes the selected range");
  TextInputSession::insertText(field, "本", TextRange16{});
  check(field.getText() == "[本]" && !field.composition().isActive(),
        "committed text replaces the preedit, whichever arrives first: \"" + field.getText() + "\"");
  TextInputSession::preeditDraw(field, 0, 0, &add, 3);
  TextInputSession::preeditDone(field);
  check(field.getText() == "[本]" && !field.composition().isActive(), "done ends a preedit without typing it");
  add = cps("a\x01" "b");
  TextInputSession::preeditDraw(field, 0, 0, &add, 2);
  check(composed(field) == "ab", "control characters never enter a composition");
  field.clearComposition();

  std::printf("\n-- composing in a TextArea, across lines --\n");
  TextArea notes;
  notes.setBounds({10.0f, 10.0f, 300.0f, 90.0f});
  panel.addChild(&notes);
  router.setFocus(&notes);
  check(focusedTextInputTarget(&router) == &notes, "a text area is an input target too");
  notes.setText("line one\nline two");
  TextInputSession::setMarkedText(notes, "xy", TextRange16{2, 0}, TextRange16{});
  check(rangeIs(TextInputSession::markedRange(notes), 17, 2) && notes.getText() == "line one\nline two",
        "marked at the end of line two: {17, 2}, the value untouched");
  Rect firstLine, secondLine;
  TextInputSession::firstRect(notes, TextRange16{0, 1}, &firstLine, &actual);
  TextInputSession::firstRect(notes, TextRange16{17, 1}, &secondLine, &actual);
  check(secondLine.y > firstLine.y + 1.0f && rangeIs(actual, 17, 1), "the composition's rectangle is on the second line");
  TextInputSession::firstRect(notes, TextRange16{5, 10}, &firstLine, &actual);
  check(rangeIs(actual, 5, 4), "a range running onto the next line stops at the line's end (actualRange {5, 4})");
  Rect areaCaret;
  focusedCaretInRoot(&router, &areaCaret);
  check(areaCaret.y > firstLine.y + 50.0f - 1.0f, "the caret in the root is on line two");
  TextInputSession::insertText(notes, "z", TextRange16{});
  check(notes.getText() == "line one\nline twoz", "insertText commits there");
  panel.removeChild(&notes);
  panel.removeChild(&field);
  root.removeChild(&panel);
}

// ── Bluetooth MIDI ─────────────────────────────────────────────────────────

void testBluetooth() {
  std::printf("\n-- the Bluetooth MIDI pairing dialogue --\n");
  check(std::strstr(sonore::detail::bluetoothMidiLinuxReason(), "BlueZ") != nullptr,
        "the Linux reason names BlueZ and what to do instead");
  using Dialogue = sonore::BluetoothMidiDevicePairingDialogue;
#if defined(_WIN32)
  _putenv_s("SONORE_BLUETOOTH_MIDI_NO_LAUNCH", "0");
  check(!sonore::detail::bluetoothMidiLaunchDisabled(), "\"0\" does not disable the launch");
  _putenv_s("SONORE_BLUETOOTH_MIDI_NO_LAUNCH", "1");
  check(sonore::detail::bluetoothMidiLaunchDisabled(), "\"1\" does");
  const bool registered = sonore::detail::bluetoothMidiSettingsRegistered();
  std::printf("       ms-settings: registered here: %s\n", registered ? "yes" : "no");
  check(Dialogue::isAvailable() == registered && Dialogue::unavailableReason().empty() == registered,
        "available exactly when the Settings protocol is registered");
  int closed = 0;
  bool result = false;
  const bool opened = Dialogue::open([&](bool ok) {
    ++closed;
    result = ok;
  });
  check(opened == registered && closed == (registered ? 1 : 0) && (!registered || result),
        "open() (launch disabled) succeeds and reports closing once");
  check(std::strstr(Dialogue::description(), "WinMM") != nullptr,
        "the description says MidiInput (WinMM) does not see BLE MIDI ports");
#elif defined(__linux__)
  check(!Dialogue::isAvailable() && Dialogue::unavailableReason() == sonore::detail::bluetoothMidiLinuxReason(),
        "unavailable on Linux, with the reason");
  bool called = false;
  check(!Dialogue::open([&](bool) { called = true; }) && !called, "open() refuses and calls nothing");
#else
  std::printf("       available here: %s %s\n", Dialogue::isAvailable() ? "yes" : "no",
              Dialogue::unavailableReason().c_str());
#endif
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("MAC/X11 INPUT TESTS (the platform-neutral half, run)\n");
  auto face = loadAnyTypeface();
  g_measured = face != nullptr;
  std::printf("  %s a typeface to lay text out with\n", g_measured ? "ok  " : "skip");
  const Font font = face ? Font(face, 14.0f) : Font();
  testRoleTable();
  testActions(font);
  testBookkeeping();
  testComposition(font);
  testBluetooth();
  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  std::printf(g_failures ? "MAC INPUT TESTS FAILED\n" : "MAC INPUT TESTS PASSED\n");
  return g_failures ? 1 : 0;
}
