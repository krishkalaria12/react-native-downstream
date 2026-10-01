#include "ThemeLayout.h"

#include <algorithm>

namespace downstream::theme_layout {

namespace {

using namespace theme;

/// The margin slots for a block that draws as a paragraph: a quote and a list
/// own their own text, so their margins replace the paragraph's rather than
/// adding to them.
void paragraphMargins(const Block &block, uint32_t &top, uint32_t &bottom) {
  for (const Container &container : block.containers) {
    if (container.kind == ContainerKind::Quote || container.kind == ContainerKind::Admonition) {
      top = kSlotBlockquoteMarginTop;
      bottom = kSlotBlockquoteMarginBottom;
      return;
    }
  }
  for (const Container &container : block.containers) {
    if (container.kind == ContainerKind::ListItem) {
      top = kSlotListMarginTop;
      bottom = kSlotListMarginBottom;
      return;
    }
  }
  top = kSlotParagraphMarginTop;
  bottom = kSlotParagraphMarginBottom;
}

/// A block that draws as a paragraph: no containers, so its margins are the
/// paragraph's own rather than a quote's or a list's.
const Block &paragraphLike() {
  static const Block block = [] {
    Block made;
    made.kind = BlockKind::Paragraph;
    return made;
  }();
  return block;
}

uint8_t headingLevel(const Block &block) {
  // A heading with no level is an h1 to the parser; clamping here means a level
  // outside one to six cannot read past the per-level arrays.
  return std::clamp<uint8_t>(block.headingLevel == 0 ? 1 : block.headingLevel, 1, 6);
}

} // namespace

BlockElement elementFor(const Block &block) {
  switch (block.kind) {
    case BlockKind::Heading:
      return BlockElement::Heading;
    case BlockKind::CodeBlock:
    // Raw HTML draws as the reader sees it, which today is monospace at the
    // code block's size, so it takes the same element rather than a fourth one.
    case BlockKind::HtmlBlock:
      return BlockElement::Code;
    case BlockKind::TableRow:
      return BlockElement::Table;
    case BlockKind::ThematicBreak:
      return BlockElement::Rule;
    case BlockKind::Paragraph:
      break;
  }
  return BlockElement::Paragraph;
}

BlockFont codeFont(const Theme &theme) {
  BlockFont font;
  font.size = theme.number(kSlotCodeBlockFontSize);
  font.weight = theme.number(kSlotCodeBlockFontWeight);
  font.family = theme.font(kSlotCodeBlockFontFamily);
  font.lineHeight = theme.number(kSlotCodeBlockLineHeight);
  font.colorSlot = kSlotCodeBlockColor;
  font.marginTop = kSlotCodeBlockMarginTop;
  font.marginBottom = kSlotCodeBlockMarginBottom;
  return font;
}

BlockFont paragraphFont(const Theme &theme) { return blockFont(theme, paragraphLike()); }

BlockFont listMarkerFont(const Theme &theme) {
  BlockFont font;
  // A marker's size is a share of the body's, so a bullet keeps its proportions
  // when a theme changes the body size instead of floating free of it.
  font.size = theme.number(kSlotListMarkerFontSize);
  font.family = theme.font(kSlotListMarkerFontFamily);
  font.weight = theme.number(kSlotListMarkerFontWeight);
  font.colorSlot = kSlotListMarkerColor;
  return font;
}

namespace {

/// The block's own typography, with a quote's or a list's layered on when the
/// block sits inside one.
///
/// A quote and a list are elements in their own right: a theme that wants
/// smaller quoted text should not have to restate every block inside the quote,
/// and one that wants a list in a serif should not have to name the paragraph.
void applyContainerFont(const Theme &theme, const Block &block, BlockFont &font) {
  for (const Container &container : block.containers) {
    switch (container.kind) {
      case ContainerKind::Quote:
      case ContainerKind::Admonition:
        // An alert takes the quote's typography: the five kinds differ in color
        // and nothing else, so a theme restyles alerts through the quote slots.
        font.size = theme.number(kSlotBlockquoteFontSize);
        font.weight = theme.number(kSlotBlockquoteFontWeight);
        font.family = theme.font(kSlotBlockquoteFontFamily);
        font.lineHeight = theme.number(kSlotBlockquoteLineHeight);
        font.italic = theme.flag(kSlotBlockquoteFontStyle);
        font.colorSlot = kSlotBlockquoteColor;
        return;
      case ContainerKind::ListItem:
        font.size = theme.number(kSlotListFontSize);
        font.weight = theme.number(kSlotListFontWeight);
        font.family = theme.font(kSlotListFontFamily);
        font.lineHeight = theme.number(kSlotListLineHeight);
        font.colorSlot = kSlotListColor;
        return;
      case ContainerKind::Footnote:
        break;
    }
  }
}

} // namespace

BlockFont blockFont(const Theme &theme, const Block &block) {
  BlockFont font;
  // Every size here is a themed slot, including the paragraph's, so a body the
  // theme changed moves with the rest of the document instead of staying at the
  // constant this file once named.
  font.size = theme.number(kSlotParagraphFontSize);
  font.weight = theme.number(kSlotParagraphFontWeight);
  font.family = theme.font(kSlotParagraphFontFamily);
  font.colorSlot = kSlotParagraphColor;
  font.lineHeight = theme.number(kSlotParagraphLineHeight);
  font.italic = theme.flag(kSlotParagraphFontStyle);
  paragraphMargins(block, font.marginTop, font.marginBottom);

  switch (elementFor(block)) {
    case BlockElement::Heading: {
      const size_t level = headingLevel(block) - 1;
      font.size = theme.number(heading::kFontSize[level]);
      font.weight = theme.number(heading::kFontWeight[level]);
      font.family = theme.font(heading::kFontFamily[level]);
      font.lineHeight = theme.number(heading::kLineHeight[level]);
      font.colorSlot = heading::kColor[level];
      font.italic = theme.flag(heading::kFontStyle[level]);
      font.marginTop = heading::kMarginTop[level];
      font.marginBottom = heading::kMarginBottom[level];
      return font;
    }
    case BlockElement::Code:
      return codeFont(theme);
    case BlockElement::Table:
      return tableFont(theme);
    case BlockElement::Rule:
      font.colorSlot = kSlotThematicBreakColor;
      font.marginTop = kSlotThematicBreakMarginTop;
      font.marginBottom = kSlotThematicBreakMarginBottom;
      return font;
    case BlockElement::Paragraph:
    case BlockElement::Count:
      break;
  }
  applyContainerFont(theme, block, font);
  return font;
}

BlockLayout blockLayout(const Theme &theme, const Block &block) {
  BlockLayout layout;
  layout.font = blockFont(theme, block);
  layout.element = elementFor(block);
  layout.marginTop = layout.font.marginTop;
  layout.marginBottom = layout.font.marginBottom;
  return layout;
}

void writeBlockLayout(const Theme &theme, const BlockLayout &layout, std::vector<double> &out, size_t offset) {
  // The family is an index into the theme's interned names, so a block's face
  // costs a number on the wire instead of a string per block per segment.
  int32_t family = -1;
  const std::string &name = layout.font.family;
  if (!name.empty()) {
    const auto &names = theme.fontNames();
    const auto found = std::find(names.begin(), names.end(), name);
    family = found == names.end() ? -1 : static_cast<int32_t>(found - names.begin());
  }
  const double values[kBlockLayoutStride] = {
      layout.font.size,
      layout.font.weight,
      static_cast<double>(family),
      layout.font.lineHeight,
      static_cast<double>(layout.font.colorSlot),
      static_cast<double>(layout.marginTop),
      static_cast<double>(layout.marginBottom),
      layout.font.italic ? 1.0 : 0.0,
      static_cast<double>(layout.element),
  };
  // Grown here rather than left to the caller: the natural call is `push`-shaped
  // per block, and a caller that reserved capacity but did not resize would
  // otherwise write past the end.
  if (out.size() < offset + kBlockLayoutStride) {
    out.resize(offset + kBlockLayoutStride);
  }
  for (size_t field = 0; field < kBlockLayoutStride; field++) {
    out[offset + field] = values[field];
  }
}

double blockGap(const Theme &theme, BlockSpacing spacing, const Block *previous, const Block &block) {
  switch (spacing) {
    case BlockSpacing::None:
      return 0;
    case BlockSpacing::Tight:
      // A tight block is another line of the same item, so its gap is the
      // list's own item spacing rather than either block's margin.
      return theme.number(kSlotListItemSpacing);
    case BlockSpacing::Normal:
      break;
  }
  const BlockFont font = blockFont(theme, block);
  double gap = theme.number(font.marginTop);
  if (previous != nullptr) {
    // The two margins collapse, as CSS collapses them, so a theme that gives
    // every block a top margin does not also give every pair of blocks two.
    const BlockFont above = blockFont(theme, *previous);
    gap = std::max(gap, theme.number(above.marginBottom));
  }
  return gap;
}

double containerIndent(const Theme &theme, ContainerKind kind) {
  // A quote and an admonition are the same box; a list item and a footnote are
  // the same column.
  switch (kind) {
    case ContainerKind::Quote:
    case ContainerKind::Admonition:
      return theme.number(kSlotBlockquoteGapWidth);
    case ContainerKind::ListItem:
    case ContainerKind::Footnote:
      break;
  }
  return theme.number(kSlotListMarginLeft);
}

double markerGap(const Theme &theme) { return theme.number(kSlotListGapWidth); }
double quoteBarWidth(const Theme &theme) { return theme.number(kSlotBlockquoteBorderWidth); }
double codeCardPadding(const Theme &theme) { return theme.number(kSlotCodeBlockPadding); }
double codeCardBorderWidth(const Theme &theme) { return theme.number(kSlotCodeBlockBorderWidth); }

double codeCardRadius(const Theme &theme) {
  // `radius` is the token an app sets once, for both the code card and the
  // table, so it fills a per-element radius the theme did not name. Asking
  // whether the slot was set is what makes the two distinguishable: comparing
  // against the default value would confuse a theme that deliberately asked for
  // the default with one that said nothing.
  if (theme.numberSet(kSlotCodeBlockBorderRadius)) {
    return theme.number(kSlotCodeBlockBorderRadius);
  }
  return theme.number(kSlotRadius);
}

BlockFont tableFont(const Theme &theme) {
  BlockFont font;
  font.size = theme.number(kSlotTableFontSize);
  font.weight = theme.number(kSlotTableFontWeight);
  font.family = theme.font(kSlotTableFontFamily);
  font.lineHeight = theme.number(kSlotTableLineHeight);
  font.colorSlot = kSlotTableColor;
  font.marginTop = kSlotTableMarginTop;
  font.marginBottom = kSlotTableMarginBottom;
  return font;
}

BlockFont tableHeaderFont(const Theme &theme) {
  // The table's size and line height, kept, so a header row's cells line up
  // with the cells under them.
  BlockFont font = tableFont(theme);
  font.family = theme.font(kSlotTableHeaderFontFamily);
  font.weight = theme.number(kSlotTableHeaderFontWeight);
  font.colorSlot = kSlotTableHeaderTextColor;
  return font;
}

double tableCellPaddingHorizontal(const Theme &theme) { return theme.number(kSlotTableCellPaddingHorizontal); }
double tableCellPaddingVertical(const Theme &theme) { return theme.number(kSlotTableCellPaddingVertical); }
double tableMinColumnWidth(const Theme &theme) { return theme.number(kSlotTableMinColumnWidth); }
double tableMaxColumnWidth(const Theme &theme) { return theme.number(kSlotTableMaxColumnWidth); }
double tableBorderWidth(const Theme &theme) { return theme.number(kSlotTableBorderWidth); }

double tableBorderRadius(const Theme &theme) {
  if (theme.numberSet(kSlotTableBorderRadius)) {
    return theme.number(kSlotTableBorderRadius);
  }
  return theme.number(kSlotRadius);
}

BlockFont linkFont(const Theme &theme, const BlockFont &block) {
  BlockFont font = block;
  // Only the two slots a link has of its own. Its color is read straight off the
  // slot, so this stays about the face.
  font.family = theme.font(kSlotLinkFontFamily);
  font.weight = theme.number(kSlotLinkFontWeight);
  return font;
}

BlockFont strongFont(const Theme &theme, const BlockFont &block) {
  BlockFont font = block;
  font.family = theme.font(kSlotStrongFontFamily);
  if (theme.numberSet(kSlotStrongFontWeight)) {
    // A theme that named a weight means it, whatever the block was.
    font.weight = theme.number(kSlotStrongFontWeight);
  } else {
    // Otherwise the weight is raised above the block's, because a fixed 700
    // inside a 700 heading would be no emphasis at all. Two steps for a bold
    // that lands on an already-bold weight, so bold in bold still reads.
    font.weight = std::min(900.0, block.weight + (block.weight >= 600 ? 200 : 300));
  }
  return font;
}

BlockFont emFont(const Theme &theme, const BlockFont &block) {
  BlockFont font = block;
  font.family = theme.font(kSlotEmFontFamily);
  // The slot is a flag rather than a string because a theme that sets nothing
  // keeps the block's own slant: a quote that is already italic should not have
  // its italics turned off by an `em` that never ran.
  if (theme.flag(kSlotEmFontStyle)) {
    font.italic = true;
  }
  return font;
}

BlockFont inlineCodeFont(const Theme &theme, const BlockFont &block) {
  BlockFont font = block;
  font.size = inlineCodeSize(theme);
  font.family = theme.font(kSlotCodeFontFamily);
  // The color is a slot rather than a field here for the same reason the
  // background is: a theme may set one and not the other.
  font.colorSlot = kSlotCodeColor;
  return font;
}

BlockFont codeHeaderFont(const Theme &theme) {
  BlockFont font = codeFont(theme);
  font.size = theme.number(kSlotCodeBlockHeaderFontSize);
  return font;
}

bool linkUnderlined(const Theme &theme) { return theme.flag(kSlotLinkUnderline); }

double footnoteSizeScale(const Theme &theme) { return theme.number(kSlotFootnoteSizeScale); }
double footnoteRaise(const Theme &theme) { return theme.number(kSlotFootnoteRaise); }
double inlineCodeScale(const Theme &theme) { return theme.number(kSlotCodeSizeScale); }
bool inlineCodeIsAbsolute(const Theme &theme) { return theme.flag(kSlotCodeAbsoluteSize); }
double inlineCodeSize(const Theme &theme) { return theme.number(kSlotCodeFontSize); }

double fontScale(const Theme &theme, double platformScale) {
  const double cap = theme.number(kSlotMaxFontSizeMultiplier);
  // A cap below 1 is the "not set" value and is ignored, as `<Text>` ignores
  // one: capping below the current scale would shrink text the reader asked to
  // enlarge.
  if (cap >= 1.0 && platformScale > cap) {
    return cap;
  }
  return platformScale;
}

} // namespace downstream::theme_layout