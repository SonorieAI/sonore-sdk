// SPDX-License-Identifier: Apache-2.0
//
// FlexBox and Grid: two ways to place things without arithmetic in resized().
//
// ── Why a plugin needs a layout system ──────────────────────────────────────
//
// Every layout in this SDK so far is hand-computed inside resized(): a row of
// knobs at kPadding + i * kRowHeight. That is fine for a row of knobs and it is
// not a layout system. The moment a plugin has a header, a body that should
// take the remaining height, and a footer pinned to the bottom, hand arithmetic
// becomes a set of subtractions that every subsequent change breaks.
//
// It matters more here than in most toolkits because a plugin editor is
// RESIZED by the host, arbitrarily and at any moment, and a generated plugin's
// layout code was written by something that never saw it happen.
//
// ── How faithful this is ────────────────────────────────────────────────────
//
// FlexBox follows CSS flexbox for the parts a plugin uses: direction, grow,
// shrink, basis, wrap, justify, align, align-self, and align-content in its
// stretch form. The size resolution is CSS's real algorithm -- freeze an item
// that hits a limit, share what is left among the rest, repeat.
//
// The first version of this file did NOT do that. It ran two passes and said so
// in a comment here, on the argument that two settle every layout a plugin has.
// They do not: an item capped by maxWidth beside an uncapped one leaves a third
// of the container unspent, which is not a pathological case but the ordinary
// one of a control with a sensible maximum next to a control without. The
// comment was wrong and the test that caught it was the second one written.
//
// What is genuinely absent, and named rather than left to be found: the other
// five align-content values, order, and content-based sizing. There is nothing
// here to measure a component against, so an `auto` Grid track is sized the
// way the reference sizes it -- by the width or height an item in it was
// GIVEN -- and never by what the component inside would draw.
#pragma once

#include <algorithm>
#include <vector>

#include "component.h"

namespace sonore {
namespace gfx {

/** How the free space along the main axis is spent. */
enum class Justify2 {
  Start,  ///< packed against the start: left in a row, top in a column, mirrored when reversed
  End,  ///< packed against the end
  Centre,  ///< packed in the middle
  SpaceBetween, ///< first at the start, last at the end, gaps equal
  SpaceAround,  ///< equal space around each, so the ends get half a gap
  SpaceEvenly,  ///< equal space everywhere, ends included
};

/** How an item sits across the axis it is not flowing along. */
enum class AlignItems {
  Stretch, ///< fill the cross axis -- flexbox's default, and usually what is meant
  Start,  ///< against the top of a row's line, the left of a column's
  End,  ///< against the bottom of a row's line, the right of a column's
  Centre,  ///< centred across the line
};

/**
 * Space kept clear around a FlexItem or a GridItem, in logical pixels: the
 * item's box is laid out with it and the component placed inside it.
 *
 * The constructors follow CSS's shorthand order -- one value for all four
 * sides, then vertical and horizontal, then top, right, bottom, left -- so a
 * margin copied from a design tool means the same thing here.
 */
struct Margin {
  float top = 0.0f, /**< above */ right = 0.0f, /**< to the right */ bottom = 0.0f, /**< below */ left = 0.0f; ///< to the left

  Margin() = default; ///< no margin on any side
  explicit Margin(float all) : top(all), right(all), bottom(all), left(all) {}  ///< the same on all four sides
  /** `vertical` above and below, `horizontal` left and right. */
  Margin(float vertical, float horizontal)
      : top(vertical), right(horizontal), bottom(vertical), left(horizontal) {}
  Margin(float t, float r, float b, float l) : top(t), right(r), bottom(b), left(l) {}  ///< CSS's order: top, right, bottom, left

  float horizontal() const { return left + right; }  ///< left + right
  float vertical() const { return top + bottom; }  ///< top + bottom
};

/**
 * One thing in a FlexBox.
 *
 * Carries a Component OR nothing: an item with no component is a SPACER, which
 * is how a layout puts a gap somewhere without inventing an invisible child.
 */
struct FlexItem {
  /** What gets the bounds; not owned. Null makes the item a spacer. */
  Component* component = nullptr;
  /** Along the main axis, in logical pixels. 0 (or less) means "use
   *  width/height", and width/height themselves default to zero -- so an item
   *  with only a grow factor starts at nothing and takes its share, which is
   *  what people expect from `flex: 1`. */
  float basis = 0.0f;
  /** This item's share of the room left on its line, relative to the other
   *  items' grow; 0 takes none. */
  float grow = 0.0f;
  /** How readily it gives up room when the line overflows, weighted by its base
   *  size as in CSS; 0 never goes below its base. */
  float shrink = 1.0f;
  /** Logical pixels. Along the main axis the base size when `basis` is 0;
   *  across it the item's size, where 0 stretches it to the line. */
  float width = 0.0f, height = 0.0f;  ///< the same for the height, axes swapped
  /** The least width it is given, in logical pixels, along or across. */
  float minWidth = 0.0f, minHeight = 0.0f;  ///< and the least height
  /** Zero means unbounded. A maximum of zero would be a control with no size,
   *  which nobody means. */
  float maxWidth = 0.0f, maxHeight = 0.0f;  ///< and the same for the height
  /** Space kept clear around it, taken out of its line. */
  Margin margin;
  /** Stretch by default, like the container; anything else overrides it. */
  bool hasAlignSelf = false;
  /** Its own cross alignment, used when `hasAlignSelf` is set. */
  AlignItems alignSelf = AlignItems::Stretch;

  /** A spacer of no size until given a basis, a size or a grow factor. */
  FlexItem() = default;
  /** `c`, not owned, with no size yet: give it a basis, a size or a grow
   *  factor, or it is laid out at zero. */
  explicit FlexItem(Component* c) : component(c) {}

  /** The base size along the main axis, logical pixels; 0 uses width or
   *  height. Each with...() returns this item, for chaining. */
  FlexItem& withBasis(float b) { basis = b; return *this; }
  /** Its share of the room left on the line. */
  FlexItem& withGrow(float g) { grow = g; return *this; }
  /** How readily it gives up room; 0 never shrinks. */
  FlexItem& withShrink(float sh) { shrink = sh; return *this; }
  /** The width: base size in a row, size across a column (0 stretches). */
  FlexItem& withWidth(float w) { width = w; return *this; }
  /** The height: base size in a column, size across a row (0 stretches). */
  FlexItem& withHeight(float h) { height = h; return *this; }
  /** The least width it is given. */
  FlexItem& withMinWidth(float w) { minWidth = w; return *this; }
  /** The least height it is given. */
  FlexItem& withMinHeight(float h) { minHeight = h; return *this; }
  /** The most width it is given; 0 is unbounded. */
  FlexItem& withMaxWidth(float w) { maxWidth = w; return *this; }
  /** The most height it is given; 0 is unbounded. */
  FlexItem& withMaxHeight(float h) { maxHeight = h; return *this; }
  /** Space kept clear around it. */
  FlexItem& withMargin(const Margin& m) { margin = m; return *this; }
  /** Align this item across the line by `a` rather than by the box's
   *  alignItems. */
  FlexItem& withAlignSelf(AlignItems a) {
    hasAlignSelf = true;
    alignSelf = a;
    return *this;
  }

  /** A gap of a fixed size, with nothing in it. */
  static FlexItem spacer(float size) {
    FlexItem item;
    item.basis = size;
    item.shrink = 0.0f;
    return item;
  }

  /** A gap that takes whatever is left. Two of these either side of something
   *  centres it, which is the flexbox idiom. */
  static FlexItem stretch(float growFactor = 1.0f) {
    FlexItem item;
    item.grow = growFactor;
    return item;
  }
};

/**
 * A row or a column, laid out like CSS flexbox.
 *
 * performLayout() sets bounds on every item's component and leaves items with
 * none alone -- so a spacer costs nothing and a caller can lay out a mix of
 * real controls and gaps in one list.
 */
class FlexBox {
public:
  /** The main axis and its direction. A reversed one is the forward layout
   *  mirrored, free space and justification included. */
  enum class Direction { Row, /**< left to right */ Column, /**< top to bottom */ RowReverse, /**< right to left */ ColumnReverse /**< bottom to top */ };

  /** The main axis, and which way along it the items go. */
  Direction direction = Direction::Row;
  /** How room left along a line is spent; each line on its own when wrapping. */
  Justify2 justifyContent = Justify2::Start;
  /** How items sit across their line, unless an item's alignSelf says
   *  otherwise. An item with no size across is stretched whatever this is. */
  AlignItems alignItems = AlignItems::Stretch;
  /** Items that do not fit start a new line rather than overflowing. */
  bool wrap = false;
  /** Between lines, when wrapping. */
  float lineGap = 0.0f;

  /** What is laid out, in order. The components are not owned. */
  std::vector<FlexItem> items;

  /** Append `item`; returns this box, for chaining. */
  FlexBox& add(const FlexItem& item) {
    items.push_back(item);
    return *this;
  }

  /**
   * Place everything inside `area`.
   *
   * `area` is in the coordinates the components' bounds are in -- their
   * parent's -- so a caller inside resized() passes localBounds() and the
   * children land where they should.
   */
  void performLayout(const Rect& area) {
    if (items.empty()) return;
    const bool horizontal = isHorizontal();
    const float mainSize = horizontal ? area.w : area.h;

    // ── Split into lines ───────────────────────────────────────────────────
    std::vector<std::vector<size_t>> lines;
    if (!wrap) {
      lines.emplace_back();
      for (size_t i = 0; i < items.size(); ++i) lines.back().push_back(i);
    } else {
      lines.emplace_back();
      float used = 0.0f;
      for (size_t i = 0; i < items.size(); ++i) {
        const float outer = baseMain(items[i], horizontal) + marginMain(items[i], horizontal);
        // Never an EMPTY line: an item wider than the container would otherwise
        // start a line, not fit, start another, and so on forever.
        if (!lines.back().empty() && used + outer > mainSize) {
          lines.emplace_back();
          used = 0.0f;
        }
        lines.back().push_back(i);
        used += outer;
      }
    }

    // ── Cross-axis sizes, line by line ─────────────────────────────────────
    std::vector<float> lineCross(lines.size(), 0.0f);
    float totalCross = 0.0f;
    for (size_t l = 0; l < lines.size(); ++l) {
      float tallest = 0.0f;
      for (size_t i : lines[l]) {
        const float outer = baseCross(items[i], horizontal) + marginCross(items[i], horizontal);
        if (outer > tallest) tallest = outer;
      }
      lineCross[l] = tallest;
      totalCross += tallest;
    }
    totalCross += lineGap * (float) (lines.size() > 0 ? lines.size() - 1 : 0);

    const float containerCross = horizontal ? area.h : area.w;
    // A single unwrapped line fills the container across, which is what makes
    // Stretch mean anything.
    if (lines.size() == 1) {
      lineCross[0] = containerCross;
    } else if (totalCross < containerCross) {
      // align-content: stretch, which is CSS's default and not an extra.
      //
      // Without it, items that were given a width but no height produce lines
      // of zero cross size -- so every wrapped line lands at the same y and the
      // layout looks like wrapping simply does not work. That is exactly what
      // the first version of this did.
      const float surplus = (containerCross - totalCross) / (float) lines.size();
      for (size_t l = 0; l < lines.size(); ++l) lineCross[l] += surplus;
    }

    float crossAt = horizontal ? area.y : area.x;

    for (size_t l = 0; l < lines.size(); ++l) {
      layoutLine(lines[l], area, crossAt, lineCross[l], horizontal);
      crossAt += lineCross[l] + lineGap;
    }
  }

  /** Whether the main axis runs across: Row or RowReverse. */
  bool isHorizontal() const {
    return direction == Direction::Row || direction == Direction::RowReverse;
  }

  /** Whether items run from the end: RowReverse or ColumnReverse. */
  bool isReversed() const {
    return direction == Direction::RowReverse || direction == Direction::ColumnReverse;
  }

private:
  static float baseMain(const FlexItem& item, bool horizontal) {
    if (item.basis > 0.0f) return item.basis;
    return horizontal ? item.width : item.height;
  }

  static float baseCross(const FlexItem& item, bool horizontal) {
    return horizontal ? item.height : item.width;
  }

  static float marginMain(const FlexItem& item, bool horizontal) {
    return horizontal ? item.margin.horizontal() : item.margin.vertical();
  }

  static float marginCross(const FlexItem& item, bool horizontal) {
    return horizontal ? item.margin.vertical() : item.margin.horizontal();
  }

  static float minMain(const FlexItem& item, bool horizontal) {
    return horizontal ? item.minWidth : item.minHeight;
  }

  static float maxMain(const FlexItem& item, bool horizontal) {
    return horizontal ? item.maxWidth : item.maxHeight;
  }

  void layoutLine(const std::vector<size_t>& line, const Rect& area, float crossStart,
                  float crossSize, bool horizontal) {
    const float mainSize = horizontal ? area.w : area.h;
    const float mainStart = horizontal ? area.x : area.y;

    std::vector<float> sizes(line.size(), 0.0f);
    float used = 0.0f;
    for (size_t k = 0; k < line.size(); ++k) {
      const FlexItem& item = items[line[k]];
      // Held to its limits from the START -- CSS's hypothetical main size.
      // Clamped only while distributing, an item with nothing to flex, or a
      // row that fitted exactly, kept a size its own maximum forbade.
      sizes[k] = clampMain(item, baseMain(item, horizontal), horizontal);
      used += sizes[k] + marginMain(item, horizontal);
    }

    // ── Distribute, freezing whatever hits a limit ─────────────────────────
    //
    // This is CSS's algorithm and not an approximation of it, because the
    // approximation failed a case any plugin would hit. The first version ran
    // two passes: one distribution, one to hand back what clamping took. With
    // an item capped by maxWidth beside an uncapped one, two passes leave a
    // third of the space unspent -- the cap absorbs its share, the second pass
    // gives away half of what came back, and the third pass that would have
    // finished the job never runs.
    //
    // Freezing costs one loop and is exactly right: an item that hits its
    // minimum or maximum stops taking part, and everything left divides what
    // remains. It terminates because each round freezes at least one item or
    // spends everything.
    {
      std::vector<bool> frozen(line.size(), false);
      for (size_t k = 0; k < line.size(); ++k) {
        const FlexItem& item = items[line[k]];
        // An item with no flexibility in the direction needed is frozen from
        // the start, so it never soaks up a share it cannot keep.
        if (item.grow <= 0.0f && item.shrink <= 0.0f) frozen[k] = true;
      }

      for (size_t round = 0; round <= line.size(); ++round) {
        float free = mainSize - used;
        if (free > -0.01f && free < 0.01f) break;

        const bool growing = free > 0.0f;
        float weight = 0.0f;
        for (size_t k = 0; k < line.size(); ++k) {
          if (frozen[k]) continue;
          const FlexItem& item = items[line[k]];
          if (growing) weight += item.grow;
          else weight += item.shrink * baseMain(item, horizontal);
        }
        if (weight <= 0.0f) break; // nothing left that can move

        bool anyFroze = false;
        float moved = 0.0f;
        for (size_t k = 0; k < line.size(); ++k) {
          if (frozen[k]) continue;
          const FlexItem& item = items[line[k]];
          const float share = growing ? item.grow
                                      : item.shrink * baseMain(item, horizontal);
          if (share <= 0.0f) {
            frozen[k] = true;
            anyFroze = true;
            continue;
          }
          const float wanted = sizes[k] + free * (share / weight);
          const float got = clampMain(item, wanted, horizontal);
          if (got != wanted) {
            // It hit a limit. Take what it could, and let the rest be shared
            // out among whatever is still free.
            frozen[k] = true;
            anyFroze = true;
          }
          moved += got - sizes[k];
          sizes[k] = got;
        }
        used += moved;
        if (!anyFroze) break; // everything took its full share; nothing left to do
      }
    }

    // ── Where the line starts, and what sits between items ─────────────────
    const float free = mainSize - used;
    float cursor = mainStart;
    float between = 0.0f;
    const float n = (float) line.size();
    switch (justifyContent) {
      case Justify2::Start:
        break;
      case Justify2::End:
        cursor += free;
        break;
      case Justify2::Centre:
        cursor += free * 0.5f;
        break;
      case Justify2::SpaceBetween:
        // One item gets no gap at all, and putting it at the start rather than
        // spreading nothing is what CSS does.
        between = n > 1.0f ? free / (n - 1.0f) : 0.0f;
        break;
      case Justify2::SpaceAround:
        between = n > 0.0f ? free / n : 0.0f;
        cursor += between * 0.5f;
        break;
      case Justify2::SpaceEvenly:
        between = free / (n + 1.0f);
        cursor += between;
        break;
    }

    for (size_t k = 0; k < line.size(); ++k) {
      // Reversed order is applied HERE rather than by reversing the item list,
      // so grow and shrink still see the order the caller wrote and a
      // row-reverse layout is the same layout mirrored rather than a different
      // one.
      //
      // Laid out forwards and then reflected, so it IS the mirror -- free
      // space, justification and margins included. Reversing only the order
      // kept packing from the start edge, which a reversed row does not.
      const size_t index = line[k];
      const size_t sizeIndex = k;
      const FlexItem& item = items[index];
      const float mainMargin = horizontal ? item.margin.left : item.margin.top;
      const float size = sizes[sizeIndex];

      const AlignItems align = item.hasAlignSelf ? item.alignSelf : alignItems;
      const float crossMarginStart = horizontal ? item.margin.top : item.margin.left;
      const float crossAvailable = crossSize - marginCross(item, horizontal);
      float crossExtent = baseCross(item, horizontal);
      if (align == AlignItems::Stretch || crossExtent <= 0.0f) crossExtent = crossAvailable;
      crossExtent = clampCross(item, crossExtent, horizontal);

      float crossPos = crossStart + crossMarginStart;
      if (align == AlignItems::End) crossPos += crossAvailable - crossExtent;
      else if (align == AlignItems::Centre) crossPos += (crossAvailable - crossExtent) * 0.5f;

      float mainPos = cursor + mainMargin;
      if (isReversed()) mainPos = mainStart + mainStart + mainSize - mainPos - size;
      if (item.component) {
        if (horizontal)
          item.component->setBounds({mainPos, crossPos, size, crossExtent});
        else
          item.component->setBounds({crossPos, mainPos, crossExtent, size});
      }
      cursor += size + marginMain(item, horizontal) + between;
    }
  }

  static float clampMain(const FlexItem& item, float v, bool horizontal) {
    const float lo = minMain(item, horizontal);
    const float hi = maxMain(item, horizontal);
    if (v < lo) v = lo;
    if (hi > 0.0f && v > hi) v = hi;
    // Never negative: shrinking past zero would give a control an inverted
    // rectangle, which hit-tests as nothing and paints as nothing.
    return v < 0.0f ? 0.0f : v;
  }

  static float clampCross(const FlexItem& item, float v, bool horizontal) {
    const float lo = horizontal ? item.minHeight : item.minWidth;
    const float hi = horizontal ? item.maxHeight : item.maxWidth;
    if (v < lo) v = lo;
    if (hi > 0.0f && v > hi) v = hi;
    return v < 0.0f ? 0.0f : v;
  }
};

/**
 * A track in a Grid: a fixed size, a share of what is left, or sized by its
 * items.
 *
 * Fr is the one that matters. A plugin editor that has to fill whatever height
 * a host gives it is a header, a `1fr` body and a footer -- three lines instead
 * of a page of subtractions that break on the next change.
 */
struct TrackSize {
  /** Which of the three a track is. */
  enum class Kind { Pixels, /**< `value` logical pixels */ Fraction, /**< `value` shares of what is left */ Auto /**< as big as its items say */ };
  /** How `value` is read. */
  Kind kind = Kind::Auto;
  /** Logical pixels for Pixels, a share for Fraction, unused for Auto. */
  float value = 0.0f;

  /** A fixed track, in logical pixels. Kept even when it overflows the grid. */
  static TrackSize px(float pixels) { return {Kind::Pixels, pixels}; }
  /** `fraction` shares of what the fixed tracks, auto tracks and gaps leave. */
  static TrackSize fr(float fraction = 1.0f) { return {Kind::Fraction, fraction}; }
  /** As big as the largest width (a column) or height (a row) given to an
   *  item that sits in this track alone, its margin on that side included;
   *  0 when no such item has one. Beside fractions that is all it gets: it is
   *  fixed space, and the fractions share the rest. With no fractions, the
   *  auto tracks also share what is left evenly -- so unsized ones divide
   *  the room evenly between them. */
  static TrackSize automatic() { return {Kind::Auto, 0.0f}; }
};

/** One thing in a Grid, placed by track index. Spans are 1-based counts, not
 *  end indices: `span 2` means two tracks, which is what CSS means by it. */
struct GridItem {
  /** What gets the bounds; not owned. An item without one is skipped. */
  Component* component = nullptr;
  int column = 0, /**< 0-based track index */ row = 0;      ///< 0-based track index
  int columnSpan = 1, /**< tracks covered */ rowSpan = 1; ///< tracks covered
  /** Space kept clear inside the cell. */
  Margin margin;
  /** What width and height hold until they are given one, spelt and valued
   *  as the reference spells and values it. Any negative size is unset. */
  static constexpr float notAssigned = -1.0f;
  /** Logical pixels, read for one thing only: an `auto` column this item
   *  covers alone (a columnSpan of 1) is as wide as the widest such width
   *  plus that item's left and right margin. The component still fills its
   *  cell less the margin, in an auto track or any other. */
  float width = notAssigned;
  /** The same for an `auto` row the item covers alone, with the top and
   *  bottom margin. */
  float height = notAssigned;

  /** No component, so placed nowhere. */
  GridItem() = default;
  /** `c`, not owned, in 0-based column `col` and row `r`, one track each. */
  GridItem(Component* c, int col, int r) : component(c), column(col), row(r) {}

  /** Cover `columns` by `rows` tracks and the gaps between them; less than 1
   *  is 1. Returns this item, for chaining. */
  GridItem& withSpan(int columns, int rows) {
    columnSpan = columns < 1 ? 1 : columns;
    rowSpan = rows < 1 ? 1 : rows;
    return *this;
  }
  /** Space kept clear inside the cell; returns this item. */
  GridItem& withMargin(const Margin& m) {
    margin = m;
    return *this;
  }
  /** The width an `auto` column reads; returns this item. */
  GridItem& withWidth(float w) {
    width = w;
    return *this;
  }
  /** The height an `auto` row reads; returns this item. */
  GridItem& withHeight(float h) {
    height = h;
    return *this;
  }
};

/** Rows and columns, sized in pixels, in shares of what is left, or by the
 *  sizes their items were given. */
class Grid {
public:
  /** The columns, left to right. A grid with no columns or no rows places
   *  nothing. */
  std::vector<TrackSize> templateColumns;
  /** The rows, top to bottom. */
  std::vector<TrackSize> templateRows;
  /** Logical pixels between neighbouring columns; none at the edges. */
  float columnGap = 0.0f;
  /** Logical pixels between neighbouring rows; none at the edges. */
  float rowGap = 0.0f;
  /** What is placed. The components are not owned. */
  std::vector<GridItem> items;

  /** Append `item`; returns this grid, for chaining. */
  Grid& add(const GridItem& item) {
    items.push_back(item);
    return *this;
  }

  /**
   * Place every item's component inside `area`, which is in the components'
   * parent's coordinates -- localBounds() from inside resized().
   *
   * An item with no component, a negative index, or a column or row past the
   * last track is skipped; a span that runs past the last track is cut there.
   * Fixed tracks, and what the items of auto tracks ask for, are kept even
   * when they overflow `area`, and the fractions then get nothing.
   */
  void performLayout(const Rect& area) {
    const std::vector<float> cols = columnSizes(area.w);
    const std::vector<float> rows = rowSizes(area.h);
    if (cols.empty() || rows.empty()) return;

    for (const GridItem& item : items) {
      if (!item.component) continue;
      if (item.column < 0 || item.row < 0) continue;
      if (item.column >= (int) cols.size() || item.row >= (int) rows.size()) continue;

      float x = area.x;
      for (int c = 0; c < item.column; ++c) x += cols[(size_t) c] + columnGap;
      float y = area.y;
      for (int r = 0; r < item.row; ++r) y += rows[(size_t) r] + rowGap;

      // A span reaches across the gaps it covers as well as the tracks, or a
      // cell spanning two columns is short by exactly one gap -- which reads as
      // a mysteriously misaligned control rather than as a layout bug.
      float w = 0.0f;
      for (int c = item.column; c < item.column + item.columnSpan && c < (int) cols.size(); ++c) {
        w += cols[(size_t) c];
        if (c > item.column) w += columnGap;
      }
      float h = 0.0f;
      for (int r = item.row; r < item.row + item.rowSpan && r < (int) rows.size(); ++r) {
        h += rows[(size_t) r];
        if (r > item.row) h += rowGap;
      }

      item.component->setBounds({x + item.margin.left, y + item.margin.top,
                                 w - item.margin.horizontal(), h - item.margin.vertical()});
    }
  }

  /** The resolved size of each track, for a caller placing something by hand
   *  beside a grid. The items are read for the auto tracks, as performLayout
   *  reads them. */
  std::vector<float> columnSizes(float width) const {
    return resolve(templateColumns, width, columnGap, measure(true));
  }
  /** The same for the rows, given the grid's height. */
  std::vector<float> rowSizes(float height) const {
    return resolve(templateRows, height, rowGap, measure(false));
  }

private:
  // What each auto track's items ask for: the largest width (columns) or
  // height (rows) given to an item that covers that track and no other, plus
  // the item's margin on that axis, because the component is placed inside
  // it. That is the reference's rule (its auto tracks are sized the same way,
  // from items with a span of one), with one difference: there an unset size
  // is -1 and still counted, so an unsized item with a margin asks for its
  // margin less a pixel. Here an unset size asks for nothing.
  //
  // An item spanning several tracks asks nothing of any of them, again as in
  // the reference: it fits whatever its tracks come to rather than this
  // guessing how to split its size among them. An item performLayout skips
  // -- no component, or outside the grid -- asks nothing either.
  std::vector<float> measure(bool columns) const {
    const std::vector<TrackSize>& tracks = columns ? templateColumns : templateRows;
    std::vector<float> need(tracks.size(), 0.0f);
    for (const GridItem& item : items) {
      if (!item.component || item.column < 0 || item.row < 0) continue;
      if (item.column >= (int) templateColumns.size() || item.row >= (int) templateRows.size()) continue;
      if ((columns ? item.columnSpan : item.rowSpan) != 1) continue;
      const size_t track = (size_t) (columns ? item.column : item.row);
      if (tracks[track].kind != TrackSize::Kind::Auto) continue;
      const float size = columns ? item.width : item.height;
      if (!(size >= 0.0f)) continue; // unset: notAssigned, or any other negative
      const float outer = size + (columns ? item.margin.horizontal() : item.margin.vertical());
      if (outer > need[track]) need[track] = outer;
    }
    return need;
  }

  static std::vector<float> resolve(const std::vector<TrackSize>& tracks, float total, float gap,
                                    const std::vector<float>& measured) {
    std::vector<float> out(tracks.size(), 0.0f);
    if (tracks.empty()) return out;

    float fixed = gap * (float) (tracks.size() - 1);
    float fractions = 0.0f;
    int autos = 0;
    for (size_t i = 0; i < tracks.size(); ++i) {
      switch (tracks[i].kind) {
        case TrackSize::Kind::Pixels:
          out[i] = tracks[i].value;
          fixed += out[i];
          break;
        case TrackSize::Kind::Fraction:
          fractions += tracks[i].value;
          break;
        case TrackSize::Kind::Auto:
          // What its items asked for is fixed space, as a pixel track is,
          // and is settled BEFORE the fractions: `1fr` means "the rest".
          out[i] = measured[i];
          fixed += out[i];
          ++autos;
          break;
      }
    }

    float remaining = total - fixed;
    if (remaining < 0.0f) remaining = 0.0f;

    // With no fractions to take the rest, the auto tracks share it evenly on
    // top of what their items asked for, which is CSS's stretching of auto
    // tracks, and keeps a grid of unsized auto tracks dividing the room
    // evenly as it always has. The reference leaves the rest empty here,
    // which would give every grid of unsized auto tracks cells of no size.
    if (autos > 0 && fractions <= 0.0f) {
      const float each = remaining / (float) autos;
      for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].kind == TrackSize::Kind::Auto) out[i] += each;
      return out;
    }
    // Beside fractions an auto track gets exactly what its items asked for,
    // as in the reference and in CSS, and the fractions share everything
    // else. It used to get half of an even share whatever was in it (a
    // quarter of the room beside one fr), a number nothing explained; a
    // layout that leaned on that now gives the item in its auto track a
    // width or a height.
    if (fractions > 0.0f)
      for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].kind == TrackSize::Kind::Fraction)
          out[i] = remaining * (tracks[i].value / fractions);

    return out;
  }
};

} // namespace gfx
} // namespace sonore
