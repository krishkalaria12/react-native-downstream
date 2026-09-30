#pragma once

#include "Document.h"

#include <cstddef>
#include <vector>

namespace downstream {

/// Committed blocks are grouped into chunks of about this many blocks, so a
/// new commit rebuilds only the last chunk instead of the whole prefix.
constexpr size_t kCommittedChunkBlocks = 16;

/// What a segment is drawn as. A code block is its own segment because it gets
/// a header and a body that does not wrap, neither of which a text segment has
/// room for; a table is its own segment because its columns are laid out
/// separately from the prose flow.
enum class SegmentKind : uint8_t {
  Text = 0,
  Table = 1,
  Code = 2,
};

/// One unit of platform layout.
struct SegmentPlan {
  size_t from{0};
  size_t to{0};
  SegmentKind kind{SegmentKind::Text};
  /// Every block is committed. Committed blocks never change under their
  /// identity, so a committed segment is keyed by its first block's identity
  /// and its block count, and can be reused by later snapshots.
  bool committed{false};

  bool isTable() const { return kind == SegmentKind::Table; }
  bool isCode() const { return kind == SegmentKind::Code; }
};

/// Splits a document into segments. Committed chunks end only after a
/// top-level block, so no quote or list continues across them, and a code
/// block ends the chunk before it.
std::vector<SegmentPlan> planSegments(const Document &document);

/// The synthetic title paragraph is the first block inside an admonition.
bool isAdmonitionTitle(const Document &document, size_t index);

/// The block's ordinal among the document's code blocks, or -1 when it is not
/// one. A reader's wrap choice is keyed by this rather than by the block index,
/// because ordinals are stable across append-only snapshots and so a choice
/// survives the blocks before it being rewritten.
int32_t codeBlockOrdinal(const Document &document, size_t index);

} // namespace downstream
