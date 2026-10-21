// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the changes that turn one string into another.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// An undo step for a text field that stores what was typed rather than two
// copies of the whole text; a preset browser showing what an edit changed in
// a patch's description; state sent to a remote editor as edits instead of
// as the whole document. The reference has it for the third; a plugin has
// more use for the first two.
//
// ── How ─────────────────────────────────────────────────────────────────────
//
// Myers' O(ND) algorithm in its linear-space form: find the "middle snake"
// of the edit graph from both ends at once, split there, recurse on the two
// halves. Memory is proportional to the inputs, not to their product, and
// the edit script is MINIMAL -- no shorter list of inserted plus removed
// characters turns the original into the target. Minimal in characters, not
// in changes: where two equally short scripts exist, one may split an edit a
// person would call one ("abcab" to "ab" removes "cab" or "abc" -- or "c" and
// "ab" apart -- at the same cost). The common prefix and
// suffix are stripped first, which is most of the work for the usual case of
// one small edit in a long text.
//
// The comparison is by CHARACTER (UTF-8 code point), so a change never
// starts or ends in the middle of one and inserted text is always whole
// characters. Invalid UTF-8 is compared byte by byte, so any std::string
// round-trips exactly.
//
// ── The size guard ──────────────────────────────────────────────────────────
//
// Myers is fast when the strings are similar and slow -- N times D -- when
// they are not, and both strings may come from outside. So the search runs
// on a work budget, and a differing region of more than two million
// characters is not searched at all (the search's memory grows with the
// region). Either way, whatever is still unresolved is described as one
// replacement: the changes still reproduce the target exactly, they are just
// no longer the fewest possible, and isMinimal() says so. A budget of
// operations rather than of time, so the same inputs give the same changes on
// every machine.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sonore {

/**
 * The list of changes between two strings: apply them in order to the
 * original and you have the target.
 *
 *     TextDiff diff(before, after);
 *     assert(diff.appliedTo(before) == after);
 */
class TextDiff {
public:
  /**
   * One edit: at byte offset `start`, remove `length` bytes, then insert
   * `insertedText`. Offsets are into the text as it stands when this change is
   * applied -- after every earlier change in the list -- and always fall on
   * character boundaries.
   */
  struct Change {
    std::string insertedText; ///< what goes in at `start`; empty for a pure deletion
    size_t start = 0;         ///< byte offset, into the text as earlier changes left it
    size_t length = 0;        ///< bytes removed at `start`; 0 for a pure insertion

    /** Removes and inserts nothing. */
    bool isDeletion() const { return insertedText.empty(); }

    /** This one change applied to `text`. Offsets past its end are clamped,
     *  so applying a change to the wrong text gives a wrong string, never an
     *  exception. */
    std::string appliedTo(std::string text) const {
      applyInPlace(text);
      return text;
    }
    /** appliedTo(), changing `text` itself. */
    void applyInPlace(std::string& text) const {
      const size_t at = start < text.size() ? start : text.size();
      const size_t n = length < text.size() - at ? length : text.size() - at;
      text.replace(at, n, insertedText);
    }
  };

  /** The default work budget: comfortably minimal for texts of tens of
   *  thousands of characters with thousands of differences, and a few tens
   *  of milliseconds at worst. */
  static constexpr size_t kDefaultMaxWork = 20000000;

  /** The size guard: a differing region of more characters than this (both
   *  sides together, after the common prefix and suffix are set aside) is
   *  described as one replacement without being searched. The search's
   *  memory is proportional to the region, and this caps it near 16 MB. */
  static constexpr size_t kMaxRegion = (size_t) 1 << 21;

  /** The changes from `original` to `target`, within the default work
   *  budget. Both are UTF-8; a byte that is not only matches itself. */
  TextDiff(const std::string& original, const std::string& target)
      : TextDiff(original, target, kDefaultMaxWork) {}
  /** The same within `maxWork`; past it, the rest of the difference is one
   *  replacement and isMinimal() is false. */
  TextDiff(const std::string& original, const std::string& target, size_t maxWork)
      : budget_(maxWork) {
    compute(original, target);
  }

  /** `text` with every change applied in order. */
  std::string appliedTo(std::string text) const {
    for (const Change& c : changes) c.applyInPlace(text);
    return text;
  }

  /** False if the work budget ran out and part of the difference was
   *  described as a single replacement rather than the fewest edits. */
  bool isMinimal() const { return minimal_; }

  /** The changes, to be applied in order; empty when the texts are equal. */
  std::vector<Change> changes;

private:
  /** One differing region as the search finds it, in code points of both
   *  texts, before compute() turns it into a byte-offset Change. Found left
   *  to right; two that touch are merged as they arrive. */
  struct Hunk {
    ///@{
    /** Code-point ranges: a[aFrom, aTo) becomes b[bFrom, bTo). */
    size_t aFrom, aTo, bFrom, bTo;
    ///@}
  };

  // Code points, one per character; a byte that is not valid UTF-8 becomes a
  // value above U+10FFFF of its own, so it only ever matches itself.
  static void decode(const std::string& s, std::vector<uint32_t>& units, std::vector<size_t>& offsets) {
    units.clear();
    offsets.clear();
    units.reserve(s.size());
    offsets.reserve(s.size() + 1);
    for (size_t i = 0; i < s.size();) {
      offsets.push_back(i);
      const unsigned char c = (unsigned char) s[i];
      size_t len = 0;
      uint32_t cp = 0;
      if (c < 0x80) {
        len = 1;
        cp = c;
      } else if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        cp = c & 0x1Fu;
      } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        cp = c & 0x0Fu;
      } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        cp = c & 0x07u;
      }
      bool valid = len > 0 && i + len <= s.size();
      for (size_t k = 1; valid && k < len; ++k) {
        const unsigned char cc = (unsigned char) s[i + k];
        if ((cc & 0xC0) != 0x80) valid = false;
        cp = (cp << 6) | (cc & 0x3Fu);
      }
      if (valid && len > 1 &&
          ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
           (cp >= 0xD800 && cp <= 0xDFFF)))
        valid = false;
      if (valid) {
        units.push_back(cp);
        i += len;
      } else {
        units.push_back(0x110000u + c);
        ++i;
      }
    }
    offsets.push_back(s.size());
  }

  void compute(const std::string& original, const std::string& target) {
    std::vector<size_t> aBytes, bBytes;
    decode(original, a_, aBytes);
    decode(target, b_, bBytes);
    diffRange(0, a_.size(), 0, b_.size(), 0);
    a_.clear();
    b_.clear();
    changes.reserve(hunks_.size());
    for (const Hunk& h : hunks_) {
      Change c;
      // Every earlier hunk has been applied, so everything before this one
      // already reads as the target does.
      c.start = bBytes[h.bFrom];
      c.length = aBytes[h.aTo] - aBytes[h.aFrom];
      c.insertedText = target.substr(bBytes[h.bFrom], bBytes[h.bTo] - bBytes[h.bFrom]);
      changes.push_back(std::move(c));
    }
    hunks_.clear();
  }

  void emit(size_t aFrom, size_t aTo, size_t bFrom, size_t bTo) {
    if (aFrom == aTo && bFrom == bTo) return;
    // Hunks arrive left to right; two that touch are one change.
    if (!hunks_.empty() && hunks_.back().aTo == aFrom && hunks_.back().bTo == bFrom) {
      hunks_.back().aTo = aTo;
      hunks_.back().bTo = bTo;
      return;
    }
    hunks_.push_back(Hunk{aFrom, aTo, bFrom, bTo});
  }

  // The recursion halves the edit distance at every level, so its depth is
  // about log2(D); the cap is a guard against a split that fails to shrink
  // the problem, not a limit anything legitimate reaches.
  void diffRange(size_t aLo, size_t aHi, size_t bLo, size_t bHi, int depth) {
    while (aLo < aHi && bLo < bHi && a_[aLo] == b_[bLo]) {
      ++aLo;
      ++bLo;
    }
    while (aLo < aHi && bLo < bHi && a_[aHi - 1] == b_[bHi - 1]) {
      --aHi;
      --bHi;
    }
    if (aLo == aHi || bLo == bHi) {
      emit(aLo, aHi, bLo, bHi);
      return;
    }
    size_t splitA = 0, splitB = 0;
    const Search found = depth > 64 ? Search::gaveUp : middleSnake(aLo, aHi, bLo, bHi, splitA, splitB);
    if (found == Search::split && !((splitA == aLo && splitB == bLo) || (splitA == aHi && splitB == bHi))) {
      diffRange(aLo, splitA, bLo, splitB, depth + 1);
      diffRange(splitA, aHi, splitB, bHi, depth + 1);
      return;
    }
    // Nothing in common: replacing the whole region IS the minimal edit.
    // Otherwise it is the fallback, and the result is no longer minimal.
    if (found != Search::nothingInCommon) minimal_ = false;
    emit(aLo, aHi, bLo, bHi);
  }

  enum class Search { split, nothingInCommon, gaveUp };

  // Myers' bidirectional search over a[aLo,aHi) and b[bLo,bHi): furthest
  // reaching paths from the top-left and from the bottom-right, one more
  // difference at a time, until they overlap. The overlap is a point on an
  // optimal path; splitting there leaves two independent problems.
  //
  // Paths that never overlap within (N+M)/2 differences mean the edit
  // distance is N+M: the two share no character at all. gaveUp is the budget
  // running out, or the region being past the size guard.
  Search middleSnake(size_t aLo, size_t aHi, size_t bLo, size_t bHi, size_t& splitA, size_t& splitB) {
    if ((aHi - aLo) + (bHi - bLo) > kMaxRegion) return Search::gaveUp;
    const uint32_t* a = a_.data() + aLo;
    const uint32_t* b = b_.data() + bLo;
    const int64_t n = (int64_t) (aHi - aLo);
    const int64_t m = (int64_t) (bHi - bLo);
    const int64_t delta = n - m;
    const bool checkForward = (delta & 1) != 0;
    const int64_t maxD = (n + m + 1) / 2;
    const int64_t offset = maxD + 1;
    const int64_t length = 2 * maxD + 3;
    // int32 is enough: the size guard keeps every coordinate under 2^21.
    std::vector<int32_t> forward((size_t) length, -1), backward((size_t) length, -1);
    forward[(size_t) (offset + 1)] = 0;
    backward[(size_t) (offset + 1)] = 0;
    int64_t fStart = 0, fEnd = 0, bStart = 0, bEnd = 0;

    for (int64_t d = 0; d < maxD; ++d) {
      for (int64_t k = -d + fStart; k <= d - fEnd; k += 2) {
        const size_t ki = (size_t) (offset + k);
        int64_t x = (k == -d || (k != d && forward[ki - 1] < forward[ki + 1])) ? forward[ki + 1]
                                                                                 : forward[ki - 1] + 1;
        int64_t y = x - k;
        const int64_t snakeFrom = x;
        while (x < n && y < m && a[x] == b[y]) {
          ++x;
          ++y;
        }
        if (!spend((size_t) (1 + x - snakeFrom))) return Search::gaveUp;
        forward[ki] = (int32_t) x;
        if (x > n) {
          fEnd += 2; // ran off the right edge
        } else if (y > m) {
          fStart += 2; // ran off the bottom edge
        } else if (checkForward) {
          const int64_t bk = offset + delta - k;
          if (bk >= 0 && bk < length && backward[(size_t) bk] != -1 &&
              x >= n - backward[(size_t) bk]) {
            splitA = aLo + (size_t) x;
            splitB = bLo + (size_t) y;
            return Search::split;
          }
        }
      }
      for (int64_t k = -d + bStart; k <= d - bEnd; k += 2) {
        const size_t ki = (size_t) (offset + k);
        int64_t x = (k == -d || (k != d && backward[ki - 1] < backward[ki + 1]))
                        ? backward[ki + 1]
                        : backward[ki - 1] + 1;
        int64_t y = x - k;
        const int64_t snakeFrom = x;
        while (x < n && y < m && a[n - x - 1] == b[m - y - 1]) {
          ++x;
          ++y;
        }
        if (!spend((size_t) (1 + x - snakeFrom))) return Search::gaveUp;
        backward[ki] = (int32_t) x;
        if (x > n) {
          bEnd += 2;
        } else if (y > m) {
          bStart += 2;
        } else if (!checkForward) {
          const int64_t fk = offset + delta - k;
          if (fk >= 0 && fk < length && forward[(size_t) fk] != -1) {
            const int64_t fx = forward[(size_t) fk];
            const int64_t fy = fx - (fk - offset);
            if (fx >= n - x) {
              splitA = aLo + (size_t) fx;
              splitB = bLo + (size_t) fy;
              return Search::split;
            }
          }
        }
      }
    }
    return Search::nothingInCommon;
  }

  bool spend(size_t cost) {
    if (cost > budget_) {
      budget_ = 0;
      return false;
    }
    budget_ -= cost;
    return true;
  }

  size_t budget_;
  bool minimal_ = true;
  std::vector<uint32_t> a_, b_;
  std::vector<Hunk> hunks_;
};

} // namespace sonore
