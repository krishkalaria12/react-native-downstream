#pragma once

#include "Document.h"
#include "Theme.h"

namespace downstream {
namespace theme_layout {

/// Which element's metrics a block draws with. Every block kind has one, so a
/// platform asks for the block and reads the answer rather than switching on
/// the kind and reaching for constants of its own.
enum class BlockElement : uint8_t {
  Paragraph = 0,
  Heading = 1,
  Code = 2,
  Table = 3,
  Rule = 4,
  Count,
};

/// The typography one block draws with, before its inline runs apply.
struct BlockFont {
  /// Points at font scale 1.
  double size{0};
  /// A CSS weight, 100 to 900.
  double weight{400};
  /// The family to resolve, empty for the platform's system font.
  std::string family;
  /// Italic on for `em` and for a themed quote; off by default.
  bool italic{false};
  /// An absolute line height, or 0 to leave the platform's own leading.
  double lineHeight{0};
  /// The color slot this block's text draws in.
  uint32_t colorSlot{0};
  /// The margin slots above and below this block, for a gap that collapses.
  uint32_t marginTop{0};
  uint32_t marginBottom{0};
};

/// The element a block draws with. A heading's level picks its own slots, so the
/// caller passes the block rather than the kind.
BlockElement elementFor(const Block &block);

/// The typography and margins for a block, in points before font scaling.
BlockFont blockFont(const Theme &theme, const Block &block);

/// The paragraph typography, for a caller that has no block in hand. Every
/// block's font starts here, and a marker or a card asks for the body's size
/// before it has a block.
BlockFont paragraphFont(const Theme &theme);

/// The code element's typography, which a raw HTML block shares with a code
/// block, and which a caller uses for a marker's fallback face.
BlockFont codeFont(const Theme &theme);

/// A list marker's typography, and the gap the marker sits in.
BlockFont listMarkerFont(const Theme &theme);

/// The typography one run of inline text draws with, which is its block's own
/// with the inline kind's slots layered on top where the theme set them.
///
/// Every one of these takes the block's font rather than building one, so a
/// `strong` inside a themed heading is a heavier heading rather than a heading
/// that drops back to the body face. A slot the theme left alone changes
/// nothing, which is what lets a theme style links without restating the body.
BlockFont linkFont(const Theme &theme, const BlockFont &block);
BlockFont strongFont(const Theme &theme, const BlockFont &block);
BlockFont emFont(const Theme &theme, const BlockFont &block);
BlockFont inlineCodeFont(const Theme &theme, const BlockFont &block);
BlockFont codeHeaderFont(const Theme &theme);

/// Whether a link is underlined. True unless the theme turned it off, which is
/// what both renderers did before theming existed.
bool linkUnderlined(const Theme &theme);

/// One block's whole resolved layout, so a platform never rebuilds it.
///
/// This is what `blockFont` produces plus the two margins and the element, all
/// in one fixed shape. It rides back from a document read as one array per
/// block, which is why the rules live here and are not written twice: a
/// renderer that assembled a block's typography itself would be a second
/// implementation of every rule in this file, free to disagree with the other
/// platform about what a theme means.
struct BlockLayout {
  BlockFont font;
  BlockElement element{BlockElement::Paragraph};
  /// The margin slots above and below, for the collapsing rule.
  uint32_t marginTop{0};
  uint32_t marginBottom{0};
};

/// `blockFont` and the margins together, which is what a renderer needs per
/// block and what it should not have to assemble from two calls.
BlockLayout blockLayout(const Theme &theme, const Block &block);

/// One entry per field, for the wire. Fixed order, and the stride below is the
/// only place that order is written down.
constexpr size_t kBlockLayoutStride = 9;

/// Writes one `BlockLayout` into `out` at `offset`, as the wire carries it.
///
/// A font family is an index into the theme's own family list rather than a
/// string, because the same list comes back once per theme rather than once per
/// block.
void writeBlockLayout(const Theme &theme, const BlockLayout &layout, std::vector<double> &out, size_t offset);

/// The gap above a block, in points before font scaling.
///
/// `spacing` is the block's own `BlockSpacing`. For `Normal`, the larger of the
/// previous block's `marginBottom` and this block's `marginTop`, which collapses
/// them as CSS does, so two margins never add up by accident; `Tight` is the
/// list's own item spacing; `None` is zero. Twelve above a paragraph and twenty
/// above a heading, four inside a list, and zero with no previous block, is what
/// both renderers hard-code today, so the default theme reproduces it exactly.
double blockGap(const Theme &theme, BlockSpacing spacing, const Block *previous, const Block &block);

/// The indent one nesting level adds to a block's content leading.
double containerIndent(const Theme &theme, ContainerKind kind);

/// The horizontal gap between a container's bar or marker and its text.
double markerGap(const Theme &theme);

/// The width of a quote's or an admonition's bar.
double quoteBarWidth(const Theme &theme);

/// Padding inside a code card, on every side.
double codeCardPadding(const Theme &theme);

/// A code card's corner radius, and its border width, which is zero today.
double codeCardRadius(const Theme &theme);
double codeCardBorderWidth(const Theme &theme);

/// A table cell's padding, and the bounds a column's width is clamped to.
double tableCellPaddingHorizontal(const Theme &theme);
double tableCellPaddingVertical(const Theme &theme);
double tableMinColumnWidth(const Theme &theme);
double tableMaxColumnWidth(const Theme &theme);

/// A table's own typography, and its header row's.
///
/// The header shares the table's size and line height and takes its own family
/// and weight, because the slot table has slots for those two and none for a
/// size. That is deliberate: a header a size larger than its column would push
/// the column's own cells around, and a header is a row of the same table, not
/// a separate element.
BlockFont tableFont(const Theme &theme);
BlockFont tableHeaderFont(const Theme &theme);

/// A table grid line's width, which is a hairline today.
double tableBorderWidth(const Theme &theme);

/// A table's corner radius and its header row's fill.
double tableBorderRadius(const Theme &theme);

/// The size and raise of a footnote reference, as fractions of its block's
/// size. Fractions rather than sizes because the reference scales with the
/// block it sits in, which is what the renderers do today.
double footnoteSizeScale(const Theme &theme);
double footnoteRaise(const Theme &theme);

/// Inline code's size, in one of two forms, and the flag that says which.
///
/// A caller reads `inlineCodeIsAbsolute` and then exactly one of the other two:
/// a ratio of its block's own size, which keeps its proportions when a theme
/// changes the body size, or an absolute size, which does not. Reading
/// `inlineCodeSize` without the flag would give a size nothing selected, which
/// is why the flag and the two values are declared together.
double inlineCodeScale(const Theme &theme);
bool inlineCodeIsAbsolute(const Theme &theme);
double inlineCodeSize(const Theme &theme);

/// The platform font scale, capped by `maxFontSizeMultiplier` when a theme sets
/// one. A theme can cap Dynamic Type and Android's font scale; it cannot turn
/// scaling off, which is what `<Text>` does.
double fontScale(const Theme &theme, double platformScale);

/// The height of a code header or a table toolbar. Fixed, because it is a
/// touch target, and part of the chrome key rather than of the theme.
constexpr double kChromeRowHeight = 32;

} // namespace theme_layout
} // namespace downstream