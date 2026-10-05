#import "DSLayoutManager.h"

#import "DSRenderedDocument.h"

#include <algorithm>

using namespace downstream;

@implementation DSRevealRun

- (instancetype)initWithRange:(NSRange)range alpha:(CGFloat)alpha rise:(CGFloat)rise {
  if (self = [super init]) {
    _range = range;
    _alpha = alpha;
    _rise = rise;
  }
  return self;
}

@end

@implementation DSLayoutManager

- (BOOL)textBoundsForCharacterRange:(NSRange)range bounds:(DSBlockTextBounds *)bounds {
  const NSRange glyphs = [self glyphRangeForCharacterRange:range actualCharacterRange:NULL];
  if (glyphs.length == 0 || NSMaxRange(glyphs) > self.numberOfGlyphs) {
    return NO;
  }
  const NSUInteger firstGlyph = glyphs.location;
  const NSUInteger lastGlyph = NSMaxRange(glyphs) - 1;

  const CGRect firstLine = [self lineFragmentRectForGlyphAtIndex:firstGlyph effectiveRange:NULL];
  const CGPoint firstLocation = [self locationForGlyphAtIndex:firstGlyph];
  UIFont *firstFont = [self.textStorage attribute:NSFontAttributeName atIndex:range.location effectiveRange:NULL];
  const CGRect lastUsed = [self lineFragmentUsedRectForGlyphAtIndex:lastGlyph effectiveRange:NULL];

  bounds->firstBaseline = CGRectGetMinY(firstLine) + firstLocation.y;
  bounds->top = bounds->firstBaseline - (firstFont != nil ? firstFont.ascender : 0);
  bounds->bottom = CGRectGetMaxY(lastUsed);
  return YES;
}

#pragma mark - The fade

/// The runs of `characters` that are fading, or nothing.
///
/// The alpha of a fade falls along text order, so the answer is a fully shown
/// prefix, a short band of runs, and a hidden suffix. An empty answer means the
/// whole range is opaque and the caller draws it exactly as it always has.
- (NSArray<DSRevealRun *> *)fadingRunsFor:(NSRange)characters {
  if (self.revealProvider == nil || characters.length == 0) {
    return nil;
  }
  return [self.revealProvider revealRunsForCharacterRange:characters
                                              blockOffset:self.blockOffset
                                              blockRanges:self.blockRanges
                                                       now:CACurrentMediaTime()];
}

/// Draws one range of glyphs at an alpha and an offset.
///
/// Splitting a glyph range is exact rather than approximate: the glyphs are
/// already shaped and positioned, so kerning, ligatures, and Arabic joining are
/// unaffected by a run boundary. Unit boundaries end after whitespace, so a split
/// never falls inside a word.
- (void)drawGlyphs:(NSRange)glyphs atPoint:(CGPoint)origin alpha:(CGFloat)alpha rise:(CGFloat)rise {
  if (alpha >= 1 && rise == 0) {
    [super drawGlyphsForGlyphRange:glyphs atPoint:origin];
    return;
  }
  CGContextRef context = UIGraphicsGetCurrentContext();
  if (context == nullptr) {
    [super drawGlyphsForGlyphRange:glyphs atPoint:origin];
    return;
  }
  CGContextSaveGState(context);
  CGContextSetAlpha(context, alpha);
  if (rise != 0) {
    CGContextTranslateCTM(context, 0, rise);
  }
  [super drawGlyphsForGlyphRange:glyphs atPoint:origin];
  CGContextRestoreGState(context);
}

/// The glyph range for one character range, clipped to the glyphs this layout
/// actually has. A range at the very end of a text storage can ask for a glyph
/// past the last one, which would be an exception rather than an empty range.
- (NSRange)clampedGlyphsFor:(NSRange)characters {
  if (characters.length == 0 || self.numberOfGlyphs == 0) {
    return NSMakeRange(0, 0);
  }
  NSRange glyphs = [self glyphRangeForCharacterRange:characters actualCharacterRange:NULL];
  if (NSMaxRange(glyphs) > self.numberOfGlyphs) {
    glyphs.length = self.numberOfGlyphs - glyphs.location;
  }
  return glyphs;
}

- (void)drawGlyphsForGlyphRange:(NSRange)glyphsToShow atPoint:(CGPoint)origin {
  if (self.revealProvider == nil) {
    [super drawGlyphsForGlyphRange:glyphsToShow atPoint:origin];
  } else {
    const NSRange characters = [self characterRangeForGlyphRange:glyphsToShow actualGlyphRange:NULL];
    [self drawFadingGlyphs:glyphsToShow characters:characters atPoint:origin];
  }
  // The caret is drawn rather than typeset, over the placeholder the measured text
  // reserved for it, and after the text so nothing paints over it.
  [self drawCaretAtPoint:origin];
}

- (void)drawFadingGlyphs:(NSRange)glyphsToShow characters:(NSRange)characters atPoint:(CGPoint)origin {
  NSArray<DSRevealRun *> *runs = [self fadingRunsFor:characters];
  if (runs.count == 0) {
    [super drawGlyphsForGlyphRange:glyphsToShow atPoint:origin];
    return;
  }
  NSUInteger cursor = characters.location;
  for (DSRevealRun *run in runs) {
    const NSRange clipped = NSIntersectionRange(run.range, characters);
    if (clipped.length == 0) {
      continue;
    }
    // Active units can surround already-settled text, including a later block
    // whose fade finished earlier. Every gap and the tail remain opaque.
    if (clipped.location > cursor) {
      [super drawGlyphsForGlyphRange:[self clampedGlyphsFor:NSMakeRange(cursor, clipped.location - cursor)]
                            atPoint:origin];
    }
    if (run.alpha > 0) {
      [self drawGlyphs:[self clampedGlyphsFor:clipped] atPoint:origin alpha:run.alpha rise:run.rise];
    }
    cursor = std::max(cursor, NSMaxRange(clipped));
  }
  if (cursor < NSMaxRange(characters)) {
    [super drawGlyphsForGlyphRange:[self clampedGlyphsFor:NSMakeRange(cursor, NSMaxRange(characters) - cursor)]
                          atPoint:origin];
  }
}

- (void)drawBackgroundForGlyphRange:(NSRange)glyphsToShow atPoint:(CGPoint)origin {
  if (self.revealProvider == nil) {
    [super drawBackgroundForGlyphRange:glyphsToShow atPoint:origin];
    [self drawDecorationsForGlyphs:glyphsToShow atPoint:origin];
    return;
  }
  const NSRange characters = [self characterRangeForGlyphRange:glyphsToShow actualGlyphRange:NULL];
  NSArray<DSRevealRun *> *runs = [self fadingRunsFor:characters];
  if (runs.count == 0) {
    [super drawBackgroundForGlyphRange:glyphsToShow atPoint:origin];
  } else {
    NSUInteger cursor = characters.location;
    for (DSRevealRun *run in runs) {
      const NSRange clipped = NSIntersectionRange(run.range, characters);
      if (clipped.length == 0) {
        continue;
      }
      if (clipped.location > cursor) {
        [super drawBackgroundForGlyphRange:[self clampedGlyphsFor:NSMakeRange(cursor, clipped.location - cursor)]
                                   atPoint:origin];
      }
      if (run.alpha > 0) {
        CGContextRef context = UIGraphicsGetCurrentContext();
        CGContextSaveGState(context);
        CGContextSetAlpha(context, run.alpha);
        if (run.rise != 0) {
          CGContextTranslateCTM(context, 0, run.rise);
        }
        [super drawBackgroundForGlyphRange:[self clampedGlyphsFor:clipped] atPoint:origin];
        CGContextRestoreGState(context);
      }
      cursor = std::max(cursor, NSMaxRange(clipped));
    }
    if (cursor < NSMaxRange(characters)) {
      [super drawBackgroundForGlyphRange:[self clampedGlyphsFor:NSMakeRange(cursor, NSMaxRange(characters) - cursor)]
                                 atPoint:origin];
    }
  }
  // Markers and bars belong to blocks, rather than to word/character runs.
  // Drawing them once also avoids repeatedly compositing the same fading marker.
  [self drawDecorationsForGlyphs:glyphsToShow atPoint:origin];
}

/// Quote bars, list markers, and rules, with no fade split of their own.
///
/// A code block's background is drawn by its card rather than here, because a card
/// is a view rather than a text layout. A marker takes the alpha of its item's
/// first fading word rather than fading on its own, which is what makes a bullet
/// appear with the text it belongs to; a rule takes its block's single unit. A
/// quote bar draws at full alpha, as Streamdown draws a border.
- (void)drawDecorationsForGlyphs:(NSRange)glyphsToShow atPoint:(CGPoint)origin {
  NSTextStorage *storage = self.textStorage;
  NSTextContainer *container = self.textContainers.firstObject;
  if (storage.length == 0 || container == nil) {
    return;
  }
  const CGFloat width = container.size.width;
  const NSRange visible = [self characterRangeForGlyphRange:glyphsToShow actualGlyphRange:NULL];

  NSUInteger index = visible.location;
  while (index < NSMaxRange(visible)) {
    NSRange blockRange;
    DSBlockInfo *info = [storage attribute:DSBlockInfoAttributeName
                                   atIndex:index
                     longestEffectiveRange:&blockRange
                                   inRange:NSMakeRange(0, storage.length)];
    index = NSMaxRange(blockRange);
    if (info == nil || (info.containers.count == 0 && info.rule == nil)) {
      continue;
    }
    DSBlockTextBounds bounds;
    if (![self textBoundsForCharacterRange:blockRange bounds:&bounds]) {
      continue;
    }
    const CGFloat top = origin.y + bounds.top;
    const CGFloat bottom = origin.y + bounds.bottom;
    // A marker fades with its item's first word, and a rule with its block's own
    // single unit, so both ask for the alpha at the block's start.
    const CGFloat blockAlpha = self.revealProvider == nil
        ? 1
        : [self.revealProvider revealAlphaAtBlock:(NSInteger)info.index offset:0 now:CACurrentMediaTime()];

    for (DSContainerInfo *containerInfo in info.containers) {
      // A zero-width bar is a bar the theme removed, so the width is checked
      // before drawing rather than only the color.
      if (containerInfo.barColor != nil && containerInfo.barWidth > 0) {
        const CGFloat barTop = containerInfo.continuesPrevious ? top - info.spacingBefore - 1 : top;
        CGContextRef context = UIGraphicsGetCurrentContext();
        CGContextSaveGState(context);
        CGContextSetAlpha(context, blockAlpha);
        [containerInfo.barColor setFill];
        const CGFloat x = info.rightToLeft
            ? width - containerInfo.leading - 2 - containerInfo.barWidth
            : containerInfo.leading + 2;
        UIRectFill(CGRectMake(origin.x + x, barTop, containerInfo.barWidth, bottom - barTop));
        CGContextRestoreGState(context);
      }
      if (containerInfo.marker != nil) {
        UIFont *markerFont = [containerInfo.marker attribute:NSFontAttributeName atIndex:0 effectiveRange:NULL];
        const CGSize markerSize = containerInfo.marker.size;
        const CGFloat x = info.rightToLeft
            ? width - containerInfo.leading - containerInfo.width + containerInfo.markerGap
            : containerInfo.leading + containerInfo.width - containerInfo.markerGap - markerSize.width;
        const CGFloat y = bounds.firstBaseline - markerFont.ascender;
        CGContextRef context = UIGraphicsGetCurrentContext();
        CGContextSaveGState(context);
        CGContextSetAlpha(context, blockAlpha);
        [containerInfo.marker drawAtPoint:CGPointMake(origin.x + x, origin.y + y)];
        CGContextRestoreGState(context);
      }
    }

    const CGFloat left = origin.x + (info.rightToLeft ? 0 : info.contentLeading);
    if (info.rule != nil) {
      // The rule's color and thickness ride on the block, so a themed rule is
      // drawn with the theme's own two values.
      CGContextRef context = UIGraphicsGetCurrentContext();
      CGContextSaveGState(context);
      CGContextSetAlpha(context, blockAlpha);
      [info.rule.color setFill];
      UIRectFill(CGRectMake(left, round((top + bottom) / 2) - (info.rule.height - 1) / 2,
                            width - info.contentLeading, info.rule.height));
      CGContextRestoreGState(context);
    }
  }
}

#pragma mark - The caret

- (void)drawCaretAtPoint:(CGPoint)origin {
  NSTextStorage *storage = self.textStorage;
  if (storage.length == 0) {
    return;
  }
  // The placeholder is always the text's last character. It is looked up there
  // rather than in the range being drawn, because while a fade runs the caret is
  // drawn at the frontier, which a partial redraw can reach without the
  // placeholder's own line being part of it.
  const NSRange placeholder = NSMakeRange(storage.length - 1, 1);
  NSNumber *kind = [storage attribute:DSCaretKindAttributeName atIndex:placeholder.location effectiveRange:NULL];
  if (kind == nil) {
    return;
  }
  // While a fade runs the caret sits at the reveal frontier, where the first unit
  // that has not started begins, so it never trails a gap of invisible words.
  NSUInteger frontier = placeholder.location;
  id<DSRevealProvider> provider = self.revealProvider;
  if (provider != nil) {
    const NSUInteger hidden = [provider revealFrontierForCharacterRange:NSMakeRange(0, placeholder.location)
                                                              blockOffset:self.blockOffset
                                                              blockRanges:self.blockRanges
                                                                       now:CACurrentMediaTime()];
    if (hidden != NSNotFound) {
      frontier = hidden;
    }
  }
  const NSUInteger glyph = [self glyphIndexForCharacterAtIndex:frontier];
  if (glyph >= self.numberOfGlyphs) {
    return;
  }
  const CGRect fragment = [self lineFragmentRectForGlyphAtIndex:glyph effectiveRange:NULL];
  if (CGRectIsEmpty(fragment)) {
    return;
  }
  // The glyph's own position in its fragment: x along the line, y the baseline.
  const CGPoint location = [self locationForGlyphAtIndex:glyph];

  NSTextAttachment *attachment = [storage attribute:NSAttachmentAttributeName
                                            atIndex:placeholder.location
                                     effectiveRange:NULL];
  UIColor *color = [storage attribute:NSForegroundColorAttributeName
                               atIndex:placeholder.location
                        effectiveRange:NULL];
  if (attachment == nil || color == nil) {
    return;
  }
  const CGRect bounds = attachment.bounds;
  // The size is the placeholder's, which is why the placeholder is in the measured
  // text: the caret never changes a line's height. Its bounds are measured up from
  // the baseline and this context is flipped, so the shape's top is the baseline
  // less its top edge.
  const CGPoint at = CGPointMake(origin.x + CGRectGetMinX(fragment) + location.x + bounds.origin.x,
                                 origin.y + CGRectGetMinY(fragment) + location.y -
                                     (bounds.origin.y + bounds.size.height));
  [self drawCaretShape:kind.integerValue bounds:bounds at:at color:color];
}

/// The two shapes, in the proportions of the characters Streamdown types.
///
/// A block is a rounded bar from just above the cap height to halfway into the
/// descender, and a circle is a dot centred on the x-height. Both are drawn from
/// the placeholder's own bounds, so they scale with the font and never change a
/// line's height.
- (void)drawCaretShape:(NSInteger)kind bounds:(CGRect)bounds at:(CGPoint)at color:(UIColor *)color {
  CGContextRef context = UIGraphicsGetCurrentContext();
  if (context == nullptr) {
    return;
  }
  CGContextSaveGState(context);
  [color setFill];
  const CGFloat width = bounds.size.width;
  const CGFloat height = bounds.size.height;
  switch (static_cast<downstream::CaretKind>(kind)) {
    case downstream::CaretKind::Block: {
      const CGFloat radius = std::min(width / 2, static_cast<CGFloat>(2));
      UIBezierPath *bar = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(at.x, at.y, width, height)
                                                     cornerRadius:radius];
      [bar fill];
      break;
    }
    case downstream::CaretKind::Circle: {
      // The dot's diameter is a little wider than the bar, and it sits on the
      // middle of the x-height rather than filling the whole placeholder.
      const CGFloat diameter = std::max(width, height * 0.5);
      const CGRect dot = CGRectMake(at.x + (width - diameter) / 2, at.y + height * 0.28, diameter, diameter);
      [[UIBezierPath bezierPathWithOvalInRect:dot] fill];
      break;
    }
    default:
      break;
  }
  CGContextRestoreGState(context);
}

@end