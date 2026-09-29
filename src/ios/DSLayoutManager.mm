#import "DSLayoutManager.h"

#import "DSRenderedDocument.h"

namespace {

constexpr CGFloat kBarWidth = 3;
constexpr CGFloat kMarkerGap = 6;
constexpr CGFloat kCodeCornerRadius = 6;

} // namespace

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

- (instancetype)init {
  if (self = [super init]) {
    _drawsCodeBackgrounds = YES;
  }
  return self;
}

- (void)drawCodeBackground:(DSBlockInfo *)info
                       top:(CGFloat)top
                    bottom:(CGFloat)bottom
                      left:(CGFloat)left
                     width:(CGFloat)width {
  const CGRect rect = CGRectMake(left, top - info.codePadding, width - info.contentLeading,
                                 bottom - top + 2 * info.codePadding);
  [UIColor.tertiarySystemFillColor setFill];
  [[UIBezierPath bezierPathWithRoundedRect:rect cornerRadius:kCodeCornerRadius] fill];
}

- (void)drawCodeBackgroundsAtPoint:(CGPoint)origin {
  NSTextStorage *storage = self.textStorage;
  NSTextContainer *container = self.textContainers.firstObject;
  if (storage.length == 0 || container == nil) {
    return;
  }
  [storage enumerateAttribute:DSBlockInfoAttributeName
                      inRange:NSMakeRange(0, storage.length)
                      options:0
                   usingBlock:^(DSBlockInfo *info, NSRange range, BOOL *stop) {
                     DSBlockTextBounds bounds;
                     if (info.decoration != DSBlockDecorationCode ||
                         ![self textBoundsForCharacterRange:range bounds:&bounds]) {
                       return;
                     }
                     [self drawCodeBackground:info
                                          top:origin.y + bounds.top
                                       bottom:origin.y + bounds.bottom
                                         left:origin.x + (info.rightToLeft ? 0 : info.contentLeading)
                                        width:container.size.width];
                   }];
}

- (void)drawBackgroundForGlyphRange:(NSRange)glyphsToShow atPoint:(CGPoint)origin {
  [super drawBackgroundForGlyphRange:glyphsToShow atPoint:origin];

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
    DSBlockTextBounds bounds;
    if (info == nil || ![self textBoundsForCharacterRange:blockRange bounds:&bounds]) {
      continue;
    }
    const CGFloat top = origin.y + bounds.top;
    const CGFloat bottom = origin.y + bounds.bottom;

    for (DSContainerInfo *containerInfo in info.containers) {
      if (containerInfo.barColor != nil) {
        const CGFloat barTop = containerInfo.continuesPrevious ? top - info.spacingBefore - 1 : top;
        [containerInfo.barColor setFill];
        const CGFloat x = info.rightToLeft
            ? width - containerInfo.leading - 2 - kBarWidth
            : containerInfo.leading + 2;
        UIRectFill(CGRectMake(origin.x + x, barTop, kBarWidth, bottom - barTop));
      }
      if (containerInfo.marker != nil) {
        UIFont *markerFont = [containerInfo.marker attribute:NSFontAttributeName atIndex:0 effectiveRange:NULL];
        const CGSize markerSize = containerInfo.marker.size;
        const CGFloat x = info.rightToLeft
            ? width - containerInfo.leading - containerInfo.width + kMarkerGap
            : containerInfo.leading + containerInfo.width - kMarkerGap - markerSize.width;
        const CGFloat y = bounds.firstBaseline - markerFont.ascender;
        [containerInfo.marker drawAtPoint:CGPointMake(origin.x + x, origin.y + y)];
      }
    }

    const CGFloat left = origin.x + (info.rightToLeft ? 0 : info.contentLeading);
    switch (info.decoration) {
      case DSBlockDecorationCode:
        if (self.drawsCodeBackgrounds) {
          [self drawCodeBackground:info top:top bottom:bottom left:left width:width];
        }
        break;
      case DSBlockDecorationRule:
        [UIColor.separatorColor setFill];
        UIRectFill(CGRectMake(left, round((top + bottom) / 2), width - info.contentLeading, 1));
        break;
      case DSBlockDecorationNone:
        break;
    }
  }
}

@end
