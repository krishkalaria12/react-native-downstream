#pragma once

#import <UIKit/UIKit.h>

#ifdef __cplusplus
#include "math/MathLayout.h"
#endif

NS_ASSUME_NONNULL_BEGIN

/// Character attribute holding the `DSMathAttachment` for a typeset formula.
///
/// The attachment is an object rather than a value because it carries the display
/// list, which is shared with the document and must not be copied per run. The
/// attribute is what a draw pass reads, and `Block::text` still holds the literal
/// underneath it, so a copy of the text is still the writer's source.
extern NSAttributedStringKey const DSMathAttachmentAttributeName;

/// Character attribute holding the spoken form of a formula, for a screen reader.
///
/// Separate from the label because a text segment's element is built from the
/// block's text, and a reader who navigates into a formula needs the sentence
/// rather than the delimiters.
extern NSAttributedStringKey const DSMathSpeechAttributeName;

/**
 * One inline formula, as an `NSTextAttachment` with no image.
 *
 * An attachment rather than a run of text because a formula is a picture in the
 * middle of a sentence: it has a width, a height above the baseline and a depth
 * below it, and none of those are expressible as an attributed string. The
 * literal run is replaced by one attachment character, and the literal itself is
 * kept as an attribute so a copy and a screen reader both still produce the
 * writer's source.
 */
@interface DSMathAttachment : NSTextAttachment
/// The formula's display list, or null when the formula could not be laid out.
@property (nonatomic, readonly, nullable) downstream::MathLayout::Handle layout;
/// The run's size in points, already scaled by the platform font scale.
@property (nonatomic, readonly) CGFloat fontSize;
/// The run's own color, which the display list does not carry.
@property (nonatomic, readonly, nullable) UIColor *color;
/// The spoken form, read by `DSDocumentView`'s accessibility elements.
@property (nonatomic, readonly, copy) NSString *speech;
/// The literal, delimiters included, which is what a copy produces.
@property (nonatomic, readonly, copy) NSString *literal;

- (instancetype)initWithLayout:(downstream::MathLayout::Handle)layout
                      fontSize:(CGFloat)fontSize
                         color:(nullable UIColor *)color
                        speech:(NSString *)speech
                       literal:(NSString *)literal;

/// The attachment's bounds for a container width: the formula's own size, scaled
/// down when it is wider than the line, and never below half size.
- (CGRect)boundsForContainerWidth:(CGFloat)width;
@end

/// Metrics of one block's text, in text container coordinates.
typedef struct {
  CGFloat top;
  CGFloat bottom;
  CGFloat firstBaseline;
} DSBlockTextBounds;

/// Draws block decorations that attributed text cannot express: quote bars,
/// list markers, and rules. A code block's background is drawn by its card, not
/// here, because a card is a view rather than a text layout.
/// Layout is identical to NSLayoutManager, so measurement uses the base class.
@interface DSLayoutManager : NSLayoutManager

/// Vertical bounds of the text in a character range, excluding paragraph
/// spacing. Returns NO when the range has no laid-out glyphs.
- (BOOL)textBoundsForCharacterRange:(NSRange)range bounds:(DSBlockTextBounds *)bounds;

@end

@class DSLayoutManager;


NS_ASSUME_NONNULL_END
