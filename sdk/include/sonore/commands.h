// SPDX-License-Identifier: Apache-2.0
//
// Named actions, and the keys that reach them.
//
// NOT part of the generated-interface protocol. A page the studio writes never
// calls these -- the protocol does not know them, and the review gate would
// reject a page that did. They exist for interfaces written by hand against
// this SDK, and for the fallback panel.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// Undo. Save preset. Next preset. Bypass. A plugin has a handful of things it
// can be asked to do that are not a parameter, and without something like this
// each one is a lambda hanging off a button, invisible to the keyboard and
// impossible to rebind.
//
// The point of a COMMAND rather than a callback is that the action has a name,
// an identity and a state -- so one thing can be reached from a button, a menu
// item and a shortcut and be greyed out in all three at once, and a user who
// wants Ctrl+S somewhere else can move it.
//
// ── This could not have existed until a week ago ───────────────────────────
//
// Ctrl+A reached WM_CHAR as 0x01 and no shortcut in this SDK worked at all. A
// command manager on top of that would have been a table nothing ever
// consulted -- present, documented, and dead. The peers deliver the letter with
// the modifier now, and this is what that fix was for.
//
// ── The rules that matter ──────────────────────────────────────────────────
//
// A shortcut belongs to ONE command. Assigning it to a second takes it from the
// first, because two commands firing on one key is worse than either.
//
// A disabled command does not fire and does NOT consume the key. That is the
// less obvious half: a swallowed key that does nothing is indistinguishable
// from a broken keyboard, where an unconsumed one falls through to whatever
// else wanted it.
//
// The component tree gets the key FIRST. A text field must have Ctrl+A for
// select-all rather than a global command stealing it out from under the
// caret -- which is why this is consulted only for keys nothing else took.
#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "gfx/component.h"
#include "json.h"

namespace sonore {

/**
 * A key combination.
 *
 * Either a CHARACTER -- lowercase, which is what all three peers deliver for a
 * modifier combination -- or a named key code. Not both: a shortcut is one key
 * and describing it twice invites the two halves to disagree.
 */
struct Shortcut {
  /** The key as a Unicode code point, ASCII letters lowercase; 0 when this
   *  is a key-code shortcut. */
  uint32_t character = 0;
  /** A gfx::KeyPress code (Return, F5, Left...); None when this is a
   *  character shortcut. */
  int keyCode = gfx::KeyPress::None;
  ///@{
  /** The modifiers that must be down -- and the ones not set must be up:
   *  Ctrl+S does not fire on Ctrl+Alt+S. */
  bool shift = false, ctrl = false, alt = false;
  ///@}

  /** A character shortcut, Ctrl+`c` unless told otherwise. An ASCII capital
   *  is lowercased, so 'S' and 's' make the same shortcut; Shift is only
   *  what `withShift` says. */
  static Shortcut fromCharacter(uint32_t c, bool withCtrl = true, bool withShift = false,
                                bool withAlt = false) {
    Shortcut out;
    // Lowercased on the way IN as well as out of the peers, so a table written
    // with 'S' matches a peer that delivers 's'.
    out.character = (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
    out.ctrl = withCtrl;
    out.shift = withShift;
    out.alt = withAlt;
    return out;
  }

  /** A named key, with no modifier unless told otherwise. */
  static Shortcut fromKeyCode(int code, bool withCtrl = false, bool withShift = false,
                              bool withAlt = false) {
    Shortcut out;
    out.keyCode = code;
    out.ctrl = withCtrl;
    out.shift = withShift;
    out.alt = withAlt;
    return out;
  }

  /** False for the default-constructed shortcut, which names no key. */
  bool isValid() const { return character != 0 || keyCode != gfx::KeyPress::None; }

  /** Same key and same modifiers. */
  bool operator==(const Shortcut& other) const {
    return character == other.character && keyCode == other.keyCode && shift == other.shift &&
           ctrl == other.ctrl && alt == other.alt;
  }
  /** Not operator==. */
  bool operator!=(const Shortcut& other) const { return !(*this == other); }

  /** Whether a key press is this shortcut: the same key, case ignored for
   *  ASCII letters, and exactly the same Shift, Ctrl and Alt. */
  bool matches(const gfx::KeyPress& key) const {
    if (ctrl != key.ctrlDown || alt != key.altDown) return false;
    if (keyCode != gfx::KeyPress::None) return key.keyCode == keyCode && shift == key.shiftDown;
    if (character == 0) return false;
    // Shift is compared for a character too, so Ctrl+Z and Ctrl+Shift+Z are
    // different shortcuts -- which is exactly what undo and redo need.
    const uint32_t typed = (key.character >= 'A' && key.character <= 'Z')
                               ? key.character - 'A' + 'a'
                               : key.character;
    return typed == character && shift == key.shiftDown;
  }

  /** "Ctrl+Shift+S". For a menu item, a tooltip, or a mapping editor -- a
   *  shortcut nobody can see is one nobody uses. */
  std::string describe() const {
    std::string out;
    if (ctrl) out += "Ctrl+";
    if (alt) out += "Alt+";
    if (shift) out += "Shift+";
    if (keyCode != gfx::KeyPress::None) {
      switch (keyCode) {
        case gfx::KeyPress::Backspace: out += "Backspace"; break;
        case gfx::KeyPress::Tab: out += "Tab"; break;
        case gfx::KeyPress::Return: out += "Return"; break;
        case gfx::KeyPress::Escape: out += "Escape"; break;
        case gfx::KeyPress::Delete: out += "Delete"; break;
        case gfx::KeyPress::Left: out += "Left"; break;
        case gfx::KeyPress::Right: out += "Right"; break;
        case gfx::KeyPress::Up: out += "Up"; break;
        case gfx::KeyPress::Down: out += "Down"; break;
        case gfx::KeyPress::Home: out += "Home"; break;
        case gfx::KeyPress::End: out += "End"; break;
        case gfx::KeyPress::PageUp: out += "PageUp"; break;
        case gfx::KeyPress::PageDown: out += "PageDown"; break;
        default:
          if (keyCode >= gfx::KeyPress::F1 && keyCode <= gfx::KeyPress::F12)
            out += "F" + std::to_string(keyCode - gfx::KeyPress::F1 + 1);
          else
            out += "Key";
          break;
      }
    } else if (character != 0) {
      // Shown in UPPER case, which is how every application prints a shortcut
      // even though the key itself is lowercase.
      const uint32_t shown =
          (character >= 'a' && character <= 'z') ? character - 'a' + 'A' : character;
      out += (char) (shown < 128 ? shown : '?');
    }
    return out;
  }
};

/** What a command is called and where it is listed. `id` is the plugin's own
 *  number for it, non-zero (registerCommand ignores 0, which is also what
 *  "no command" means elsewhere), and it is what saved mappings refer to --
 *  so it must stay the same from one version to the next. */
struct CommandInfo {
  /** The command's number: non-zero, stable across versions. */
  int id = 0;
  /** What a menu item says. */
  std::string name;
  /** For grouping in a mapping editor -- "Presets", "Edit". Braced, like
   *  description, so {id, name} and {id, name, category} are complete
   *  initialisers to gcc and clang's -Wextra, not missing fields. */
  std::string category{};
  /** A sentence for a tooltip or a mapping editor. */
  std::string description{};
};

/**
 * The plugin's commands, their enabled and ticked state, and the shortcuts
 * that reach them -- the part the reference's application command manager
 * plays, with one onInvoke handler in place of command targets. See the file
 * comment for the rules about shortcuts and disabled commands.
 *
 * UI THREAD, all of it: keyPressed() is fed the keys the component tree did
 * not take (MouseRouter::onUnhandledKey), and onInvoke runs on whichever
 * thread called invoke() or keyPressed(). Commands are registered in code
 * each time the plugin loads; only the user's mappings are saved
 * (saveMappings / loadMappings). A pointer from commandInfo() lasts until the
 * next registerCommand of a new id.
 */
class CommandManager {
public:
  /** `defaults` are what resetToDefaults() restores, and are applied now unless
   *  the key is already taken by something registered earlier. Registering an
   *  id again updates its description and its defaults and leaves its current
   *  shortcuts alone: those may be the user's. */
  void registerCommand(CommandInfo info, std::vector<Shortcut> defaults = {}) {
    if (info.id == 0) return;
    if (Entry* existing = find(info.id)) {
      existing->info = std::move(info);
      existing->defaults = std::move(defaults);
      return;
    }
    Entry entry;
    entry.info = std::move(info);
    entry.defaults = std::move(defaults);
    entries_.push_back(std::move(entry));
    const Entry& added = entries_.back();
    const int id = added.info.id;
    const std::vector<Shortcut> wanted = added.defaults;
    for (const Shortcut& s : wanted) applyDefault(id, s);
  }

  /** What `id` was registered with, or null for an unknown id. See the class
   *  comment for how long the pointer lasts. */
  const CommandInfo* commandInfo(int id) const {
    const Entry* entry = find(id);
    return entry ? &entry->info : nullptr;
  }

  /** Every registered id, in the order they were registered. */
  std::vector<int> commandIds() const {
    std::vector<int> out;
    out.reserve(entries_.size());
    for (const Entry& e : entries_) out.push_back(e.info.id);
    return out;
  }

  /** How many commands are registered. */
  int numCommands() const { return (int) entries_.size(); }

  // ── State ────────────────────────────────────────────────────────────────

  /** Greys a command out, or back in. A disabled command does not run and
   *  does not take its keys. Commands start enabled; an unknown id does
   *  nothing. */
  void setEnabled(int id, bool enabled) {
    if (Entry* entry = find(id)) entry->enabled = enabled;
  }

  /** False for a disabled command, and for an unknown id. */
  bool isEnabled(int id) const {
    const Entry* entry = find(id);
    return entry && entry->enabled;
  }

  /** A checkmark, for something that is on or off -- bypass, a view toggle. */
  void setTicked(int id, bool ticked) {
    if (Entry* entry = find(id)) entry->ticked = ticked;
  }

  /** The checkmark set by setTicked(); false for an unknown id. Only shown,
   *  never acted on here. */
  bool isTicked(int id) const {
    const Entry* entry = find(id);
    return entry && entry->ticked;
  }

  // ── Doing them ───────────────────────────────────────────────────────────

  /** Where a command ends up. One handler rather than one per command, so a
   *  caller can log or undo-wrap every action in a single place. */
  std::function<void(int id)> onInvoke;

  /** False for an unknown or disabled command, so a caller driving this from a
   *  menu can tell the difference between "did it" and "could not". */
  bool invoke(int id) {
    const Entry* entry = find(id);
    if (!entry || !entry->enabled) return false;
    if (onInvoke) onInvoke(id);
    return true;
  }

  /**
   * Offer a key to the commands. True if one took it.
   *
   * Consult this only for keys the component tree did NOT take: a text field
   * must have Ctrl+A for select-all rather than a global command stealing it
   * out from under the caret.
   */
  bool keyPressed(const gfx::KeyPress& key) {
    for (const Entry& entry : entries_) {
      for (const Shortcut& s : entry.shortcuts) {
        if (!s.matches(key)) continue;
        // A DISABLED command does not consume the key. A swallowed key that
        // does nothing is indistinguishable from a broken keyboard; an
        // unconsumed one falls through to whatever else wanted it.
        if (!entry.enabled) return false;
        if (onInvoke) onInvoke(entry.info.id);
        return true;
      }
    }
    return false;
  }

  // ── Mappings ─────────────────────────────────────────────────────────────

  /**
   * Give a shortcut to a command, taking it from whoever had it.
   *
   * The taking is the point. Two commands on one key is worse than either
   * being unbound, and a mapping editor that let it happen would produce a
   * configuration the user cannot reason about.
   */
  void addShortcut(int id, const Shortcut& shortcut) {
    if (!shortcut.isValid()) return;
    Entry* target = find(id);
    if (!target) return;
    for (Entry& entry : entries_)
      entry.shortcuts.erase(
          std::remove(entry.shortcuts.begin(), entry.shortcuts.end(), shortcut),
          entry.shortcuts.end());
    // Re-found: erasing above cannot move entries_, but taking the pointer
    // before a loop that edits every entry is the habit that eventually does.
    target = find(id);
    if (target) target->shortcuts.push_back(shortcut);
  }

  /** A DEFAULT takes a key only if nobody has it: the first command
   *  registered with it keeps it. addShortcut() is the user's choice and
   *  moves a key; this is the plugin's suggestion and does not. A later
   *  command defaulting to Ctrl+Z took it from Undo. */
  void applyDefault(int id, const Shortcut& shortcut) {
    const int owner = commandForShortcut(shortcut);
    if (owner == 0) addShortcut(id, shortcut);
  }

  /** Takes one shortcut from a command, leaving the key unbound. */
  void removeShortcut(int id, const Shortcut& shortcut) {
    if (Entry* entry = find(id))
      entry->shortcuts.erase(
          std::remove(entry->shortcuts.begin(), entry->shortcuts.end(), shortcut),
          entry->shortcuts.end());
  }

  /** Takes every shortcut from a command. Its defaults are kept for
   *  resetToDefaults(). */
  void clearShortcuts(int id) {
    if (Entry* entry = find(id)) entry->shortcuts.clear();
  }

  /** A command's shortcuts now, in the order they were given; empty for an
   *  unknown id. */
  std::vector<Shortcut> shortcutsFor(int id) const {
    const Entry* entry = find(id);
    return entry ? entry->shortcuts : std::vector<Shortcut>();
  }

  /** Which command owns a key, or 0. What a mapping editor shows before it
   *  lets somebody take it away. */
  int commandForShortcut(const Shortcut& shortcut) const {
    for (const Entry& entry : entries_)
      for (const Shortcut& s : entry.shortcuts)
        if (s == shortcut) return entry.info.id;
    return 0;
  }

  /** Throws away every mapping and applies each command's defaults again,
   *  in registration order: where two defaults name one key, the command
   *  registered first gets it. */
  void resetToDefaults() {
    for (Entry& entry : entries_) entry.shortcuts.clear();
    for (Entry& entry : entries_) {
      const std::vector<Shortcut> defaults = entry.defaults;
      const int id = entry.info.id;
      for (const Shortcut& s : defaults) applyDefault(id, s);
    }
  }

  // ── Remembering what the user chose ──────────────────────────────────────

  /**
   * The mappings as JSON, for a settings file.
   *
   * Only the mappings. The COMMANDS are the plugin's own and are registered in
   * code every time it loads -- writing them out would let a stale settings
   * file resurrect a command a later version removed.
   */
  std::string saveMappings() const {
    JsonValue root = JsonValue::object();
    JsonValue list = JsonValue::array();
    for (const Entry& entry : entries_) {
      for (const Shortcut& s : entry.shortcuts) {
        JsonValue item = JsonValue::object();
        item.set("command", JsonValue((double) entry.info.id));
        item.set("character", JsonValue((double) s.character));
        item.set("keyCode", JsonValue((double) s.keyCode));
        item.set("shift", JsonValue(s.shift));
        item.set("ctrl", JsonValue(s.ctrl));
        item.set("alt", JsonValue(s.alt));
        list.append(std::move(item));
      }
    }
    root.set("shortcuts", std::move(list));
    return root.toString();
  }

  /**
   * Read them back. False for text that is not this.
   *
   * A mapping naming a command that no longer exists is SKIPPED rather than
   * failing the load: a plugin that removed a command should not refuse a
   * settings file over it, which would lose every other binding the user set.
   */
  bool loadMappings(const std::string& text) {
    std::string error;
    const JsonValue root = JsonValue::parse(text, &error);
    if (!root.isObject()) return false;
    const JsonValue& list = root["shortcuts"];
    if (!list.isArray()) return false;

    for (Entry& entry : entries_) entry.shortcuts.clear();
    for (size_t i = 0; i < list.size(); ++i) {
      const JsonValue& item = list.at(i);
      if (!item.isObject()) continue;
      const int id = item["command"].asInt(0);
      if (!find(id)) continue; // a command this version no longer has
      Shortcut s;
      s.character = (uint32_t) item["character"].asInt(0);
      s.keyCode = item["keyCode"].asInt(gfx::KeyPress::None);
      s.shift = item["shift"].asBool(false);
      s.ctrl = item["ctrl"].asBool(false);
      s.alt = item["alt"].asBool(false);
      if (s.isValid()) addShortcut(id, s);
    }
    return true;
  }

private:
  /** One registered command: what it is, the shortcuts it has now (the
   *  user's, once they have changed any), the defaults resetToDefaults()
   *  goes back to, and its state. */
  struct Entry {
    CommandInfo info;
    std::vector<Shortcut> shortcuts;
    std::vector<Shortcut> defaults;
    bool enabled = true;
    bool ticked = false;
  };

  Entry* find(int id) {
    for (Entry& entry : entries_)
      if (entry.info.id == id) return &entry;
    return nullptr;
  }

  const Entry* find(int id) const {
    for (const Entry& entry : entries_)
      if (entry.info.id == id) return &entry;
    return nullptr;
  }

  std::vector<Entry> entries_;
};

} // namespace sonore
