#pragma once

#include "Document.h"

#include <cstddef>
#include <vector>

namespace downstream {

/// Committed blocks are grouped into chunks of about this many blocks, so a
/// new commit rebuilds only the last chunk instead of the whole prefix.
constexpr size_t kCommittedChunkBlocks = 16;

/// One unit of platform layout: a table, or consecutive other blocks laid
/// out as one text.
struct SegmentPlan {
  size_t from{0};
  size_t to{0};
  bool table{false};
  /// Every block is committed. Committed blocks never change under their
  /// identity, so a committed segment is keyed by its first block's identity
  /// and its block count, and can be reused by later snapshots.
  bool committed{false};
};

/// Splits a document into segments. Committed chunks end only after a
/// top-level block, so no quote or list continues across them.
std::vector<SegmentPlan> planSegments(const Document &document);

/// The synthetic title paragraph is the first block inside an admonition.
bool isAdmonitionTitle(const Document &document, size_t index);

} // namespace downstream
