// SPDX-License-Identifier: Apache-2.0
//
// A single-line text field.
//
// ── Why a plugin needs one ──────────────────────────────────────────────────
//
// Naming a preset. Typing a cutoff instead of hunting for it with a knob.
// Searching a sample list. Every one of those was free in a webview and
// impossible in the native UI until the component tree learned about keys.
//
// ── Single line, on purpose ─────────────────────────────────────────────────
//
// A multi-line editor is a different object: it needs line breaking, a scroll
// position, up and down between lines, and a selection that spans them. Almost
// nothing in a plugin wants that, and pretending one class can be both is how
// a text field ends up mediocre at the job it actually has.
//
// ── Where the text lives ────────────────────────────────────────────────────
//
// As UTF-32 code points, not as a UTF-8 std::string. Every operation a caret
// performs -- move left, delete forward, select a range -- is defined on
// CHARACTERS, and doing that on a UTF-8 buffer means every one of them has to
// walk continuation bytes. Getting that wrong does not crash; it splits a
// character in half and leaves the field showing a replacement glyph, on
// somebody else's keyboard layout, where nobody testing in English will ever
// see it. Converting at the edges costs a copy on setText and getText and
// nothing anywhere else.
//
// ── Composed text ───────────────────────────────────────────────────────────
//
// An input method's composition (text_input.h) is shown at the caret,
// underlined, and is not part of the value until it is committed: getText()
// never includes it. Everything that DRAWS -- paint, the caret, the scroll --
// works on the shown text; everything that EDITS commits a composition first
// and then works on the committed text, so there is one string being edited at
// a time.
#pragma once

#include <algorithm>
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
 * An editable single-line text field. It has the reference's TextEditor's
 * name and its single-line use; the multi-line one here is TextArea. The file
 * comment says why, and how the text and an input method's composition are
 * kept.
 *
 * Text goes in and out as UTF-8; a pasted line break becomes a space.
 * setText does not notify unless asked. A typed value is committed on
 * onReturn AND on onFocusLost -- listen to both, or a value typed and then
 * clicked away from is lost -- while onTextChange fires on every keystroke.
 * Message thread only.
 */
class TextEditor : public Widget, public TextInputTarget {
public:
  /** Empty and focusable, with an I-beam cursor. */
  TextEditor() {
    setWantsKeyboardFocus(true);
    setCursor(MouseCursor::Text);
  }

  /** Fired on Return. The usual place to commit a value. */
  std::function<void(const std::string&)> onReturn;
  /** Fired on every change, including each typed character. */
  std::function<void(const std::string&)> onTextChange;
  /** Fired when the field loses focus, which is the OTHER way a person commits
   *  a value: they type it and click elsewhere, and a field that only reported
   *  on Return would silently discard it. */
  std::function<void(const std::string&)> onFocusLost;

  /** Replace the text with `utf8`, the caret after it and nothing selected;
   *  a composition in progress is dropped. Taken as given: the character
   *  filter, the length limit and the line-break rule apply to typing and
   *  pasting only. onTextChange fires only when `notify` is true. */
  void setText(const std::string& utf8, bool notify = false) {
    // A composition belonged to the text being replaced.
    clearComposition();
    text_ = fromUtf8(utf8);
    caret_ = (int) text_.size();
    selectionAnchor_ = caret_;
    // The scroll belonged to the old text: a short one set after a long one
    // sat off the left edge, and the field looked empty.
    scrollIntoView();
    repaint();
    if (notify && onTextChange) onTextChange(getText());
  }

  /** The text, as UTF-8. */
  std::string getText() const { return toUtf8(text_); }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::TextField);
    info.value = getText();
    // The placeholder is what the field is FOR when it is empty, which is
    // exactly the moment a reader has nothing else to say about it.
    if (info.name.empty()) info.name = placeholder_;
    info.valueSettable = isEnabled() && !readOnly_;
    return info;
  }

  /** A reader typing the whole value: through the same rules a paste meets
   *  -- the character filter and the length limit -- and reported as a
   *  change. */
  bool setAccessibleText(const std::string& utf8) override {
    if (!isEnabled() || readOnly_) return false;
    commitComposition();
    return replaceInputRange(0, text_.size(), fromUtf8(utf8));
  }

  /** Whether there is no text (a composition in progress does not count). */
  bool isEmpty() const { return text_.empty(); }

  // ── TextInputTarget ──────────────────────────────────────────────────────

  TextInputTarget* textInputTarget() override { return this; }
  const std::vector<uint32_t>& inputText() const override { return text_; }
  size_t inputSelectionStart() const override { return (size_t) selectionStart(); }
  size_t inputSelectionEnd() const override { return (size_t) selectionEnd(); }
  bool isInputReadOnly() const override { return readOnly_ || !isEnabled(); }

  void setInputSelection(size_t start, size_t end) override {
    selectionAnchor_ = (int) (std::min)(start, text_.size());
    caret_ = (int) (std::min)(end, text_.size());
    scrollIntoView();
    repaint();
  }

  /** The paste rules: a line break becomes a space, other control
   *  characters go, the filter and the length limit apply. */
  bool replaceInputRange(size_t start, size_t end, const std::vector<uint32_t>& with) override {
    if (isInputReadOnly()) return false;
    start = (std::min)(start, text_.size());
    end = (std::max)(start, (std::min)(end, text_.size()));
    const bool erased = end > start;
    text_.erase(text_.begin() + (long) start, text_.begin() + (long) end);
    size_t at = start;
    for (uint32_t c : with) {
      if (c == '\n' || c == '\r' || c == '\t') c = ' ';
      if (c < 0x20 || c == 0x7f || !isAllowed(c)) continue;
      if (maxLength_ > 0 && (int) text_.size() >= maxLength_) break;
      text_.insert(text_.begin() + (long) at, c);
      ++at;
    }
    caret_ = selectionAnchor_ = (int) at;
    if (erased || at > start) changed();
    return true;
  }

  Rect inputCharacterBounds(size_t index) const override {
    const Rect inner = localBounds().reduced(kPadding, 0.0f);
    const CharacterLayout chars = layout();
    const float originX = inner.x - scrollX_;
    if (index >= chars.size()) return Rect(originX + chars.caretX(chars.size()), inner.y + 3.0f, 0.0f, inner.h - 6.0f);
    const float left = (std::min)(chars.left[index], chars.right[index]);
    const float right = (std::max)(chars.left[index], chars.right[index]);
    return Rect(originX + left, inner.y + 3.0f, right - left, inner.h - 6.0f);
  }

  size_t inputIndexAt(Point local) const override {
    return layout().indexAt(local.x - (localBounds().x + kPadding) + scrollX_);
  }

  /** Shown greyed when the field is empty, so a user knows what belongs here
   *  without a label taking up a row of its own. */
  void setPlaceholder(const std::string& utf8) {
    placeholder_ = utf8;
    repaint();
  }

  /** Refuses everything that is not in the set. The point is a numeric field
   *  that cannot be made to hold "12abc" in the first place, rather than one
   *  that accepts it and complains afterwards. */
  void setAllowedCharacters(const std::string& utf8) { allowed_ = fromUtf8(utf8); }

  /** 0 means no limit. */
  void setMaxLength(int chars) { maxLength_ = chars; }

  /** A read-only field can still be focused, selected and copied from, but not
   *  changed by typing, pasting or an input method. */
  void setReadOnly(bool readOnly) {
    readOnly_ = readOnly;
    // A read-only field still takes focus: text in one has to be selectable,
    // or it cannot be copied.
    repaint();
  }
  /** Whether setReadOnly(true) is in force. */
  bool isReadOnly() const { return readOnly_; }

  /** The caret as a 0-based code point index: 0 before the first character,
   *  the length after the last. */
  int caretPosition() const { return caret_; }
  /** How far the text is scrolled left, in pixels: zero whenever it fits. */
  float scrollOffset() const { return scrollX_; }

  /**
   * The caret as a rectangle, for an input method to put its candidate list
   * beside. Uses the SAME width measurement the caret is drawn from, so the
   * list cannot end up beside a different character than the one being typed.
   */
  bool caretBounds(Rect* out) const override {
    if (!out) return false;
    // The SAME arithmetic paint() draws the caret with: the inner rect is the
    // bounds reduced by kPadding horizontally, and the text origin is that
    // inset minus the horizontal scroll.
    // Inside the composition while there is one: the candidate list belongs
    // beside what is being composed, not where it began.
    const Rect inner = localBounds().reduced(kPadding, 0.0f);
    const float cx = inner.x - scrollX_ + layout().caretX(shownCaret());
    *out = Rect(cx, inner.y + 3.0f, 1.0f, inner.h - 6.0f);
    return true;
  }
  /** Whether a range is selected rather than just a caret. */
  bool hasSelection() const { return caret_ != selectionAnchor_; }
  /** The selection's first code point index; the caret when none is selected. */
  int selectionStart() const { return caret_ < selectionAnchor_ ? caret_ : selectionAnchor_; }
  /** One past the selection's last code point; the caret when none is
   *  selected. */
  int selectionEnd() const { return caret_ < selectionAnchor_ ? selectionAnchor_ : caret_; }

  /** Select the whole text, the caret at its end. No callback. */
  void selectAll() {
    selectionAnchor_ = 0;
    caret_ = (int) text_.size();
    repaint();
  }

  /** The selected text, or empty. */
  std::string selectedText() const {
    if (!hasSelection()) return {};
    return toUtf8(std::vector<uint32_t>(text_.begin() + selectionStart(),
                                        text_.begin() + selectionEnd()));
  }

  /** Copies the SELECTION, not the whole field. Copying everything when
   *  nothing is selected is a thing some editors do and it is always a
   *  surprise. */
  bool copyToClipboard() const {
    if (!hasSelection()) return false;
    return Clipboard::setText(selectedText());
  }

  /**
   * Paste, replacing the selection.
   *
   * Every rule the field already has applies: the character filter, the length
   * limit, and control characters. A paste that bypassed them would be the one
   * way to get "12abc" into a numeric field, and pasting a multi-line block
   * into a single-line field would otherwise embed newlines nobody can see.
   */
  bool pasteFromClipboard() {
    if (readOnly_) return false;
    std::string incoming;
    if (!Clipboard::getText(&incoming) || incoming.empty()) return false;

    // Filtered BEFORE anything is touched: a paste the rules refuse entirely
    // must leave the selection it would have replaced. Deleting first erased
    // a selected number when letters were pasted over it, silently.
    std::vector<uint32_t> accepted;
    for (uint32_t c : fromUtf8(incoming)) {
      // A newline becomes a space rather than being dropped: pasting two lines
      // into a one-line field should give both words, not one.
      if (c == '\n' || c == '\r' || c == '\t') c = ' ';
      if (c < 0x20 || c == 0x7f) continue;
      if (!isAllowed(c)) continue;
      accepted.push_back(c);
    }
    if (accepted.empty()) return false;

    const bool replaced = hasSelection();
    if (replaced) deleteSelectionQuiet();
    bool any = false;
    for (uint32_t c : accepted) {
      if (maxLength_ > 0 && (int) text_.size() >= maxLength_) break;
      text_.insert(text_.begin() + caret_, c);
      ++caret_;
      any = true;
    }
    selectionAnchor_ = caret_;
    if (any || replaced) changed();
    return any;
  }

  // ── Keyboard ─────────────────────────────────────────────────────────────

  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled()) return false;
    // An input method consumes the keys while it composes; one that reaches
    // the field anyway commits the composition before it edits anything.
    commitComposition();

    switch (key.keyCode) {
      case KeyPress::Left:
        // With a selection and no shift, an arrow COLLAPSES it rather than
        // moving by one -- left goes to its start, right to its end. That is
        // what every text field on every desktop does, and a field that moved
        // by one instead feels broken in a way people cannot name.
        if (hasSelection() && !key.shiftDown) collapseSelection(/*toStart=*/true);
        else moveCaret((int) layout().previousStop((size_t) caret_), key.shiftDown);
        return true;
      case KeyPress::Right:
        if (hasSelection() && !key.shiftDown) collapseSelection(/*toStart=*/false);
        else moveCaret((int) layout().nextStop((size_t) caret_), key.shiftDown);
        return true;
      case KeyPress::Home:
      case KeyPress::Up:
        moveCaret(0, key.shiftDown);
        return true;
      case KeyPress::End:
      case KeyPress::Down:
        moveCaret((int) text_.size(), key.shiftDown);
        return true;
      case KeyPress::Backspace:
        if (readOnly_) return true;
        if (hasSelection()) deleteSelection();
        else if (caret_ > 0) {
          // One character back, so a mark typed by mistake goes and leaves
          // its letter -- but an emoji goes whole: a skin tone, a flag's
          // second letter or a family's last member left behind would draw
          // as something else.
          int from = caret_ - 1;
          const int start = (int) layout().previousStop((size_t) caret_);
          for (int i = start; i < caret_; ++i)
            if (detail::shaping::isEmojiPart(text_[(size_t) i])) from = start;
          text_.erase(text_.begin() + from, text_.begin() + caret_);
          caret_ = from;
          selectionAnchor_ = caret_;
        }
        changed();
        return true;
      case KeyPress::Delete:
        if (readOnly_) return true;
        if (hasSelection()) deleteSelection();
        // Forward, a whole cluster goes: a conjunct's or a letter's marks
        // left behind would be drawn on a dotted circle.
        else if (caret_ < (int) text_.size())
          text_.erase(text_.begin() + caret_,
                      text_.begin() + (long) (std::min)(layout().nextStop((size_t) caret_), text_.size()));
        changed();
        return true;
      case KeyPress::Return:
        if (onReturn) onReturn(getText());
        return true;
      case KeyPress::Escape:
        // NOT handled. A field that swallowed Escape would keep a menu or a
        // dialog open around it, and the walk up the tree exists precisely so
        // whatever contains this can close instead.
        return false;
      case KeyPress::Tab:
        // Also not handled: Tab belongs to focus traversal, which is the
        // router's job, and a field that ate it would trap the keyboard.
        return false;
      default:
        break;
    }

    // Ctrl on Windows and Linux, and this SDK maps Command to the same flag on
    // macOS, so one branch covers all three.
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
          if (!readOnly_) {
            copyToClipboard();
            deleteSelection();
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
    // Any other modified key is a shortcut belonging to somebody else -- the
    // host's, usually. Typing it into the field would be wrong twice.
    if (key.ctrlDown || key.altDown) return false;

    if (!key.isCharacter() || readOnly_) return false;
    // Control characters are not text. A tab or a bell arriving as a character
    // would be inserted and then drawn as a missing glyph. Nor is what no
    // character is: a surrogate, or past U+10FFFF.
    if (key.character < 0x20 || key.character == 0x7f) return false;
    if ((key.character >= 0xD800 && key.character <= 0xDFFF) || key.character > 0x10FFFF) return false;
    if (!isAllowed(key.character)) return true; // consumed, and refused
    insert(key.character);
    return true;
  }

  void focusGained() override { repaint(); }

  void focusLost() override {
    // What was being composed is what the user was typing: committed, as
    // every platform's own fields do, before the value is reported.
    commitComposition();
    // The other way a person commits a value: type it, then click elsewhere. A
    // field that only reported on Return would silently discard that. Last:
    // the handler is its owner's code, and committing a colour or a file name
    // may close the panel this field is in -- the repaint after it was a
    // write into the deleted field.
    repaint();
    if (onFocusLost) onFocusLost(getText());
  }

  // ── Mouse ────────────────────────────────────────────────────────────────

  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    commitComposition();
    const int at = indexAt(e.position.x);
    // A double click selects everything, which for a single-line field is what
    // "select the word under the cursor" degenerates to anyway.
    if (e.clickCount >= 2) {
      selectAll();
      return;
    }
    caret_ = at;
    selectionAnchor_ = at;
    repaint();
  }

  void mouseDrag(const MouseEvent& e) override {
    if (!isEnabled()) return;
    const int at = indexAt(e.position.x);
    if (at == caret_) return;
    // The anchor stays where the press was, so dragging back past it selects
    // the other way without the selection collapsing.
    caret_ = at;
    repaint();
  }

  // ── Painting ─────────────────────────────────────────────────────────────

  void paint(Graphics& g) override {
    LookAndFeel& lf = lookAndFeel();
    const Rect area = localBounds();
    lf.drawTextEditorBackground(g, area, hasKeyboardFocus(), isEnabled());

    const Rect inner = area.reduced(kPadding, 0.0f);
    if (inner.isEmpty()) return;

    Graphics::ScopedState clip(g);
    // Clipped, or a string longer than the field paints over whatever is next
    // to it -- which for a row of controls is the label of the next one.
    g.clipTo(inner);

    const std::string shown = shownUtf8();
    if (shown.empty() && !hasKeyboardFocus() && !placeholder_.empty()) {
      g.setColour(lf.dimText());
      font().drawIn(g, placeholder_, inner, Justify::Left);
      return;
    }

    const float baseline = inner.y + (inner.h + font().ascent() - font().descent()) * 0.5f;
    const float originX = inner.x - scrollX_;

    const CharacterLayout chars = layout();
    const TextComposition& composing = composition();
    if (hasSelection() && !composing.isActive()) {
      // The selected characters, which in right-to-left and mixed text are
      // not one span on the screen: each run of them that is, one rectangle.
      std::vector<std::pair<float, float>> spans;
      for (int i = selectionStart(); i < selectionEnd() && i < (int) chars.size(); ++i)
        spans.push_back({chars.left[(size_t) i], chars.right[(size_t) i]});
      std::sort(spans.begin(), spans.end());
      for (size_t k = 0; k < spans.size();) {
        float x0 = spans[k].first, x1 = spans[k].second;
        size_t j = k + 1;
        while (j < spans.size() && spans[j].first <= x1 + 0.01f) x1 = (std::max)(x1, spans[j++].second);
        lf.drawTextEditorSelection(g, Rect(originX + x0, inner.y + 2.0f, x1 - x0, inner.h - 4.0f));
        k = j;
      }
    }

    g.setColour(isEnabled() ? lf.text() : lf.dimText());
    font().draw(g, shown, originX, baseline);

    // The composition, underlined: the convention on every desktop for "the
    // input method has not finished with this". The clause being converted
    // gets a heavier line, as in the platforms' own text views.
    if (composing.isActive()) {
      const size_t from = (size_t) selectionStart();
      const float underline = (std::min)(baseline + 2.0f, inner.bottom() - 2.0f);
      for (size_t i = 0; i < composing.text.size() && from + i < chars.size(); ++i) {
        const size_t k = from + i;
        const float x0 = (std::min)(chars.left[k], chars.right[k]);
        const float x1 = (std::max)(chars.left[k], chars.right[k]);
        const bool clause = i >= composing.clauseStart && i < composing.clauseEnd;
        g.fillRect(Rect(originX + x0, underline, (std::max)(x1 - x0, 1.0f), clause ? 2.0f : 1.0f));
      }
    }

    // The caret is drawn only with focus. A field showing one when it has no
    // focus is telling the user their typing will land there, and it will not.
    // Not while a clause is being converted: the clause IS where the input
    // method is, and a caret beside it would say otherwise.
    const bool converting = composing.isActive() && composing.clauseEnd > composing.clauseStart;
    if (hasKeyboardFocus() && !readOnly_ && !converting) {
      const float cx = originX + chars.caretX(shownCaret());
      lf.drawTextEditorCaret(g, Rect(cx, inner.y + 3.0f, 1.0f, inner.h - 6.0f));
    }
  }

  void resized() override { scrollIntoView(); }

  /** The inset of the text from the field's left and right edges, logical
   *  pixels. */
  static constexpr float kPadding = 6.0f;

protected:
  void compositionChanged() override {
    scrollIntoView();
    repaint();
  }

private:
  void changed() {
    scrollIntoView();
    repaint();
    if (onTextChange) onTextChange(getText());
  }

  /** What is on screen: the value, with any composition in it. */
  std::string shownUtf8() const {
    if (!composition().isActive()) return getText();
    return toUtf8(TextInputSession::shownText(*this));
  }

  size_t shownCaret() const { return TextInputSession::shownCaret(*this, (size_t) caret_); }

  void insert(uint32_t c) {
    if (hasSelection()) deleteSelectionQuiet();
    if (maxLength_ > 0 && (int) text_.size() >= maxLength_) return;
    text_.insert(text_.begin() + caret_, c);
    ++caret_;
    selectionAnchor_ = caret_;
    changed();
  }

  void deleteSelection() { deleteSelectionQuiet(); }

  void deleteSelectionQuiet() {
    const int from = selectionStart(), to = selectionEnd();
    if (from == to) return;
    text_.erase(text_.begin() + from, text_.begin() + to);
    caret_ = from;
    selectionAnchor_ = from;
  }

  /** The first version of this lived inside moveCaret and asked whether the
   *  NEW caret equalled the old one -- which it never does, so the branch was
   *  dead and a plain Left over a selection moved by one character instead of
   *  collapsing. Out here it cannot be written that way. */
  void collapseSelection(bool toStart) {
    caret_ = toStart ? selectionStart() : selectionEnd();
    selectionAnchor_ = caret_;
    scrollIntoView();
    repaint();
  }

  void moveCaret(int to, bool extendSelection) {
    const int clamped = to < 0 ? 0 : (to > (int) text_.size() ? (int) text_.size() : to);
    caret_ = clamped;
    if (!extendSelection) selectionAnchor_ = caret_;
    scrollIntoView();
    repaint();
  }

  bool isAllowed(uint32_t c) const {
    if (allowed_.empty()) return true;
    return std::find(allowed_.begin(), allowed_.end(), c) != allowed_.end();
  }

  /** Where each character is drawn: the caret, a click, the arrows and the
   *  selection all ask this, which shapes the line once -- measuring every
   *  prefix, as this did, shaped each one again, and put a caret by an
   *  Arabic or Hindi word where the prefix alone would end. */
  CharacterLayout layout() const { return font().layoutCharacters(shownUtf8()); }

  /** The caret index nearest an x in local coordinates. Nearest EDGE, not
   *  nearest character: clicking the right half of a letter puts the caret
   *  after it, which is what a person means -- on the left, in a
   *  right-to-left word. Never inside a cluster. */
  int indexAt(float x) const {
    const size_t at = layout().indexAt(x - (localBounds().x + kPadding) + scrollX_);
    return (int) (std::min)(at, text_.size());
  }

  /** Keep the caret inside the visible part of a string longer than the field.
   *  Without this, typing past the right edge carries on into pixels nobody
   *  can see and the user is typing blind. */
  void scrollIntoView() {
    const float inner = localBounds().w - kPadding * 2.0f;
    if (inner <= 0.0f) return;
    const CharacterLayout chars = layout();
    const float caretX = chars.caretX(shownCaret());
    if (caretX - scrollX_ > inner) scrollX_ = caretX - inner;
    if (caretX - scrollX_ < 0.0f) scrollX_ = caretX;
    const float total = chars.width;
    // Never scrolled so far that empty space shows on the right while text
    // hangs off the left.
    if (scrollX_ > total - inner) scrollX_ = total - inner;
    if (scrollX_ < 0.0f) scrollX_ = 0.0f;
  }

  // ── UTF-8 at the edges only ──────────────────────────────────────────────

  static std::vector<uint32_t> fromUtf8(const std::string& in) {
    std::vector<uint32_t> out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
      const unsigned char c = (unsigned char) in[i];
      uint32_t cp = c;
      int extra = 0;
      // A byte that cannot start a character -- a stray continuation, or one
      // of the leads UTF-8 never uses -- is not one.
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
      // A broken sequence is one U+FFFD and the walk resumes at the next
      // byte, so the rest of the string survives. Keeping the lead byte with
      // its length bits masked off gave control characters: "abc\xC3" was
      // "abc" and U+0003.
      if (!ok) {
        out.push_back(0xFFFDu);
        ++i;
        continue;
      }
      // Decoded, but no character: a surrogate, or past U+10FFFF.
      if ((cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) cp = 0xFFFDu;
      out.push_back(cp);
      i += (size_t) extra + 1;
    }
    return out;
  }

  static std::string toUtf8(const std::vector<uint32_t>& in) {
    std::string out;
    out.reserve(in.size());
    for (uint32_t cp : in) {
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
  std::vector<uint32_t> allowed_;
  std::string placeholder_;
  int caret_ = 0;
  int selectionAnchor_ = 0;
  int maxLength_ = 0;
  float scrollX_ = 0.0f;
  bool readOnly_ = false;
};

} // namespace gfx
} // namespace sonore
