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
/// points from the text container's leading edge.
@interface DSContainerInfo : NSObject
@property (nonatomic, readonly) DSContainerKind kind;
@property (nonatomic, readonly) CGFloat leading;
@property (nonatomic, readonly) CGFloat width;
/// Marker drawn in the container's margin on the block's first line, or nil.
@property (nonatomic, readonly, nullable) NSAttributedString *marker;
/// True when the previous block belongs to the same container, so bars join.
@property (nonatomic, readonly) BOOL continuesPrevious;
@property (nonatomic, readonly, nullable) UIColor *barColor;
@end

typedef NS_ENUM(NSInteger, DSBlockDecoration) {
  DSBlockDecorationNone = 0,
  DSBlockDecorationCode = 1,
  DSBlockDecorationRule = 2,
  DSBlockDecorationTableHeader = 3,
};

@interface DSBlockInfo : NSObject
@property (nonatomic, readonly) NSInteger index;
@property (nonatomic, readonly) DSBlockDecoration decoration;
@property (nonatomic, readonly) NSInteger headingLevel;
/// Gap above the block's text that bars of continuing containers bridge.
@property (nonatomic, readonly) CGFloat spacingBefore;
@property (nonatomic, readonly) CGFloat contentLeading;
@property (nonatomic, readonly) CGFloat codePadding;
@property (nonatomic, readonly) NSArray<DSContainerInfo *> *containers;
/// Spoken summary of the block's markers, such as "bullet" or "checked".
@property (nonatomic, readonly, nullable) NSString *accessibilityPrefix;
@end

/// An immutable platform rendering of one Document at one font scale. The
/// same object is used to measure and to draw, so both see identical text.
@interface DSRenderedDocument : NSObject
@property (nonatomic, readonly) NSAttributedString *attributedString;
/// Extra space above the first and below the last line, for code padding.
@property (nonatomic, readonly) CGFloat topInset;
@property (nonatomic, readonly) CGFloat bottomInset;
@property (nonatomic, readonly) NSArray<DSBlockInfo *> *blocks;
@property (nonatomic, readonly) NSArray<NSValue *> *blockRanges;

#ifdef __cplusplus
/// Returns a cached rendering. Safe to call from any thread.
+ (instancetype)renderedDocumentFor:(const downstream::DocumentHandle &)document fontScale:(CGFloat)fontScale;
#endif

/// Measures the rendering at a width with a standalone TextKit stack.
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
