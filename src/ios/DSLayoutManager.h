#pragma once

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

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

NS_ASSUME_NONNULL_END
