package com.downstream

/**
 * The Kotlin mirror of `src/cpp/downstream/TableLayout.h`, which is the one
 * definition of how a table lays out. The shadow node, the measure path, and the
 * view all read this, so a measured table and a drawn table cannot disagree.
 *
 * Mirrored by `downstream::resolveTable` and `downstream::tableHeight`;
 * `testTableLayout` and `testTableHeight` in `src/cpp/tests/test_export.cpp` are
 * the table both sides have to agree on.
 */
internal data class TableLayoutProps(
  /** `tableMaxHeight` in pixels. 0 means no cap. */
  val maxHeight: Float = 300f,
) {
  // A NaN height must not make two identical props compare different, which is
  // the same rule `CodeLayoutProps` follows and for the same reason.
  override fun equals(other: Any?): Boolean =
    other is TableLayoutProps &&
      (maxHeight == other.maxHeight || (maxHeight.isNaN() && other.maxHeight.isNaN()))

  override fun hashCode(): Int = 31 + maxHeight.hashCode()
}

/**
 * The reader's own per-table choices, by table ordinal, each ascending.
 *
 * Ordinals are counted as the segment plan is built, so they are stable across
 * append-only snapshots and a choice survives streaming — the same property
 * `CodeChoices` has, and the same reason.
 */
internal class TableChoices(
  /** The tables whose height cap the expand button lifted. */
  val expanded: IntArray = IntArray(0),
) {
  override fun equals(other: Any?): Boolean =
    other is TableChoices && expanded.contentEquals(other.expanded)

  override fun hashCode(): Int = expanded.contentHashCode()

  /** The same choices with [ordinal]'s expand choice toggled, kept ascending. */
  fun copyWithExpand(ordinal: Int): TableChoices = TableChoices(CodeChoices.toggled(expanded, ordinal))
}

/**
 * How one table lays out. The only place props, choices, and props meet.
 *
 * A data class rather than the three loose fields a `CodeLayout` has, because a
 * table's cap is one number and its choice is one set.
 */
internal data class TableLayout(
  val props: TableLayoutProps = TableLayoutProps(),
  val choices: TableChoices = TableChoices(),
)

/** How one table lays out, from the layout and its ordinal. Mirrors `resolveTable`. */
internal data class TableBlockLayout(
  /** The cap this table actually has, in pixels. 0 when uncapped or expanded. */
  val maxHeight: Float = 0f,
  /** The reader lifted this table's cap. */
  val expanded: Boolean = false,
)

/**
 * Mirrors `resolveTable`.
 *
 * A negative ordinal means the caller does not know which table this is, and the
 * answer is "no cap": an unknown table has no reader's choice to apply and must
 * not draw one the reader did not make. Both platforms read the same answer, so a
 * segment that somehow lost its ordinal draws uncapped on either.
 */
internal fun resolveTable(layout: TableLayout, ordinal: Int): TableBlockLayout {
  if (ordinal < 0) {
    return TableBlockLayout()
  }
  val expanded = layout.props.maxHeight > 0 && hasChoice(layout.choices.expanded, ordinal)
  return TableBlockLayout(
    maxHeight = if (expanded) 0f else layout.props.maxHeight,
    expanded = expanded,
  )
}

/**
 * The lines a capped table still shows, whatever the font scale.
 *
 * A header row and two body rows, rather than the code block's five lines: a table
 * row is at least twice a line tall once its padding is counted, so three of them
 * is about the same readable amount.
 */
internal const val MIN_VISIBLE_TABLE_ROWS = 3

/** The space one table row takes, in pixels. Mirrors `kTableRowHeight`. */
internal const val TABLE_ROW_HEIGHT = 44f

/**
 * The height of one table: its toolbar plus the grid, capped. Mirrors
 * `tableHeight`.
 *
 * The cap is on the grid and the toolbar is outside it, which is the box a reader
 * scrolls and the box whose buttons they use to leave.
 */
internal fun tableHeight(table: TableBlockLayout, rowCount: Int, toolbarHeight: Float): Float {
  val grid = rowCount * TABLE_ROW_HEIGHT
  if (table.maxHeight <= 0f) {
    return toolbarHeight + grid
  }
  // The floor wins over the cap: a cap that would show one header row is not a
  // cap a reader can use.
  val floor = MIN_VISIBLE_TABLE_ROWS * TABLE_ROW_HEIGHT
  return toolbarHeight + minOf(grid, maxOf(table.maxHeight, floor))
}