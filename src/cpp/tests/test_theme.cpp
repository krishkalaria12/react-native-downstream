// Engine checks for the theme: that the default theme reproduces every constant
// the two renderers hard-code today, that interning shares and separates the
// themes it should, and that the slot table and the shared layout rules hold the
// table of cases a platform's mirror has to agree with.
//
//     node --test src/cpp/tests/engine.test.mjs   (runs test_theme)

#include "MarkdownParser.h"
#include "SegmentPlan.h"
#include "Theme.h"
#include "ThemeLayout.h"
#include "tests/check.h"
#include "TokenPalette.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace downstream;
using namespace downstream::theme;
using namespace downstream::theme_layout;

namespace {

/// The heading sizes DSRenderedDocument.mm and RenderedDocument.kt named, so the
/// loop over levels below compares against the value rather than the slot.
constexpr double kExpectedHeadingSizes[] = {28, 24, 20, 18, 16, 15};

double expectedHeadingSize(uint8_t level) {
  return kExpectedHeadingSizes[std::clamp<size_t>(level == 0 ? 1 : level, 1, 6) - 1];
}

Theme::Handle emptyTheme() { return Theme::defaultTheme(); }

/// Interns a theme from slot ids and values, sorted into the ascending order
/// `src/theme.ts` sends them. The helper sorts rather than the caller, so a case
/// reads as the slots it sets rather than as an ordering it also has to get
/// right; the ordering itself is checked in `testInterning`.
Theme::Handle make(std::vector<uint32_t> slots, std::vector<double> values,
                   std::vector<std::string> fonts = {}) {
  std::vector<size_t> order(slots.size());
  for (size_t index = 0; index < order.size(); index++) {
    order[index] = index;
  }
  std::stable_sort(order.begin(), order.end(),
                   [&slots](size_t a, size_t b) { return slots[a] < slots[b]; });
  std::vector<uint32_t> sorted;
  std::vector<double> sortedValues;
  sorted.reserve(slots.size());
  sortedValues.reserve(values.size());
  for (const size_t index : order) {
    sorted.push_back(slots[index]);
    sortedValues.push_back(values[index]);
  }
  return ThemeRegistry::intern(sorted, sortedValues, fonts);
}

bool nearly(double actual, double expected) {
  return std::abs(actual - expected) < 1e-9;
}

Block blockOf(BlockKind kind, BlockSpacing spacing = BlockSpacing::Normal, uint8_t headingLevel = 0) {
  Block block;
  block.kind = kind;
  block.spacingBefore = spacing;
  block.headingLevel = headingLevel;
  return block;
}

/// A block inside one container, which is what a quote or a list item is.
Block blockIn(ContainerKind kind, BlockKind inner = BlockKind::Paragraph) {
  Block block = blockOf(inner);
  Container container;
  container.kind = kind;
  container.id = 1;
  block.containers.push_back(container);
  return block;
}

// ------------------------------------------------------- the default values

/// The constants DSRenderedDocument.mm and RenderedDocument.kt named, before
/// they moved into the slot table. This is the table that makes "no theme means
/// no change" checkable rather than assumed: if a default moves, this fails.
void testDefaultMetrics() {
  const Theme::Handle theme = emptyTheme();

  CHECK(theme->id() == 0, "the empty theme is id 0");
  // Zero, not a hash of nothing, so a cache key holding it is the key the same
  // layout had before theming existed and a real theme cannot collide with it.
  CHECK(theme->layoutKey() == 0, "the empty theme's layout key is zero too");

  CHECK(nearly(theme->number(kSlotParagraphFontSize), 16), "body size is 16");
  CHECK(nearly(theme->number(kSlotCodeBlockFontSize), 14), "code size is 14");
  CHECK(nearly(theme->number(kSlotCodeSizeScale), 0.875), "inline code is 14/16 of its block");
  CHECK(nearly(theme->number(kSlotCodeFontSize), 14), "an absolute inline code size is 14");

  for (size_t level = 0; level < 6; level++) {
    CHECK(nearly(theme->number(heading::kFontSize[level]), kExpectedHeadingSizes[level]), "heading sizes");
    // Bold for one and two, semibold for three through six.
    const double weight = level < 2 ? 700 : 600;
    CHECK(nearly(theme->number(heading::kFontWeight[level]), weight), "heading weights");
  }

  CHECK(nearly(theme->number(kSlotParagraphMarginTop), 12), "the normal gap is 12");
  CHECK(nearly(theme->number(kSlotParagraphMarginBottom), 0), "a paragraph has no bottom margin");
  CHECK(nearly(theme->number(kSlotHeading1MarginTop), 20), "the gap before a heading is 20");
  CHECK(nearly(theme->number(kSlotListItemSpacing), 4), "the tight gap is 4");

  CHECK(nearly(theme->number(kSlotBlockquoteGapWidth), 16), "a quote indents 16");
  CHECK(nearly(theme->number(kSlotListMarginLeft), 32), "a list item indents 32");
  CHECK(nearly(theme->number(kSlotBlockquoteBorderWidth), 3), "a quote bar is 3 wide");
  CHECK(nearly(theme->number(kSlotListGapWidth), 6), "a marker gap is 6");

  CHECK(nearly(theme->number(kSlotCodeBlockPadding), 10), "a code card pads 10");
  CHECK(nearly(theme_layout::codeCardRadius(*theme), 6), "a code card's radius is 6");
  CHECK(nearly(theme_layout::codeCardBorderWidth(*theme), 0), "a code card has no border");

  CHECK(nearly(theme_layout::tableCellPaddingHorizontal(*theme), 12), "a cell pads 12 across");
  CHECK(nearly(theme_layout::tableCellPaddingVertical(*theme), 8), "a cell pads 8 down");
  CHECK(nearly(theme_layout::tableMinColumnWidth(*theme), 56), "a column is at least 56");
  CHECK(nearly(theme_layout::tableMaxColumnWidth(*theme), 280), "a column is at most 280");
  CHECK(nearly(theme_layout::tableBorderWidth(*theme), 1), "a grid line is one point");

  CHECK(nearly(theme_layout::footnoteSizeScale(*theme), 0.75), "a footnote reference is 0.75 of its block");
  CHECK(nearly(theme_layout::footnoteRaise(*theme), 0.35), "a footnote reference is raised 0.35");

  CHECK(nearly(theme_layout::kChromeRowHeight, 32), "a control row is 32 points");
  CHECK(nearly(theme_layout::fontScale(*theme, 1.5), 1.5), "no cap leaves the platform scale alone");
  CHECK(nearly(theme_layout::fontScale(*theme, 0.5), 0.5), "a cap below the scale does not shrink text");
}

/// The color defaults are roles rather than values, so an unset color keeps
/// following the platform's appearance. This is what a fully normalized integer
/// cannot do, and it is why the slot table stores them.
void testDefaultRoles() {
  const Theme::Handle theme = emptyTheme();

  CHECK(theme->color(kSlotParagraphColor).defaultRole == static_cast<uint16_t>(ColorRole::kRoleLabel),
        "body text falls back to the label color");
  CHECK(theme->color(kSlotHeading6Color).defaultRole == static_cast<uint16_t>(ColorRole::kRoleSecondaryLabel),
        "an h6 falls back to the secondary label color");
  CHECK(theme->color(kSlotBlockquoteColor).defaultRole == static_cast<uint16_t>(ColorRole::kRoleSecondaryLabel),
        "a quote falls back to the secondary label color");
  CHECK(theme->color(kSlotBlockquoteBorderColor).defaultRole == static_cast<uint16_t>(ColorRole::kRoleGray3),
        "a quote bar falls back to the neutral gray");
  CHECK(theme->color(kSlotLinkColor).defaultRole == static_cast<uint16_t>(ColorRole::kRoleLink),
        "a link falls back to the link color");
  CHECK(theme->color(kSlotCodeBackgroundColor).defaultRole == static_cast<uint16_t>(ColorRole::kRoleTertiaryFill),
        "inline code falls back to the tertiary fill");
  CHECK(theme->color(kSlotTableBorderColor).defaultRole == static_cast<uint16_t>(ColorRole::kRoleSeparator),
        "a grid line falls back to the separator");
  CHECK(theme->color(kSlotThematicBreakColor).defaultRole == static_cast<uint16_t>(ColorRole::kRoleSeparator),
        "a rule falls back to the separator");
  CHECK(theme->color(kSlotStrongColor).defaultRole == static_cast<uint16_t>(ColorRole::kRoleInherit),
        "strong text takes its block's color");
  CHECK(theme->color(kSlotEmColor).defaultRole == static_cast<uint16_t>(ColorRole::kRoleInherit),
        "emphasis takes its block's color");
  CHECK(theme->color(kSlotAdmonitionNote).defaultRole == static_cast<uint16_t>(ColorRole::kRoleBlue),
        "a note is blue");
  CHECK(theme->color(kSlotAdmonitionCaution).defaultRole == static_cast<uint16_t>(ColorRole::kRoleRed),
        "a caution is red");
  CHECK(theme->color(kSlotSelectionColor).defaultRole == static_cast<uint16_t>(ColorRole::kRoleSelection),
        "the selection highlight is the platform's own");

  // A syntax slot names a token class rather than a role, which is how one
  // default field holds both.
  for (size_t token = 0; token < static_cast<size_t>(TokenClass::Count); token++) {
    const uint16_t expected =
        static_cast<uint16_t>(kRoleTokenClassBase + static_cast<uint16_t>(token));
    CHECK(theme->color(kSyntax[token]).defaultRole == expected, "a syntax slot names its token class");
  }
  CHECK(theme->color(kSyntax[static_cast<size_t>(TokenClass::Keyword)]).defaultRole !=
            theme->color(kSyntax[static_cast<size_t>(TokenClass::Comment)]).defaultRole,
        "two token classes are two defaults");
}

/// Every block kind, heading level, spacing, and container, against the values
/// the two renderers computed for themselves. A gap at a chunk boundary has to
/// equal the same gap inside a chunk, which is what the previous block's
/// argument is for.
void testBlockLayout() {
  const Theme::Handle theme = emptyTheme();

  const Block paragraph = blockOf(BlockKind::Paragraph);
  const Block heading = blockOf(BlockKind::Heading, BlockSpacing::Normal, 2);
  const Block tight = blockOf(BlockKind::Paragraph, BlockSpacing::Tight);
  const Block flush = blockOf(BlockKind::Paragraph, BlockSpacing::None);

  CHECK(nearly(blockGap(*theme, BlockSpacing::Normal, nullptr, paragraph), 12),
        "a normal gap with nothing above is the paragraph's own top margin");
  CHECK(nearly(blockGap(*theme, BlockSpacing::Normal, &paragraph, heading), 20),
        "a heading takes its own twenty");
  CHECK(nearly(blockGap(*theme, BlockSpacing::Normal, &heading, paragraph), 12),
        "a paragraph after a heading takes twelve");
  CHECK(nearly(blockGap(*theme, BlockSpacing::Tight, &paragraph, tight), 4),
        "a tight block takes the list's item spacing");
  CHECK(nearly(blockGap(*theme, BlockSpacing::None, &paragraph, flush), 0),
        "no spacing is no gap");

  // A code block, a table, and a rule are each their own kind, and each keeps
  // the twelve the renderers gave them.
  CHECK(nearly(blockGap(*theme, BlockSpacing::Normal, &paragraph, blockOf(BlockKind::CodeBlock)), 12),
        "a code block keeps twelve");
  CHECK(nearly(blockGap(*theme, BlockSpacing::Normal, &paragraph, blockOf(BlockKind::TableRow)), 12),
        "a table keeps twelve");
  CHECK(nearly(blockGap(*theme, BlockSpacing::Normal, &paragraph, blockOf(BlockKind::ThematicBreak)), 12),
        "a rule keeps twelve");
  CHECK(nearly(blockGap(*theme, BlockSpacing::Normal, &paragraph, blockOf(BlockKind::HtmlBlock)), 12),
        "escaped HTML keeps twelve");

  // A quote and a list draw their own text, so their slots win over the
  // paragraph's. Their margins are the paragraph's today, which is what makes
  // the renderers' values fall out of this table rather than out of a case.
  Block quoted = paragraph;
  quoted.containers.push_back({ContainerKind::Quote, 1, false, TaskState::None, u""});
  CHECK(nearly(blockGap(*theme, BlockSpacing::Normal, &paragraph, quoted), 12),
        "a quoted block keeps twelve by default");
  Block listed = paragraph;
  listed.containers.push_back({ContainerKind::ListItem, 1, true, TaskState::None, u"-"});
  CHECK(nearly(blockGap(*theme, BlockSpacing::Normal, &paragraph, listed), 12),
        "a listed block keeps twelve by default");

  // Every heading level reads its own slots.
  for (uint8_t level = 1; level <= 6; level++) {
    const Block block = blockOf(BlockKind::Heading, BlockSpacing::Normal, level);
    const BlockFont font = blockFont(*theme, block);
    CHECK(nearly(font.size, expectedHeadingSize(level)), "a heading level reads its own size");
    CHECK(font.colorSlot == heading::kColor[level - 1], "a heading level reads its own color slot");
  }
  // A level outside one to six cannot read past the per-level arrays.
  const Block wild = blockOf(BlockKind::Heading, BlockSpacing::Normal, 9);
  CHECK(nearly(blockFont(*theme, wild).size, 15), "a level past six clamps to the last");
  const Block low = blockOf(BlockKind::Heading, BlockSpacing::Normal, 0);
  CHECK(nearly(blockFont(*theme, low).size, 28), "a level below one clamps to the first");
  const Block none = blockOf(BlockKind::Heading);
  CHECK(nearly(blockFont(*theme, none).size, 28), "a heading with no level is an h1");

  // A block's element decides which margins a gap collapses against it.
  CHECK(blockFont(*theme, heading).marginTop == heading::kMarginTop[1], "a heading's own top margin");
  CHECK(blockFont(*theme, quoted).marginTop == kSlotBlockquoteMarginTop, "a quote's own top margin");
  CHECK(blockFont(*theme, listed).marginTop == kSlotListMarginTop, "a list's own top margin");
  CHECK(blockFont(*theme, paragraph).marginTop == kSlotParagraphMarginTop, "a paragraph's own top margin");

  // Containers.
  CHECK(nearly(containerIndent(*theme, ContainerKind::Quote), 16), "a quote indents sixteen");
  CHECK(nearly(containerIndent(*theme, ContainerKind::Admonition), 16), "an admonition indents sixteen");
  CHECK(nearly(containerIndent(*theme, ContainerKind::ListItem), 32), "a list item indents thirty two");
  CHECK(nearly(containerIndent(*theme, ContainerKind::Footnote), 32), "a footnote indents thirty two");
}

/// A theme can change any of it, and the platform reading is one call either
/// way.
void testThemedLayout() {
  const Theme::Handle theme =
      make({kSlotParagraphFontSize, kSlotHeading1MarginTop, kSlotCodeBlockPadding, kSlotRadius,
            kSlotBlockquoteGapWidth, kSlotListItemSpacing},
           {20, 32, 16, 12, 24, 8});

  const Block paragraph = blockOf(BlockKind::Paragraph);
  const Block heading = blockOf(BlockKind::Heading, BlockSpacing::Normal, 1);

  CHECK(nearly(blockFont(*theme, paragraph).size, 20), "a themed body size reaches the block");
  CHECK(nearly(blockGap(*theme, BlockSpacing::Normal, &paragraph, heading), 32),
        "a themed heading margin reaches the gap");
  CHECK(nearly(theme_layout::codeCardPadding(*theme), 16), "a themed code padding reaches the card");
  CHECK(nearly(theme_layout::codeCardRadius(*theme), 12), "the radius token reaches the card");
  CHECK(nearly(containerIndent(*theme, ContainerKind::Quote), 24), "a themed quote indent reaches the container");
  CHECK(nearly(blockGap(*theme, BlockSpacing::Tight, &paragraph, paragraph), 8),
        "a themed item spacing reaches the tight gap");

  // An element's own radius wins over the token, which is what keeps the two
  // distinguishable without a second slot.
  const Theme::Handle own =
      make({kSlotRadius, kSlotCodeBlockBorderRadius}, {12, 4});
  CHECK(nearly(theme_layout::codeCardRadius(*own), 4), "an element radius beats the token");

  // A margin bottom collapses with the next block's top margin, as CSS does, so
  // two margins never add up by accident.
  const Theme::Handle collapsing =
      make({kSlotParagraphMarginTop, kSlotParagraphMarginBottom}, {10, 30});
  const Block second = blockOf(BlockKind::Paragraph);
  CHECK(nearly(blockGap(*collapsing, BlockSpacing::Normal, &paragraph, second), 30),
        "a bottom margin wins when it is the larger");
  const Theme::Handle other = make({kSlotParagraphMarginTop}, {40});
  CHECK(nearly(blockGap(*other, BlockSpacing::Normal, &paragraph, second), 40),
        "a top margin wins when it is the larger");

  // A cap clamps the platform scale but cannot shrink text below it.
  const Theme::Handle capped = make({kSlotMaxFontSizeMultiplier}, {1.4});
  CHECK(nearly(theme_layout::fontScale(*capped, 2.0), 1.4), "a cap above one clamps the scale");
  CHECK(nearly(theme_layout::fontScale(*capped, 1.2), 1.2), "a cap below the scale does nothing");
  const Theme::Handle invalid = make({kSlotMaxFontSizeMultiplier}, {0.5});
  CHECK(nearly(theme_layout::fontScale(*invalid, 2.0), 2.0), "a cap below one is the unset value");

  // An inline code size is absolute once a theme sets one, and a ratio until
  // then.
  const Theme::Handle absolute = make({kSlotCodeAbsoluteSize, kSlotCodeFontSize}, {1, 18});
  CHECK(theme_layout::inlineCodeIsAbsolute(*absolute), "an absolute size is flagged");
  CHECK(nearly(theme_layout::inlineCodeSize(*absolute), 18), "the absolute size is read back");
}

// -------------------------------------------------- per-element typography

/// What one block draws with, which is the whole point of resolving it in the
/// engine rather than in each renderer.
void testElementTypography() {
  const Theme::Handle theme =
      make({kSlotBlockquoteFontSize, kSlotBlockquoteFontWeight, kSlotListFontSize, kSlotCodeSizeScale,
            kSlotCodeAbsoluteSize, kSlotCodeFontSize, kSlotStrongFontWeight, kSlotHeading3FontSize},
           {13, 600, 11, 0.9, 0, 0, 500, 15});

  // A quote is an element of its own, not a paragraph with an indent.
  const Block quoted = blockIn(ContainerKind::Quote);
  const BlockFont inQuote = blockFont(*theme, quoted);
  CHECK(nearly(inQuote.size, 13), "a themed quote size reaches the block inside it");
  CHECK(nearly(inQuote.weight, 600), "and its weight");
  CHECK(inQuote.colorSlot == kSlotBlockquoteColor, "and its color slot");

  const Block item = blockIn(ContainerKind::ListItem);
  CHECK(nearly(blockFont(*theme, item).size, 11), "a themed list size reaches the block inside it");

  // A block outside either keeps the paragraph's, which is what makes a theme
  // that styles only quotes leave the rest of the document alone.
  const Block plain = blockOf(BlockKind::Paragraph);
  CHECK(nearly(blockFont(*theme, plain).size, 16), "a block outside a quote keeps the body size");

  // An alert takes the quote's typography: the five kinds differ in color and
  // nothing else.
  const Block alert = blockIn(ContainerKind::Admonition);
  CHECK(nearly(blockFont(*theme, alert).size, 13), "an alert takes the quote's typography");

  // A heading's own level picks its size.
  const Block heading = blockOf(BlockKind::Heading, BlockSpacing::Normal, 3);
  CHECK(nearly(blockFont(*theme, heading).size, 15), "a heading's level picks its size");

  // Inline code's size comes in two forms and the flag says which, so a caller
  // reads both and picks: a ratio of the block's own size, which keeps its
  // proportions when the body size changes, or an absolute size, which does not.
  CHECK(!theme_layout::inlineCodeIsAbsolute(*theme), "a theme that named no flag gets the ratio");
  CHECK(nearly(theme_layout::inlineCodeScale(*theme), 0.9), "and the ratio is read back");
  const Theme::Handle sized = make({kSlotCodeAbsoluteSize, kSlotCodeFontSize}, {1, 18});
  CHECK(theme_layout::inlineCodeIsAbsolute(*sized), "an absolute size is flagged");
  CHECK(nearly(theme_layout::inlineCodeSize(*sized), 18), "and the absolute size is read back");

  // A `strong` whose weight slot was named means exactly that weight.
  const BlockFont strong = theme_layout::strongFont(*theme, blockFont(*emptyTheme(), plain));
  CHECK(nearly(strong.weight, 500), "a named strong weight is final");
  // One that was not named raises the block's own, so bold inside a 700 heading
  // is still bolder rather than the same weight.
  const Theme::Handle noWeight = make({kSlotStrongFontFamily}, {0}, {"Inter-Bold"});
  const BlockFont bold = blockFont(*noWeight, plain);
  const BlockFont raised = theme_layout::strongFont(*noWeight, bold);
  CHECK(raised.weight > bold.weight, "an unnamed strong weight raises the block's");
  const Theme::Handle boldHeading = make({kSlotHeading2FontWeight}, {700});
  const BlockFont headingFont = blockFont(*boldHeading, blockOf(BlockKind::Heading, BlockSpacing::Normal, 2));
  CHECK(theme_layout::strongFont(*boldHeading, headingFont).weight > headingFont.weight,
        "and it still reads inside a bold heading");

  // An `em` that names no slant leaves the block's alone, so a quote that is
  // already italic does not lose it.
  const Theme::Handle noSlant = make({kSlotParagraphFontStyle}, {1});
  const BlockFont italicQuote = theme_layout::emFont(*noSlant, blockFont(*noSlant, quoted));
  CHECK(italicQuote.italic, "an em that says nothing keeps the block's slant");
  const Theme::Handle slanted = make({kSlotEmFontStyle}, {1});
  CHECK(theme_layout::emFont(*slanted, blockFont(*emptyTheme(), plain)).italic,
        "an em that names the slant adds it");

  // A link is underlined unless a theme turned it off.
  CHECK(theme_layout::linkUnderlined(*emptyTheme()), "a link is underlined by default");
  const Theme::Handle plain_ = make({kSlotLinkUnderline}, {0});
  CHECK(!theme_layout::linkUnderlined(*plain_), "and a theme can turn it off");

  // `blockLayout` is the same answer as the two calls beside it, so a renderer
  // that takes the whole struct and one that takes the font cannot disagree.
  const theme_layout::BlockLayout layout = theme_layout::blockLayout(*theme, quoted);
  CHECK(nearly(layout.font.size, inQuote.size), "the struct's font is the font");
  CHECK(layout.marginTop == layout.font.marginTop, "and its margins are the font's");
  CHECK(layout.element == BlockElement::Paragraph, "and it names the element");

  // The wire form round-trips through the same order.
  std::vector<double> wire;
  theme_layout::writeBlockLayout(*theme, layout, wire, 0);
  CHECK(wire.size() == theme_layout::kBlockLayoutStride, "one entry per field");
  CHECK(nearly(wire[0], layout.font.size), "size first");
  CHECK(nearly(wire[1], layout.font.weight), "then weight");
  CHECK(wire[3] == layout.font.lineHeight, "then line height");
  CHECK(wire[4] == layout.font.colorSlot, "then the color slot");
  // A family the theme never named is -1, which is the platform's own font, so
  // a block's face costs a number rather than a string.
  const Theme::Handle noFamily = emptyTheme();
  wire.clear();
  theme_layout::writeBlockLayout(*noFamily, theme_layout::blockLayout(*noFamily, plain), wire, 0);
  CHECK(wire[2] == -1, "a block with no family sends -1");
}

// -------------------------------------------------------------- interning

void testInterning() {
  ThemeRegistry::clear();

  // Equal slot lists share one object, which is what keeps every message in a
  // chat on one theme object.
  const Theme::Handle first = make({kSlotParagraphFontSize}, {20});
  const Theme::Handle second = make({kSlotParagraphFontSize}, {20});
  CHECK(first.get() == second.get(), "equal slot lists share one theme");

  // A different value is a different theme.
  const Theme::Handle other = make({kSlotParagraphFontSize}, {22});
  CHECK(first.get() != other.get(), "a different value is a different theme");
  CHECK(other->id() != first->id(), "and a different id");

  // A color-only change keeps the layout key, so every measured size survives.
  const Theme::Handle recolored = make({kSlotParagraphFontSize, kSlotLinkColor}, {20, 0xff0000ff});
  const Theme::Handle repainted = make({kSlotParagraphFontSize, kSlotLinkColor}, {20, 0xff00ff00});
  CHECK(recolored->layoutKey() == repainted->layoutKey(), "a color-only change keeps the layout key");
  CHECK(recolored->id() != repainted->id(), "and changes the drawing id");
  CHECK(recolored->layoutKey() == first->layoutKey(), "a color adds nothing to the layout key");

  // A size change changes both.
  const Theme::Handle resized = make({kSlotParagraphFontSize, kSlotLinkColor}, {21, 0xff0000ff});
  CHECK(resized->layoutKey() != recolored->layoutKey(), "a size change changes the layout key");
  CHECK(resized->id() != recolored->id(), "and the drawing id");

  // A pair is two slots, so both appearances are hashed.
  const Theme::Handle light = make({kSlotLinkColor}, {0xff112233});
  const Theme::Handle dark = make({kSlotLinkColor, kSlotLinkColor + 1}, {0xff112233, 0xff445566});
  CHECK(light->id() != dark->id(), "a color pair is not one color");

  // Unsorted input is refused rather than half-applied, because a caller that
  // gets the default can see its value was rejected while one that got a
  // partial theme cannot.
  CHECK(ThemeRegistry::intern({kSlotParagraphFontWeight, kSlotParagraphFontSize}, {20, 20}, {}).get() ==
            emptyTheme().get(),
        "an unsorted list is refused");
  // An empty list is the default theme, and no theme is the same theme.
  CHECK(ThemeRegistry::intern({}, {}, {}).get() == emptyTheme().get(), "no theme is the default theme");
  CHECK(ThemeRegistry::intern({}, {}, {}).get() == ThemeRegistry::intern({}, {}, {}).get(),
        "the default theme is one object");

  // A malformed list falls back to the default rather than half-applying.
  CHECK(ThemeRegistry::intern({kSlotParagraphFontSize, kSlotParagraphFontSize}, {20, 20}, {}).get() ==
            emptyTheme().get(),
        "a repeated id is refused");
  CHECK(ThemeRegistry::intern({kSlotParagraphFontSize, kSlotParagraphFontSize}, {20, 20}, {}).get() ==
            emptyTheme().get(),
        "a repeated id is refused a second time");
  CHECK(ThemeRegistry::intern({kSlotParagraphFontSize}, {20, 30}, {}).get() == emptyTheme().get(),
        "a length mismatch is refused");
  CHECK(ThemeRegistry::intern({kSlotIdCount + 10}, {1}, {}).get() == emptyTheme().get(),
        "a theme whose every id is from a newer table is the default");

  // An id this build does not have is dropped and the rest of the theme lands.
  const Theme::Handle partial = make({kSlotParagraphFontSize, kSlotIdCount + 10}, {20, 1});
  CHECK(partial.get() != emptyTheme().get(), "a newer id beside a real one still builds a theme");
  CHECK(nearly(partial->number(kSlotParagraphFontSize), 20), "and the rest of the theme applies");

  // An evicted theme is rebuilt with the same hashes, so a cache key built
  // before the eviction still matches.
  const uint64_t evictedId = first->id();
  const uint64_t evictedLayout = first->layoutKey();
  for (int index = 0; index < 64; index++) {
    make({kSlotParagraphFontSize}, {static_cast<double>(100 + index)});
  }
  const Theme::Handle again = make({kSlotParagraphFontSize}, {20});
  CHECK(again->id() == evictedId, "an evicted theme is rebuilt with the same id");
  CHECK(again->layoutKey() == evictedLayout, "and the same layout key");

  // Android receives the id through the Fabric state as a double, so an id a
  // double cannot hold rounds to one the pool does not have and the view draws
  // the default theme. Many distinct themes, so a wide hash would show.
  for (int index = 0; index < 64; index++) {
    const Theme::Handle themed = make({kSlotParagraphFontSize}, {static_cast<double>(200 + index)});
    const uint64_t id = themed->id();
    CHECK(static_cast<uint64_t>(static_cast<double>(id)) == id, "a theme id survives a double");
    CHECK(ThemeRegistry::find(static_cast<uint64_t>(static_cast<double>(id))).get() == themed.get(),
          "and the id read back from a double finds the same theme");
  }

  // Android's `processColor` sends a color as a signed int, iOS's as unsigned.
  // Both are the same ARGB bits, so both draw the same color, and two different
  // signed colors stay two themes rather than collapsing to one.
  const double signedInk = static_cast<double>(static_cast<int32_t>(0xff1f2328u));
  const double signedRed = static_cast<double>(static_cast<int32_t>(0xffcf222eu));
  const uint32_t paragraphDark = kSlotDark[kSlotParagraphColor - 1];
  const Theme::Handle fromAndroid = make({kSlotParagraphColor, paragraphDark}, {signedInk, signedInk});
  const Theme::Handle fromIos = make({kSlotParagraphColor, paragraphDark},
                                     {static_cast<double>(0xff1f2328u), static_cast<double>(0xff1f2328u)});
  CHECK(fromAndroid->color(kSlotParagraphColor).light == 0xff1f2328u, "a signed color keeps its bits");
  CHECK(fromAndroid->color(kSlotParagraphColor).dark == 0xff1f2328u, "in both appearances");
  CHECK(fromIos->color(kSlotParagraphColor).light == 0xff1f2328u, "an unsigned one does too");
  CHECK(make({kSlotParagraphColor, paragraphDark}, {signedRed, signedRed}).get() != fromAndroid.get(),
        "two signed colors are two themes");

  // A theme may name one appearance only, and the other keeps its default.
  const Theme::Handle lightOnly = make({kSlotParagraphColor}, {static_cast<double>(0xff111111u)});
  const Theme::Color &lightHalf = lightOnly->color(kSlotParagraphColor);
  CHECK(lightHalf.set && lightHalf.lightSet && !lightHalf.darkSet, "a light id alone names light mode only");
  CHECK(lightHalf.light == 0xff111111u, "with its value");
  const Theme::Handle darkOnly = make({paragraphDark}, {static_cast<double>(0xff222222u)});
  const Theme::Color &darkHalf = darkOnly->color(kSlotParagraphColor);
  CHECK(darkHalf.set && darkHalf.darkSet && !darkHalf.lightSet, "a dark id alone names dark mode only");
  CHECK(darkHalf.dark == 0xff222222u, "with its value");
  CHECK(lightOnly.get() != darkOnly.get(), "and the two are different themes");
  const Theme::Handle both = make({kSlotParagraphColor, paragraphDark},
                                  {static_cast<double>(0xff111111u), static_cast<double>(0xff111111u)});
  CHECK(both.get() != lightOnly.get(), "naming both halves is not naming one");

  // A syntax slot named for dark mode only keeps the palette in light mode.
  const uint32_t keyword = kSyntax[static_cast<size_t>(TokenClass::Keyword)];
  const Theme::Handle darkKeyword = make({kSlotDark[keyword - 1]}, {static_cast<double>(0xff333333u)});
  CHECK(darkKeyword->color(keyword).light == kTokenColors[static_cast<size_t>(TokenClass::Keyword)].light,
        "a dark-only syntax color keeps the light palette");
  CHECK(darkKeyword->color(keyword).dark == 0xff333333u, "and draws its own in dark mode");

  ThemeRegistry::clear();
}

void testResolvedValues() {
  // A metric, a color pair, and a font, all resolved the way a platform reads
  // them.
  const Theme::Handle theme = make(
      {kSlotParagraphFontSize, kSlotHeading1FontFamily, kSlotLinkColor, kSlotLinkColor + 1},
      // A font slot's value is an index into the font list, not the family.
      {20, 0, 0xff112233, 0xff445566},
      {"Inter", "JetBrainsMono-Regular"});
  CHECK(nearly(theme->number(kSlotParagraphFontSize), 20), "a metric resolves");
  CHECK(!theme->number(kSlotParagraphLineHeight) ? theme->number(kSlotParagraphLineHeight) == 0 : true,
        "an untouched metric keeps its default");
  CHECK(nearly(theme->number(kSlotParagraphFontWeight), 400), "an untouched weight keeps its default");

  const Theme::Color color = theme->color(kSlotLinkColor);
  CHECK(color.set, "an overridden color is set");
  CHECK(color.light == 0xff112233, "the light value resolves");
  CHECK(theme->color(kSlotLinkColor + 1).dark == 0xff445566, "the dark value resolves");
  CHECK(!theme->color(kSlotParagraphColor).set, "an untouched color is unset");
  CHECK(theme->color(kSlotParagraphColor).defaultRole == static_cast<uint16_t>(ColorRole::kRoleLabel),
        "and still names its role");

  CHECK(theme->font(kSlotHeading1FontFamily) == "Inter", "a font slot resolves to its family");
  CHECK(theme->font(kSlotParagraphFontFamily).empty(), "an untouched font slot is the system font");
  CHECK(theme->font(kSlotHeading2FontFamily) == theme->font(kSlotHeading3FontFamily),
        "two untouched font slots read as one shared name");

  // A font index past the list is the system font rather than a bad read.
  const Theme::Handle missing = make({kSlotHeading1FontFamily}, {7}, {"Inter"});
  CHECK(missing->font(kSlotHeading1FontFamily).empty(), "a font index past the list is the system font");
  const Theme::Handle negative = make({kSlotHeading1FontFamily}, {-1}, {"Inter"});
  CHECK(negative->font(kSlotHeading1FontFamily).empty(), "a negative font index is the system font");

  // A flag reads as a flag.
  CHECK(theme->flag(kSlotLinkUnderline), "a flag set to one reads true");
  CHECK(!theme->flag(kSlotCodeAbsoluteSize), "a flag at zero reads false");
}

/// A slot table whose ids would repoint a theme an app already wrote is refused
/// at generation time, so this check pins the ids the two languages share.
void testSlotTable() {
  CHECK(kSlotIdCount > kSyntax[static_cast<size_t>(TokenClass::Count) - 1],
        "the highest syntax slot is inside the table");

  // Names and ids line up one for one, because a platform reads a slot by name
  // when it warns about one.
  for (uint32_t id = 1; id < kSlotIdCount; id++) {
    CHECK(kSlotNames[id - 1] != nullptr, "every id has a name");
  }
  // Kind and layout arrays are indexed the same way.
  for (uint32_t id = 1; id < kSlotIdCount; id++) {
    CHECK(kSlotKinds[id - 1] <= static_cast<uint8_t>(SlotKind::Flag), "every id has a known kind");
  }
  // Every heading level names its own slots, so one level gaining a property
  // cannot repoint the levels above it.
  for (size_t level = 0; level < 6; level++) {
    for (uint32_t slot : {heading::kFontSize[level], heading::kFontWeight[level], heading::kFontFamily[level],
                          heading::kLineHeight[level], heading::kColor[level], heading::kFontStyle[level],
                          heading::kMarginTop[level], heading::kMarginBottom[level]}) {
      CHECK(slot != 0 && slot < kSlotIdCount, "a heading slot is inside the table");
    }
  }
  // A pair's two ids are adjacent and the dark one is marked as such.
  for (uint32_t id = 1; id < kSlotIdCount; id++) {
    if (kSlotDark[id - 1] != 0) {
      CHECK(kSlotDark[id - 1] == id + 1, "a pair's two ids are adjacent");
      CHECK(kSlotKinds[kSlotDark[id - 1] - 1] == static_cast<uint8_t>(SlotKind::Color), "and the second is a color");
      CHECK(kSlotNames[kSlotDark[id - 1] - 1] == kSlotNames[id - 1], "and it names the same slot");
    }
  }

  // Every syntax slot is a color and every non-syntax one is not named as one.
  for (size_t token = 0; token < static_cast<size_t>(TokenClass::Count); token++) {
    CHECK(kSlotKinds[kSyntax[token] - 1] == static_cast<uint8_t>(SlotKind::Color),
          "a syntax slot is a color");
    CHECK(kSlotLayout[kSyntax[token] - 1] == 0, "a syntax color changes no size");
  }

  // A color never changes layout, or the layout key would move on a palette
  // change and every measured size would be thrown away.
  for (uint32_t id = 1; id < kSlotIdCount; id++) {
    if (kSlotKinds[id - 1] == static_cast<uint8_t>(SlotKind::Color)) {
      CHECK(kSlotLayout[id - 1] == 0, "no color slot changes layout");
    }
  }
  // Every metric, weight and font does, or a theme could change a size without
  // the measurement noticing.
  for (uint32_t id = 1; id < kSlotIdCount; id++) {
    const auto kind = static_cast<SlotKind>(kSlotKinds[id - 1]);
    if (kind != SlotKind::Color && kind != SlotKind::Flag) {
      CHECK(kSlotLayout[id - 1] != 0, "every metric, weight and font slot changes layout");
    }
  }
}

/// The theme reaches the renderer's own palette through the generated table, so
/// the default syntax colors cannot drift between the two platforms again.
void testTokenDefaults() {
  const Theme::Handle theme = emptyTheme();
  for (size_t token = 0; token < static_cast<size_t>(TokenClass::Count); token++) {
    const uint16_t role = theme->color(kSyntax[token]).defaultRole;
    CHECK(role >= kRoleTokenClassBase, "a syntax default is a token class");
    const auto klass = static_cast<TokenClass>(role - kRoleTokenClassBase);
    CHECK(static_cast<size_t>(klass) == token, "and it is the class the slot is for");
    CHECK(tokenColor(klass, false) == kTokenColors[token].light, "and the engine's light value is what a platform reads");
    CHECK(tokenColor(klass, true) == kTokenColors[token].dark, "and the engine's dark value");
    // The unset slot carries the palette itself, which is what a renderer reads
    // for a class the theme left alone.
    const Theme::Color &color = theme->color(kSyntax[token]);
    CHECK(!color.set, "an unthemed syntax slot is unset");
    CHECK(color.light == kTokenColors[token].light, "and holds the palette's light value");
    CHECK(color.dark == kTokenColors[token].dark, "and its dark value");
  }
}

/// A platform looks a theme up by the id the state carried, which is what keeps
/// every slot from being copied across the boundary on every update.
/// Whether the theme named a slot, which is the question a renderer cannot
/// answer from the value alone.
void testNamedSlots() {
  ThemeRegistry::clear();
  const Theme::Handle empty = emptyTheme();
  for (uint32_t id = 1; id < kSlotIdCount; id++) {
    const bool named = empty->numberSet(id);
    // A default that is not the unset value still counts as unnamed, which is
    // the whole point: 700 is the default and also a value a theme may choose.
    if (named) {
      CHECK(false, "the empty theme names nothing");
    }
  }

  const Theme::Handle theme = make({kSlotStrongFontWeight, kSlotRadius, kSlotLinkColor, kSlotCodeBlockFontFamily},
                                   {700, 12, 0xff112233, 0},
                                   {"Inter"});
  // A value equal to the default is still named, which is what lets a theme
  // ask for 700 on a `strong` and mean it rather than get the "unset" answer.
  CHECK(theme->numberSet(kSlotStrongFontWeight), "a value that equals the default is still named");
  CHECK(theme->numberSet(kSlotRadius), "a metric is named");
  CHECK(!theme->numberSet(kSlotParagraphFontSize), "and a slot the theme left alone is not");
  CHECK(!theme->numberSet(kSlotHeading2MarginTop), "including one that was never in the list");

  // The one accessor answers for every kind, which is what a renderer that
  // does not know a slot's kind can use.
  CHECK(theme->slotSet(kSlotStrongFontWeight), "slotSet agrees for a metric");
  CHECK(theme->slotSet(kSlotLinkColor), "and for a color");
  CHECK(theme->slotSet(kSlotCodeBlockFontFamily), "and for a font");
  CHECK(!theme->slotSet(kSlotParagraphColor), "and for one nothing set");

  // A font slot naming an empty family is still named: the theme asked for the
  // platform font by name, which is different from not having an opinion.
  const Theme::Handle emptyFamily = make({kSlotParagraphFontFamily}, {0}, {""});
  CHECK(emptyFamily->slotSet(kSlotParagraphFontFamily), "an empty family is still a name");

  ThemeRegistry::clear();
}

void testLookupById() {
  ThemeRegistry::clear();

  CHECK(ThemeRegistry::find(0).get() == Theme::defaultTheme().get(), "no theme looks up as the default theme");

  const Theme::Handle theme = make({kSlotParagraphFontSize, kSlotLinkColor}, {20, 0xff112233});
  CHECK(ThemeRegistry::find(theme->id()).get() == theme.get(), "an interned theme is found by its id");

  // The same content found twice is the same object, which is what lets a view
  // compare handles instead of comparing themes.
  const Theme::Handle again = make({kSlotParagraphFontSize, kSlotLinkColor}, {20, 0xff112233});
  CHECK(ThemeRegistry::find(again->id()).get() == theme.get(), "and equal content is the same object");

  // An id the pool never held has no answer rather than a wrong one.
  CHECK(ThemeRegistry::find(theme->id() + 1) == nullptr, "an id the pool never held has no theme");
  CHECK(ThemeRegistry::find(0x7fffffffffffffffull) == nullptr, "and neither does an arbitrary one");

  // A cleared pool answers nothing for a real id, which is the one case where a
  // caller holding a handle would still be holding it.
  const uint64_t id = theme->id();
  ThemeRegistry::clear();
  CHECK(ThemeRegistry::find(id) == nullptr, "a cleared pool has nothing to find");
  CHECK(theme->number(kSlotParagraphFontSize) == 20, "but a handle still holds its own theme");
}

} // namespace

int main() {
  testSlotTable();
  testDefaultMetrics();
  testDefaultRoles();
  testBlockLayout();
  testThemedLayout();
  testElementTypography();
  testInterning();
  testNamedSlots();
  testLookupById();
  testResolvedValues();
  testTokenDefaults();
  return ds_check_result();
}