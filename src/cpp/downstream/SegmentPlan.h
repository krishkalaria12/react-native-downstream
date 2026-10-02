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
/// separately from the prose flow. A formula is its own segment because it is
/// one drawable box that scrolls sideways on its own, and an image because its
/// height comes from a load rather than from text.
enum class SegmentKind : uint8_t {
  Text = 0,
  Table = 1,
  Code = 2,
  Math = 3,
  Image = 4,
};

/// One unit of platform layout.
struct SegmentPlan {
  size_t from{0};
  size_t to{0};
  SegmentKind kind{SegmentKind::Text};
  /// Code segments only: the block's ordinal among the document's code blocks.
  /// It is counted as the plan is built, so a platform resolves a reader's wrap
  /// choice for a block in constant time however many code blocks precede it.
  /// -1 for every other segment.
  int32_t codeOrdinal{-1};
  /// Image segments only: the ordinal of this block's first image, which is its
  /// index in `Document::images`. Counted as the plan is built, for the same
  /// reason as `codeOrdinal`, and -1 for every other segment.
  ///
  /// A block's images are looked up through `Block::images` rather than counted
  /// here, so this is the lookup's starting point rather than a second count
  /// that could disagree with the document's.
  int32_t imageOrdinal{-1};
  /// Every block is committed. Committed blocks never change under their
  /// identity, so a committed segment is keyed by its first block's identity
  /// and its block count, and can be reused by later snapshots.
  bool committed{false};

  bool isTable() const { return kind == SegmentKind::Table; }
  bool isCode() const { return kind == SegmentKind::Code; }
  bool isMath() const { return kind == SegmentKind::Math; }
  bool isImage() const { return kind == SegmentKind::Image; }
};

/// Splits a document into segments. Committed chunks end only after a
/// top-level block, so no quote or list continues across them, and a code
/// block ends the chunk before it.
std::vector<SegmentPlan> planSegments(const Document &document);

/// The synthetic title paragraph is the first block inside an admonition.
bool isAdmonitionTitle(const Document &document, size_t index);

} // namespace downstream
