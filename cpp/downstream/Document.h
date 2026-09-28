#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace downstream {

/// Half-open range of UTF-16 code units in the canonical source string.
struct SourceRange {
  uint32_t start{0};
  uint32_t end{0};

  bool operator==(const SourceRange &other) const {
    return start == other.start && end == other.end;
  }
};

enum class BlockKind : uint8_t {
  Paragraph = 0,
  Heading = 1,
  CodeBlock = 2,
  HtmlBlock = 3,
  ThematicBreak = 4,
  TableRow = 5,
};

enum class ContainerKind : uint8_t {
  Quote = 0,
  ListItem = 1,
  Admonition = 2,
  Footnote = 3,
};

enum class TaskState : uint8_t {
  None = 0,
  Unchecked = 1,
  Checked = 2,
};

/// Vertical gap before a block. Platforms map these to point values.
enum class BlockSpacing : uint8_t {
  None = 0,
  Tight = 1,
  Normal = 2,
};

enum class CellAlign : uint8_t {
  Default = 0,
  Left = 1,
  Center = 2,
  Right = 3,
};

namespace InlineStyle {
constexpr uint32_t Strong = 1u << 0;
constexpr uint32_t Emphasis = 1u << 1;
constexpr uint32_t Strikethrough = 1u << 2;
constexpr uint32_t Code = 1u << 3;
constexpr uint32_t Link = 1u << 4;
constexpr uint32_t ImageAlt = 1u << 5;
constexpr uint32_t FootnoteRef = 1u << 6;
} // namespace InlineStyle

/// A maximal run of block text sharing one style set. Offsets are UTF-16
/// code units into Block::text. Runs never overlap; unstyled text has no run.
struct InlineRun {
  uint32_t start{0};
  uint32_t end{0};
  uint32_t styles{0};
  /// Index into Document::links, or -1.
  int32_t link{-1};
};

/// One level of block nesting around a leaf block, outermost first.
struct Container {
  ContainerKind kind{ContainerKind::Quote};
  /// Unique within the document; adjacent blocks share it when they belong
  /// to the same quote or list item.
  uint32_t id{0};
  /// ListItem and Footnote: true only on the first leaf block of the item,
  /// where the marker is drawn.
  bool showsMarker{false};
  TaskState task{TaskState::None};
  /// Rendered marker for list items and footnotes ("•", "3.", "1."), or the
  /// admonition type. Markers are decoration and are not part of the text.
  std::u16string marker;
};

struct TableCell {
  uint32_t start{0};
  uint32_t end{0};
  CellAlign align{CellAlign::Default};
};

/// A leaf block: the unit of text layout. Container blocks (quotes, lists)
/// are flattened into each leaf's container path.
struct Block {
  BlockKind kind{BlockKind::Paragraph};
  uint8_t headingLevel{0};
  bool tableHeader{false};
  BlockSpacing spacingBefore{BlockSpacing::Normal};
  std::vector<Container> containers;
  /// Visible text. Hard line breaks and code lines are '\n'; table cells are
  /// separated by '\t'. Thematic breaks have empty text.
  std::u16string text;
  std::vector<InlineRun> runs;
  std::vector<TableCell> cells;
  /// Fenced code info string language, UTF-8.
  std::string language;
};

/// An actionable link. Links that fail the URL policy are plain text and are
/// not listed here.
struct Link {
  std::string url;
  /// The link's Markdown occurrence in the source, including delimiters.
  SourceRange range;
};

/// Immutable parse result shared by measurement and mounting.
struct Document {
  uint64_t id{0};
  std::vector<Block> blocks;
  std::vector<Link> links;
};

using DocumentHandle = std::shared_ptr<const Document>;

} // namespace downstream
