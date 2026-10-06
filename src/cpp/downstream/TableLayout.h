#pragma once

#include "CodeLayout.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace downstream {

/**
 * The table layout inputs that come from props, and so are equal for every table
 * in one view.
 *
 * The same shape as `CodeLayoutProps`, for the same reason: `codeBlockMaxHeight`
 * and `tableMaxHeight` are the same decision about a different block, and one
 * definition of the rules is what keeps the two from drifting.
 */
struct TableLayoutProps {
  /**
   * `tableMaxHeight` in points. 0 means no cap, exactly as for a code block, and
   * the same float rather than a double for the same reason.
   */
  float maxHeight{300};

  /// A float compares by value, so a `NaN` height cannot make two identical
  /// props compare different. JavaScript normalizes a `NaN` away before it
  /// reaches native, so this only guards a direct caller.
  bool operator==(const TableLayoutProps &other) const {
    return maxHeight == other.maxHeight || (std::isnan(maxHeight) && std::isnan(other.maxHeight));
  }
};

/**
 * The reader's own per-table choices, by table ordinal, each sorted.
 *
 * Ordinals are counted as the segment plan is built, so they are stable across
 * append-only snapshots and a choice survives streaming — the same property
 * `CodeChoices` has, for the same reason.
 */
struct TableChoices {
  /// The tables whose height cap the expand button lifted.
  std::vector<uint32_t> expanded;

  bool operator==(const TableChoices &) const = default;
};

/// Everything that decides how a table lays out: the props and the reader's choices.
struct TableLayout {
  TableLayoutProps props;
  ReaderChoices choices;

  bool operator==(const TableLayout &) const = default;
};

/// The table bundle for a set of props and the reader's one set of choices.
inline TableLayout tableLayout(TableLayoutProps props, const ReaderChoices &choices) {
  return TableLayout{std::move(props), choices};
}

/// How one table lays out. The only place props, choices, and props meet.
struct TableBlockLayout {
  /**
   * The cap this table actually has, in points. 0 when uncapped or expanded,
   * which is what makes "no cap" one case rather than two.
   *
   * Initialised to 0 rather than left to default-initialisation, because a
   * default-constructed block is the natural way to say "no cap" and an
   * indeterminate float here would be a height that differs run to run.
   */
  float maxHeight{0};
  /// The reader lifted this table's cap. The toolbar keeps its button either
  /// way, so an expanded table can put the cap back.
  bool expanded = false;
};

/// The layout of the table at `ordinal`, from the props and the reader's choices.
///
/// A negative ordinal means the caller does not know which table this is, and the
/// answer is "no cap": an unknown table has no reader's choice to apply and must
/// not draw one the reader did not make. Both platforms read the same answer, so a
/// segment that somehow lost its ordinal draws uncapped on either rather than
/// capped on one and not the other.
TableBlockLayout resolveTable(const TableLayout &layout, int32_t ordinal);

/**
 * The lines a capped table still shows, whatever the font scale.
 *
 * A header row and two body rows, rather than the code block's five lines: a
 * table row is at least twice a line tall once its padding is counted, so three
 * of them is about the same readable amount, and a reader who cannot see the
 * columns they came for is better served by the expand control than by a table
 * cut at one row.
 */
inline constexpr float kMinVisibleTableRows = 3;

/**
 * The space a table's toolbar occupies, in points, and the cell padding a row
 * adds. Both are the same constants the platforms draw with, and both live here
 * rather than in either platform because a height decided in two places is a
 * height that eventually differs.
 */
inline constexpr float kTableToolbarHeight = 36;
inline constexpr float kTableRowHeight = 44;

/**
 * The height of one table: its toolbar plus the grid, capped.
 *
 * The cap is on the grid box and the toolbar is outside it, which is the box
 * Streamdown caps and the box a reader scrolls: the toolbar is what they use to
 * leave, so it must stay.
 *
 * The floor is `kMinVisibleTableRows` rows, so an accessibility text size still
 * shows a table a reader can read rather than one header and nothing else. A
 * table already shorter than the floor is laid out at its own height, so the
 * floor never adds space to a small table.
 */
float tableHeight(const TableBlockLayout &table, uint32_t rowCount, float toolbarHeight);

} // namespace downstream