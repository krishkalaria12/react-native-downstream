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

    if (first.kind == BlockKind::CodeBlock) {
      // One code block is one card. A card carries its own header row and a
      // body that scrolls sideways, so it can never share a text segment with
      // the prose around it.
      plan.push_back({from, from + 1, SegmentKind::Code, committed});
      from += 1;
      continue;
    }

    if (first.kind == BlockKind::TableRow) {
      size_t to = from + 1;
      while (to < limit && blocks[to].kind == BlockKind::TableRow && blocks[to].table == first.table) {
        to++;
      }
      plan.push_back({from, to, SegmentKind::Table, committed});
      from = to;
      continue;
    }

    size_t to = from + 1;
    while (to < limit && blocks[to].kind != BlockKind::TableRow &&
           blocks[to].kind != BlockKind::CodeBlock &&
           !(committed && to - from >= kCommittedChunkBlocks && blocks[to - 1].containers.empty())) {
      to++;
    }
    plan.push_back({from, to, SegmentKind::Text, committed});
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

int32_t codeBlockOrdinal(const Document &document, size_t index) {
  if (index >= document.blocks.size() || document.blocks[index].kind != BlockKind::CodeBlock) {
    return -1;
  }
  int32_t ordinal = 0;
  for (size_t i = 0; i < index; i++) {
    if (document.blocks[i].kind == BlockKind::CodeBlock) {
      ordinal++;
    }
  }
  return ordinal;
}

} // namespace downstream
