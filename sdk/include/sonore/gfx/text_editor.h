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
#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "clipboard.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

class TextEditor : public Widget {
public:
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

  void setText(const std::string& utf8, bool notify = false) {
    text_ = fromUtf8(utf8);
    caret_ = (int) text_.size();
    selectionAnchor_ = caret_;
    repaint();
    if (notify && onTextChange) onTextChange(getText());
  }

  std::string getText() const { return toUtf8(text_); }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::TextField);
    info.value = getText();
    // The placeholder is what the field is FOR when it is empty, which is
    // exactly the moment a reader has nothing else to say about it.
    if (info.name.empty()) info.name = placeholder_;
    return info;
  }
  bool isEmpty() const { return text_.empty(); }

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

  void setReadOnly(bool readOnly) {
    readOnly_ = readOnly;
    // A read-only field still takes focus: text in one has to be selectable,
    // or it cannot be copied.
    repaint();
  }
  bool isReadOnly() const { return readOnly_; }

  int caretPosition() const { return caret_; }

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
    const Rect inner = localBounds().reduced(kPadding, 0.0f);
    const float cx = inner.x - scrollX_ + widthOf(0, caret_);
    *out = Rect(cx, inner.y + 3.0f, 1.0f, inner.h - 6.0f);
    return true;
  }
  bool hasSelection() const { return caret_ != selectionAnchor_; }
  int selectionStart() const { return caret_ < selectionAnchor_ ? caret_ : selectionAnchor_; }
  int selectionEnd() const { return caret_ < selectionAnchor_ ? selectionAnchor_ : caret_; }

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

    if (hasSelection()) deleteSelectionQuiet();
    bool any = false;
    for (uint32_t c : fromUtf8(incoming)) {
      // A newline becomes a space rather than being dropped: pasting two lines
      // into a one-line field should give both words, not one.
      if (c == '\n' || c == '\r' || c == '\t') c = ' ';
      if (c < 0x20 || c == 0x7f) continue;
      if (!isAllowed(c)) continue;
      if (maxLength_ > 0 && (int) text_.size() >= maxLength_) break;
      text_.insert(text_.begin() + caret_, c);
      ++caret_;
      any = true;
    }
    selectionAnchor_ = caret_;
    if (any) changed();
    return any;
  }

  // ── Keyboard ─────────────────────────────────────────────────────────────

  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled()) return false;

    switch (key.keyCode) {
      case KeyPress::Left:
        // With a selection and no shift, an arrow COLLAPSES it rather than
        // moving by one -- left goes to its start, right to its end. That is
        // what every text field on every desktop does, and a field that moved
        // by one instead feels broken in a way people cannot name.
        if (hasSelection() && !key.shiftDown) collapseSelection(/*toStart=*/true);
        else moveCaret(caret_ - 1, key.shiftDown);
        return true;
      case KeyPress::Right:
        if (hasSelection() && !key.shiftDown) collapseSelection(/*toStart=*/false);
        else moveCaret(caret_ + 1, key.shiftDown);
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
          text_.erase(text_.begin() + (caret_ - 1));
          --caret_;
          selectionAnchor_ = caret_;
        }
        changed();
        return true;
      case KeyPress::Delete:
        if (readOnly_) return true;
        if (hasSelection()) deleteSelection();
        else if (caret_ < (int) text_.size()) text_.erase(text_.begin() + caret_);
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
    // would be inserted and then drawn as a missing glyph.
    if (key.character < 0x20 || key.character == 0x7f) return false;
    if (!isAllowed(key.character)) return true; // consumed, and refused
    insert(key.character);
    return true;
  }

  void focusGained() override { repaint(); }

  void focusLost() override {
    // The other way a person commits a value: type it, then click elsewhere. A
    // field that only reported on Return would silently discard that.
    if (onFocusLost) onFocusLost(getText());
    repaint();
  }

  // ── Mouse ────────────────────────────────────────────────────────────────

  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
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

    const std::string shown = getText();
    if (shown.empty() && !hasKeyboardFocus() && !placeholder_.empty()) {
      g.setColour(lf.dimText());
      font().drawIn(g, placeholder_, inner, Justify::Left);
      return;
    }

    const float baseline = inner.y + (inner.h + font().ascent() - font().descent()) * 0.5f;
    const float originX = inner.x - scrollX_;

    if (hasSelection()) {
      const float x0 = originX + widthOf(0, selectionStart());
      const float x1 = originX + widthOf(0, selectionEnd());
      lf.drawTextEditorSelection(g, Rect(x0, inner.y + 2.0f, x1 - x0, inner.h - 4.0f));
    }

    g.setColour(isEnabled() ? lf.text() : lf.dimText());
    font().draw(g, shown, originX, baseline);

    // The caret is drawn only with focus. A field showing one when it has no
    // focus is telling the user their typing will land there, and it will not.
    if (hasKeyboardFocus() && !readOnly_) {
      const float cx = originX + widthOf(0, caret_);
      lf.drawTextEditorCaret(g, Rect(cx, inner.y + 3.0f, 1.0f, inner.h - 6.0f));
    }
  }

  void resized() override { scrollIntoView(); }

  static constexpr float kPadding = 6.0f;

private:
  void changed() {
    scrollIntoView();
    repaint();
    if (onTextChange) onTextChange(getText());
  }

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

  float widthOf(int from, int to) const {
    if (to <= from) return 0.0f;
    return font().stringWidth(toUtf8(std::vector<uint32_t>(text_.begin() + from,
                                                           text_.begin() + to)));
  }

  /** The caret index nearest an x in local coordinates. Nearest EDGE, not
   *  nearest character: clicking the right half of a letter puts the caret
   *  after it, which is what a person means. */
  int indexAt(float x) const {
    const float target = x - (localBounds().x + kPadding) + scrollX_;
    float best = 1e30f;
    int bestIndex = 0;
    for (int i = 0; i <= (int) text_.size(); ++i) {
      const float d = std::fabs(widthOf(0, i) - target);
      if (d < best) {
        best = d;
        bestIndex = i;
      }
    }
    return bestIndex;
  }

  /** Keep the caret inside the visible part of a string longer than the field.
   *  Without this, typing past the right edge carries on into pixels nobody
   *  can see and the user is typing blind. */
  void scrollIntoView() {
    const float inner = localBounds().w - kPadding * 2.0f;
    if (inner <= 0.0f) return;
    const float caretX = widthOf(0, caret_);
    if (caretX - scrollX_ > inner) scrollX_ = caretX - inner;
    if (caretX - scrollX_ < 0.0f) scrollX_ = caretX;
    const float total = widthOf(0, (int) text_.size());
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
      // A truncated sequence takes the lead byte as-is and moves on. Returning
      // early instead would silently drop the rest of the string.
      if (i + (size_t) extra >= in.size()) extra = 0;
      for (int k = 0; k < extra; ++k) cp = (cp << 6) | ((unsigned char) in[i + 1 + (size_t) k] & 0x3fu);
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
