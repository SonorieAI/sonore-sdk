// SPDX-License-Identifier: Apache-2.0
//
// Composed text: what an input method has typed before it has decided what it
// typed.
//
// ── The problem ─────────────────────────────────────────────────────────────
//
// Japanese, Chinese and Korean are not typed one key per character. The user
// types a reading -- "nihon" -- the input method shows it, offers candidates,
// and only when the user accepts one does "日本" become text. Between the two
// there is a COMPOSITION (macOS calls it marked text, X11 a preedit): text that
// is on screen, in the field, at the caret, and is not yet the field's value.
//
// Each platform hands it over differently, and all three need the same two
// answers from the field:
//
//  - WHERE the caret is, so the candidate list opens beside it. That is
//    Component::caretBounds, and focusedCaretInRoot below turns it into the
//    window's coordinates for all three peers: Windows hands the point to
//    imm32, X11 to the XIC as XNSpotLocation, macOS returns it from
//    firstRectForCharacterRange.
//
//  - WHAT is being composed, when the platform expects the APPLICATION to draw
//    it. Windows does not (the IME draws its own composition window at the
//    point above, which is the path window_win32.h takes and has tested). macOS
//    always does: a view implementing NSTextInputClient is handed marked text
//    and must show it. X11 does when the input style is on-the-spot
//    (XIMPreeditCallbacks). Those two write a TextComposition into the focused
//    field through TextInputSession, and the field paints it -- underlined, at
//    its caret, never part of its value until the input method commits it.
//
// ── Why the rules live here and not in the peers ────────────────────────────
//
// NSTextInputClient is a dozen methods over UTF-16 ranges -- a marked range, a
// selected range inside it, replacement ranges in the document -- and the
// arithmetic of those ranges against a field that stores code points is where
// an implementation goes wrong: an emoji is two UTF-16 units and one
// character, and a range that splits it puts half a surrogate into a string.
// None of that needs a Mac. TextInputSession is that arithmetic, run against
// any TextInputTarget, and tests/mac_input_test.cpp drives it on Windows with
// real TextEditor and TextArea instances. What is left in window_cocoa.h is
// converting NSString and NSRange at the edge; what is left in window_x11.h is
// unpacking XIMText.
//
// ── What the field promises ─────────────────────────────────────────────────
//
// While a composition is showing, the field's selection is collapsed at its
// start, and the field does not edit: an input method consumes the keys while
// it composes, and a click or a key that does reach the field commits the
// composition first. getText() never includes it -- a value box must not
// commit "nihon" because a reader asked for its value mid-composition.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "component.h"

namespace sonore {
namespace gfx {

/**
 * What an input method is composing in a field.
 *
 * In code points, as the fields store their text. Empty means nothing is
 * being composed.
 */
struct TextComposition {
  /** What is being composed, in code points; not part of the field's value
   *  until committed. */
  std::vector<uint32_t> text;
  /** Where the input method's caret is, inside `text`. */
  size_t caret = 0;
  /** The clause the input method is converting, inside `text` -- drawn with a
   *  heavier underline, as every platform's own text views do. Empty for
   *  none. */
  size_t clauseStart = 0, clauseEnd = 0;  ///< one past the clause's last code point

  /** Whether anything is being composed. */
  bool isActive() const { return !text.empty(); }
};

/**
 * A component an input method can compose into.
 *
 * The primitives a field already has, named so TextInputSession can reach
 * them: its committed text, its selection, a replacement that applies the
 * field's own rules, and where a character is drawn. Implemented by TextEditor
 * and TextArea; a component returns itself from Component::textInputTarget.
 *
 * Indices are code points of the committed text, except where they say SHOWN:
 * the committed text with the composition in place at the caret.
 */
class TextInputTarget {
public:
  virtual ~TextInputTarget() = default;

  /** The committed text. */
  virtual const std::vector<uint32_t>& inputText() const = 0;
  /** The selection's first code point in the committed text -- the caret
   *  itself when nothing is selected. */
  virtual size_t inputSelectionStart() const = 0;
  /** One past the selection's last code point; inputSelectionStart() again for
   *  a bare caret. */
  virtual size_t inputSelectionEnd() const = 0;
  /** Select from `start` (the anchor) to `end` (where the caret goes), in
   *  either order, each held to the text's length. */
  virtual void setInputSelection(size_t start, size_t end) = 0;

  /**
   * Replace committed characters [start, end) with `with`, as typing would:
   * the field's character filter, length limit and line-break rules apply,
   * the caret ends after what was inserted, and the field reports the change
   * as it reports a keystroke.
   *
   * False ONLY when the field will not be edited at all -- read-only. A
   * filter that refuses every character is an edit that happened (the
   * replaced range is gone), as a refused keystroke is a key consumed.
   */
  virtual bool replaceInputRange(size_t start, size_t end, const std::vector<uint32_t>& with) = 0;
  /** Whether the field refuses edits now: read-only, or disabled. */
  virtual bool isInputReadOnly() const = 0;

  /** The box the SHOWN character `index` is drawn in, in this component's
   *  own coordinates; for index == the shown length, a zero-width box where a
   *  caret after the last character stands. */
  virtual Rect inputCharacterBounds(size_t index) const = 0;
  /** The SHOWN index whose caret position is nearest `local`. */
  virtual size_t inputIndexAt(Point local) const = 0;

  /** What is being composed; inactive when nothing is. */
  const TextComposition& composition() const { return composition_; }

  /** Show `c` at the caret. The field repaints; its value does not change. */
  void setComposition(TextComposition c) {
    composition_ = std::move(c);
    if (composition_.caret > composition_.text.size()) composition_.caret = composition_.text.size();
    if (composition_.clauseEnd > composition_.text.size()) composition_.clauseEnd = composition_.text.size();
    if (composition_.clauseStart > composition_.clauseEnd) composition_.clauseStart = composition_.clauseEnd;
    compositionChanged();
  }

  /** Drop the composition WITHOUT committing it. */
  void clearComposition() {
    if (!composition_.isActive()) return;
    composition_ = TextComposition();
    compositionChanged();
  }

  /** Commit the composition as typed text, at the caret: what a field does
   *  when it loses focus or is clicked mid-composition, and what macOS's
   *  unmarkText asks for. False when there was nothing to commit. */
  bool commitComposition() {
    if (!composition_.isActive()) return false;
    std::vector<uint32_t> text = std::move(composition_.text);
    composition_ = TextComposition();
    compositionChanged();
    const size_t at = inputSelectionStart();
    replaceInputRange(at, at, text);
    return true;
  }

protected:
  /** The composition changed: repaint, and keep its caret in view. */
  virtual void compositionChanged() = 0;

private:
  TextComposition composition_;
};

/**
 * A range in UTF-16 code units of the SHOWN text, as NSTextInputClient speaks
 * them. `kNotFound` is NSNotFound (NSIntegerMax), which macOS uses for "no
 * range" -- no marked text, or "replace whatever you think best".
 */
struct TextRange16 {
  /** NSNotFound: no range. */
  static constexpr size_t kNotFound = (std::numeric_limits<size_t>::max)() / 2;
  /** The first UTF-16 code unit, or kNotFound. */
  size_t location = kNotFound;
  /** In UTF-16 code units. */
  size_t length = 0;

  /** Whether this is "no range". */
  bool isNotFound() const { return location >= kNotFound; }
  /** One past the end, without overflowing for a length that runs past
   *  everything. */
  size_t end() const { return length > kNotFound - location ? kNotFound : location + length; }
};

namespace detail {

/** UTF-8 to code points, a broken sequence as U+FFFD and the walk resumed at
 *  the next byte -- the same rule as TextEditor's own decoder. */
inline std::vector<uint32_t> textInputDecode(const std::string& in) {
  std::vector<uint32_t> out;
  out.reserve(in.size());
  size_t i = 0;
  while (i < in.size()) {
    const unsigned char c = (unsigned char) in[i];
    uint32_t cp = c;
    int extra = 0;
    bool ok = c < 0x80 || (c >= 0xc0 && c < 0xf8);
    if (c >= 0xf0) {
      cp = c & 0x07u;
      extra = 3;
    } else if (c >= 0xe0) {
      cp = c & 0x0fu;
      extra = 2;
    } else if (c >= 0xc0) {
      cp = c & 0x1fu;
      extra = 1;
    }
    for (int k = 1; ok && k <= extra; ++k) {
      if (i + (size_t) k >= in.size() || ((unsigned char) in[i + (size_t) k] & 0xc0u) != 0x80u) {
        ok = false;
        break;
      }
      cp = (cp << 6) | ((unsigned char) in[i + (size_t) k] & 0x3fu);
    }
    if (!ok) {
      out.push_back(0xFFFDu);
      ++i;
      continue;
    }
    if ((cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) cp = 0xFFFDu;
    out.push_back(cp);
    i += (size_t) extra + 1;
  }
  return out;
}

inline std::string textInputEncode(const uint32_t* from, const uint32_t* to) {
  std::string out;
  for (; from != to; ++from) {
    const uint32_t cp = *from;
    if (cp < 0x80) {
      out.push_back((char) cp);
    } else if (cp < 0x800) {
      out.push_back((char) (0xc0 | (cp >> 6)));
      out.push_back((char) (0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
      out.push_back((char) (0xe0 | (cp >> 12)));
      out.push_back((char) (0x80 | ((cp >> 6) & 0x3f)));
      out.push_back((char) (0x80 | (cp & 0x3f)));
    } else {
      out.push_back((char) (0xf0 | (cp >> 18)));
      out.push_back((char) (0x80 | ((cp >> 12) & 0x3f)));
      out.push_back((char) (0x80 | ((cp >> 6) & 0x3f)));
      out.push_back((char) (0x80 | (cp & 0x3f)));
    }
  }
  return out;
}

/** UTF-16 units in s[from, to). Past the basic plane a character is two. */
inline size_t textInputUnits(const std::vector<uint32_t>& s, size_t from, size_t to) {
  size_t units = 0;
  for (size_t i = from; i < to && i < s.size(); ++i) units += s[i] >= 0x10000u ? 2u : 1u;
  return units;
}

/** The character index `units` UTF-16 units in -- rounded DOWN when it falls
 *  between the halves of a surrogate pair, and clamped to the end. A range
 *  that split an emoji would otherwise put half of one into a string. */
inline size_t textInputIndexOfUnits(const std::vector<uint32_t>& s, size_t units) {
  size_t i = 0, at = 0;
  while (i < s.size()) {
    const size_t width = s[i] >= 0x10000u ? 2u : 1u;
    if (at + width > units) break;
    at += width;
    ++i;
  }
  return i;
}

} // namespace detail

/**
 * The input-method protocol, once, over any TextInputTarget.
 *
 * Named and shaped after NSTextInputClient because it is the most demanding
 * of the three -- X11's preedit callbacks and Windows' composition string are
 * each a subset of it, and the preedit* functions below are that subset in
 * XIM's own terms (code points, a change at a position).
 */
class TextInputSession {
public:
  /** The committed text with the composition in place at the caret. */
  static std::vector<uint32_t> shownText(const TextInputTarget& t) {
    const std::vector<uint32_t>& text = t.inputText();
    const TextComposition& c = t.composition();
    if (!c.isActive()) return text;
    const size_t at = clampIndex(t.inputSelectionStart(), text.size());
    std::vector<uint32_t> out;
    out.reserve(text.size() + c.text.size());
    out.insert(out.end(), text.begin(), text.begin() + (std::ptrdiff_t) at);
    out.insert(out.end(), c.text.begin(), c.text.end());
    out.insert(out.end(), text.begin() + (std::ptrdiff_t) at, text.end());
    return out;
  }

  /** Where the caret is drawn, in SHOWN indices: inside the composition while
   *  there is one, the field's own caret otherwise. */
  static size_t shownCaret(const TextInputTarget& t, size_t fieldCaret) {
    const TextComposition& c = t.composition();
    if (!c.isActive()) return fieldCaret;
    return clampIndex(t.inputSelectionStart(), t.inputText().size()) + c.caret;
  }

  /** markedRange: the composition in the shown text, or kNotFound. */
  static TextRange16 markedRange(const TextInputTarget& t) {
    const TextComposition& c = t.composition();
    if (!c.isActive()) return TextRange16{};
    const std::vector<uint32_t>& text = t.inputText();
    const size_t at = clampIndex(t.inputSelectionStart(), text.size());
    return TextRange16{detail::textInputUnits(text, 0, at),
                       detail::textInputUnits(c.text, 0, c.text.size())};
  }

  /** selectedRange: inside the composition while there is one -- the clause
   *  being converted, or the input method's caret -- and the field's own
   *  selection otherwise. */
  static TextRange16 selectedRange(const TextInputTarget& t) {
    const std::vector<uint32_t>& text = t.inputText();
    const TextComposition& c = t.composition();
    if (c.isActive()) {
      const size_t base = detail::textInputUnits(text, 0, clampIndex(t.inputSelectionStart(), text.size()));
      if (c.clauseEnd > c.clauseStart)
        return TextRange16{base + detail::textInputUnits(c.text, 0, c.clauseStart),
                           detail::textInputUnits(c.text, c.clauseStart, c.clauseEnd)};
      return TextRange16{base + detail::textInputUnits(c.text, 0, c.caret), 0};
    }
    const size_t from = clampIndex(t.inputSelectionStart(), text.size());
    const size_t to = clampIndex(t.inputSelectionEnd(), text.size());
    return TextRange16{detail::textInputUnits(text, 0, from), detail::textInputUnits(text, from, to)};
  }

  /**
   * setMarkedText:selectedRange:replacementRange:.
   *
   * `selected` is inside the new marked text; `replacement` is in the shown
   * text, or kNotFound for "the current composition, or else the selection".
   * Whatever is replaced is deleted from the committed text NOW -- typing
   * over a selection removes it the moment composing starts, as it does in
   * every text view -- and the new text is shown at that place without
   * becoming part of the value. An empty string ends the composition with
   * nothing typed. False for a read-only field.
   */
  static bool setMarkedText(TextInputTarget& t, const std::string& utf8, TextRange16 selected,
                            TextRange16 replacement) {
    if (t.isInputReadOnly()) return false;
    const std::vector<uint32_t> text = printable(detail::textInputDecode(utf8));
    size_t from = 0, to = 0;
    replacedRange(t, replacement, &from, &to);
    t.clearComposition();
    if (from != to) t.replaceInputRange(from, to, {});
    else t.setInputSelection(from, from);
    if (text.empty()) return true;

    TextComposition c;
    c.text = text;
    if (selected.isNotFound()) {
      c.caret = text.size();
    } else {
      const size_t s0 = detail::textInputIndexOfUnits(text, selected.location);
      const size_t s1 = detail::textInputIndexOfUnits(text, selected.end());
      // A collapsed selection is the input method's caret; a range is the
      // clause it is converting, and the candidates belong at its start.
      c.caret = s0;
      if (s1 > s0) {
        c.clauseStart = s0;
        c.clauseEnd = s1;
      }
    }
    t.setComposition(std::move(c));
    return true;
  }

  /**
   * insertText:replacementRange:, and X11's committed string.
   *
   * Replaces the composition when there is one -- which is what committing it
   * means -- or `replacement`, or the selection, through the field's own
   * rules. False for a read-only field.
   */
  static bool insertText(TextInputTarget& t, const std::string& utf8, TextRange16 replacement) {
    if (t.isInputReadOnly()) return false;
    size_t from = 0, to = 0;
    replacedRange(t, replacement, &from, &to);
    t.clearComposition();
    return t.replaceInputRange(from, to, detail::textInputDecode(utf8));
  }

  /** unmarkText: accept the composition as it stands. */
  static void unmarkText(TextInputTarget& t) { t.commitComposition(); }

  /** attributedSubstringForProposedRange:actualRange:, as UTF-8 -- the
   *  shown text, so an input method that reads back what it is composing
   *  (to reconvert it, or to see its context) gets what is on screen. */
  static std::string substring(const TextInputTarget& t, TextRange16 range, TextRange16* actual) {
    if (actual) *actual = TextRange16{};
    if (range.isNotFound()) return {};
    const std::vector<uint32_t> shown = shownText(t);
    const size_t s0 = detail::textInputIndexOfUnits(shown, range.location);
    const size_t s1 = (std::max)(s0, detail::textInputIndexOfUnits(shown, range.end()));
    if (actual)
      *actual = TextRange16{detail::textInputUnits(shown, 0, s0), detail::textInputUnits(shown, s0, s1)};
    if (s0 >= shown.size()) return {};
    return detail::textInputEncode(shown.data() + s0, shown.data() + s1);
  }

  /**
   * firstRectForCharacterRange:actualRange:, in the target's own
   * coordinates: the box of the range's first character, widened along its
   * line for as long as the range stays on that line -- which is what the
   * name means and what the candidate window is placed against. An empty
   * range is a zero-width box: the caret there.
   */
  static bool firstRect(const TextInputTarget& t, TextRange16 range, Rect* local, TextRange16* actual) {
    if (actual) *actual = TextRange16{};
    if (!local) return false;
    const std::vector<uint32_t> shown = shownText(t);
    const size_t s0 = range.isNotFound() ? shownCaret(t, t.inputSelectionStart())
                                         : detail::textInputIndexOfUnits(shown, range.location);
    const size_t s1 = range.isNotFound() ? s0 : (std::max)(s0, detail::textInputIndexOfUnits(shown, range.end()));
    Rect r = t.inputCharacterBounds(s0);
    size_t end = s0;
    if (s1 > s0 && s0 < shown.size()) {
      end = s0 + 1;
      for (size_t i = s0 + 1; i < s1 && i < shown.size(); ++i) {
        const Rect b = t.inputCharacterBounds(i);
        // Another line: a box whose top is not this line's. Half a pixel of
        // slack, because both come from the same float arithmetic.
        if (b.y > r.y + 0.5f || b.y < r.y - 0.5f) break;
        const float left = (std::min)(r.x, b.x);
        const float right = (std::max)(r.right(), b.right());
        r = Rect(left, r.y, right - left, (std::max)(r.h, b.h));
        end = i + 1;
      }
    } else {
      r = Rect(r.x, r.y, 0.0f, r.h);
    }
    *local = r;
    if (actual)
      *actual = TextRange16{detail::textInputUnits(shown, 0, s0), detail::textInputUnits(shown, s0, end)};
    return true;
  }

  /** characterIndexForPoint:, from the target's own coordinates, in UTF-16
   *  units of the shown text. */
  static size_t characterIndexAt(const TextInputTarget& t, Point local) {
    const std::vector<uint32_t> shown = shownText(t);
    return detail::textInputUnits(shown, 0, (std::min)(t.inputIndexAt(local), shown.size()));
  }

  // ── X11's on-the-spot preedit, in its own terms ──────────────────────────

  /** XIMPreeditStartCallback: a composition begins at the caret, and like a
   *  typed character it replaces the selection. */
  static void preeditStart(TextInputTarget& t) {
    if (t.isInputReadOnly() || t.composition().isActive()) return;
    const size_t from = t.inputSelectionStart(), to = t.inputSelectionEnd();
    if (to > from) t.replaceInputRange(from, to, {});
  }

  /**
   * XIMPreeditDrawCallback: replace `length` characters of the composition at
   * `first` with `inserted` (none for a pure deletion), and put the caret at
   * `caret`. Out-of-range positions are clamped rather than trusted -- they
   * come from another process.
   */
  static void preeditDraw(TextInputTarget& t, size_t first, size_t length,
                          const std::vector<uint32_t>* inserted, size_t caret) {
    if (t.isInputReadOnly()) return;
    if (!t.composition().isActive()) preeditStart(t);
    TextComposition c = t.composition();
    first = (std::min)(first, c.text.size());
    length = (std::min)(length, c.text.size() - first);
    c.text.erase(c.text.begin() + (std::ptrdiff_t) first, c.text.begin() + (std::ptrdiff_t) (first + length));
    if (inserted) {
      const std::vector<uint32_t> add = printable(*inserted);
      c.text.insert(c.text.begin() + (std::ptrdiff_t) first, add.begin(), add.end());
    }
    c.caret = (std::min)(caret, c.text.size());
    c.clauseStart = c.clauseEnd = 0;
    if (c.text.empty()) t.clearComposition();
    else t.setComposition(std::move(c));
  }

  /** The clause being converted, inside the composition (X11 reports it as
   *  per-character feedback; the peer finds the highlighted run). */
  static void setClause(TextInputTarget& t, size_t start, size_t end) {
    if (!t.composition().isActive()) return;
    TextComposition c = t.composition();
    c.clauseStart = start;
    c.clauseEnd = end;
    t.setComposition(std::move(c));
  }

  /** XIMCaretDirection, in the same order, so the X11 peer can cast. */
  enum class CaretMove {
    ForwardChar, BackwardChar, ForwardWord, BackwardWord, Up, Down,
    NextLine, PreviousLine, LineStart, LineEnd, Absolute, DontChange,
  };

  /** XIMPreeditCaretCallback: move the composition's caret, and return where
   *  it now is -- XIM wants the answer written back. */
  static size_t preeditCaret(TextInputTarget& t, CaretMove move, size_t position) {
    const TextComposition& current = t.composition();
    if (!current.isActive()) return 0;
    const size_t n = current.text.size();
    size_t at = current.caret;
    switch (move) {
      case CaretMove::ForwardChar: at = at < n ? at + 1 : n; break;
      case CaretMove::BackwardChar: at = at > 0 ? at - 1 : 0; break;
      case CaretMove::ForwardWord:
        while (at < n && current.text[at] != ' ') ++at;
        while (at < n && current.text[at] == ' ') ++at;
        break;
      case CaretMove::BackwardWord:
        while (at > 0 && current.text[at - 1] == ' ') --at;
        while (at > 0 && current.text[at - 1] != ' ') --at;
        break;
      case CaretMove::LineStart: at = 0; break;
      case CaretMove::LineEnd: at = n; break;
      case CaretMove::Absolute: at = (std::min)(position, n); break;
      default: break; // one line of composition: up, down and the rest stay
    }
    if (at != current.caret) {
      TextComposition c = current;
      c.caret = at;
      t.setComposition(std::move(c));
    }
    return at;
  }

  /** XIMPreeditDoneCallback: the composition is over. What it became, if
   *  anything, arrives separately as committed text. */
  static void preeditDone(TextInputTarget& t) { t.clearComposition(); }

private:
  static size_t clampIndex(size_t i, size_t n) { return i < n ? i : n; }

  /** Control characters are not composed text: an input method has no reason
   *  to send one, and one that did would draw a missing glyph. */
  static std::vector<uint32_t> printable(const std::vector<uint32_t>& in) {
    std::vector<uint32_t> out;
    out.reserve(in.size());
    for (uint32_t c : in)
      if (c >= 0x20 && c != 0x7f && !(c >= 0x80 && c < 0xa0)) out.push_back(c);
    return out;
  }

  /** The committed range a replacement means: `replacement` mapped out of
   *  the shown text (a position inside the composition is its start), else
   *  the composition's place, else the selection. */
  static void replacedRange(const TextInputTarget& t, TextRange16 replacement, size_t* from, size_t* to) {
    const std::vector<uint32_t>& text = t.inputText();
    const TextComposition& c = t.composition();
    const size_t anchor = clampIndex(t.inputSelectionStart(), text.size());
    if (!replacement.isNotFound()) {
      const std::vector<uint32_t> shown = shownText(t);
      const size_t s0 = detail::textInputIndexOfUnits(shown, replacement.location);
      const size_t s1 = (std::max)(s0, detail::textInputIndexOfUnits(shown, replacement.end()));
      const size_t n = c.isActive() ? c.text.size() : 0;
      auto committed = [&](size_t s) { return s <= anchor ? s : (s <= anchor + n ? anchor : s - n); };
      *from = committed(s0);
      *to = committed(s1);
      return;
    }
    if (c.isActive()) {
      *from = *to = anchor;
      return;
    }
    *from = anchor;
    *to = clampIndex((std::max)(t.inputSelectionStart(), t.inputSelectionEnd()), text.size());
  }
};

/** The focused component's text input target, or null. */
inline TextInputTarget* focusedTextInputTarget(const MouseRouter* router) {
  Component* c = router ? router->focused() : nullptr;
  return c ? c->textInputTarget() : nullptr;
}

/**
 * The focused component's caret, in the ROOT's coordinates (logical units).
 *
 * The one conversion every input-method path needs -- imm32's composition
 * point, X11's spot location, macOS's first rect -- and the one that silently
 * puts the candidate list in the window's top-left corner when it is left
 * out, which looks exactly like not having implemented input methods at all.
 */
inline bool focusedCaretInRoot(const MouseRouter* router, Rect* out) {
  if (!router || !out) return false;
  Component* focused = router->focused();
  if (!focused) return false;
  Rect caret;
  if (!focused->caretBounds(&caret)) return false;
  const Point origin = focused->localToRoot({caret.x, caret.y});
  *out = Rect(origin.x, origin.y, caret.w, caret.h);
  return true;
}

} // namespace gfx
} // namespace sonore
