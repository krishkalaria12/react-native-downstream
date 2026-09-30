#pragma once

#import <UIKit/UIKit.h>

#ifdef __cplusplus
#include "Document.h"
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
@end

typedef NS_ENUM(NSInteger, DSBlockDecoration) {
  DSBlockDecorationNone = 0,
  DSBlockDecorationCode = 1,
  DSBlockDecorationRule = 2,
};

@interface DSBlockInfo : NSObject
@property (nonatomic, readonly) NSInteger index;
@property (nonatomic, readonly) DSBlockDecoration decoration;
@property (nonatomic, readonly) NSInteger headingLevel;
/// Gap above the block's text that bars of continuing containers bridge.
@property (nonatomic, readonly) CGFloat spacingBefore;
@property (nonatomic, readonly) CGFloat contentLeading;
@property (nonatomic, readonly) CGFloat codePadding;
@property (nonatomic, readonly) BOOL rightToLeft;
@property (nonatomic, readonly) NSArray<DSContainerInfo *> *containers;
/// Spoken summary of the block's markers, such as "checked".
@property (nonatomic, readonly, nullable) NSString *accessibilityPrefix;
@end

/// A run of non-table blocks laid out as one text layout.
@interface DSTextSegment : NSObject
@property (nonatomic, readonly) NSAttributedString *attributedString;
/// Extra space above the first and below the last line, for code padding.
@property (nonatomic, readonly) CGFloat topInset;
@property (nonatomic, readonly) CGFloat bottomInset;
@property (nonatomic, readonly) NSArray<DSBlockInfo *> *blocks;
@property (nonatomic, readonly) NSArray<NSValue *> *blockRanges;
/// Unrounded layout height at a width; segmentFramesForWidth rounds edges.
- (CGFloat)heightForWidth:(CGFloat)width;
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
@end

/// An immutable platform rendering of one Document at one font scale. The
/// same object is used to measure and to lay out, so both agree.
@interface DSRenderedDocument : NSObject
/// DSTextSegment and DSTableSegment objects in document order.
@property (nonatomic, readonly) NSArray<id> *segments;

#ifdef __cplusplus
/// Returns a cached rendering. Safe to call from any thread.
///
/// `syntaxHighlighting` decides whether code blocks carry token colors. Tokens
/// are always computed, so this only chooses the rendering, and it is part of
/// the cache key because the two renderings differ. Measurement passes true:
/// colors do not change metrics, so a measurement is valid either way.
+ (instancetype)renderedDocumentFor:(const downstream::DocumentHandle &)document
                          fontScale:(CGFloat)fontScale
                 syntaxHighlighting:(BOOL)syntaxHighlighting;
#endif

/// Drops every cached rendering. Colors are stored as dynamic UIColors and the
/// platform resolves them when it draws, so a rendering survives an appearance
/// switch; this is for the case where a resolved color would be cheaper to
/// rebuild than to re-resolve, and for tests.
+ (void)invalidateCaches;

/// Segment frames stacked at a width. Safe to call from any thread.
- (NSArray<NSValue *> *)segmentFramesForWidth:(CGFloat)width;
- (CGSize)sizeForWidth:(CGFloat)width;
@end

/// TextKit 1 objects configured identically for measuring and drawing.
@interface DSTextKitStack : NSObject
@property (nonatomic, readonly) NSTextStorage *textStorage;
@property (nonatomic, readonly) NSLayoutManager *layoutManager;
@property (nonatomic, readonly) NSTextContainer *textContainer;
- (instancetype)initWithLayoutManager:(NSLayoutManager *)layoutManager;
@end

NS_ASSUME_NONNULL_END
