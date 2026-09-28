#import "DSMarkdownTextView.h"

#import "DSLayoutManager.h"
#import "DSRenderedDocument.h"

namespace {

constexpr CGFloat kLinkHitSlop = 4;

} // namespace

@implementation DSMarkdownTextView {
  downstream::DocumentHandle _document;
  CGFloat _fontScale;
  DSRenderedDocument *_rendered;
  DSTextKitStack *_stack;
  NSArray *_cachedAccessibilityElements;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    self.opaque = NO;
    self.backgroundColor = UIColor.clearColor;
    self.contentMode = UIViewContentModeRedraw;
    self.isAccessibilityElement = NO;
    _fontScale = 1;
    _stack = [[DSTextKitStack alloc] initWithLayoutManager:[DSLayoutManager new]];
  }
  return self;
}

- (void)setDocument:(downstream::DocumentHandle)document fontScale:(CGFloat)fontScale {
  if (document == _document && fontScale == _fontScale && _rendered != nil) {
    return;
  }
  _document = std::move(document);
  _fontScale = fontScale;
  _rendered = _document ? [DSRenderedDocument renderedDocumentFor:_document fontScale:fontScale] : nil;
  [_stack.textStorage setAttributedString:_rendered.attributedString ?: [NSAttributedString new]];
  _cachedAccessibilityElements = nil;
  [self setNeedsDisplay];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const CGFloat width = self.bounds.size.width;
  if (_stack.textContainer.size.width != width) {
    _stack.textContainer.size = CGSizeMake(width, CGFLOAT_MAX);
    _cachedAccessibilityElements = nil;
    [self setNeedsDisplay];
  }
}

- (CGPoint)textOrigin {
  return CGPointMake(0, _rendered.topInset);
}

- (void)drawRect:(CGRect)rect {
  if (_rendered == nil || _stack.textStorage.length == 0) {
    return;
  }
  const CGPoint origin = [self textOrigin];
  NSLayoutManager *layoutManager = _stack.layoutManager;
  const NSRange glyphs =
      [layoutManager glyphRangeForBoundingRect:CGRectOffset(rect, -origin.x, -origin.y)
                               inTextContainer:_stack.textContainer];
  [layoutManager drawBackgroundForGlyphRange:glyphs atPoint:origin];
  [layoutManager drawGlyphsForGlyphRange:glyphs atPoint:origin];
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if ([self.traitCollection hasDifferentColorAppearanceComparedToTraitCollection:previousTraitCollection]) {
    [self setNeedsDisplay];
  }
}

- (NSInteger)linkIndexAtPoint:(CGPoint)point {
  if (_rendered == nil || _stack.textStorage.length == 0) {
    return -1;
  }
  const CGPoint origin = [self textOrigin];
  const CGPoint location = CGPointMake(point.x - origin.x, point.y - origin.y);
  NSLayoutManager *layoutManager = _stack.layoutManager;
  const NSUInteger glyph = [layoutManager glyphIndexForPoint:location
                                             inTextContainer:_stack.textContainer
                              fractionOfDistanceThroughGlyph:NULL];
  if (glyph >= layoutManager.numberOfGlyphs) {
    return -1;
  }
  const CGRect glyphRect = [layoutManager boundingRectForGlyphRange:NSMakeRange(glyph, 1)
                                                    inTextContainer:_stack.textContainer];
  if (!CGRectContainsPoint(CGRectInset(glyphRect, -kLinkHitSlop, -kLinkHitSlop), location)) {
    return -1;
  }
  const NSUInteger character = [layoutManager characterIndexForGlyphAtIndex:glyph];
  NSNumber *link = [_stack.textStorage attribute:DSLinkIndexAttributeName atIndex:character effectiveRange:NULL];
  return link != nil ? link.integerValue : -1;
}

#pragma mark - Accessibility

- (NSArray *)accessibilityElements {
  if (_cachedAccessibilityElements == nil) {
    _cachedAccessibilityElements = [self buildAccessibilityElements];
  }
  return _cachedAccessibilityElements;
}

- (NSArray *)buildAccessibilityElements {
  if (_rendered == nil) {
    return @[];
  }
  DSLayoutManager *layoutManager = (DSLayoutManager *)_stack.layoutManager;
  [layoutManager ensureLayoutForTextContainer:_stack.textContainer];
  NSTextStorage *storage = _stack.textStorage;
  const CGPoint origin = [self textOrigin];
  NSMutableArray *elements = [NSMutableArray arrayWithCapacity:_rendered.blocks.count];

  [_rendered.blocks enumerateObjectsUsingBlock:^(DSBlockInfo *info, NSUInteger index, BOOL *stop) {
    const NSRange range = self->_rendered.blockRanges[index].rangeValue;
    DSBlockTextBounds bounds;
    if (info.decoration == DSBlockDecorationRule || ![layoutManager textBoundsForCharacterRange:range bounds:&bounds]) {
      return;
    }

    NSString *text = [[storage.string substringWithRange:range] stringByReplacingOccurrencesOfString:@"\t"
                                                                                           withString:@", "];
    UIAccessibilityElement *element = [[UIAccessibilityElement alloc] initWithAccessibilityContainer:self];
    element.accessibilityLabel =
        info.accessibilityPrefix != nil ? [NSString stringWithFormat:@"%@, %@", info.accessibilityPrefix, text] : text;
    element.accessibilityFrameInContainerSpace =
        CGRectMake(origin.x, origin.y + bounds.top, self.bounds.size.width, bounds.bottom - bounds.top);
    element.accessibilityTraits = info.headingLevel > 0 ? UIAccessibilityTraitHeader : UIAccessibilityTraitStaticText;

    NSMutableArray<UIAccessibilityCustomAction *> *actions = [NSMutableArray new];
    __weak DSMarkdownTextView *weakSelf = self;
    [storage enumerateAttribute:DSLinkIndexAttributeName
                        inRange:range
                        options:0
                     usingBlock:^(NSNumber *link, NSRange linkRange, BOOL *innerStop) {
                       if (link == nil) {
                         return;
                       }
                       NSString *name = [NSString
                           stringWithFormat:@"Link: %@", [storage.string substringWithRange:linkRange]];
                       const NSInteger linkIndex = link.integerValue;
                       [actions addObject:[[UIAccessibilityCustomAction alloc]
                                                   initWithName:name
                                                  actionHandler:^BOOL(UIAccessibilityCustomAction *action) {
                                                    DSMarkdownTextView *strongSelf = weakSelf;
                                                    [strongSelf.delegate markdownTextView:strongSelf
                                                                      didPressLinkAtIndex:linkIndex];
                                                    return strongSelf != nil;
                                                  }]];
                     }];
    if (actions.count > 0) {
      element.accessibilityCustomActions = actions;
      element.accessibilityHint = @"Links available in the actions rotor.";
    }
    [elements addObject:element];
  }];
  return elements;
}

@end
