#pragma once

#ifdef __cplusplus
#include <vector>

#include "Reveal.h"
#endif

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@class DSLayoutManager;

/// One run of a segment's characters and the alpha it draws at.
@interface DSRevealRun : NSObject
/// In the layout manager's own character coordinates.
@property (nonatomic, readonly) NSRange range;
/// 0 to 1. A run at 1 is drawn by the base class rather than by a fade.
@property (nonatomic, readonly) CGFloat alpha;
/// How far below its line the run is drawn, in points. Zero for every effect but
/// a rise, and it never changes the layout.
@property (nonatomic, readonly) CGFloat rise;
- (instancetype)initWithRange:(NSRange)range alpha:(CGFloat)alpha rise:(CGFloat)rise;
@end

/// Where a view's fading characters live.
///
/// Weak, because the layout manager is owned by a view that the document view
/// owns, and a fade that outlived its provider would draw into nothing. An empty
/// answer means everything is opaque, which is the case a draw pass costs one nil
/// check for.
///
/// `blockOffset` and `blockRanges` are the layout's own, so the provider maps a
/// document block's offsets into whichever layout asked rather than holding a
/// reference to it. `now` is `CACurrentMediaTime()`, in seconds, in every method.
@protocol DSRevealProvider <NSObject>

/// The runs of `range` whose alpha is below 1 at `now`, in text order, in this
/// layout's own character coordinates. `blockRanges[i]` is where document block
/// `blockOffset + i` sits in the layout's text.
- (NSArray<DSRevealRun *> *)revealRunsForCharacterRange:(NSRange)range
                                            blockOffset:(NSInteger)blockOffset
                                            blockRanges:(nullable NSArray<NSValue *> *)blockRanges
                                                     now:(double)now;

/// First hidden character in the requested layout range, or NSNotFound. The
/// caret needs only this position, without allocating every fading draw run.
- (NSUInteger)revealFrontierForCharacterRange:(NSRange)range
                                   blockOffset:(NSInteger)blockOffset
                                   blockRanges:(nullable NSArray<NSValue *> *)blockRanges
                                            now:(double)now;

/// The alpha at one character of a document block.
///
/// A list marker and a rule ask for offset 0, which is their block's first
/// character; a code gutter asks for each line's own first character, which is
/// what makes a new line bring its number with it rather than showing the number
/// before the code.
- (CGFloat)revealAlphaAtBlock:(NSInteger)block offset:(NSUInteger)offset now:(double)now;

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

/// Where this layout's fading characters are. Nil means nothing is fading, which
/// is every draw pass when the animation is off.
@property (nonatomic, weak, nullable) id<DSRevealProvider> revealProvider;

/// The document block index of this layout's first character, so a reveal unit's
/// block-and-offset pair maps into this layout's own text.
@property (nonatomic) NSInteger blockOffset;

/// Where each of this layout's blocks sits in its text, starting at `blockOffset`.
@property (nonatomic, copy, nullable) NSArray<NSValue *> *blockRanges;

/// Draws the caret's shape at its placeholder.
///
/// The placeholder is a text attachment that draws nothing, so the shape is drawn
/// here, in the run's own color and inside the attachment's bounds. Drawing rather
/// than typesetting is what makes the caret look the same in every font and keeps
/// it from changing a line's height.
///
/// The placeholder is the text's last character when there is one. While a fade
/// runs the shape is drawn at the reveal frontier instead.
- (void)drawCaretAtPoint:(CGPoint)origin;

@end

@class DSLayoutManager;


NS_ASSUME_NONNULL_END
