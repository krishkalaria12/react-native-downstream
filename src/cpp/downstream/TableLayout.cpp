#include "TableLayout.h"

namespace downstream {

TableBlockLayout resolveTable(const TableLayout &layout, uint32_t ordinal) {
  TableBlockLayout table;
  // Expanded only while there is a cap to lift, so a choice left over from
  // before the prop went to 0 does not offer to collapse an uncapped table.
  table.expanded = layout.props.maxHeight > 0 && hasChoice(layout.choices.expanded, ordinal);
  // An expanded table has no cap, and so has an uncapped one: both reach the
  // view as the single "no cap" value, as a code block's does.
  table.maxHeight = table.expanded ? 0.0f : layout.props.maxHeight;
  return table;
}

float tableHeight(const TableBlockLayout &table, uint32_t rowCount, float toolbarHeight) {
  const float grid = static_cast<float>(rowCount) * kTableRowHeight;
  if (!(table.maxHeight > 0)) {
    return toolbarHeight + grid;
  }
  // The floor wins over the cap, not the reverse: a cap that would show one
  // header row is not a cap a reader can use. A table already shorter than the
  // floor is laid out at its own height, so the floor never adds space.
  const float floor = kMinVisibleTableRows * kTableRowHeight;
  return toolbarHeight + std::min(grid, std::max(table.maxHeight, floor));
}

} // namespace downstream
