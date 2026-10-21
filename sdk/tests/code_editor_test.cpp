// SPDX-License-Identifier: Apache-2.0
// The code editor: CodeDocument, the three tokenisers, CodeEditorComponent.
//
// What is checked, with real numbers:
//   * thousands of random inserts, deletes and replaces, with transactions,
//     undo and redo, against a std::u32string and a stack of its snapshots;
//   * maintained positions through thousands of random edits, against
//     offsets moved by the rules a caret is moved by;
//   * line endings: mixed files saved byte for byte, a break counted once,
//     a CR break and an LF break kept apart through undo, normalising and
//     undoing it, UTF-8 and UTF-16 with byte order marks, a file round trip;
//   * each tokeniser on sample code, token by token, comments and strings
//     across lines included -- and the state-only scan agreeing with the
//     full one;
//   * the editor driven by KeyPress: selection, word moves, Home, up and
//     down through short lines, indent and outdent, auto-indent, backspace to
//     a tab stop, undo of a run of typing, the clipboard, read-only, find;
//   * rendering to a Bitmap: digits in the gutter, tokens in their colours;
//   * a hundred thousand lines: open, scroll to the end, edit there, undo,
//     each timed.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <sonore/gfx/bitmap.h>
#include <sonore/gfx/code_editor.h>
#include <sonore/gfx/graphics.h>

#include "temp_path.h"

using namespace sonore::gfx;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* msg) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", msg);
}

#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__)
constexpr bool kTimed = true;
#else
constexpr bool kTimed = false; // unoptimised or sanitised: report, do not fail
#endif

std::u32string u32(const std::string& s) { return detail::code::decodeText(s); }
std::string utf8(const std::u32string& s) { return detail::code::encode(s.data(), s.size()); }

// ── Tokens ───────────────────────────────────────────────────────────────────

struct Tok {
  std::string text;
  int type;
};

/** Tokenises lines with the state carried, and checks that the state-only
 *  scan ends every line in the same state. */
std::vector<Tok> tokenise(const CodeTokeniser& t, const std::vector<std::string>& lines, bool* agrees) {
  std::vector<Tok> all;
  uint32_t state = 0, quick = 0;
  bool same = true;
  for (const std::string& line : lines) {
    const std::u32string u = u32(line);
    std::vector<CodeTokeniser::Token> toks;
    const uint32_t next = t.tokeniseLine(u.data(), (int) u.size(), state, &toks);
    const uint32_t nextQuick = t.tokeniseLine(u.data(), (int) u.size(), quick, nullptr);
    if (next != nextQuick) same = false;
    for (const CodeTokeniser::Token& k : toks) all.push_back({utf8(u.substr((size_t) k.start, (size_t) k.length)), k.type});
    state = next;
    quick = nextQuick;
  }
  if (agrees) *agrees = same;
  return all;
}

void expectTokens(const char* what, const CodeTokeniser& t, const std::vector<std::string>& lines,
                  const std::vector<Tok>& want) {
  bool agrees = false;
  const std::vector<Tok> got = tokenise(t, lines, &agrees);
  bool ok = agrees && got.size() == want.size();
  for (size_t i = 0; ok && i < got.size(); ++i) ok = got[i].text == want[i].text && got[i].type == want[i].type;
  char msg[512];
  std::snprintf(msg, sizeof(msg), "%s: %d tokens as expected%s", what, (int) want.size(),
                agrees ? "" : " (state-only scan DISAGREES)");
  check(ok, msg);
  if (!ok) {
    std::printf("       got:");
    for (const Tok& k : got) std::printf(" [%s|%d]", k.text.c_str(), k.type);
    std::printf("\n      want:");
    for (const Tok& k : want) std::printf(" [%s|%d]", k.text.c_str(), k.type);
    std::printf("\n");
  }
}

// ── Keys ─────────────────────────────────────────────────────────────────────

bool press(CodeEditorComponent& ed, int code, bool shift = false, bool ctrl = false) {
  KeyPress k;
  k.keyCode = code;
  k.shiftDown = shift;
  k.ctrlDown = ctrl;
  return ed.keyPressed(k);
}
bool chord(CodeEditorComponent& ed, char c, bool shift = false) {
  KeyPress k;
  k.character = (uint32_t) (unsigned char) c;
  k.ctrlDown = true;
  k.shiftDown = shift;
  return ed.keyPressed(k);
}
void type(CodeEditorComponent& ed, const std::string& text) {
  for (char32_t c : u32(text)) {
    KeyPress k;
    k.character = (uint32_t) c;
    ed.keyPressed(k);
  }
}
bool at(const CodeEditorComponent& ed, int line, int index) {
  return ed.getCaretPos().getLineNumber() == line && ed.getCaretPos().getIndexInLine() == index;
}

double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// ═════════════════════════════════════════════════════════════════════════════

void testDocumentRoundTrips() {
  std::printf("\n-- CodeDocument: random edits, undo and redo against a model --\n");
  std::mt19937 rng(20261013u);
  static const char* const kPieces[] = {"a", "b", "xyz", " ", "\t", "\n", "\r\n", "\r", "\xC3\xA9",
                                        "\xE6\x97\xA5", "\xF0\x9F\x98\x80", "e\xCC\x81", "{", "}"};
  auto randomText = [&]() {
    std::string s;
    const int pieces = (int) (rng() % 7);
    for (int i = 0; i < pieces; ++i) s += kPieces[rng() % (sizeof(kPieces) / sizeof(kPieces[0]))];
    return s;
  };
  CodeDocument doc;
  std::u32string model;
  std::vector<std::u32string> undoStack, redoStack;
  bool open = false;
  auto opening = [&] {
    if (!open) {
      undoStack.push_back(model);
      redoStack.clear();
      open = true;
    }
  };
  int mismatches = 0, positionMismatches = 0, undos = 0, redos = 0, steps = 6000;
  for (int step = 0; step < steps; ++step) {
    const int op = (int) (rng() % 100);
    const int size = (int) model.size();
    if (op < 40) {
      const int off = (int) (rng() % (uint32_t) (size + 1));
      const std::string text = randomText();
      const std::u32string u = u32(text);
      if (!u.empty()) {
        opening();
        model.insert((size_t) off, u);
      }
      doc.insertText(off, text);
    } else if (op < 62) {
      int a = (int) (rng() % (uint32_t) (size + 1)), b = (int) (rng() % (uint32_t) (size + 1));
      if (rng() % 2) b = (std::min)(size, a + (int) (rng() % 4)); // short ones, like backspace
      if (a > b) std::swap(a, b);
      if (a != b) {
        opening();
        model.erase((size_t) a, (size_t) (b - a));
      }
      doc.deleteSection(b, a); // either order
    } else if (op < 70) {
      int a = (int) (rng() % (uint32_t) (size + 1)), b = (int) (rng() % (uint32_t) (size + 1));
      if (a > b) std::swap(a, b);
      const std::string text = randomText();
      const std::u32string u = u32(text);
      if (a != b || !u.empty()) {
        opening();
        model.replace((size_t) a, (size_t) (b - a), u);
      }
      doc.replaceSection(a, b, text);
    } else if (op < 80) {
      doc.newTransaction();
      open = false;
    } else if (op < 91) {
      const bool did = doc.undo();
      open = false;
      if (did != !undoStack.empty()) ++mismatches;
      if (!undoStack.empty()) {
        redoStack.push_back(model);
        model = undoStack.back();
        undoStack.pop_back();
        ++undos;
      }
    } else {
      const bool did = doc.redo();
      open = false;
      if (did != !redoStack.empty()) ++mismatches;
      if (!redoStack.empty()) {
        undoStack.push_back(model);
        model = redoStack.back();
        redoStack.pop_back();
        ++redos;
      }
    }
    int lines = 1;
    for (char32_t c : model)
      if (c == '\n') ++lines;
    if (doc.getAllContent() != utf8(model) || doc.getNumCharacters() != (int) model.size() || doc.getNumLines() != lines)
      ++mismatches;
    // Offsets and positions agree, both ways, at a few random places.
    for (int k = 0; k < 3; ++k) {
      const int off = (int) (rng() % (uint32_t) (model.size() + 1));
      const CodeDocument::Position p(doc, off);
      if (p.getPosition() != off) ++positionMismatches;
      const char32_t want = off < (int) model.size() ? model[(size_t) off] : 0;
      if (p.getCharacter() != want) ++positionMismatches;
    }
  }
  char msg[256];
  std::snprintf(msg, sizeof(msg), "%d steps (%d undos, %d redos): text, size and lines match the model every step "
                "(%d mismatches)", steps, undos, redos, mismatches);
  check(mismatches == 0 && undos > 300 && redos > 50, msg);
  std::snprintf(msg, sizeof(msg), "offset -> position -> offset and the character there agree (%d mismatches)",
                positionMismatches);
  check(positionMismatches == 0, msg);

  // Undo everything, then redo everything.
  const std::string end = doc.getAllContent();
  int back = 0;
  while (doc.undo()) ++back;
  const bool emptyAtStart = doc.getAllContent() == utf8(undoStack.empty() ? model : undoStack.front());
  int forward = 0;
  while (doc.redo()) ++forward;
  std::snprintf(msg, sizeof(msg), "all %d transactions undone to the start, and redone to the same text", back);
  check(emptyAtStart && back == forward && doc.getAllContent() == end, msg);
}

void testPositions() {
  std::printf("\n-- CodeDocument::Position: maintained through edits --\n");
  std::mt19937 rng(77u);
  CodeDocument doc;
  doc.loadFromText("first line\nsecond\n\nfourth line here\nfifth");
  std::vector<std::unique_ptr<CodeDocument::Position>> ps;
  std::vector<int> model;
  for (int off : {0, 7, 11, 30}) {
    ps.push_back(std::make_unique<CodeDocument::Position>(doc, off));
    ps.back()->setPositionMaintained(true);
    model.push_back(off);
  }
  static const char* const kPieces[] = {"x", "yz", "\n", "\r\n", " ", "\xE6\x97\xA5"};
  int wrong = 0;
  for (int step = 0; step < 4000; ++step) {
    const int size = doc.getNumCharacters();
    if (rng() % 2) {
      const int off = (int) (rng() % (uint32_t) (size + 1));
      std::string text;
      for (int i = (int) (rng() % 4); i >= 0; --i) text += kPieces[rng() % 6];
      const int m = (int) u32(text).size();
      doc.insertText(off, text);
      for (int& p : model)
        if (p >= off) p += m;
    } else {
      int a = (int) (rng() % (uint32_t) (size + 1)), b = (std::min)(size, a + (int) (rng() % 6));
      doc.deleteSection(a, b);
      for (int& p : model) {
        if (p >= b) p -= b - a;
        else if (p > a) p = a;
      }
    }
    for (size_t i = 0; i < ps.size(); ++i)
      if (ps[i]->getPosition() != model[i]) ++wrong;
  }
  char msg[200];
  std::snprintf(msg, sizeof(msg), "four maintained positions through 4000 random edits: %d disagreements with the "
                "offset rules", wrong);
  check(wrong == 0, msg);

  doc.loadFromText("abc\ndef");
  CodeDocument::Position caret(doc, 1, 1);
  caret.setPositionMaintained(true);
  doc.insertText(CodeDocument::Position(doc, 1, 1), "XY");
  const bool pushed = caret.getLineNumber() == 1 && caret.getIndexInLine() == 3;
  doc.deleteSection(CodeDocument::Position(doc, 0, 1), CodeDocument::Position(doc, 1, 2));
  const bool collapsed = caret.getLineNumber() == 0 && caret.getIndexInLine() == 2; // "a" + "Y|ef"
  const CodeDocument::Position copy(caret);
  CodeDocument::Position assigned(doc, 0, 0);
  assigned.setPositionMaintained(true);
  assigned = copy;
  doc.insertText(0, "__");
  check(pushed && collapsed, "text inserted at a maintained position pushes it; deleted around it, collapses it");
  check(!copy.isPositionMaintained() && copy.getIndexInLine() == 2 && assigned.isPositionMaintained() &&
            assigned.getIndexInLine() == 4,
        "a copy is not maintained; assigning to a maintained position keeps it maintained");
  check(caret.movedBy(-3).getPosition() == caret.getPosition() - 3 && caret.movedByLines(5).getLineNumber() == 0,
        "movedBy walks characters; movedByLines stays inside the document");

  // A position that outlives its document is let go, not left dangling.
  CodeDocument::Position orphan;
  {
    CodeDocument temporary;
    temporary.loadFromText("hello");
    orphan = CodeDocument::Position(temporary, 0, 3);
    orphan.setPositionMaintained(true);
  }
  check(orphan.getOwner() == nullptr && !orphan.isPositionMaintained() && orphan.getPosition() == 0,
        "a maintained position outliving its document is released by it");
}

void testLineEndings() {
  std::printf("\n-- CodeDocument: line endings, encodings, files --\n");
  CodeDocument doc;
  const std::string mixed = "one\r\ntwo\nthree\rfour";
  doc.loadFromText(mixed);
  check(doc.getNumLines() == 4 && doc.getLineEnding(0) == CodeDocument::LineEnding::CRLF &&
            doc.getLineEnding(1) == CodeDocument::LineEnding::LF && doc.getLineEnding(2) == CodeDocument::LineEnding::CR &&
            doc.getLineEnding(3) == CodeDocument::LineEnding::None,
        "CR LF, LF and a lone CR each end a line, and each line remembers which");
  check(doc.getAllContent() == mixed && doc.hasMixedLineEndings(), "a mixed file comes back byte for byte");
  check(doc.getNumCharacters() == 18 && doc.getLineStart(1) == 4 && doc.getLineStart(3) == 14 &&
            doc.getTextBetween(0, 18) == "one\ntwo\nthree\nfour",
        "a break is one character in an offset, and LF in getTextBetween, however it is saved");
  check(!doc.hasChangedSinceSavePoint(), "a loaded document is at its save point");

  doc.insertText(4, "X\nY");
  check(doc.getAllContent() == "one\r\nX\nYtwo\nthree\rfour", "a new break takes the document's newline");
  const std::string before = doc.getAllContent();
  doc.newTransaction();
  doc.deleteSection(3, 4); // the CR LF after "one"
  check(doc.getAllContent() == "oneX\nYtwo\nthree\rfour", "deleting a CR LF takes both bytes");
  doc.undo();
  check(doc.getAllContent() == before, "undo puts the CR LF back, not a newline of the document's");

  doc.setNewLineCharacters("\r\n");
  doc.normaliseLineEndings();
  check(doc.getAllContent() == "one\r\nX\r\nYtwo\r\nthree\r\nfour" && !doc.hasMixedLineEndings(),
        "normaliseLineEndings makes every break the newline");
  doc.undo();
  check(doc.getAllContent() == before, "and is undone in one step");

  // A CR break followed by an LF break is two breaks, through undo and redo
  // -- in bytes they would read as one CR LF, so the record must not hold
  // them as bytes.
  doc.loadFromText("");
  doc.setNewLineCharacters("\r");
  doc.insertText(0, "a\n");
  doc.setNewLineCharacters("\n");
  doc.insertText(2, "\nb");
  const bool three = doc.getNumLines() == 3 && doc.getLineEnding(0) == CodeDocument::LineEnding::CR &&
                     doc.getLineEnding(1) == CodeDocument::LineEnding::LF;
  doc.newTransaction();
  doc.deleteSection(1, 3);
  doc.undo();
  const bool restored = doc.getNumLines() == 3 && doc.getLineEnding(0) == CodeDocument::LineEnding::CR &&
                        doc.getLineEnding(1) == CodeDocument::LineEnding::LF && doc.getNumCharacters() == 4;
  doc.redo();
  doc.undo();
  check(three && restored && doc.getNumLines() == 3, "a CR break then an LF break stay two breaks through undo and redo");

  // The newline follows the file.
  doc.loadFromText("a\r\nb\r\nc\nd");
  check(doc.getNewLineCharacters() == "\r\n", "loading makes the commonest ending the newline");

  // Encodings.
  doc.loadFromText("\xEF\xBB\xBFh\xC3\xA9\r\n");
  check(doc.getEncoding() == CodeDocument::Encoding::Utf8WithBom && doc.getLine(0) == "h\xC3\xA9" &&
            doc.getEncodedContent() == "\xEF\xBB\xBFh\xC3\xA9\r\n",
        "a UTF-8 byte order mark is read, kept out of the text, and written back");
  const std::string utf16("\xFF\xFE" "h\0\xE9\0\x3D\xD8\x00\xDE\r\0\n\0z\0", 16);
  doc.loadFromText(utf16);
  check(doc.getEncoding() == CodeDocument::Encoding::Utf16LE && doc.getLine(0) == "h\xC3\xA9\xF0\x9F\x98\x80" &&
            doc.getLine(1) == "z" && doc.getEncodedContent() == utf16,
        "UTF-16 LE with a surrogate pair reads, and writes back the same bytes");
  doc.loadFromText("ok\xFF\xC0\x80!");
  check(doc.getLine(0) == "ok\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD!", "a bad byte, and an overlong NUL, each load as U+FFFD");

  // A file, through a path.
  const char* path = sonoretest::tempPath("code_editor_test.cpp.txt");
  doc.loadFromText("int a;\r\n// \xE6\x97\xA5\nint b;");
  doc.insertText(0, "x");
  const bool dirty = doc.hasChangedSinceSavePoint();
  const bool saved = doc.saveToFile(path);
  CodeDocument reread;
  const bool loaded = reread.loadFromFile(path);
  check(dirty && saved && !doc.hasChangedSinceSavePoint() && loaded && reread.getAllContent() == doc.getAllContent(),
        "saveToFile writes what loadFromFile reads, and sets the save point");
  check(!reread.loadFromFile(std::string(path) + ".missing") && reread.getNumLines() == 3,
        "a missing file is refused and the document left as it was");
}

void testHistoryAndLimits() {
  std::printf("\n-- CodeDocument: save point, undo limits, maximum size, listeners --\n");
  CodeDocument doc;
  doc.loadFromText("abc");
  doc.insertText(3, "d");
  doc.newTransaction();
  doc.setSavePoint();
  doc.insertText(4, "e");
  const bool changed = doc.hasChangedSinceSavePoint();
  doc.undo();
  const bool back = !doc.hasChangedSinceSavePoint();
  doc.undo();
  const bool before = doc.hasChangedSinceSavePoint();
  doc.redo();
  check(changed && back && before && !doc.hasChangedSinceSavePoint(),
        "the save point: changed after an edit, unchanged when undo or redo comes back to it");

  doc.loadFromText("");
  doc.setUndoLimits(3, 1000000);
  for (int i = 0; i < 6; ++i) {
    doc.insertText(doc.getNumCharacters(), std::to_string(i));
    doc.newTransaction();
  }
  int undone = 0;
  while (doc.undo()) ++undone;
  check(undone == 3 && doc.getAllContent() == "012", "with a limit of three, the oldest steps go");

  doc.loadFromText("hello");
  doc.setMaximumSize(10);
  const bool refused = !doc.insertText(5, " world");
  const bool fits = doc.insertText(5, "12345");
  const bool replaceRefused = !doc.replaceSection(0, 1, "XX") && doc.getAllContent() == "hello12345";
  const bool loadRefused = !doc.loadFromText("this is far too long") && doc.getAllContent() == "hello12345";
  check(refused && fits && replaceRefused && loadRefused,
        "the maximum size refuses an insert, a replace and a load whole, leaving the text");
  doc.setMaximumSize(CodeDocument::kDefaultMaximumSize);

  struct Recorder : CodeDocument::Listener {
    std::string log;
    void codeDocumentTextInserted(int a, int b) override { log += "+" + std::to_string(a) + "," + std::to_string(b) + " "; }
    void codeDocumentTextDeleted(int a, int b) override { log += "-" + std::to_string(a) + "," + std::to_string(b) + " "; }
    void codeDocumentLinesChanged(int line, int delta) override {
      log += "L" + std::to_string(line) + "," + std::to_string(delta) + " ";
    }
  } rec;
  doc.loadFromText("ab\ncd");
  doc.addListener(&rec);
  doc.insertText(4, "X\nY"); // in "cd"
  doc.newTransaction();
  doc.deleteSection(0, 3);
  doc.undo();
  doc.removeListener(&rec);
  doc.insertText(0, "unheard");
  char msg[256];
  std::snprintf(msg, sizeof(msg), "listeners hear lines then offsets, undo included: %s", rec.log.c_str());
  check(rec.log == "L1,1 +4,7 L0,-1 -0,3 L0,1 +0,3 ", msg);

  // Find.
  doc.loadFromText("alpha beta\nGamma alpha\nbeta gamma");
  CodeDocument::Position s, e;
  const bool f1 = doc.find("alpha", CodeDocument::Position(doc, 0, 1), true, true, false, &s, &e) &&
                  s.getLineNumber() == 1 && s.getIndexInLine() == 6 && e.getIndexInLine() == 11;
  const bool f2 = doc.find("gamma", CodeDocument::Position(doc, 0, 0), true, false, false, &s, &e) &&
                  s.getLineNumber() == 1 && s.getIndexInLine() == 0;
  const bool f3 = doc.find("alpha\nbeta", CodeDocument::Position(doc, 0, 0), true, true, false, &s, &e) &&
                  s.getLineNumber() == 1 && s.getIndexInLine() == 6 && e.getLineNumber() == 2 && e.getIndexInLine() == 4;
  const bool f4 = doc.find("beta", CodeDocument::Position(doc, 2, 0), false, true, false, &s, &e) &&
                  s.getLineNumber() == 0 && s.getIndexInLine() == 6;
  const bool f5 = doc.find("alpha", CodeDocument::Position(doc, 2, 0), true, true, true, &s, &e) &&
                  s.getLineNumber() == 0 && s.getIndexInLine() == 0;
  const bool f6 = !doc.find("alpha", CodeDocument::Position(doc, 2, 0), true, true, false, &s, &e);
  check(f1 && f2 && f3 && f4 && f5 && f6,
        "find: forwards, ignoring case, across a line break, backwards, round the end, and not round it");

  doc.loadFromText("foo_bar(baz)  qux");
  CodeDocument::Position a(doc, 0, 0);
  const int w1 = doc.findWordBreakAfter(a).getIndexInLine();
  const int w2 = doc.findWordBreakAfter(CodeDocument::Position(doc, 0, 12)).getIndexInLine();
  const int w3 = doc.findWordBreakBefore(CodeDocument::Position(doc, 0, 15)).getIndexInLine();
  doc.findTokenContaining(CodeDocument::Position(doc, 0, 9), s, e);
  std::snprintf(msg, sizeof(msg), "word breaks: after 0 -> %d, after 12 -> %d, before 15 -> %d; token at 9 is [%d,%d)",
                w1, w2, w3, s.getIndexInLine(), e.getIndexInLine());
  check(w1 == 7 && w2 == 14 && w3 == 14 && s.getIndexInLine() == 8 && e.getIndexInLine() == 11, msg);
}

void testTokenisers() {
  std::printf("\n-- Tokenisers --\n");
  using C = CPlusPlusCodeTokeniser;
  C cpp;
  expectTokens("C++ declaration and line comment", cpp, {"int x = 42; // hi"},
               {{"int", C::tokenType_keyword}, {"x", C::tokenType_identifier}, {"=", C::tokenType_operator},
                {"42", C::tokenType_integer}, {";", C::tokenType_punctuation}, {"// hi", C::tokenType_comment}});
  expectTokens("C++ numbers", cpp, {"float f = 1.5e-3f + 0x1Fu - 0b101 + 1'000'000ULL + .5;"},
               {{"float", C::tokenType_keyword}, {"f", C::tokenType_identifier}, {"=", C::tokenType_operator},
                {"1.5e-3f", C::tokenType_float}, {"+", C::tokenType_operator}, {"0x1Fu", C::tokenType_integer},
                {"-", C::tokenType_operator}, {"0b101", C::tokenType_integer}, {"+", C::tokenType_operator},
                {"1'000'000ULL", C::tokenType_integer}, {"+", C::tokenType_operator}, {".5", C::tokenType_float},
                {";", C::tokenType_punctuation}});
  expectTokens("C++ directive, stopped by a comment", cpp, {"#include <vector> // c"},
               {{"#include <vector> ", C::tokenType_preprocessor}, {"// c", C::tokenType_comment}});
  expectTokens("C++ block comment over three lines", cpp, {"a /* b", "c", "d */ e"},
               {{"a", C::tokenType_identifier}, {"/* b", C::tokenType_comment}, {"c", C::tokenType_comment},
                {"d */", C::tokenType_comment}, {"e", C::tokenType_identifier}});
  expectTokens("C++ raw string with a delimiter over three lines", cpp, {"auto s = R\"x(one", ")\" still", "two)x\";"},
               {{"auto", C::tokenType_keyword}, {"s", C::tokenType_identifier}, {"=", C::tokenType_operator},
                {"R\"x(one", C::tokenType_string}, {")\" still", C::tokenType_string}, {"two)x\"", C::tokenType_string},
                {";", C::tokenType_punctuation}});
  expectTokens("C++ string continued by a backslash", cpp, {"const char* s = \"abc\\", "def\";"},
               {{"const", C::tokenType_keyword}, {"char", C::tokenType_keyword}, {"*", C::tokenType_operator},
                {"s", C::tokenType_identifier}, {"=", C::tokenType_operator}, {"\"abc\\", C::tokenType_string},
                {"def\"", C::tokenType_string}, {";", C::tokenType_punctuation}});
  expectTokens("C++ splice after an escaped backslash leaves no escape pending", cpp, {"s = \"a\\\\\\", "\";"},
               {{"s", C::tokenType_identifier}, {"=", C::tokenType_operator}, {"\"a\\\\\\", C::tokenType_string},
                {"\"", C::tokenType_string}, {";", C::tokenType_punctuation}});
  expectTokens("C++ splice after a lone backslash escapes the next line's quote", cpp, {"\"a\\\\", "\"x\";"},
               {{"\"a\\\\", C::tokenType_string}, {"\"x\"", C::tokenType_string}, {";", C::tokenType_punctuation}});
  expectTokens("C++ line comment continued by a backslash", cpp, {"// one \\", "two", "three"},
               {{"// one \\", C::tokenType_comment}, {"two", C::tokenType_comment}, {"three", C::tokenType_identifier}});
  expectTokens("C++ unterminated string ends with its line", cpp, {"\"abc", "x"},
               {{"\"abc", C::tokenType_string}, {"x", C::tokenType_identifier}});
  expectTokens("C++ literal prefixes", cpp, {"u8\"h\xC3\xA9\" L'x' 'a'"},
               {{"u8\"h\xC3\xA9\"", C::tokenType_string}, {"L'x'", C::tokenType_string}, {"'a'", C::tokenType_string}});
  expectTokens("C++ operators by maximal munch, brackets, scope", cpp, {"a->b <=> c::d(e[1]) {}"},
               {{"a", C::tokenType_identifier}, {"->", C::tokenType_operator}, {"b", C::tokenType_identifier},
                {"<=>", C::tokenType_operator}, {"c", C::tokenType_identifier}, {"::", C::tokenType_punctuation},
                {"d", C::tokenType_identifier}, {"(", C::tokenType_bracket}, {"e", C::tokenType_identifier},
                {"[", C::tokenType_bracket}, {"1", C::tokenType_integer}, {"]", C::tokenType_bracket},
                {")", C::tokenType_bracket}, {"{", C::tokenType_bracket}, {"}", C::tokenType_bracket}});
  expectTokens("C++ a stray character is an error", cpp, {"a @ b"},
               {{"a", C::tokenType_identifier}, {"@", C::tokenType_error}, {"b", C::tokenType_identifier}});
  expectTokens("C++ macro continued over lines", cpp, {"#define X(a) \\", "  (a + 1)", "int y;"},
               {{"#define X(a) \\", C::tokenType_preprocessor}, {"  (a + 1)", C::tokenType_preprocessor},
                {"int", C::tokenType_keyword}, {"y", C::tokenType_identifier}, {";", C::tokenType_punctuation}});
  expectTokens("C++ malformed hex", cpp, {"0x;"}, {{"0x", C::tokenType_error}, {";", C::tokenType_punctuation}});
  {
    static const char* const kAll[] = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
        "char", "char16_t", "char32_t", "char8_t", "class", "co_await", "co_return", "co_yield", "compl", "concept",
        "const", "const_cast", "consteval", "constexpr", "constinit", "continue", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "final", "float",
        "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not",
        "not_eq", "nullptr", "operator", "or", "or_eq", "override", "private", "protected", "public", "register",
        "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef",
        "typeid", "typename", "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor",
        "xor_eq"};
    int found = 0;
    for (const char* k : kAll) {
      const std::u32string u = u32(k);
      found += C::isReservedKeyword(u.data(), (int) u.size()) ? 1 : 0;
    }
    const std::u32string almost = u32("constexpr_x"), upper = u32("Int");
    char msg[128];
    std::snprintf(msg, sizeof(msg), "all %d C++20 keywords found by the sorted table, and near misses not", found);
    check(found == (int) (sizeof(kAll) / sizeof(kAll[0])) && !C::isReservedKeyword(almost.data(), (int) almost.size()) &&
              !C::isReservedKeyword(upper.data(), (int) upper.size()),
          msg);
  }

  using L = LuaTokeniser;
  L lua;
  expectTokens("Lua local and comment", lua, {"local x = 10 -- comment"},
               {{"local", L::tokenType_keyword}, {"x", L::tokenType_identifier}, {"=", L::tokenType_operator},
                {"10", L::tokenType_integer}, {"-- comment", L::tokenType_comment}});
  expectTokens("Lua long comment of level 2, a ]] inside it", lua, {"--[==[ long", "still ]] here", "]==] print(\"hi\")"},
               {{"--[==[ long", L::tokenType_comment}, {"still ]] here", L::tokenType_comment}, {"]==]", L::tokenType_comment},
                {"print", L::tokenType_identifier}, {"(", L::tokenType_bracket}, {"\"hi\"", L::tokenType_string},
                {")", L::tokenType_bracket}});
  expectTokens("Lua long string, concatenation, escapes", lua, {"s = [[one", "two]] .. 'x' .. \"y\\\"z\""},
               {{"s", L::tokenType_identifier}, {"=", L::tokenType_operator}, {"[[one", L::tokenType_string},
                {"two]]", L::tokenType_string}, {"..", L::tokenType_operator}, {"'x'", L::tokenType_string},
                {"..", L::tokenType_operator}, {"\"y\\\"z\"", L::tokenType_string}});
  expectTokens("Lua numbers", lua, {"0x1p4 3.0 1e10 0xFF 42 3abc"},
               {{"0x1p4", L::tokenType_float}, {"3.0", L::tokenType_float}, {"1e10", L::tokenType_float},
                {"0xFF", L::tokenType_integer}, {"42", L::tokenType_integer}, {"3abc", L::tokenType_error}});
  expectTokens("Lua operators", lua, {"a // b ~= c ... #t"},
               {{"a", L::tokenType_identifier}, {"//", L::tokenType_operator}, {"b", L::tokenType_identifier},
                {"~=", L::tokenType_operator}, {"c", L::tokenType_identifier}, {"...", L::tokenType_operator},
                {"#", L::tokenType_operator}, {"t", L::tokenType_identifier}});
  expectTokens("Lua string continued by an escaped newline", lua, {"s = \"a\\", "b\""},
               {{"s", L::tokenType_identifier}, {"=", L::tokenType_operator}, {"\"a\\", L::tokenType_string},
                {"b\"", L::tokenType_string}});
  expectTokens("Lua \\z skipping across the line break", lua, {"s = \"a\\z", "   b\""},
               {{"s", L::tokenType_identifier}, {"=", L::tokenType_operator}, {"\"a\\z", L::tokenType_string},
                {"   b\"", L::tokenType_string}});
  expectTokens("Lua keywords", lua, {"function f() return nil end"},
               {{"function", L::tokenType_keyword}, {"f", L::tokenType_identifier}, {"(", L::tokenType_bracket},
                {")", L::tokenType_bracket}, {"return", L::tokenType_keyword}, {"nil", L::tokenType_keyword},
                {"end", L::tokenType_keyword}});

  using X = XmlTokeniser;
  X xml;
  expectTokens("XML element, attribute, text", xml, {"<a href=\"x\">text &amp; more</a>"},
               {{"<a", X::tokenType_keyword}, {"href", X::tokenType_identifier}, {"=", X::tokenType_operator},
                {"\"x\"", X::tokenType_string}, {">", X::tokenType_keyword}, {"text", X::tokenType_identifier},
                {"&amp;", X::tokenType_identifier}, {"more", X::tokenType_identifier}, {"</a", X::tokenType_keyword},
                {">", X::tokenType_keyword}});
  expectTokens("XML comment over two lines, then an empty element", xml, {"<!-- one", "two -->  <b/>"},
               {{"<!-- one", X::tokenType_comment}, {"two -->", X::tokenType_comment}, {"<b", X::tokenType_keyword},
                {"/>", X::tokenType_keyword}});
  expectTokens("XML declaration and DOCTYPE with an internal subset", xml,
               {"<?xml version=\"1.0\"?><!DOCTYPE note [ <!ELEMENT note (#PCDATA)> ]>"},
               {{"<?xml version=\"1.0\"?>", X::tokenType_preprocessor},
                {"<!DOCTYPE note [ <!ELEMENT note (#PCDATA)> ]>", X::tokenType_preprocessor}});
  expectTokens("XML CDATA over two lines", xml, {"<![CDATA[ a < b", "]]> after"},
               {{"<![CDATA[ a < b", X::tokenType_string}, {"]]>", X::tokenType_string}, {"after", X::tokenType_identifier}});
  expectTokens("XML attributes and a value over three lines", xml, {"<node", "  name='v", "w' x=\"1\"/>"},
               {{"<node", X::tokenType_keyword}, {"name", X::tokenType_identifier}, {"=", X::tokenType_operator},
                {"'v", X::tokenType_string}, {"w'", X::tokenType_string}, {"x", X::tokenType_identifier},
                {"=", X::tokenType_operator}, {"\"1\"", X::tokenType_string}, {"/>", X::tokenType_keyword}});
  expectTokens("XML: a bare '<' is an error, a quote in text is text", xml, {"a < b say \"hi\""},
               {{"a", X::tokenType_identifier}, {"<", X::tokenType_error}, {"b", X::tokenType_identifier},
                {"say", X::tokenType_identifier}, {"\"hi\"", X::tokenType_identifier}});

  const CodeTokeniser::ColourScheme cs = cpp.getDefaultColourScheme(), ls = lua.getDefaultColourScheme(),
                                    xs = xml.getDefaultColourScheme();
  auto argb = [](Colour c) { return ((uint32_t) c.a << 24) | ((uint32_t) c.r << 16) | ((uint32_t) c.g << 8) | c.b; };
  check(cs.types.size() == 11 && cs.types[C::tokenType_keyword].name == "Keyword" &&
            argb(cs.types[C::tokenType_keyword].colour) == 0xff0000cc &&
            cs.types[C::tokenType_preprocessor].name == "Preprocessor Text" &&
            argb(cs.types[C::tokenType_comment].colour) == 0xff00aa00 && ls.types.size() == 10 &&
            argb(ls.types[L::tokenType_comment].colour) == 0xff3c3c3c && xs.types.size() == 9 &&
            xs.types[X::tokenType_preprocessor].name == "Preprocessor Text" && xs.indexOf("String") == X::tokenType_string,
        "the default colour schemes: one named colour per token type, in type order");
}

void testEditorKeys() {
  std::printf("\n-- CodeEditorComponent: keys --\n");
  CodeDocument doc;
  CPlusPlusCodeTokeniser cpp;
  CodeEditorComponent ed(doc, &cpp);
  ed.setBounds({0.0f, 0.0f, 600.0f, 300.0f});
  ed.loadContent("int main() {\n    return 0;\n}");
  check(at(ed, 0, 0) && !ed.hasSelection(), "loadContent puts the caret at the top");

  press(ed, KeyPress::End);
  const bool end = at(ed, 0, 12);
  press(ed, KeyPress::Left, false, true);
  const bool w1 = at(ed, 0, 11);
  press(ed, KeyPress::Left, false, true);
  const bool w2 = at(ed, 0, 8);
  press(ed, KeyPress::Left, false, true);
  const bool w3 = at(ed, 0, 4);
  press(ed, KeyPress::Right, false, true);
  const bool w4 = at(ed, 0, 8);
  press(ed, KeyPress::Right, false, true);
  const bool w5 = at(ed, 0, 11);
  check(end && w1 && w2 && w3 && w4 && w5, "Ctrl+Left and Ctrl+Right step by words and runs of punctuation");

  press(ed, KeyPress::Home);
  press(ed, KeyPress::End, true);
  check(ed.getSelectedText() == "int main() {", "Shift+End selects to the line's end");
  press(ed, KeyPress::Down, true);
  check(ed.getSelectedText() == "int main() {\n    return 0", "Shift+Down extends the selection a line, to the same column");
  press(ed, KeyPress::Left);
  check(at(ed, 0, 0) && !ed.hasSelection(), "Left collapses a selection to its start");
  press(ed, KeyPress::Right, true);
  press(ed, KeyPress::Right, true);
  press(ed, KeyPress::Right);
  check(at(ed, 0, 2) && !ed.hasSelection(), "Right collapses a selection to its end");

  press(ed, KeyPress::Down);
  press(ed, KeyPress::End);
  press(ed, KeyPress::Home);
  const bool h1 = at(ed, 1, 4);
  press(ed, KeyPress::Home);
  const bool h2 = at(ed, 1, 0);
  press(ed, KeyPress::Home);
  check(h1 && h2 && at(ed, 1, 4), "Home goes to the indent, then the line's start, then back");

  ed.loadContent("abcdef\nab\nabcdef");
  press(ed, KeyPress::End);
  press(ed, KeyPress::Down);
  const bool d1 = at(ed, 1, 2);
  press(ed, KeyPress::Down);
  const bool d2 = at(ed, 2, 6);
  press(ed, KeyPress::Up);
  press(ed, KeyPress::Up);
  const bool d3 = at(ed, 0, 6);
  press(ed, KeyPress::Up);
  const bool d4 = at(ed, 0, 0);
  press(ed, KeyPress::End, false, true);
  check(d1 && d2 && d3 && d4 && at(ed, 2, 6),
        "Down through a short line keeps the column; Up past the top is the start; Ctrl+End the end");

  // Columns: tabs to their stops, wide characters in two, marks in none.
  ed.loadContent("\tA\xE6\x97\xA5" "b\ne\xCC\x81x");
  check(ed.indexToColumn(0, 1) == 4 && ed.indexToColumn(0, 3) == 7 && ed.indexToColumn(0, 4) == 8 &&
            ed.columnToIndex(0, 5) == 2 && ed.columnToIndex(0, 6) == 3,
        "a tab reaches its stop, a wide character takes two columns");
  press(ed, KeyPress::Down);
  press(ed, KeyPress::Home);
  press(ed, KeyPress::Right);
  const bool overMark = at(ed, 1, 2);
  press(ed, KeyPress::Backspace);
  check(overMark && doc.getLine(1) == "x", "Right steps over a combining mark; Backspace takes the letter with it");

  // Typing, a run of it one undo step.
  ed.loadContent("");
  type(ed, "hello world");
  press(ed, KeyPress::Return);
  type(ed, "again");
  check(doc.getAllContent() == "hello world\nagain", "typed text and a line break");
  chord(ed, 'z');
  const bool u1 = doc.getAllContent() == "hello world\n" && at(ed, 1, 0);
  chord(ed, 'z');
  const bool u2 = doc.getAllContent() == "hello world";
  chord(ed, 'z');
  const bool u3 = doc.getAllContent().empty();
  chord(ed, 'z', true);
  chord(ed, 'y');
  check(u1 && u2 && u3 && doc.getAllContent() == "hello world\n",
        "undo takes back a run of typing at once, then the Return, then the run before; Ctrl+Shift+Z and Ctrl+Y redo");
  press(ed, KeyPress::End, false, true);
  press(ed, KeyPress::Backspace);
  press(ed, KeyPress::Backspace);
  press(ed, KeyPress::Backspace);
  check(doc.getAllContent() == "hello wor", "Backspace at a line's start joins it to the line above");
  chord(ed, 'z');
  check(doc.getAllContent() == "hello world\n", "and a run of Backspace is one undo step");
  press(ed, KeyPress::Backspace);
  press(ed, KeyPress::Backspace, false, true);
  check(doc.getAllContent() == "hello ", "Ctrl+Backspace deletes a word");

  // Indentation.
  ed.loadContent("a\nb\n\nc");
  press(ed, KeyPress::Tab);
  check(doc.getLine(0) == "    a" && at(ed, 0, 4), "Tab inserts spaces to the next stop");
  press(ed, KeyPress::Left);
  press(ed, KeyPress::Left);
  press(ed, KeyPress::Tab);
  check(doc.getLine(0) == "      a" && at(ed, 0, 4), "from column 2, Tab reaches column 4");
  chord(ed, 'a');
  press(ed, KeyPress::Tab);
  check(doc.getAllContent() == "          a\n    b\n\n    c" && ed.getSelectionStart().getIndexInLine() == 0,
        "Tab over several lines indents each that has text, and keeps them selected");
  press(ed, KeyPress::Tab, true);
  press(ed, KeyPress::Tab, true);
  check(doc.getAllContent() == "  a\nb\n\nc", "Shift+Tab outdents by a level, or what indent there is");
  chord(ed, ']');
  chord(ed, '[');
  check(doc.getAllContent() == "  a\nb\n\nc", "Ctrl+] and Ctrl+[ indent and outdent");
  ed.loadContent("        x");
  press(ed, KeyPress::Home);
  press(ed, KeyPress::Backspace);
  check(doc.getLine(0) == "    x" && at(ed, 0, 4), "Backspace in an indent of spaces goes back to the previous stop");
  ed.setTabSize(4, false);
  ed.loadContent("x");
  press(ed, KeyPress::Tab);
  check(doc.getLine(0) == "\tx", "with tabs, Tab inserts a tab");
  ed.setTabSize(4, true);

  // Auto-indent.
  ed.loadContent("    return 0;");
  press(ed, KeyPress::End);
  press(ed, KeyPress::Return);
  check(doc.getAllContent() == "    return 0;\n    " && at(ed, 1, 4), "Return keeps the indent");
  ed.loadContent("void f() {}");
  press(ed, KeyPress::End);
  press(ed, KeyPress::Left);
  press(ed, KeyPress::Return);
  check(doc.getAllContent() == "void f() {\n    \n}" && at(ed, 1, 4),
        "Return between braces indents a new line and puts the closing brace below it");
  ed.loadContent("    if (x) {");
  press(ed, KeyPress::End);
  press(ed, KeyPress::Return);
  type(ed, "y;");
  press(ed, KeyPress::Return);
  type(ed, "}");
  check(doc.getAllContent() == "    if (x) {\n        y;\n    }", "after a '{' one level more; a '}' takes it away");

  // Delete forwards.
  ed.loadContent("ab\ncd");
  press(ed, KeyPress::End);
  press(ed, KeyPress::Delete);
  check(doc.getAllContent() == "abcd", "Delete at a line's end joins the next line");
  press(ed, KeyPress::Home);
  press(ed, KeyPress::Delete, false, true);
  check(doc.getAllContent().empty(), "Ctrl+Delete deletes a word");

  // The clipboard. The user's own clipboard is put back afterwards.
  std::string saved;
  const bool hadClipboard = Clipboard::getText(&saved);
  ed.loadContent("    return 0;\nnext");
  press(ed, KeyPress::Home);
  press(ed, KeyPress::End, true);
  chord(ed, 'c');
  std::string got;
  Clipboard::getText(&got);
  const bool copied = got == "return 0;";
  chord(ed, 'x');
  const bool cut = doc.getLine(0) == "    ";
  press(ed, KeyPress::Down);
  press(ed, KeyPress::End);
  chord(ed, 'v');
  check(copied && cut && doc.getAllContent() == "    \nnextreturn 0;", "Ctrl+C, Ctrl+X and Ctrl+V");
  Clipboard::setText("one\r\ntwo\rthree");
  chord(ed, 'v');
  check(doc.getAllContent() == "    \nnextreturn 0;one\ntwo\nthree", "a paste's line breaks become the document's");
  doc.setNewLineCharacters("\r\n");
  chord(ed, 'a');
  chord(ed, 'c');
  Clipboard::getText(&got);
  check(got == "    \r\nnextreturn 0;one\r\ntwo\r\nthree", "a copy's line breaks are the document's newline");
  if (hadClipboard) Clipboard::setText(saved);

  // Read-only.
  ed.loadContent("fixed");
  ed.setReadOnly(true);
  KeyPress letter;
  letter.character = 'z';
  const bool typedRefused = !ed.keyPressed(letter);
  press(ed, KeyPress::Backspace);
  press(ed, KeyPress::Return);
  const bool tabRefused = !press(ed, KeyPress::Tab);
  press(ed, KeyPress::End, true);
  check(typedRefused && tabRefused && doc.getAllContent() == "fixed" && ed.getSelectedText() == "fixed",
        "read-only: typing, Backspace, Return and Tab change nothing; selection still works");
  ed.setReadOnly(false);

  // Controls are not typed text; Escape is not the editor's.
  KeyPress control;
  control.character = '\r';
  const bool cr = !ed.keyPressed(control);
  control.character = '\t';
  const bool tab = !ed.keyPressed(control);
  check(cr && tab && !press(ed, KeyPress::Escape) && doc.getAllContent() == "fixed",
        "a CR or a tab arriving as a character, and Escape, are refused");

  // Find and replace.
  ed.loadContent("one two one\nthree one");
  int asked = 0;
  ed.onFindRequested = [&] { ++asked; };
  chord(ed, 'f');
  const bool hook = asked == 1;
  const bool f1 = ed.findNext("one") && ed.getSelectionStart().getIndexInLine() == 0;
  chord(ed, 'g');
  const bool f2 = ed.getSelectionStart().getIndexInLine() == 8;
  chord(ed, 'g');
  const bool f3 = ed.getSelectionStart().getLineNumber() == 1 && ed.getSelectionStart().getIndexInLine() == 6;
  chord(ed, 'g', true);
  const bool f4 = ed.getSelectionStart().getLineNumber() == 0 && ed.getSelectionStart().getIndexInLine() == 8;
  ed.replaceSelection("1");
  const int replaced = ed.replaceAll("one", "uno");
  check(hook && f1 && f2 && f3 && f4 && replaced == 2 && doc.getAllContent() == "uno two 1\nthree uno",
        "Ctrl+F asks the plugin; findNext, Ctrl+G and Ctrl+Shift+G; replaceSelection and replaceAll");
  chord(ed, 'z');
  check(doc.getAllContent() == "one two 1\nthree one", "replaceAll is one undo step");

  // Page keys.
  std::string many;
  for (int i = 0; i < 200; ++i) many += "line " + std::to_string(i) + "\n";
  ed.loadContent(many);
  const int page = ed.getNumLinesOnScreen();
  press(ed, KeyPress::PageDown);
  const bool pd = ed.getCaretPos().getLineNumber() == page && ed.getFirstLineOnScreen() > 0;
  press(ed, KeyPress::PageUp);
  char msg[200];
  std::snprintf(msg, sizeof(msg), "PageDown moves %d lines and scrolls; PageUp comes back", page);
  check(pd && at(ed, 0, 0) && ed.getFirstLineOnScreen() == 0, msg);
  press(ed, KeyPress::Down, false, true);
  check(ed.getFirstLineOnScreen() == 1 && at(ed, 0, 0), "Ctrl+Down scrolls without moving the caret");
}

void testEditorMouseAndInfo() {
  std::printf("\n-- CodeEditorComponent: mouse, accessibility, caret --\n");
  CodeDocument doc;
  LuaTokeniser lua;
  CodeEditorComponent ed(doc, &lua);
  ed.setBounds({0.0f, 0.0f, 500.0f, 200.0f});
  ed.loadContent("local name = value\nprint(name)");
  const Rect r = ed.getCharacterBounds(CodeDocument::Position(doc, 1, 6));
  MouseEvent e;
  e.position = {r.x + r.w * 0.25f, r.y + r.h * 0.5f};
  ed.mouseDown(e);
  ed.mouseUp(e);
  const bool clicked = at(ed, 1, 6);
  e.clickCount = 2;
  ed.mouseDown(e);
  ed.mouseUp(e);
  const bool word = ed.getSelectedText() == "name";
  e.clickCount = 3;
  ed.mouseDown(e);
  ed.mouseUp(e);
  const bool line = ed.getSelectedText() == "print(name)";
  check(clicked && word && line, "a click places the caret; a double click selects a word, a triple the line");
  e.clickCount = 1;
  const Rect r0 = ed.getCharacterBounds(CodeDocument::Position(doc, 0, 0));
  e.position = {r0.x + 1.0f, r0.y + r0.h * 0.5f};
  ed.mouseDown(e);
  const Rect r1 = ed.getCharacterBounds(CodeDocument::Position(doc, 0, 5));
  e.position = {r1.x + 1.0f, r1.y + r1.h * 0.5f};
  ed.mouseDrag(e);
  ed.mouseUp(e);
  check(ed.getSelectedText() == "local", "a drag selects");
  e.position = {2.0f, r1.y + r1.h * 0.5f};
  ed.mouseDown(e);
  ed.mouseUp(e);
  check(ed.getSelectedText() == "local name = value\n", "a click in the gutter selects the line, its break included");

  ed.moveCaretTo(CodeDocument::Position(doc, 1, 2), false);
  const AccessibleInfo info = ed.accessibleInfo();
  check(info.role == AccessibleRole::TextField && info.value == "local name = value\nprint(name)" &&
            info.description == "Line 2 of 2, column 3" && info.focusable,
        "accessible: a text field, its text the value, the caret's line and column described");
  Rect caret;
  const bool hasCaret = ed.caretBounds(&caret);
  const Rect c2 = ed.getCharacterBounds(CodeDocument::Position(doc, 1, 2));
  check(hasCaret && caret.x == c2.x && caret.y == c2.y && caret.h == ed.getLineHeight(),
        "caretBounds is where the caret is drawn, for an input method's candidate list");
  int changes = 0;
  ed.onTextChange = [&] { ++changes; };
  type(ed, "ab");
  doc.insertText(0, "--");
  check(changes == 3 && at(ed, 1, 4), "onTextChange hears every change, the document's own edits included");
}

/** The number of pixels in a rectangle near an opaque colour. */
int pixelsNear(const Bitmap& b, Rect area, int r, int g, int bl, int tolerance) {
  int count = 0;
  for (int y = (int) area.y; y < (int) (area.y + area.h) && y < b.height(); ++y)
    for (int x = (int) area.x; x < (int) (area.x + area.w) && x < b.width(); ++x) {
      const PremulColour p = b.pixelAt(x, y);
      if (std::abs(p.r - r) <= tolerance && std::abs(p.g - g) <= tolerance && std::abs(p.b - bl) <= tolerance) ++count;
    }
  return count;
}
/** The number of pixels in a rectangle that are not one colour. */
int pixelsNot(const Bitmap& b, Rect area, PremulColour c) {
  int count = 0;
  for (int y = (int) area.y; y < (int) (area.y + area.h) && y < b.height(); ++y)
    for (int x = (int) area.x; x < (int) (area.x + area.w) && x < b.width(); ++x) {
      const PremulColour p = b.pixelAt(x, y);
      if (p.r != c.r || p.g != c.g || p.b != c.b) ++count;
    }
  return count;
}

void testRendering() {
  std::printf("\n-- CodeEditorComponent: rendering --\n");
  if (!monospacedTypeface()) {
    std::printf("  skip no monospaced typeface on this machine\n");
    return;
  }
  CodeDocument doc;
  CPlusPlusCodeTokeniser cpp;
  CodeEditorComponent ed(doc, &cpp);
  ed.setFontSize(28.0f);
  ed.setBounds({0.0f, 0.0f, 640.0f, 160.0f});
  ed.loadContent("int x = 42; // hi\nfloat y;");
  Bitmap bitmap(640, 160);
  bitmap.clear(Colour::fromRGB(0x808080));
  Graphics g(bitmap);
  ed.paintTree(g);
  const float cw = ed.getCharWidth(), lh = ed.getLineHeight();
  const float gutter = (3.0f + 1.5f) * cw;
  const PremulColour gutterBg = bitmap.pixelAt(2, (int) (lh * 2.5f));
  const int digits1 = pixelsNot(bitmap, Rect(0.0f, 0.0f, gutter, lh), gutterBg);
  const int digits2 = pixelsNot(bitmap, Rect(0.0f, lh, gutter, lh), gutterBg);
  const int none3 = pixelsNot(bitmap, Rect(0.0f, lh * 2.0f + 1.0f, gutter, lh - 2.0f), gutterBg);
  char msg[256];
  std::snprintf(msg, sizeof(msg), "the gutter: %d and %d pixels of digits on lines 1 and 2, %d past the last line",
                digits1, digits2, none3);
  check(digits1 > 20 && digits2 > 10 && none3 == 0 && gutterBg.r < 250, msg);
  const float x0 = gutter + CodeEditorComponent::kTextPadding;
  auto cell = [&](int line, int from, int to) { return Rect(x0 + (float) from * cw, (float) line * lh, (float) (to - from) * cw, lh); };
  const int keyword = pixelsNear(bitmap, cell(0, 0, 3), 0x00, 0x00, 0xcc, 40);
  const int integer = pixelsNear(bitmap, cell(0, 8, 10), 0x88, 0x00, 0x00, 40);
  const int comment = pixelsNear(bitmap, cell(0, 12, 17), 0x00, 0xaa, 0x00, 40);
  const int keyword2 = pixelsNear(bitmap, cell(1, 0, 5), 0x00, 0x00, 0xcc, 40);
  const int identifier = pixelsNear(bitmap, cell(0, 4, 5), 0x00, 0x00, 0x00, 40);
  const int blueInComment = pixelsNear(bitmap, cell(0, 12, 17), 0x00, 0x00, 0xcc, 40);
  const int whiteSpace = pixelsNot(bitmap, cell(0, 5, 6), PremulColour(255, 255, 255, 255));
  std::snprintf(msg, sizeof(msg), "tokens in their colours: keyword %d px blue, integer %d px dark red, comment %d px "
                "green, keyword %d px on line 2, identifier %d px black; %d blue px in the comment, %d marks in a space",
                keyword, integer, comment, keyword2, identifier, blueInComment, whiteSpace);
  check(keyword > 15 && integer > 15 && comment > 15 && keyword2 > 15 && identifier > 5 && blueInComment == 0 &&
            whiteSpace == 0,
        msg);

  // A selection, and the caret with focus, are drawn too.
  ed.selectRegion(CodeDocument::Position(doc, 1, 0), CodeDocument::Position(doc, 1, 6));
  bitmap.clear(Colour::fromRGB(0x808080));
  ed.paintTree(g);
  const PremulColour sel = bitmap.pixelAt((int) (x0 + cw * 5.5f), (int) (lh * 1.1f));
  check(sel.b > sel.r + 10, "the selection is drawn in the highlight colour");
}

void testLargeDocument() {
  std::printf("\n-- 100,000 lines --\n");
  std::string text;
  text.reserve(5000000);
  for (int i = 0; i < 100000; ++i) {
    if (i % 1000 == 500) text += "/* a block comment\n   spanning lines */\n";
    text += "    int value" + std::to_string(i) + " = " + std::to_string(i * 7) + "; // note " + std::to_string(i) + "\n";
  }
  CPlusPlusCodeTokeniser cpp;
  Bitmap bitmap(900, 700);
  Graphics g(bitmap);

  // The font rasterises each glyph once per size and quarter-pixel phase,
  // the first time it is drawn -- a cost of the size, not of the document:
  // a ten-line file pays it as a hundred-thousand-line one does. Paid here,
  // on every printable ASCII character at every column, and reported.
  double warm = 0.0;
  {
    CodeDocument small;
    CodeEditorComponent warmer(small, &cpp);
    warmer.setBounds({0.0f, 0.0f, 900.0f, 700.0f});
    std::string all;
    for (int line = 0; line < 40; ++line) {
      for (int k = 0; k < 95; ++k) all.push_back((char) (32 + (k + line) % 95));
      all += "\n";
    }
    warmer.loadContent(all);
    const auto w0 = std::chrono::steady_clock::now();
    warmer.paintTree(g);
    warm = msSince(w0);
  }

  // The whole sequence three times, on a new document each time: a wall
  // clock on a machine doing other work is noisy, and the best of three is
  // what the code costs. The worst is printed too.
  enum { kOpen, kEnd, kType, kReturn, kBackspace, kUndo, kMiddle, kTopEdit, kRescan, kActions };
  static const char* const kNames[kActions] = {"open+paint", "Ctrl+End+paint", "type", "Return", "Backspace",
                                               "undo", "scroll to middle", "edit at top", "back to end"};
  double best[kActions], worst[kActions];
  for (int a = 0; a < kActions; ++a) {
    best[a] = 1e30;
    worst[a] = 0.0;
  }
  bool allOk = true;
  int lines = 0, scannedAtTop = 0, rescanLines = 0;
  // Three runs, and up to nine while the best of them is still over the bound:
  // the claim is about the editor, and a run beside a full build once came out
  // at 95 ms for an action that takes 4. A real regression stays slow in every
  // run; a busy moment does not.
  auto slowestBest = [&]() {
    double s = 0.0;
    for (int a = 0; a < kActions; ++a) s = (std::max)(s, best[a]);
    return s;
  };
  int runs = 0;
  for (int run = 0; run < 3 || (kTimed && run < 9 && slowestBest() >= 50.0); ++run) {
    ++runs;
    CodeDocument doc;
    CodeEditorComponent ed(doc, &cpp);
    ed.setBounds({0.0f, 0.0f, 900.0f, 700.0f});
    double ms[kActions] = {};
    auto t0 = std::chrono::steady_clock::now();
    ed.loadContent(text);
    ed.paintTree(g);
    ms[kOpen] = msSince(t0);
    lines = doc.getNumLines();
    scannedAtTop = ed.getNumLinesWithKnownState();

    t0 = std::chrono::steady_clock::now();
    press(ed, KeyPress::End, false, true);
    ed.paintTree(g);
    ms[kEnd] = msSince(t0);
    const bool atEnd = ed.getCaretPos().getLineNumber() == lines - 1 && ed.getFirstLineOnScreen() > lines - 100;

    press(ed, KeyPress::Up);
    press(ed, KeyPress::Up);
    press(ed, KeyPress::End);
    t0 = std::chrono::steady_clock::now();
    type(ed, "x");
    ed.paintTree(g);
    ms[kType] = msSince(t0);

    t0 = std::chrono::steady_clock::now();
    press(ed, KeyPress::Return);
    ed.paintTree(g);
    ms[kReturn] = msSince(t0);

    t0 = std::chrono::steady_clock::now();
    press(ed, KeyPress::Backspace);
    ed.paintTree(g);
    ms[kBackspace] = msSince(t0);

    t0 = std::chrono::steady_clock::now();
    chord(ed, 'z');
    ed.paintTree(g);
    ms[kUndo] = msSince(t0);
    // The Return kept the indent; the Backspace took it back to the stop
    // before, and undo put it back.
    const bool contentOk = doc.getLine(lines - 3) == "    int value99998 = 699986; // note 99998x" &&
                           doc.getLine(lines - 2) == "    " && doc.getNumLines() == lines + 1;

    t0 = std::chrono::steady_clock::now();
    ed.scrollToLine(50000);
    ed.paintTree(g);
    ms[kMiddle] = msSince(t0);

    // An edit at the top, then the end again. The states below the edit are
    // kept, and the re-scan stops at the first line that agrees with them.
    press(ed, KeyPress::Home, false, true);
    t0 = std::chrono::steady_clock::now();
    type(ed, "/");
    ed.paintTree(g);
    ms[kTopEdit] = msSince(t0);
    t0 = std::chrono::steady_clock::now();
    press(ed, KeyPress::End, false, true);
    ed.paintTree(g);
    ms[kRescan] = msSince(t0);
    rescanLines = ed.getNumLinesWithKnownState();
    // And the colours down there are the ones a scan from the top gives.
    const std::vector<CodeTokeniser::Token> bottom = ed.getTokensForLine(lines - 5);
    const bool tokensOk = bottom.size() == 6 && bottom[0].type == CPlusPlusCodeTokeniser::tokenType_keyword &&
                          bottom[5].type == CPlusPlusCodeTokeniser::tokenType_comment;
    allOk = allOk && atEnd && contentOk && rescanLines == doc.getNumLines() && tokensOk;
    for (int a = 0; a < kActions; ++a) {
      best[a] = (std::min)(best[a], ms[a]);
      worst[a] = (std::max)(worst[a], ms[a]);
    }
  }

  char msg[512];
  std::snprintf(msg, sizeof(msg), "%d lines: painting the top scanned the states of %d of them", lines, scannedAtTop);
  check(lines == 100201 && scannedAtTop < 200, msg);
  check(allOk, "Ctrl+End reaches the last line and shows it; an edit there, a Return, a Backspace and an undo leave "
               "the right text; after an edit at the top, the end is coloured as a scan from the top colours it");
  std::printf("       (font warm-up, every glyph at this size, once: %.2f ms)\n", warm);
  double slowest = 0.0;
  for (int a = 0; a < kActions; ++a) {
    std::printf("       %-18s best %6.2f ms   worst %6.2f ms\n", kNames[a], best[a], worst[a]);
    slowest = (std::max)(slowest, best[a]);
  }
  std::snprintf(msg, sizeof(msg), "every action under 50 ms (best of %d runs), the slowest %.2f ms%s", runs, slowest,
                kTimed ? "" : " (unoptimised or sanitised build: reported, not enforced)");
  check(!kTimed || slowest < 50.0, msg);
}

/** Handlers that delete the editor, or stop listening, in the middle of a
 *  change -- and what a handler sees when it runs. */
void testCallbacksThatDelete() {
  std::printf("\n-- callbacks that delete --\n");
  {
    // A listener that stops listening inside codeDocumentLinesChanged is not
    // told the rest of that change: it may have been destroyed by then.
    struct Leaving : CodeDocument::Listener {
      CodeDocument& doc;
      int told = 0;
      explicit Leaving(CodeDocument& d) : doc(d) { doc.addListener(this); }
      void codeDocumentLinesChanged(int, int) override { doc.removeListener(this); }
      void codeDocumentTextInserted(int, int) override { ++told; }
    };
    CodeDocument doc;
    Leaving l(doc);
    doc.insertText(0, "x");
    check(l.told == 0, "a listener that leaves in codeDocumentLinesChanged hears no more of that change");
  }
  {
    // An editor deleted from onTextChange by an edit made elsewhere, by
    // typing over a selection, and from onCaretMoved by an insert: the edit
    // completes and nothing of the editor runs on (ASan watches the rest).
    static CodeEditorComponent* victim = nullptr;
    auto kill = [] {
      CodeEditorComponent* e = victim;
      victim = nullptr;
      delete e;
    };
    CodeDocument doc;
    doc.loadFromText("alpha\nbeta\n");
    victim = new CodeEditorComponent(doc, nullptr);
    victim->onTextChange = kill;
    doc.insertText(0, "1");
    const bool external = victim == nullptr;
    victim = new CodeEditorComponent(doc, nullptr);
    victim->selectRegion(CodeDocument::Position(doc, 0, 2), CodeDocument::Position(doc, 1, 1));
    victim->onTextChange = kill;
    KeyPress k;
    k.character = 'q';
    victim->keyPressed(k);
    const bool typed = victim == nullptr;
    victim = new CodeEditorComponent(doc, nullptr);
    victim->onCaretMoved = kill;
    victim->insertTextAtCaret("z\n");
    const bool inserted = victim == nullptr;
    delete victim;
    check(external && typed && inserted && doc.getAllContent() == "z\n1aqeta\n",
          "an editor deleted from onTextChange or onCaretMoved mid-edit: the edit completes, nothing runs on");
  }
  {
    // What the handlers see: the whole operation, once, and a selection
    // deleted is a caret moved.
    CodeDocument doc;
    doc.loadFromText("one two");
    CodeEditorComponent ed(doc, nullptr);
    ed.selectRegion(CodeDocument::Position(doc, 0, 0), CodeDocument::Position(doc, 0, 3));
    int calls = 0;
    std::string seen;
    ed.onTextChange = [&] {
      ++calls;
      seen = doc.getAllContent();
    };
    KeyPress k;
    k.character = 'X';
    ed.keyPressed(k);
    check(calls == 1 && seen == "X two", "typing over a selection: onTextChange once, the character already in");
    int moved = 0;
    ed.onCaretMoved = [&] { ++moved; };
    ed.selectRegion(CodeDocument::Position(doc, 0, 1), CodeDocument::Position(doc, 0, 5));
    moved = 0;
    KeyPress bs;
    bs.keyCode = KeyPress::Backspace;
    ed.keyPressed(bs);
    check(moved == 1 && at(ed, 0, 1) && doc.getAllContent() == "X", "Backspace over a selection tells onCaretMoved");
  }
}

} // namespace

int main() {
  std::printf("code editor tests\n");
  testDocumentRoundTrips();
  testPositions();
  testLineEndings();
  testHistoryAndLimits();
  testTokenisers();
  testEditorKeys();
  testEditorMouseAndInfo();
  testRendering();
  testLargeDocument();
  testCallbacksThatDelete();
  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  std::printf(g_failures == 0 ? "CODE EDITOR TESTS PASSED\n" : "CODE EDITOR TESTS FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
