#pragma once

#import <UIKit/UIKit.h>

#import "DSThemeColors.h"

#ifdef __cplusplus
#include "Document.h"
#include "RenderPreparer.h"
#include "theme/Theme.h"
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
@property (nonatomic, readonly) NSInteger blockIndex;
/// The block's ordinal among the document's code blocks, which is what a
/// reader's wrap choice is keyed by.
@property (nonatomic, readonly) uint32_t codeOrdinal;
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

- (CGFloat)headerHeight;
/// Body height at a card width, wrapped or not. A wrapped body's height depends
/// on the width; an unwrapped one's does not, which is why a rotation does not
/// lay code out again.
- (CGFloat)bodyHeightForWidth:(CGFloat)width wrapped:(BOOL)wrapped;
/// Body width at a card width, wrapped or not. Unwrapped, it is the longest
/// line, which is what the horizontal scroll view scrolls.
- (CGFloat)bodyWidthForWidth:(CGFloat)width wrapped:(BOOL)wrapped;
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
/// DSTextSegment, DSCodeSegment, and DSTableSegment objects in document order.
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
/// `chrome` and the reader's wrap choices are part of the key rather than the
/// rendering, so a change to either restacks frames without rebuilding any
/// attributed text.
- (NSArray<NSValue *> *)segmentFramesForWidth:(CGFloat)width
                                     chrome:(uint32_t)chrome
                              codeBlockWrap:(BOOL)codeBlockWrap
                                wrapToggles:(const std::vector<uint32_t> &)wrapToggles;
- (CGSize)sizeForWidth:(CGFloat)width
                chrome:(uint32_t)chrome
         codeBlockWrap:(BOOL)codeBlockWrap
           wrapToggles:(const std::vector<uint32_t> &)wrapToggles;
/// The chrome a view lays out with for the props it has.
- (uint32_t)chromeForControls:(uint32_t)controls codeBlockWrap:(BOOL)codeBlockWrap;

@end

/// TextKit 1 objects configured identically for measuring and drawing.
@interface DSTextKitStack : NSObject
@property (nonatomic, readonly) NSTextStorage *textStorage;
@property (nonatomic, readonly) NSLayoutManager *layoutManager;
@property (nonatomic, readonly) NSTextContainer *textContainer;
- (instancetype)initWithLayoutManager:(NSLayoutManager *)layoutManager;
@end

NS_ASSUME_NONNULL_END
