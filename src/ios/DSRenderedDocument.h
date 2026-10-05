#pragma once

#import <UIKit/UIKit.h>

#import "DSThemeColors.h"

#ifdef __cplusplus
#include <utility>
#include <vector>

#include "CodeLayout.h"
#include "Document.h"
#include "HostLayout.h"
#include "ImageLayout.h"
#include "RenderPreparer.h"
#include "math/MathLayout.h"
#include "theme/Theme.h"

/// One measurement's answer: the document's size, and where each React child
/// that replaced a fenced block goes.
///
/// The rectangles are the content rectangle of the block the child hosts, in
/// the document view's coordinates. They come from the same stacking pass that
/// produced the size, so the shadow node's placement and the view's own chrome
/// are two readings of one set of numbers rather than two computations that
/// could disagree.
struct DSMeasurement {
  CGSize size{0, 0};
  std::vector<std::pair<uint32_t, CGRect>> hosts;
};

/// One stacking pass's answer: every segment frame, the document's size, and
/// the content rectangle of each hosted fence. The view draws from the frames
/// and the shadow node places children at the rectangles.
struct DSStackedLayout {
  std::vector<CGRect> frames;
  std::vector<std::pair<uint32_t, CGRect>> hosts;
  CGSize size{0, 0};
};
#endif

NS_ASSUME_NONNULL_BEGIN

/// Character attribute holding the DSBlockInfo for each block's range.
extern NSAttributedStringKey const DSBlockInfoAttributeName;
/// Character attribute holding the NSNumber index into Document::links.
extern NSAttributedStringKey const DSLinkIndexAttributeName;

typedef NS_ENUM(NSInteger, DSContainerKind) {
  DSContainerKindQuote = 0,
  DSContainerKindListItem = 1,
  DSContainerKindAdmonition = 2,
  DSContainerKindFootnote = 3,
};

/// Decoration for one nesting level around a block. Leading offsets are in
/// points from the segment's leading edge.
@interface DSContainerInfo : NSObject
@property (nonatomic, readonly) DSContainerKind kind;
@property (nonatomic, readonly) CGFloat leading;
@property (nonatomic, readonly) CGFloat width;
/// Marker drawn in the container's margin on the block's first line, or nil.
@property (nonatomic, readonly, nullable) NSAttributedString *marker;
/// True when the previous block in the same segment belongs to the same
/// container, so bars join across the gap.
@property (nonatomic, readonly) BOOL continuesPrevious;
@property (nonatomic, readonly, nullable) UIColor *barColor;
/// The bar's width and the gap between a marker and the text, from the theme.
/// Zero-width gaps and bars are what a theme that sets them to zero draws.
@property (nonatomic, readonly) CGFloat barWidth;
@property (nonatomic, readonly) CGFloat markerGap;
@end

typedef NS_ENUM(NSInteger, DSBlockDecoration) {
  DSBlockDecorationNone = 0,
  DSBlockDecorationRule = 1,
};

/// A rule's own color and thickness, from the theme, rather than a literal the
/// layout manager would have to know about.
@interface DSDecorationMetrics : NSObject
@property (nonatomic, readonly) UIColor *color;
@property (nonatomic, readonly) CGFloat height;
@end

@interface DSBlockInfo : NSObject
@property (nonatomic, readonly) NSInteger index;
@property (nonatomic, readonly) DSBlockDecoration decoration;
@property (nonatomic, readonly) NSInteger headingLevel;
/// Gap above the block's text that bars of continuing containers bridge.
@property (nonatomic, readonly) CGFloat spacingBefore;
@property (nonatomic, readonly) CGFloat contentLeading;
@property (nonatomic, readonly) BOOL rightToLeft;
@property (nonatomic, readonly) NSArray<DSContainerInfo *> *containers;
/// Set when the block draws a rule. Nil otherwise.
@property (nonatomic, readonly, nullable) DSDecorationMetrics *rule;
/// Spoken summary of the block's markers, such as "checked".
@property (nonatomic, readonly, nullable) NSString *accessibilityPrefix;
@end

/// A run of non-code, non-table blocks laid out as one text layout.
@interface DSTextSegment : NSObject
@property (nonatomic, readonly) NSAttributedString *attributedString;
@property (nonatomic, readonly) NSArray<DSBlockInfo *> *blocks;
@property (nonatomic, readonly) NSArray<NSValue *> *blockRanges;
/// The theme this segment was built with, so a view draws its chrome from the
/// same theme as its text.
@property (nonatomic, readonly) DSThemeColors *theme;
/// Unrounded layout height at a width; segmentFramesForWidth rounds edges.
- (CGFloat)heightForWidth:(CGFloat)width;
/// The width the laid-out text occupies: the container width when the text
/// wraps, or its longest line when it does not.
- (CGFloat)usedWidthForWidth:(CGFloat)width;
@end

/// One code block: a card with a header row and a body that does not wrap.
///
/// The body is a one-block text segment, so selection, the plain-text `copy:`
/// override, token colors, and link hit testing are the ones a text segment
/// already has. Code is always laid out left to right, as in Streamdown.
@interface DSCodeSegment : NSObject
@property (nonatomic, readonly) DSTextSegment *text;
/// The fence language as written, or empty for an indented block.
@property (nonatomic, readonly) NSString *language;
/// The fence's title when it named one, which replaces the language in the
/// header. Sanitized by the engine's parser, so it is safe to draw as a label.
@property (nonatomic, readonly) NSString *title;
/// The fence's own meta, as the engine parsed it. The gutter and the cap both
/// resolve through `resolveCodeBlock` against this, so a measurement and a draw
/// cannot disagree about them.
@property (nonatomic, readonly) downstream::FenceMeta fenceMeta;
/// The UTF-16 offset of each logical line, found once on the worker so the
/// gutter's draw pass never searches the text.
@property (nonatomic, readonly) const std::vector<uint32_t> *lineStarts;
/// The number of logical lines, so the gutter knows its last number.
@property (nonatomic, readonly) uint32_t lineCount;
/// The advance of one digit in the code font at this font scale, which is what
/// makes the gutter exactly as wide as its widest number.
@property (nonatomic, readonly) CGFloat digitAdvance;
/// The code font at this font scale, which the gutter draws its numbers in.
@property (nonatomic, readonly) UIFont *codeFont;
@property (nonatomic, readonly) NSInteger blockIndex;
/// The block's ordinal among the document's code blocks, which is what a
/// reader's wrap or expand choice is keyed by, and what a React child that
/// replaced this block is keyed by.
@property (nonatomic, readonly) uint32_t codeOrdinal;
/// Whether the block's text runs right to left, which mirrors a hosted child's
/// content rectangle to the trailing edge, the same way the container bars and
/// the list marker are mirrored.
@property (nonatomic, readonly) BOOL rightToLeft;
@property (nonatomic, readonly) NSArray<DSContainerInfo *> *containers;
@property (nonatomic, readonly) CGFloat contentLeading;
/// The block's first line, which identifies it across snapshots while a fence
/// is still growing.
@property (nonatomic, readonly) NSString *firstLine;
/// The theme this card was built with, so the card's own chrome and its body's
/// text come from the same one.
@property (nonatomic, readonly) DSThemeColors *theme;
/// The header row's own typography and its two remaining colors: the buttons,
/// which a theme can tint apart from the row's text, and the rule under it.
@property (nonatomic, readonly) UIFont *headerFont;
@property (nonatomic, readonly, nullable) UIColor *headerButtonColor;
@property (nonatomic, readonly, nullable) UIColor *headerBorderColor;
/// Padding inside the card, its corner radius, and its border width, all from
/// the theme. A radius or width of zero is what a theme that removes the border
/// or squares the card asks for.
@property (nonatomic, readonly) CGFloat padding;
@property (nonatomic, readonly) CGFloat borderRadius;
@property (nonatomic, readonly) CGFloat borderWidth;
/// The card's line height, which the height floor in `codeCardHeight` needs.
@property (nonatomic, readonly) CGFloat lineHeight;

- (CGFloat)headerHeight;
/// The first nonblank logical line starting at or after this UTF-16 offset,
/// or NSNotFound. Searches offsets cached during preparation, never the text.
- (NSUInteger)baselineCharacterOffsetAtOrAfter:(NSUInteger)offset;
/// The width the body lays out at, given the card's width, whether it wraps, and
/// how much room the gutter takes. Unbounded when it does not wrap, which is what
/// makes a long line scroll instead of wrapping.
- (CGFloat)layoutWidthForCard:(CGFloat)cardWidth wrapped:(BOOL)wrapped gutterWidth:(CGFloat)gutterWidth;
/// The gutter's width: its widest number's digits in the code font, plus the gap
/// before the code. Zero when this block draws no numbers.
- (CGFloat)gutterWidthForLayout:(const downstream::CodeBlockLayout &)layout;
@end

/// One display formula: a box that draws vectors and scrolls sideways.
///
/// The layout came from the engine and is in em units, so this segment holds no
/// size of its own — a measurement and a draw both ask `mathBlockHeight` at the
/// size the theme and the font scale give, which is why a rotation or a theme
/// change cannot change a formula's height without re-laying it out.
@interface DSMathSegment : NSObject
/// The formula's display list, or null when the engine has no layout for it: a
/// formula that failed, or a document with no math package installed. Either way
/// the segment draws the formula's source, which is what a reader sees today.
@property (nonatomic, readonly, nullable) downstream::MathLayout::Handle layout;
/// The TeX, for the fallback and for the spoken form.
@property (nonatomic, readonly) std::string tex;
/// The source the reader sees when there is no layout: the delimiters and the
/// TeX as written, which is the whole literal.
@property (nonatomic, readonly) std::string literal;
/// Why the formula has no layout, for a development log. Empty when it does.
@property (nonatomic, readonly) std::string error;
@property (nonatomic, readonly) NSInteger blockIndex;
@property (nonatomic, readonly) NSArray<DSContainerInfo *> *containers;
@property (nonatomic, readonly) CGFloat contentLeading;
/// The formula's font size in points at this rendering's font scale, and the
/// padding and margins the theme gave it.
@property (nonatomic, readonly) CGFloat fontSize;
@property (nonatomic, readonly) CGFloat padding;
@property (nonatomic, readonly) CGFloat marginTop;
@property (nonatomic, readonly) CGFloat marginBottom;
/// 0 for left, 1 for center, 2 for right. Centered by default, as a display
/// formula is.
@property (nonatomic, readonly) CGFloat textAlign;
/// The theme's colors for this formula: the box behind it, the glyphs, and the
/// source it falls back to.
@property (nonatomic, readonly, nullable) UIColor *backgroundColor;
@property (nonatomic, readonly, nullable) UIColor *color;
@property (nonatomic, readonly, nullable) UIColor *errorColor;
/// The size the box takes, from `mathBlockHeight`. The one definition, which is
/// what keeps a measured formula and a drawn one the same height.
- (CGFloat)height;
/// The width the formula draws at, which is its own em width at this size.
- (CGFloat)contentWidth;
@end

/// One image block: the images, their states, and the theme's box metrics.
///
/// The block holds no size. A load changes a state, the state changes a box, and
/// `imageBox` is the only function that turns one into the other — so a message
/// that is measured at a placeholder height and then loaded is re-measured
/// through a state update rather than by any code here deciding a height.
@interface DSImageSegment : NSObject
/// The images this block holds, as indexes into `Document::images`, in order.
@property (nonatomic, readonly) std::vector<uint32_t> images;
/// Alt texts, one per image, which is the block's own text and what accessibility,
/// export, and the fallback all read.
@property (nonatomic, readonly) std::vector<std::string> altTexts;
/// Destinations, one per image, empty where the engine's policy refused one.
@property (nonatomic, readonly) std::vector<std::string> urls;
/// Titles, one per image, for a press event.
@property (nonatomic, readonly) std::vector<std::string> titles;
/// The enclosing link's index per image, or -1.
@property (nonatomic, readonly) std::vector<int32_t> links;
/// The block's ordinal among the document's image blocks, which is what a reader's
/// per-image choice would be keyed by.
@property (nonatomic, readonly) uint32_t imageOrdinal;
@property (nonatomic, readonly) NSInteger blockIndex;
@property (nonatomic, readonly) NSArray<DSContainerInfo *> *containers;
@property (nonatomic, readonly) CGFloat contentLeading;
@property (nonatomic, readonly) downstream::ImageMetrics metrics;
/// The placeholder's own color, which pulses before a size is known.
@property (nonatomic, readonly, nullable) UIColor *placeholderColor;
/// What each image is in this view. One entry per image, in the same order as
/// `images`, and a `Disabled` entry for an image the view has no answer for.
- (std::vector<const downstream::ImageEntry *>)entriesFor:(const downstream::ImageStateLayout &)answers;
/// The box for one image at a column width, through the engine's own rule.
- (downstream::ImageBox)boxAtIndex:(NSUInteger)index
                            column:(CGFloat)column
                           answers:(const downstream::ImageStateLayout &)answers;
/// The whole block's height at a column width: every image's box plus the
/// gaps between them, and the theme's margins.
- (CGFloat)heightForColumn:(CGFloat)column answers:(const downstream::ImageStateLayout &)answers;
@end

@interface DSTableCell : NSObject
@property (nonatomic, readonly) NSAttributedString *text;
@end

/// One table. Column widths depend only on content, so the layout is the
/// same at every view width; wide tables scroll horizontally.
@interface DSTableSegment : NSObject
@property (nonatomic, readonly) NSArray<NSArray<DSTableCell *> *> *rows;
@property (nonatomic, readonly) BOOL hasHeader;
@property (nonatomic, readonly) CGFloat contentLeading;
@property (nonatomic, readonly) BOOL rightToLeft;
/// Identifies the table across snapshots while its first row is unchanged.
@property (nonatomic, readonly) uint32_t tableIdentifier;
@property (nonatomic, readonly) NSString *firstRowText;
@property (nonatomic, readonly) NSArray<DSContainerInfo *> *containers;
@property (nonatomic, readonly) NSArray<NSNumber *> *columnWidths;
@property (nonatomic, readonly) NSArray<NSNumber *> *rowHeights;
@property (nonatomic, readonly) CGSize tableSize;
@property (nonatomic, readonly) UIEdgeInsets cellPadding;
/// The theme this table was built with, plus the grid and header metrics that
/// used to be literals in the grid view.
@property (nonatomic, readonly) DSThemeColors *theme;
@property (nonatomic, readonly) CGFloat borderWidth;
@property (nonatomic, readonly) CGFloat borderRadius;
/// Row fills, alternating from the first data row. Nil where a theme sets none.
@property (nonatomic, readonly, nullable) UIColor *rowBackgroundColor;
/// The toolbar's button color, which a theme can set apart from the table's own
/// text so the chrome reads as secondary without being smaller.
@property (nonatomic, readonly, nullable) UIColor *toolbarButtonColor;
/// Index of the segment's first block, which is what a copy or a download of
/// the whole table reads.
@property (nonatomic, readonly) NSInteger blockIndex;
@property (nonatomic, readonly) uint32_t blockCount;
@end

/// An immutable platform rendering of one Document at one font scale. The
/// same object is used to measure and to lay out, so both agree.
@interface DSRenderedDocument : NSObject
/// DSTextSegment, DSCodeSegment, DSMathSegment, DSImageSegment, and
/// DSTableSegment objects in document order.
@property (nonatomic, readonly) NSArray<id> *segments;
/// The document's first block index per segment, so a control knows what to read.
@property (nonatomic, readonly) NSArray<NSNumber *> *segmentBlockIndexes;
/// The number of leading blocks no later input can change, which is what turns
/// a segment's controls and its selection on.
@property (nonatomic, readonly) uint32_t settledBlocks;
/// Which control rows the document lays out with. See `downstream::Chrome`.
@property (nonatomic, readonly) uint32_t chrome;
/// The theme this rendering was built with, so a view draws its chrome from the
/// same theme as its text.
@property (nonatomic, readonly) DSThemeColors *theme;

#ifdef __cplusplus
/// Returns a cached rendering. Safe to call from any thread.
///
/// `syntaxHighlighting` decides whether code blocks carry token colors. Tokens
/// are always computed, so this only chooses the rendering, and it is part of
/// the cache key because the two renderings differ.
///
/// `theme` is the view's own theme rather than the default one, and measurement
/// passes the same theme the view will mount. That is what keeps a themed
/// measure and a themed draw from being two different sets of numbers, and it is
/// why the theme is part of the cache key: a themed rendering is not
/// interchangeable with the default one.
+ (instancetype)renderedDocumentFor:(const downstream::DocumentHandle &)document
                          fontScale:(CGFloat)fontScale
                 syntaxHighlighting:(BOOL)syntaxHighlighting
                              theme:(const downstream::Theme::Handle &)theme;
#endif

/// Drops every cached rendering. For tests and for a caller that wants to force
/// every rendering to be rebuilt rather than reused.
///
/// A color appearance change does not need this: token colors are dynamic
/// UIColors, which the platform resolves against the trait collection when it
/// draws, so a cached rendering already follows the appearance.
+ (void)invalidateCaches;

/// Segment frames stacked at a width. Safe to call from any thread.
///
/// `chrome` and the whole code layout are part of the key rather than the
/// rendering, so a change to either restacks frames without rebuilding any
/// attributed text. Every input that changes a card's height is in that layout,
/// which is what keeps a measurement and the frame the view draws from agreeing.
- (NSArray<NSValue *> *)segmentFramesForWidth:(CGFloat)width
                                     chrome:(uint32_t)chrome
                                        code:(const downstream::CodeLayout &)code
                                      images:(const downstream::ImageStateLayout &)images
                                       hosts:(const downstream::HostLayout &)hosts;
- (CGSize)sizeForWidth:(CGFloat)width
                chrome:(uint32_t)chrome
                   code:(const downstream::CodeLayout &)code
                 images:(const downstream::ImageStateLayout &)images
                  hosts:(const downstream::HostLayout &)hosts;

#ifdef __cplusplus
/// The size and the hosted children's rectangles, from one stacking pass.
///
/// `measure` is what the shadow node asks for, and the hosted rectangles are
/// what it places the children at: the same call produces the document's height
/// and the positions inside it, so a frame that draws a child where the
/// document was measured to put it is not possible.
- (DSMeasurement)measureForWidth:(CGFloat)width
                          chrome:(uint32_t)chrome
                             code:(const downstream::CodeLayout &)code
                          images:(const downstream::ImageStateLayout &)images
                           hosts:(const downstream::HostLayout &)hosts;
#endif

@end

/// TextKit 1 objects configured identically for measuring and drawing.
@interface DSTextKitStack : NSObject
@property (nonatomic, readonly) NSTextStorage *textStorage;
@property (nonatomic, readonly) NSLayoutManager *layoutManager;
@property (nonatomic, readonly) NSTextContainer *textContainer;
- (instancetype)initWithLayoutManager:(NSLayoutManager *)layoutManager;
@end

NS_ASSUME_NONNULL_END
