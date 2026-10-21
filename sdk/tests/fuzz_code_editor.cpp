// SPDX-License-Identifier: Apache-2.0
// The code editor under random hands.
//
// Each case is one of: a random edit, undo or redo of a CodeDocument checked
// against a std::u32string and a stack of its snapshots; a random key --
// arrows, words, pages, Home and End, Backspace and Delete, Return, Tab,
// shortcuts, typed characters from ASCII to emoji, controls and joiners --
// or mouse gesture sent to a CodeEditorComponent; random bytes through the
// three tokenisers; random bytes loaded, saved and loaded again; find and
// replace; settings changed under the editor's feet. Properties:
//   * nothing crashes, reads out of bounds or hangs (run it under ASan);
//   * a tokeniser's tokens are in order, inside the line, never empty, of a
//     type its colour scheme has, and its state-only scan ends every line in
//     the state the full one does;
//   * the editor's tokens for a line -- from states it kept and reused
//     across edits -- are the ones a scan from the top gives;
//   * the caret and the selection stay inside the document, the scroll
//     inside the text; undoing everything and redoing it comes back to the
//     same text; a document saved and loaded again reads the same.
//
//   fuzz_code_editor [--steps N] [--seed S]
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sonore/gfx/bitmap.h>
#include <sonore/gfx/code_editor.h>
#include <sonore/gfx/graphics.h>

using namespace sonore::gfx;

namespace {

#if !defined(__has_feature)
#define SONORE_HAS_FEATURE(x) 0
#else
#define SONORE_HAS_FEATURE(x) __has_feature(x)
#endif
#if defined(__SANITIZE_ADDRESS__) || SONORE_HAS_FEATURE(address_sanitizer) || !defined(NDEBUG)
constexpr long long kHangMs = 120000;
#else
constexpr long long kHangMs = 20000;
#endif

std::atomic<long long> g_deadline{0};
std::atomic<long> g_step{0};
std::atomic<const char*> g_what{""};
std::atomic<bool> g_done{false};

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int g_failures = 0;
void fail(const char* what, long step) {
  if (++g_failures <= 20) std::printf("  FAIL step %ld: %s\n", step, what);
}

std::u32string u32(const std::string& s) { return detail::code::decodeText(s); }
std::string utf8(const std::u32string& s) { return detail::code::encode(s.data(), s.size()); }

/** Bytes biased towards what changes a tokeniser's mind. */
std::string syntaxBytes(std::mt19937& rng, size_t length) {
  static const char kAlphabet[] = "/*\"'\\#R()[]{}<>!-=?:;.,$@ \t\r\n0123456789xXeEpP_aAzZuLU8 []=-!?&|^~%+";
  static const char* const kWords[] = {"/*", "*/", "//", "<!--", "-->", "<![CDATA[", "]]>", "<?", "?>", "--[[",
                                       "]]", "--[==[", "]==]", "R\"x(", ")x\"", "u8\"", "\\z", "<!DOCTYPE", "#define ",
                                       "\\\n", "\xC3\xA9", "\xE6\x97\xA5", "\xF0\x9F\x98\x80", "e\xCC\x81"};
  std::string s;
  while (s.size() < length) {
    const uint32_t r = rng() % 100;
    if (r < 70) s.push_back(kAlphabet[rng() % (sizeof(kAlphabet) - 1)]);
    else if (r < 92) s += kWords[rng() % (sizeof(kWords) / sizeof(kWords[0]))];
    else s.push_back((char) (rng() & 0xFF));
  }
  return s;
}

/** Text for the documents: code-shaped, with every kind of break. */
std::string randomText(std::mt19937& rng) {
  static const char* const kPieces[] = {"a", "bc", "int ", " ", "\t", "\n", "\r\n", "\r", "{", "}", "(", ")", "/*",
                                        "*/", "//", "\"", "'", "\\", "#", "\xC3\xA9", "\xE6\x97\xA5",
                                        "\xF0\x9F\x98\x80", "e\xCC\x81", "\xE2\x80\x8D", "    ", "x = 1;"};
  std::string s;
  for (int i = (int) (rng() % 8); i > 0; --i) s += kPieces[rng() % (sizeof(kPieces) / sizeof(kPieces[0]))];
  return s;
}

/** The tokeniser's rules for one line's tokens; false if one is broken. */
bool tokensValid(const std::vector<CodeTokeniser::Token>& toks, int n, int types) {
  int end = 0;
  for (const CodeTokeniser::Token& t : toks) {
    if (t.length <= 0 || t.start < end || t.start + t.length > n || t.type < 0 || t.type >= types) return false;
    end = t.start + t.length;
  }
  return true;
}

/** Tokens of line k as a scan from the top finds them. */
std::vector<CodeTokeniser::Token> fromTheTop(const CodeTokeniser& t, const CodeDocument& doc, int k) {
  uint32_t state = 0;
  for (int i = 0; i < k; ++i) {
    const std::u32string& l = doc.getLineCodePoints(i);
    state = t.tokeniseLine(l.data(), (int) l.size(), state, nullptr);
  }
  std::vector<CodeTokeniser::Token> out;
  const std::u32string& l = doc.getLineCodePoints(k);
  t.tokeniseLine(l.data(), (int) l.size(), state, &out);
  return out;
}

} // namespace

int main(int argc, char** argv) {
  long steps = 20000;
  uint32_t seed = 1;
  bool verbose = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--verbose")) verbose = true;
    else if (i + 1 < argc && !std::strcmp(argv[i], "--steps")) steps = std::atol(argv[++i]);
    else if (i + 1 < argc && !std::strcmp(argv[i], "--seed")) seed = (uint32_t) std::strtoul(argv[++i], nullptr, 10);
  }
  std::printf("fuzz_code_editor: %ld steps, seed %u\n", steps, seed);
  std::mt19937 rng(seed);

  // A step that never returns is a failure too: the watchdog says which.
  std::thread watchdog([] {
    while (!g_done.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      const long long deadline = g_deadline.load();
      if (deadline != 0 && nowMs() > deadline) {
        std::printf("  FAIL step %ld hung in %s\nFUZZ CODE EDITOR FAILED\n", g_step.load(), g_what.load());
        std::fflush(stdout);
        std::_Exit(2);
      }
    }
  });

  CPlusPlusCodeTokeniser cpp;
  LuaTokeniser lua;
  XmlTokeniser xml;
  CodeTokeniser* const tokenisers[3] = {&cpp, &lua, &xml};
  int schemeSizes[3];
  for (int i = 0; i < 3; ++i) schemeSizes[i] = (int) tokenisers[i]->getDefaultColourScheme().types.size();

  // The document against its model.
  CodeDocument mdoc;
  std::u32string model;
  std::vector<std::u32string> undoStack, redoStack;
  bool open = false;

  // The editor.
  CodeDocument edoc;
  CodeEditorComponent ed(edoc, &cpp);
  CodeTokeniser* current = &cpp;
  std::shared_ptr<Typeface> face = monospacedTypeface();
  // No fallback faces: a random code point would otherwise send the font
  // reading the system's font files, which is a different test.
  if (face) ed.setFont(Font(face, 13.0f).withoutFallback());
  ed.setBounds({0.0f, 0.0f, 320.0f, 200.0f});
  Bitmap bitmap(320, 200);
  Graphics g(bitmap);

  long counts[9] = {};
  static const char* const kKinds[9] = {"document", "keys", "mouse", "tokeniser bytes", "editor tokens",
                                        "load and save", "find", "settings", "undo all"};
  static const int kCodes[] = {KeyPress::Left,     KeyPress::Right,     KeyPress::Up,       KeyPress::Down,
                               KeyPress::Home,     KeyPress::End,       KeyPress::PageUp,   KeyPress::PageDown,
                               KeyPress::Backspace, KeyPress::Delete,   KeyPress::Return,   KeyPress::Tab,
                               KeyPress::Escape};
  static const uint32_t kOdd[] = {'{', '}', '(', ')', '\t', '\r', '\n', 0x7F, 0xE9, 0x65E5, 0x1F600, 0x301, 0x200D,
                                  0xFEFF, 0, 0x85, 0x2028, 0xD800, 0x110000, 0x3000, 0xFF21};
  static const char kShortcuts[] = {'a', 'z', 'y', ']', '[', 'f', 'h', 'g', 'Z', 'q'
#if !defined(_WIN32)
                                    , 'c', 'x', 'v' // the in-process clipboard; on Windows it is the user's
#endif
  };

  for (long step = 0; step < steps; ++step) {
    g_step = step;
    g_deadline = nowMs() + kHangMs;
    const uint32_t kind = rng() % 100;

    if (kind < 22) {
      g_what = kKinds[0];
      ++counts[0];
      const int op = (int) (rng() % 100);
      const int size = (int) model.size();
      auto opening = [&] {
        if (!open) {
          undoStack.push_back(model);
          redoStack.clear();
          open = true;
        }
      };
      if (op < 40) {
        const int off = (int) (rng() % (uint32_t) (size + 1));
        const std::string text = randomText(rng);
        const std::u32string u = u32(text);
        if (!u.empty()) {
          opening();
          model.insert((size_t) off, u);
        }
        mdoc.insertText(off, text);
      } else if (op < 62) {
        int a = (int) (rng() % (uint32_t) (size + 1)), b = (std::min)(size, a + (int) (rng() % 12));
        if (a != b) {
          opening();
          model.erase((size_t) a, (size_t) (b - a));
        }
        mdoc.deleteSection(b, a);
      } else if (op < 70) {
        int a = (int) (rng() % (uint32_t) (size + 1)), b = (int) (rng() % (uint32_t) (size + 1));
        if (a > b) std::swap(a, b);
        const std::string text = randomText(rng);
        const std::u32string u = u32(text);
        if (a != b || !u.empty()) {
          opening();
          model.replace((size_t) a, (size_t) (b - a), u);
        }
        mdoc.replaceSection(a, b, text);
      } else if (op < 80) {
        mdoc.newTransaction();
        open = false;
      } else if (op < 90) {
        mdoc.undo();
        open = false;
        if (!undoStack.empty()) {
          redoStack.push_back(model);
          model = undoStack.back();
          undoStack.pop_back();
        }
      } else {
        mdoc.redo();
        open = false;
        if (!redoStack.empty()) {
          undoStack.push_back(model);
          model = redoStack.back();
          redoStack.pop_back();
        }
      }
      if (mdoc.getAllContent() != utf8(model) || mdoc.getNumCharacters() != (int) model.size()) fail("document differs from its model", step);
      const int off = (int) (rng() % (uint32_t) (model.size() + 1));
      if (CodeDocument::Position(mdoc, off).getPosition() != off) fail("offset -> position -> offset", step);
      if (model.size() > 6000) { // keep each check cheap: start again
        mdoc.loadFromText("");
        model.clear();
        undoStack.clear();
        redoStack.clear();
        open = false;
      }
    } else if (kind < 50) {
      g_what = kKinds[1];
      ++counts[1];
      KeyPress k;
      const uint32_t r = rng() % 100;
      if (r < 45) {
        k.keyCode = kCodes[rng() % (sizeof(kCodes) / sizeof(kCodes[0]))];
      } else if (r < 60) {
        k.character = (uint32_t) (unsigned char) kShortcuts[rng() % sizeof(kShortcuts)];
        k.ctrlDown = true;
      } else if (r < 85) {
        k.character = 32u + rng() % 95u;
      } else {
        k.character = kOdd[rng() % (sizeof(kOdd) / sizeof(kOdd[0]))];
      }
      k.shiftDown = rng() % 3 == 0;
      if (r < 45) k.ctrlDown = rng() % 4 == 0;
      k.altDown = rng() % 20 == 0;
      ed.keyPressed(k);
    } else if (kind < 60) {
      g_what = kKinds[2];
      ++counts[2];
      MouseEvent e;
      auto randomPoint = [&] {
        return Point{(float) ((int) (rng() % 420) - 50), (float) ((int) (rng() % 300) - 50)};
      };
      e.position = randomPoint();
      e.clickCount = 1 + (int) (rng() % 3);
      e.shiftDown = rng() % 4 == 0;
      const uint32_t r = rng() % 10;
      if (r < 6) {
        ed.mouseDown(e);
        for (int d = (int) (rng() % 4); d > 0; --d) {
          e.position = randomPoint();
          ed.mouseDrag(e);
        }
        ed.mouseUp(e);
      } else if (r < 9) {
        ed.mouseWheel(e, (float) ((int) (rng() % 7) - 3) * 0.5f);
      } else {
        ed.mouseDrag(e); // a drag with no press
      }
    } else if (kind < 70) {
      g_what = kKinds[3];
      ++counts[3];
      const std::string bytes = syntaxBytes(rng, rng() % 10 == 0 ? 4000u + rng() % 4000u : rng() % 300u);
      std::vector<std::u32string> lines(1);
      for (char32_t c : u32(bytes)) {
        if (c == '\n') lines.emplace_back();
        else lines.back().push_back(c);
      }
      for (int t = 0; t < 3; ++t) {
        uint32_t state = 0, quick = 0;
        std::vector<CodeTokeniser::Token> toks;
        for (const std::u32string& l : lines) {
          toks.clear();
          state = tokenisers[t]->tokeniseLine(l.data(), (int) l.size(), state, &toks);
          quick = tokenisers[t]->tokeniseLine(l.data(), (int) l.size(), quick, nullptr);
          if (!tokensValid(toks, (int) l.size(), schemeSizes[t])) fail("a token out of order, empty or out of the line", step);
          if (state != quick) {
            fail("the state-only scan disagrees with the full one", step);
            quick = state;
          }
        }
      }
    } else if (kind < 76) {
      g_what = kKinds[4];
      ++counts[4];
      if (current && edoc.getNumLines() > 0) {
        for (int i = 0; i < 3; ++i) {
          const int k = (int) (rng() % (uint32_t) edoc.getNumLines());
          const std::vector<CodeTokeniser::Token> kept = ed.getTokensForLine(k);
          const std::vector<CodeTokeniser::Token> fresh = fromTheTop(*current, edoc, k);
          bool same = kept.size() == fresh.size();
          for (size_t j = 0; same && j < kept.size(); ++j)
            same = kept[j].start == fresh[j].start && kept[j].length == fresh[j].length && kept[j].type == fresh[j].type;
          if (!same) fail("the editor's kept states colour a line differently from a scan from the top", step);
        }
      }
    } else if (kind < 82) {
      g_what = kKinds[5];
      ++counts[5];
      std::string bytes = syntaxBytes(rng, rng() % 400u);
      const uint32_t bom = rng() % 8;
      if (bom == 0) bytes = "\xEF\xBB\xBF" + bytes;
      else if (bom == 1) bytes = std::string("\xFF\xFE") + bytes;
      else if (bom == 2) bytes = std::string("\xFE\xFF") + bytes;
      CodeDocument a, b;
      if (rng() % 10 == 0) a.setMaximumSize(1 + (int) (rng() % 64));
      if (a.loadFromText(bytes)) {
        if (!b.loadFromText(a.getEncodedContent()) || b.getAllContent() != a.getAllContent() ||
            b.getNumLines() != a.getNumLines() || b.getEncoding() != a.getEncoding())
          fail("loaded, saved and loaded again, a document reads differently", step);
        for (int k = 0; k < a.getNumLines(); ++k)
          if (a.getLineEnding(k) != b.getLineEnding(k)) fail("a line ending changed through a save", step);
      } else if ((long long) bytes.size() <= a.getMaximumSize()) {
        // Refused only when too big -- and no encoding makes more characters
        // than it has bytes.
        fail("a document within its size was refused", step);
      }
      if (rng() % 4 == 0) ed.loadContent(bytes);
    } else if (kind < 88) {
      g_what = kKinds[6];
      ++counts[6];
      std::string needle;
      if (edoc.getNumCharacters() > 0 && rng() % 3) {
        const int from = (int) (rng() % (uint32_t) edoc.getNumCharacters());
        needle = edoc.getTextBetween(from, from + 1 + (int) (rng() % 6));
      } else {
        needle = randomText(rng);
      }
      const uint32_t r = rng() % 5;
      if (r == 0) ed.findNext(needle, rng() % 2 == 0, rng() % 2 == 0);
      else if (r == 1) ed.findPrevious(needle, rng() % 2 == 0, rng() % 2 == 0);
      else if (r == 2) ed.replaceSelection(randomText(rng));
      else if (r == 3 && edoc.getNumCharacters() < 4000) ed.replaceAll(needle, randomText(rng), rng() % 2 == 0);
      else ed.insertTextAtCaret(randomText(rng));
    } else if (kind < 97) {
      g_what = kKinds[7];
      ++counts[7];
      switch (rng() % 11) {
        case 0: ed.setTabSize(1 + (int) (rng() % 8), rng() % 2 == 0); break;
        case 1: ed.setReadOnly(rng() % 4 == 0); break;
        case 2: ed.setLineNumbersShown(rng() % 2 == 0); break;
        case 3:
          if (face) ed.setFont(Font(face, 6.0f + (float) (rng() % 30)).withoutFallback());
          break;
        case 4: ed.setBounds({0.0f, 0.0f, (float) (rng() % 400), (float) (rng() % 260)}); break;
        case 5: {
          const uint32_t t = rng() % 4;
          current = t == 3 ? nullptr : tokenisers[t];
          ed.setTokeniser(current);
          break;
        }
        case 6: ed.loadContent(randomText(rng) + randomText(rng) + randomText(rng)); break;
        case 7: edoc.setNewLineCharacters(rng() % 3 == 0 ? "\r\n" : (rng() % 2 ? "\n" : "\r")); break;
        case 8: edoc.normaliseLineEndings(); break;
        case 9: ed.scrollToLine((int) (rng() % 200) - 20); break;
        default: ed.setAutoIndent(rng() % 3 != 0); break;
      }
    } else {
      g_what = kKinds[8];
      ++counts[8];
      // Everything undone, then as many redone: the same text again. (Not
      // redo until it stops: steps undone before this were on the redo
      // stack already, and redoing them is going further.)
      const std::string before = edoc.getAllContent();
      int back = 0;
      while (edoc.undo() && back < 100000) ++back;
      int forward = 0;
      while (forward < back && edoc.redo()) ++forward;
      if (back != forward || edoc.getAllContent() != before) {
        fail("undo everything, redo everything: not the same", step);
        if (verbose) {
          std::printf("    %d undone, %d redone\n    before: ", back, forward);
          for (char ch : before) std::printf(ch >= 32 && ch < 127 ? "%c" : "\\x%02X", ch >= 32 && ch < 127 ? ch : (unsigned char) ch);
          std::printf("\n    after:  ");
          for (char ch : edoc.getAllContent()) std::printf(ch >= 32 && ch < 127 ? "%c" : "\\x%02X", ch >= 32 && ch < 127 ? ch : (unsigned char) ch);
          std::printf("\n");
        }
      }
      if (edoc.getNumCharacters() > 20000) ed.loadContent("");
    }

    // What the editor reports stays inside what it has.
    const int lines = edoc.getNumLines();
    const CodeDocument::Position c = ed.getCaretPos(), s = ed.getSelectionStart(), e = ed.getSelectionEnd();
    if (c.getLineNumber() < 0 || c.getLineNumber() >= lines || c.getIndexInLine() < 0 ||
        c.getIndexInLine() > edoc.getLineLength(c.getLineNumber()) || e < s)
      fail("the caret or the selection is outside the document", step);
    if (ed.getScrollY() < 0.0f || ed.getScrollX() < 0.0f ||
        ed.getScrollY() > (float) lines * ed.getLineHeight() + 1.0f) {
      fail("the scroll is outside the text", step);
      if (verbose)
        std::printf("    scroll %.1f, %.1f; %d lines of %.1f px; last case %s\n", ed.getScrollX(), ed.getScrollY(), lines,
                    ed.getLineHeight(), g_what.load());
    }
    if (step % 16 == 0) {
      g_what = "paint";
      bitmap.clear(Colour::fromRGB(0x202020));
      ed.paintTree(g);
      (void) ed.accessibleInfo();
      Rect caret;
      (void) ed.caretBounds(&caret);
    }
  }
  g_done = true;
  watchdog.join();

  std::printf("  cases:");
  for (int i = 0; i < 9; ++i) std::printf(" %s %ld%s", kKinds[i], counts[i], i < 8 ? "," : "\n");
  std::printf("  %ld steps, %d failures\n", steps, g_failures);
  std::printf(g_failures == 0 ? "FUZZ CODE EDITOR PASSED\n" : "FUZZ CODE EDITOR FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
