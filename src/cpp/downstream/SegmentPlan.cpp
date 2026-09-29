#include "SegmentPlan.h"

namespace downstream {

std::vector<SegmentPlan> planSegments(const Document &document) {
  const auto &blocks = document.blocks;
  const size_t count = blocks.size();
  // Committed blocks form a prefix that ends at a top-level block.
  size_t completed = 0;
  while (completed < count && blocks[completed].identity != 0) {
    completed++;
  }

  std::vector<SegmentPlan> plan;
  size_t from = 0;
  while (from < count) {
    const Block &first = blocks[from];
    const bool committed = from < completed;
    const size_t limit = committed ? completed : count;
    const bool table = first.kind == BlockKind::TableRow;
    size_t to = from + 1;
    if (table) {
      while (to < limit && blocks[to].kind == BlockKind::TableRow && blocks[to].table == first.table) {
        to++;
      }
    } else {
      while (to < limit && blocks[to].kind != BlockKind::TableRow &&
             !(committed && to - from >= kCommittedChunkBlocks && blocks[to - 1].containers.empty())) {
        to++;
      }
    }
    plan.push_back({from, to, table, committed});
    from = to;
  }
  return plan;
}

bool isAdmonitionTitle(const Document &document, size_t index) {
  const Block &block = document.blocks[index];
  if (block.kind != BlockKind::Paragraph || block.containers.empty() ||
      block.containers.back().kind != ContainerKind::Admonition) {
    return false;
  }
  if (index == 0) {
    return true;
  }
  const size_t depth = block.containers.size() - 1;
  const auto &previous = document.blocks[index - 1].containers;
  return previous.size() <= depth || previous[depth].id != block.containers.back().id;
}

} // namespace downstream
