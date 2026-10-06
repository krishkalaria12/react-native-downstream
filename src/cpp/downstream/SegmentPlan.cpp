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
  // Counted as the plan is built, so every code segment knows its ordinal for
  // free. Ordinals are stable across append-only snapshots, which is what lets
  // a reader's wrap choice survive the blocks before it being rewritten.
  int32_t codeOrdinal = 0;
  int32_t tableOrdinal = 0;
  while (from < count) {
    const Block &first = blocks[from];
    const bool committed = from < completed;
    const size_t limit = committed ? completed : count;

    if (first.kind == BlockKind::CodeBlock) {
      // One code block is one card. A card carries its own header row and a
      // body that scrolls sideways, so it can never share a text segment with
      // the prose around it.
      plan.push_back({from, from + 1, SegmentKind::Code, codeOrdinal, -1, -1, committed});
      codeOrdinal++;
      from += 1;
      continue;
    }

    if (first.kind == BlockKind::MathBlock) {
      // One formula is one box. A wide formula scrolls sideways inside its own
      // viewport rather than being scaled into the column, and a text segment
      // has neither a scroll view nor a drawable of its own.
      plan.push_back({from, from + 1, SegmentKind::Math, -1, -1, -1, committed});
      from += 1;
      continue;
    }

    if (first.kind == BlockKind::Image) {
      // One image block is one segment. Several images in one paragraph stack
      // vertically inside it, which is the same decision Streamdown's own
      // image-only paragraph makes, and it keeps a load from resizing a segment
      // that also holds prose.
      plan.push_back({from, from + 1, SegmentKind::Image, -1, static_cast<int32_t>(first.images.front()), -1,
                      committed});
      from += 1;
      continue;
    }

    if (first.kind == BlockKind::TableRow) {
      size_t to = from + 1;
      while (to < limit && blocks[to].kind == BlockKind::TableRow && blocks[to].table == first.table) {
        to++;
      }
      plan.push_back({from, to, SegmentKind::Table, -1, -1, tableOrdinal, committed});
      tableOrdinal++;
      from = to;
      continue;
    }

    size_t to = from + 1;
    while (to < limit && blocks[to].kind != BlockKind::TableRow && blocks[to].kind != BlockKind::CodeBlock &&
           blocks[to].kind != BlockKind::MathBlock && blocks[to].kind != BlockKind::Image &&
           !(committed && to - from >= kCommittedChunkBlocks && blocks[to - 1].containers.empty())) {
      to++;
    }
    plan.push_back({from, to, SegmentKind::Text, -1, -1, -1, committed});
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
