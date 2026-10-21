// SPDX-License-Identifier: Apache-2.0
//
// A text field of several lines.
//
// ── Why a second class ──────────────────────────────────────────────────────
//
// text_editor.h keeps to one line and says why: up and down between lines, a
// vertical scroll and a selection that spans lines make a different object,
// and one class pretending to be both does neither job well. This is the
// other object -- the notes a user keeps with a preset, a patch's
// description, a comment saved with a session.
//
// Lines wrap where Font::wrapText wraps them, at the breaks Unicode's line
// breaking algorithm allows, and break where the text has a line break. The
// text is kept as UTF-32 for the reason TextEditor keeps it so: every caret
// operation is on characters. Line breaks are stored as LF whatever the
// clipboard or setText brought -- CR LF and a lone CR become one LF, and so
// do VT, FF, NEL and the line and paragraph separators -- so a caret never
// stands between the two halves of a Windows line ending, and every break a
// line can end at is one a triple click and Return agree on.
//
// An input method's composition is shown and laid out as TextEditor's is (see
// text_editor.h): every path that lays lines out or places the caret reads the
// SHOWN text -- the value with the composition in it -- and every path that
// edits commits the composition first.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "clipboard.h"
#include "text_input.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * An editable text field of several wrapped lines, where Return is a new
 * line; TextEditor is the single-line one. See the file comment for why they
 * are two classes and how line breaks are stored.
 *
 * Text goes in and out as UTF-8 (setText, getText); every break comes back as
 * LF. setText does not notify unless asked. With no Return to commit a value,
 * onFocusLost is where a typed value is taken, and onTextChange fires on
 * every keystroke. Message thread only.
 */
class TextArea : public Widget, public TextInputTarget {
public:
  /** Empty and focusable, with an I-beam cursor. */
  TextArea() {
    setWantsKeyboardFocus(true);
    setCursor(MouseCursor::Text);
  }

  /** Fired on every change, including each typed character. */
  std::function<void(const std::string&)> onTextChange;
  /** Fired when the field loses focus: where a value typed into it is
   *  committed, there being no Return to commit it with -- Return is a new
   *  line here. */
  std::function<void(const std::string&)> onFocusLost;

  /** Replace the text with `utf8`, the caret after it and nothing selected; a
   *  CR LF or a lone CR is stored as LF, and a composition in progress is
   *  dropped. onTextChange fires only when `notify` is true. */
  void setText(const std::string& utf8, bool notify = false) {
    clearComposition();
    text_ = decode(utf8);
    caret_ = anchor_ = (int) text_.size();
    desiredX_ = -1.0f;
    ++version_;
    scrollToCaret();
    repaint();
    if (notify && onTextChange) onTextChange(getText());
  }

  /** The text as UTF-8, every line break as LF. */
  std::string getText() const { return encode(text_.begin(), text_.end()); }
  /** Whether there is no text (a composition in progress does not count). */
  bool isEmpty() const { return text_.empty(); }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::TextField);
    info.value = getText();
    if (info.name.empty()) info.name = placeholder_;
    info.valueSettable = isEnabled() && !readOnly_;
    return info;
  }

  /** A reader typing the whole value, through the paste rules. */
  bool setAccessibleText(const std::string& utf8) override {
    if (!isEnabled() || readOnly_) return false;
    commitComposition();
    return replaceInputRange(0, text_.size(), decode(utf8));
  }

  // ── TextInputTarget ──────────────────────────────────────────────────────

  TextInputTarget* textInputTarget() override { return this; }
  const std::vector<uint32_t>& inputText() const override { return text_; }
  size_t inputSelectionStart() const override { return (size_t) selectionStart(); }
  size_t inputSelectionEnd() const override { return (size_t) selectionEnd(); }
  bool isInputReadOnly() const override { return readOnly_ || !isEnabled(); }

  void setInputSelection(size_t start, size_t end) override {
    anchor_ = (int) (std::min)(start, text_.size());
    caret_ = (int) (std::min)(end, text_.size());
    desiredX_ = -1.0f;
    atLineEnd_ = false;
    scrollToCaret();
    repaint();
  }

  /** The paste rules: line breaks kept as LF, a tab as a space, other
   *  control characters dropped, the length limit applied. */
  bool replaceInputRange(size_t start, size_t end, const std::vector<uint32_t>& with) override {
    if (isInputReadOnly()) return false;
    start = (std::min)(start, text_.size());
    end = (std::max)(start, (std::min)(end, text_.size()));
    const bool erased = end > start;
    text_.erase(text_.begin() + (long) start, text_.begin() + (long) end);
    size_t at = start;
    for (size_t i = 0; i < with.size(); ++i) {
      uint32_t c = with[i];
      if (c == '\r') {
        if (i + 1 < with.size() && with[i + 1] == '\n') ++i;
        c = '\n';
      }
      if (c == '\t') c = ' ';
      c = lineBreakAsLf(c);
      if ((c < 0x20 && c != '\n') || c == 0x7f || (c >= 0x80 && c < 0xa0)) continue;
      if (maxLength_ > 0 && (int) text_.size() >= maxLength_) break;
      text_.insert(text_.begin() + (long) at, c);
      ++at;
    }
    caret_ = anchor_ = (int) at;
    if (erased || at > start) changed();
    return true;
  }

  Rect inputCharacterBounds(size_t index) const override {
    const std::vector<Line>& all = lines();
    const std::vector<uint32_t>& text = shown();
    index = (std::min)(index, text.size());
    const size_t k = lineOf((int) index);
    const Line& line = all[k];
    const Rect inner = innerBounds();
    if (index >= line.begin && index < line.end) {
      const CharacterLayout& chars = layoutOf(k);
      const size_t at = index - line.begin;
      if (at < chars.size()) {
        const float left = (std::min)(chars.left[at], chars.right[at]);
        const float right = (std::max)(chars.left[at], chars.right[at]);
        return Rect(inner.x + left, lineTop(k), right - left, lineHeight());
      }
    }
    // Past what the line draws -- a hanging space, a break, the end -- where a
    // caret there would stand.
    return Rect(inner.x + caretXOn(k, (int) index), lineTop(k), 0.0f, lineHeight());
  }

  size_t inputIndexAt(Point local) const override { return (size_t) indexAt(local, nullptr); }

  /** Shown greyed while the field is empty and has no focus. */
  void setPlaceholder(const std::string& utf8) {
    placeholder_ = utf8;
    repaint();
  }

  /** In characters, a line break counting as one; 0 means no limit. */
  void setMaxLength(int chars) { maxLength_ = chars; }

  /** A read-only area can still be focused, selected and copied from, but not
   *  changed by typing, pasting or an input method. */
  void setReadOnly(bool readOnly) {
    readOnly_ = readOnly;
    repaint();
  }
  /** Whether setReadOnly(true) is in force. */
  bool isReadOnly() const { return readOnly_; }

  /** The caret as a 0-based code point index, a line break counting as one. */
  int caretPosition() const { return caret_; }
  /** How far the text is scrolled up, in pixels: zero whenever it fits. */
  float scrollOffset() const { return scrollY_; }
  /** How many lines the text makes at the field's width. */
  int numLines() const { return (int) lines().size(); }
  /** The line the caret is on, from 0. */
  int caretLine() const { return (int) caretLineIndex(); }

  bool caretBounds(Rect* out) const override {
    if (!out) return false;
    const size_t k = caretLineIndex();
    const Rect inner = innerBounds();
    *out = Rect(inner.x + caretXOn(k, shownCaret()), lineTop(k), 1.0f, lineHeight());
    return true;
  }

  /** Whether a range is selected rather than just a caret. */
  bool hasSelection() const { return caret_ != anchor_; }
  /** The selection's first code point index; the caret when none is selected. */
  int selectionStart() const { return (std::min)(caret_, anchor_); }
  /** One past the selection's last code point; the caret when none is
   *  selected. */
  int selectionEnd() const { return (std::max)(caret_, anchor_); }

  /** Select the whole text, the caret at its end. No callback. */
  void selectAll() {
    anchor_ = 0;
    caret_ = (int) text_.size();
    desiredX_ = -1.0f;
    repaint();
  }

  /** The selected text, line breaks as LF, or empty. */
  std::string selectedText() const {
    return encode(text_.begin() + selectionStart(), text_.begin() + selectionEnd());
  }

  /** Copies the selection, not the whole field. */
  bool copyToClipboard() const {
    if (!hasSelection()) return false;
    return Clipboard::setText(selectedText());
  }

  /** Paste, replacing the selection: line breaks kept, as LF; a tab as a
   *  space, which is what a field without tab stops can show; other
   *  control characters dropped; no more than the length limit allows. */
  bool pasteFromClipboard() {
    if (readOnly_) return false;
    std::string incoming;
    if (!Clipboard::getText(&incoming) || incoming.empty()) return false;
    std::vector<uint32_t> accepted;
    for (uint32_t c : decode(incoming)) {
      if (c == '\t') c = ' ';
      if ((c < 0x20 && c != '\n') || c == 0x7f) continue;
      accepted.push_back(c);
    }
    if (accepted.empty()) return false;
    const bool replaced = hasSelection();
    if (replaced) eraseSelection();
    bool any = false;
    for (uint32_t c : accepted) {
      if (maxLength_ > 0 && (int) text_.size() >= maxLength_) break;
      text_.insert(text_.begin() + caret_, c);
      ++caret_;
      any = true;
    }
    anchor_ = caret_;
    if (any || replaced) changed();
    return any;
  }

  // ── Keyboard ─────────────────────────────────────────────────────────────

  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled()) return false;
    // A key that reaches the field mid-composition commits it first.
    commitComposition();
    const size_t line = caretLineIndex();
    switch (key.keyCode) {
      case KeyPress::Left:
        if (hasSelection() && !key.shiftDown) collapse(true);
        else moveTo((int) previousStop(caret_), key.shiftDown);
        return true;
      case KeyPress::Right:
        if (hasSelection() && !key.shiftDown) collapse(false);
        else moveTo((int) nextStop(caret_), key.shiftDown);
        return true;
      case KeyPress::Up:
        moveVertically((int) line - 1, key.shiftDown);
        return true;
      case KeyPress::Down:
        moveVertically((int) line + 1, key.shiftDown);
        return true;
      case KeyPress::PageUp:
        moveVertically((int) line - linesPerPage(), key.shiftDown);
        return true;
      case KeyPress::PageDown:
        moveVertically((int) line + linesPerPage(), key.shiftDown);
        return true;
      case KeyPress::Home:
        moveTo(key.ctrlDown ? 0 : (int) lines()[line].begin, key.shiftDown);
        return true;
      case KeyPress::End: {
        // The text's end on its last line; before a line break, after any
        // spaces in front of it; where a line wraps, after what it draws --
        // and the end of THIS line, though a line wrapped with nothing
        // hanging ends at the index the next begins at.
        const std::vector<Line>& all = lines();
        int to = (int) text_.size();
        bool wrapped = false;
        if (!key.ctrlDown && line + 1 < all.size()) {
          const Line& here = all[line];
          const bool broken = here.next > here.end && text_[here.next - 1] == '\n';
          to = broken ? (int) here.next - 1 : (int) here.end;
          wrapped = !broken && softEnd(line);
        }
        moveTo(to, key.shiftDown);
        atLineEnd_ = wrapped;
        return true;
      }
      case KeyPress::Backspace:
        if (readOnly_) return true;
        if (hasSelection()) eraseSelection();
        else if (caret_ > 0) {
          // One character back, so a mark typed by mistake goes alone -- but
          // an emoji goes whole, as in TextEditor.
          int from = caret_ - 1;
          const int start = (int) previousStop(caret_);
          for (int i = start; i < caret_; ++i)
            if (detail::shaping::isEmojiPart(text_[(size_t) i])) from = start;
          text_.erase(text_.begin() + from, text_.begin() + caret_);
          caret_ = anchor_ = from;
        }
        changed();
        return true;
      case KeyPress::Delete:
        if (readOnly_) return true;
        if (hasSelection()) eraseSelection();
        // Forward, a whole cluster goes, or the line break.
        else if (caret_ < (int) text_.size())
          text_.erase(text_.begin() + caret_, text_.begin() + (long) nextStop(caret_));
        changed();
        return true;
      case KeyPress::Return:
        if (readOnly_) return true;
        insert('\n');
        return true;
      case KeyPress::Escape:
      case KeyPress::Tab:
        // Neither is the field's: Escape closes what is around it, Tab moves
        // the focus on.
        return false;
      default:
        break;
    }
    if (key.ctrlDown) {
      switch (key.character) {
        case 'a':
        case 'A':
          selectAll();
          return true;
        case 'c':
        case 'C':
          copyToClipboard();
          return true;
        case 'x':
        case 'X':
          if (!readOnly_ && copyToClipboard()) {
            eraseSelection();
            changed();
          }
          return true;
        case 'v':
        case 'V':
          if (!readOnly_) pasteFromClipboard();
          return true;
        default:
          break;
      }
    }
    if (key.ctrlDown || key.altDown) return false;
    if (!key.isCharacter() || readOnly_) return false;
    // The CR that follows Return as a character on Windows is refused here
    // with the other control characters, C1's among them: Return above made
    // the line break. So is what no character is: a surrogate, or past
    // U+10FFFF.
    const uint32_t c = key.character;
    if (c < 0x20 || (c >= 0x7f && c < 0xa0) || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) return false;
    insert(lineBreakAsLf(c));
    return true;
  }

  void focusGained() override { repaint(); }

  void focusLost() override {
    commitComposition();
    // The handler last, as TextEditor's is: it is the owner's code, and
    // committing a note or a description may close the panel this area is
    // in -- the repaint after it was a write into the deleted area.
    repaint();
    if (onFocusLost) onFocusLost(getText());
  }

  // ── Mouse ────────────────────────────────────────────────────────────────

  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    commitComposition();
    bool atEnd = false;
    const int at = indexAt(e.position, &atEnd);
    desiredX_ = -1.0f;
    atLineEnd_ = false;
    if (e.clickCount >= 3) {
      // The paragraph: from the line break before to the one after.
      int from = at, to = at;
      while (from > 0 && text_[(size_t) from - 1] != '\n') --from;
      while (to < (int) text_.size() && text_[(size_t) to] != '\n') ++to;
      anchor_ = from;
      caret_ = to;
    } else if (e.clickCount == 2) {
      int from = at, to = at;
      while (from > 0 && isWordCharacter(text_[(size_t) from - 1])) --from;
      while (to < (int) text_.size() && isWordCharacter(text_[(size_t) to])) ++to;
      anchor_ = from;
      caret_ = to;
    } else {
      caret_ = at;
      atLineEnd_ = atEnd;
      if (!e.shiftDown) anchor_ = at;
    }
    scrollToCaret();
    repaint();
  }

  void mouseDrag(const MouseEvent& e) override {
    if (!isEnabled()) return;
    bool atEnd = false;
    const int at = indexAt(e.position, &atEnd);
    if (at == caret_ && atEnd == atLineEnd_) return;
    caret_ = at;
    atLineEnd_ = atEnd;
    desiredX_ = -1.0f;
    scrollToCaret();
    repaint();
  }

  bool mouseWheel(const MouseEvent&, float delta) override {
    const float before = scrollY_;
    scrollY_ = clampScroll(scrollY_ - delta * kWheelStep);
    if (scrollY_ == before) return false; // nothing to scroll: the parent's turn
    repaint();
    return true;
  }

  // ── Painting ─────────────────────────────────────────────────────────────

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    const Rect area = localBounds();
    lf.drawTextEditorBackground(g, area, hasKeyboardFocus(), isEnabled());
    const Rect inner = innerBounds();
    if (inner.isEmpty()) return;
    Graphics::ScopedState clip(g);
    g.clipTo(inner);

    const float lh = lineHeight();
    if (shown().empty() && !hasKeyboardFocus() && !placeholder_.empty()) {
      g.setColour(lf.dimText());
      font().drawWrapped(g, placeholder_, inner, Justify::Left);
      return;
    }

    const std::vector<Line>& all = lines();
    const size_t first = (size_t) (std::max)(0.0f, std::floor(scrollY_ / lh));
    const size_t last = (std::min)(all.size(), (size_t) std::ceil((scrollY_ + inner.h) / lh) + 1);
    const TextComposition& composing = composition();
    // No selection is drawn while composing: the composition replaced it.
    const int s = composing.isActive() ? 0 : selectionStart();
    const int e = composing.isActive() ? 0 : selectionEnd();
    for (size_t k = first; k < last; ++k) {
      const Line& line = all[k];
      const float top = lineTop(k);
      if (s < e && s < (int) line.next && e > (int) line.begin) {
        const CharacterLayout& chars = layoutOf(k);
        std::vector<std::pair<float, float>> spans;
        for (int i = (std::max)(s, (int) line.begin); i < (std::min)(e, (int) line.end); ++i) {
          const size_t at = (size_t) i - line.begin;
          if (at < chars.size()) spans.push_back({chars.left[at], chars.right[at]});
        }
        // The line's break or its hanging spaces selected: a sliver past its
        // end, so a selection running on to the next line shows that it does.
        if (e > (int) line.end && line.next > line.end) spans.push_back({chars.width, chars.width + lh * 0.3f});
        std::sort(spans.begin(), spans.end());
        for (size_t a = 0; a < spans.size();) {
          float x0 = spans[a].first, x1 = spans[a].second;
          size_t b = a + 1;
          while (b < spans.size() && spans[b].first <= x1 + 0.01f) x1 = (std::max)(x1, spans[b++].second);
          lf.drawTextEditorSelection(g, Rect(inner.x + x0, top, x1 - x0, lh));
          a = b;
        }
      }
    }
    g.setColour(isEnabled() ? lf.text() : lf.dimText());
    for (size_t k = first; k < last; ++k)
      if (!lineText(k).empty()) font().draw(g, lineText(k), inner.x, lineTop(k) + baselineOffset());
    // The composition, underlined character by character -- it may wrap --
    // with the clause being converted heavier.
    if (composing.isActive()) {
      const size_t from = (size_t) selectionStart();
      for (size_t i = 0; i < composing.text.size(); ++i) {
        const Rect box = inputCharacterBounds(from + i);
        const bool clause = i >= composing.clauseStart && i < composing.clauseEnd;
        const float y = (std::min)(box.y + baselineOffset() + 2.0f, box.bottom() - 2.0f);
        g.fillRect(Rect(box.x, y, (std::max)(box.w, 1.0f), clause ? 2.0f : 1.0f));
      }
    }
    // Not while a clause is being converted: the clause is where the input
    // method is.
    const bool converting = composing.isActive() && composing.clauseEnd > composing.clauseStart;
    if (hasKeyboardFocus() && !readOnly_ && !converting) {
      const size_t k = caretLineIndex();
      lf.drawTextEditorCaret(g, Rect(inner.x + caretXOn(k, shownCaret()), lineTop(k) + 1.0f, 1.0f, lh - 2.0f));
    }
  }

  void resized() override { scrollToCaret(); }

  /** The inset of the text from the left and right edges, logical pixels (4 at
   *  the top and bottom). */
  static constexpr float kPadding = 6.0f;
  /** Logical pixels scrolled per wheel notch. */
  static constexpr float kWheelStep = 48.0f;

protected:
  void compositionChanged() override {
    // The lines are laid out from the shown text, which just changed.
    ++version_;
    scrollToCaret();
    repaint();
  }

private:
  /** The value with any composition in it: what the lines are made of. The
   *  value itself while nothing is being composed, so nothing is copied. */
  const std::vector<uint32_t>& shown() const {
    if (!composition().isActive()) return text_;
    if (!shownValid_ || shownVersion_ != version_) {
      shown_ = TextInputSession::shownText(*this);
      shownVersion_ = version_;
      shownValid_ = true;
    }
    return shown_;
  }

  int shownCaret() const { return (int) TextInputSession::shownCaret(*this, (size_t) caret_); }

  /** A line of the wrapped text, in characters: where it begins, where what
   *  it draws ends -- before the spaces and the break it ends with -- and
   *  where the next begins. */
  struct Line {
    size_t begin = 0, end = 0, next = 0;
  };

  Rect innerBounds() const { return localBounds().reduced(kPadding, 4.0f); }
  float lineHeight() const {
    const float lh = font().lineHeight();
    return lh > 0.0f ? lh : 16.0f;
  }
  float baselineOffset() const { return (lineHeight() - font().ascent() - font().descent()) * 0.5f + font().ascent(); }
  float lineTop(size_t k) const { return innerBounds().y + (float) k * lineHeight() - scrollY_; }
  int linesPerPage() const { return (std::max)(1, (int) (innerBounds().h / lineHeight())); }

  /** The lines at the field's width, made again only when the text, the
   *  width or the font has changed since. */
  const std::vector<Line>& lines() const {
    const Font& f = font();
    const float width = innerBounds().w;
    if (built_ && builtVersion_ == version_ && builtWidth_ == width && builtFace_ == f.typeface().get() &&
        builtHeight_ == f.height())
      return lines_;
    built_ = true;
    builtVersion_ = version_;
    builtWidth_ = width;
    builtFace_ = f.typeface().get();
    builtHeight_ = f.height();
    lines_.clear();
    const std::vector<uint32_t>& text = shown();
    const size_t n = text.size();
    for (const Font::WrappedLine& l : f.wrapLines(encode(text.begin(), text.end()), (std::max)(width, 1.0f)))
      lines_.push_back({l.begin, l.end, l.next});
    if (lines_.empty()) lines_.push_back({0, 0, 0});
    // Text that ends with a line break has an empty line after it, where the
    // caret goes.
    if (n > 0 && text[n - 1] == '\n') lines_.push_back({n, n, n});
    texts_.assign(lines_.size(), std::string());
    layouts_.assign(lines_.size(), CharacterLayout());
    laidOut_.assign(lines_.size(), 0);
    return lines_;
  }

  const std::string& lineText(size_t k) const {
    const Line& line = lines()[k];
    if (!laidOut_[k]) {
      texts_[k] = encode(shown().begin() + (long) line.begin, shown().begin() + (long) line.end);
      layouts_[k] = font().layoutCharacters(texts_[k]);
      laidOut_[k] = 1;
    }
    return texts_[k];
  }

  const CharacterLayout& layoutOf(size_t k) const {
    lineText(k);
    return layouts_[k];
  }

  /** The line a caret index is on: the last that begins at or before it. */
  size_t lineOf(int index) const {
    const std::vector<Line>& all = lines();
    size_t lo = 0, hi = all.size();
    while (hi - lo > 1) {
      const size_t mid = (lo + hi) / 2;
      if ((int) all[mid].begin <= index) lo = mid;
      else hi = mid;
    }
    return lo;
  }

  /** Whether line k wraps with nothing hanging at its end: its end is then
   *  the index the next line begins at, and a caret there may be on either. */
  bool softEnd(size_t k) const {
    const std::vector<Line>& all = lines();
    return k + 1 < all.size() && all[k].end == all[k].next;
  }

  /** The line the caret is on: the next line's start, unless End, a click or
   *  a move up or down put it at the end of a line that wraps there. */
  size_t caretLineIndex() const {
    const int caret = shownCaret();
    const size_t k = lineOf(caret);
    if (atLineEnd_ && k > 0 && softEnd(k - 1) && lines()[k - 1].next == (size_t) caret) return k - 1;
    return k;
  }

  float spaceWidth() const { return font().stringWidth(" "); }

  /** The caret's x before character `index` on line k: on what the line
   *  draws, or past it, in its hanging spaces -- a space's width each, kept
   *  inside the field. */
  float caretXOn(size_t k, int index) const {
    const Line& line = lines()[k];
    const CharacterLayout& chars = layoutOf(k);
    const size_t at = (size_t) (std::max)(index, (int) line.begin);
    if (at <= line.end) return chars.caretX(at - line.begin);
    float x = chars.width;
    const float space = spaceWidth();
    const std::vector<uint32_t>& text = shown();
    for (size_t i = line.end; i < at && i < text.size(); ++i)
      if (text[i] == ' ') x += space;
    return (std::min)(x, (std::max)(chars.width, innerBounds().w - 1.0f));
  }

  /** The caret index nearest x on line k: on what it draws, or in its
   *  hanging spaces past it, never on the next line. `atEnd` says whether
   *  that is the end of a line that wraps with nothing hanging. */
  int indexOnLine(size_t k, float x, bool* atEnd) const {
    const Line& line = lines()[k];
    const CharacterLayout& chars = layoutOf(k);
    size_t at = line.begin + (std::min)(chars.indexAt(x), line.end - line.begin);
    if (at == line.end && x > chars.width && line.next > line.end) {
      const float space = spaceWidth();
      size_t spaces = 0;
      const std::vector<uint32_t>& text = shown();
      for (size_t i = line.end; i < line.next && text[i] == ' '; ++i) ++spaces;
      if (space > 0.0f) at += (std::min)(spaces, (size_t) ((x - chars.width) / space + 0.5f));
      at = (std::min)(at, line.next - 1);
    }
    if (atEnd) *atEnd = at == line.end && softEnd(k);
    return (int) at;
  }

  int indexAt(Point p, bool* atEnd) const {
    const Rect inner = innerBounds();
    const float row = std::floor((p.y - inner.y + scrollY_) / lineHeight());
    const size_t count = lines().size();
    const size_t k = row < 0.0f ? 0 : (std::min)((size_t) row, count - 1);
    return indexOnLine(k, p.x - inner.x, atEnd);
  }

  /** The next place a caret may stand: past a cluster, a hanging space or a
   *  line break. */
  size_t nextStop(int index) const {
    if (index >= (int) text_.size()) return text_.size();
    const size_t k = lineOf(index);
    const Line& line = lines()[k];
    if ((size_t) index >= line.end) return (size_t) index + 1;
    return (std::min)(line.begin + layoutOf(k).nextStop((size_t) index - line.begin), line.end);
  }

  size_t previousStop(int index) const {
    if (index <= 0) return 0;
    size_t k = lineOf(index);
    if ((size_t) index == lines()[k].begin) --k; // back across the line's start
    const Line& line = lines()[k];
    if ((size_t) index > line.end) return (size_t) index - 1;
    return line.begin + layoutOf(k).previousStop((size_t) index - line.begin);
  }

  void moveTo(int to, bool extend) {
    caret_ = (std::max)(0, (std::min)(to, (int) text_.size()));
    if (!extend) anchor_ = caret_;
    desiredX_ = -1.0f;
    atLineEnd_ = false;
    scrollToCaret();
    repaint();
  }

  /** To line `target`, at the x the caret had when vertical movement began
   *  -- so moving down through a short line and on does not drift left.
   *  Past the first line is the start of the text, past the last its end. */
  void moveVertically(int target, bool extend) {
    if (desiredX_ < 0.0f) desiredX_ = caretXOn(caretLineIndex(), caret_);
    const float x = desiredX_;
    atLineEnd_ = false;
    if (target < 0) caret_ = 0;
    else if (target >= (int) lines().size()) caret_ = (int) text_.size();
    else caret_ = indexOnLine((size_t) target, x, &atLineEnd_);
    if (!extend) anchor_ = caret_;
    scrollToCaret();
    repaint();
  }

  void collapse(bool toStart) {
    caret_ = anchor_ = toStart ? selectionStart() : selectionEnd();
    desiredX_ = -1.0f;
    atLineEnd_ = false;
    scrollToCaret();
    repaint();
  }

  void insert(uint32_t c) {
    const bool erased = hasSelection();
    if (erased) eraseSelection();
    if (maxLength_ > 0 && (int) text_.size() >= maxLength_) {
      // Full: the selection typed over is gone, and that is the change.
      if (erased) changed();
      return;
    }
    text_.insert(text_.begin() + caret_, c);
    anchor_ = ++caret_;
    changed();
  }

  void eraseSelection() {
    const int from = selectionStart(), to = selectionEnd();
    if (from == to) return;
    text_.erase(text_.begin() + from, text_.begin() + to);
    caret_ = anchor_ = from;
  }

  void changed() {
    ++version_;
    desiredX_ = -1.0f;
    atLineEnd_ = false;
    scrollToCaret();
    repaint();
    if (onTextChange) onTextChange(getText());
  }

  float clampScroll(float y) const {
    const float most = (float) lines().size() * lineHeight() - innerBounds().h;
    return (std::max)(0.0f, (std::min)(y, (std::max)(0.0f, most)));
  }

  /** The caret's line brought into view, and the scroll kept within the
   *  text: a text that shrank does not leave the field scrolled past it. */
  void scrollToCaret() {
    const Rect inner = innerBounds();
    if (inner.h <= 0.0f) return;
    const float lh = lineHeight();
    const float top = (float) caretLineIndex() * lh;
    if (top < scrollY_) scrollY_ = top;
    if (top + lh > scrollY_ + inner.h) scrollY_ = top + lh - inner.h;
    scrollY_ = clampScroll(scrollY_);
  }

  static bool isWordCharacter(uint32_t c) {
    if (c == '\n' || c == ' ' || c == '\t') return false;
    return c == '_' || c == '\'' || detail::shaping::isDecimal(c) || detail::shaping::continuesWord(c);
  }

  /** A mandatory break -- VT, FF, NEL, the line and paragraph separators
   *  -- as the LF every break is stored as; anything else as it is. */
  static uint32_t lineBreakAsLf(uint32_t c) {
    return (c == 0x0B || c == 0x0C || c == 0x85 || c == 0x2028 || c == 0x2029) ? (uint32_t) '\n' : c;
  }

  /** UTF-8 in, line breaks as LF: CR LF and a lone CR are one LF, and so is
   *  every other mandatory break. What decodes to no character -- a
   *  surrogate, or past U+10FFFF -- is U+FFFD. */
  static std::vector<uint32_t> decode(const std::string& utf8) {
    std::vector<uint32_t> out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size();) {
      uint32_t c = Font::nextCodepoint(utf8, i);
      if (c == '\r') {
        if (i < utf8.size() && utf8[i] == '\n') ++i;
        out.push_back('\n');
        continue;
      }
      if ((c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) c = 0xFFFD;
      out.push_back(lineBreakAsLf(c));
    }
    return out;
  }

  static std::string encode(std::vector<uint32_t>::const_iterator from, std::vector<uint32_t>::const_iterator to) {
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

  std::vector<uint32_t> text_;
  std::string placeholder_;
  int caret_ = 0;
  int anchor_ = 0;
  int maxLength_ = 0;
  bool readOnly_ = false;
  float scrollY_ = 0.0f;
  float desiredX_ = -1.0f; // the x up and down keep to, or none
  bool atLineEnd_ = false;  // at the end of a line that wraps there, not the next's start
  uint64_t version_ = 0;   // which text the lines below were made from

  mutable std::vector<Line> lines_;
  mutable std::vector<uint32_t> shown_; // the shown text, while composing
  mutable uint64_t shownVersion_ = 0;
  mutable bool shownValid_ = false;
  mutable std::vector<std::string> texts_;
  mutable std::vector<CharacterLayout> layouts_;
  mutable std::vector<uint8_t> laidOut_;
  mutable bool built_ = false;
  mutable uint64_t builtVersion_ = 0;
  mutable float builtWidth_ = 0.0f, builtHeight_ = 0.0f;
  mutable const Typeface* builtFace_ = nullptr;
};

} // namespace gfx
} // namespace sonore
