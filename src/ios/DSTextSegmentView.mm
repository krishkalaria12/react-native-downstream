#import "DSSegmentViews.h"

#import "DSLayoutManager.h"
#import "DSMathFonts.h"

namespace {

NSString *const kPlaceholder = @"\u200b";

} // namespace

#pragma mark - Text segments

@implementation DSTextSegmentView {
  DSTextKitStack *_stack;
  DSTextSegment *_segment;
  NSArray *_cachedAccessibilityElements;
  /// The document block this view's first character is. A reveal unit names a
  /// block and an offset within it, and this is what turns that into a range in
  /// this view's own text.
  NSInteger _blockOffset;
}

- (instancetype)init {
  DSLayoutManager *layoutManager = [DSLayoutManager new];
  DSTextKitStack *stack = [[DSTextKitStack alloc] initWithLayoutManager:layoutManager];
  if (self = [super initWithFrame:CGRectZero textContainer:stack.textContainer]) {
    _stack = stack;
    stack.textContainer.widthTracksTextView = YES;
    self.editable = NO;
    self.selectable = NO;
    self.scrollEnabled = NO;
    self.backgroundColor = UIColor.clearColor;
    self.textContainerInset = UIEdgeInsetsZero;
    self.dataDetectorTypes = UIDataDetectorTypeNone;
    self.isAccessibilityElement = NO;
    self.textDragInteraction.enabled = NO;
    // The tap recognizer sits ahead of the text view's own, so a press on a
    // summary is a container press rather than the start of a selection. It fails
    // as soon as the touch is anywhere else, which is every other touch in a
    // document without a `<details>` in it.
    UITapGestureRecognizer *detailsTap =
        [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(handleDetailsTap:)];
    detailsTap.delegate = (id<UIGestureRecognizerDelegate>)self;
    detailsTap.cancelsTouchesInView = YES;
    [self addGestureRecognizer:detailsTap];
  }
  return self;
}

- (void)setSegment:(DSTextSegment *)segment {
  if (segment == _segment) {
    return;
  }
  _segment = segment;
  ((DSLayoutManager *)self.layoutManager).blockRanges = segment.blockRanges;
  [self applyDetailsState];
  [_stack.textStorage setAttributedString:segment.attributedString];
  self.selectedRange = NSMakeRange(0, 0);
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
}

/// The document block index this view's text starts at, which is what maps a
/// reveal unit's block-and-offset pair into this view's own coordinates.
- (void)setBlockOffset:(NSInteger)blockOffset {
  _blockOffset = blockOffset;
  ((DSLayoutManager *)self.layoutManager).blockOffset = blockOffset;
}

- (void)handleDetailsTap:(UITapGestureRecognizer *)recognizer {
  if (recognizer.state != UIGestureRecognizerStateEnded) {
    return;
  }
  const uint32_t container = [self detailsContainerAtPoint:[recognizer locationInView:self]];
  if (container == 0) {
    return;
  }
  [self.controlDelegate didToggleDetailsAtContainer:container
                                               open:![self isOpenForContainer:container]];
}

/// The tap that opens a body only ever begins on one, so the text view's own
/// gesture recognizers are left alone everywhere else.
- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer *)gestureRecognizer {
  if (![gestureRecognizer.view isKindOfClass:UITextView.class]) {
    return YES;
  }
  return [self detailsContainerAtPoint:gestureRecognizer.locationInView:self] == 0;
}

/// The layout manager is what splits the glyph runs, so the provider is its as
/// well as this view's.
- (void)setRevealProvider:(id<DSRevealProvider>)revealProvider {
  if (_revealProvider == revealProvider) {
    return;
  }
  _revealProvider = revealProvider;
  ((DSLayoutManager *)self.layoutManager).revealProvider = revealProvider;
  [self setNeedsDisplay];
}

- (void)setDetailsToggle:(BOOL)toggle {
  if (toggle == _detailsToggle) {
    return;
  }
  _detailsToggle = toggle;
  [self applyDetailsState];
}

- (void)setDetailsChoices:(downstream::ReaderChoices)choices {
  if (choices == _detailsChoices) {
    return;
  }
  _detailsChoices = choices;
  [self applyDetailsState];
}

/// Hands the chevron's two inputs to the layout manager, which is what draws the
/// decoration beside the text rather than this view drawing over it.
- (void)applyDetailsState {
  DSLayoutManager *layout = (DSLayoutManager *)self.layoutManager;
  [layout setDetailsToggle:_detailsToggle];
  [layout setDetailsChoices:_detailsChoices];
  [self setNeedsDisplay];
}

/**
 * The `<details>` container a touch lands on, or 0.
 *
 * The whole summary line is the control, because a reader aiming at a disclosure
 * aims at the words next to it and a hit test that needed the glyph to be exactly
 * right would miss more often than it hit. A link inside a summary is still a
 * summary: opening the body is what a press on that line means.
 */
- (uint32_t)detailsContainerAtPoint:(CGPoint)point {
  if (!_detailsToggle || _segment == nil) {
    return 0;
  }
  const NSUInteger at = [self layoutManager glyphIndexForPoint:point inTextContainer:self.textContainer];
  if (at >= self.textStorage.length) {
    return 0;
  }
  NSRange blockRange;
  DSBlockInfo *info = [self.textStorage attribute:DSBlockInfoAttributeName
                                          atIndex:at
                            longestEffectiveRange:&blockRange
                                          inRange:NSMakeRange(0, self.textStorage.length)];
  if (info == nil) {
    return 0;
  }
  for (DSContainerInfo *frame in info.containers) {
    if (frame.kind == DSContainerKindDetails && frame.summary) {
      return frame.containerId;
    }
  }
  return 0;
}

/// This view's own block ranges, which the reveal provider maps units through.
/// This view's block ranges, which the document view counts blocks with.
- (NSArray<NSValue *> *)blockRanges {
  return _segment.blockRanges;
}

/// Redraws one block's characters, which is what a frame of animation asks for.
///
/// The range is in the block's own text, and this view knows where the block
/// starts in its own, so the translation is one addition. TextKit then redraws
/// only the line fragments that range touches, with no text storage edit, no glyph
/// invalidation, and no layout.
- (void)invalidateRevealCharacterRange:(NSRange)range
                            blockOffset:(NSInteger)blockOffset
                                     now:(double)now {
  (void)now;
  // `blockOffset` is the unit's document block, which can be any block this view
  // holds, not only its first.
  if (blockOffset < _blockOffset || _segment == nil || range.length == 0) {
    return;
  }
  const NSUInteger index = (NSUInteger)(blockOffset - _blockOffset);
  if (index >= _segment.blockRanges.count) {
    return;
  }
  const NSRange base = _segment.blockRanges[index].rangeValue;
  const NSRange characters = NSMakeRange(base.location + range.location, range.length);
  if (NSMaxRange(characters) > _stack.textStorage.length) {
    return;
  }
  // The background and the glyphs are separate passes over the same range, so an
  // inline-code background that outlived its glyph would leave a rectangle behind.
  [self.layoutManager invalidateDisplayForCharacterRange:characters];
}

- (void)setLinkDelegate:(id<DSLinkDelegate>)linkDelegate {
  _linkDelegate = linkDelegate;
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
}

/// Whether a container's body is showing, as the chevron drew it.
///
/// The same rule the chevron used, read back from the same frames, so a summary
/// cannot read as expanded while its glyph points the other way.
- (BOOL)isOpenForContainer:(uint32_t)containerId {
  if (_segment == nil) {
    return NO;
  }
  for (DSBlockInfo *info in _segment.blocks) {
    for (DSContainerInfo *frame in info.containers) {
      if (frame.kind == DSContainerKindDetails && frame.containerId == containerId) {
        return downstream::resolveDetails(_detailsChoices, containerId, frame.writtenOpen).open;
      }
    }
  }
  return NO;
}

/// UITextView draws its selection in the view's tint color, which is the only
/// handle on it that a deployment target still declares: `selectionColor` was
/// withdrawn from the headers in favour of the iOS 18 highlight attributes,
/// which color a text selection's own background rather than the selection
/// itself. Nothing else here is tinted, because this view draws its text, its
/// links and its own chrome with explicit colors.
- (void)setSelectionColor:(UIColor *)selectionColor {
  _selectionColor = selectionColor;
  self.tintColor = selectionColor;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  if ([self.traitCollection hasDifferentColorAppearanceComparedToTraitCollection:previousTraitCollection]) {
    [self setNeedsDisplay];
  }
}

// Copy plain visible text; internal attributes and placeholders stay inside.
- (void)copy:(id)sender {
  const NSRange range = self.selectedRange;
  if (range.length == 0) {
    return;
  }
  // An inline formula is one attachment character carrying its literal, and the
  // literal is what a reader copying the sentence expects.
  NSMutableString *text = [NSMutableString string];
  [_stack.textStorage enumerateAttribute:DSMathLiteralAttributeName
                                 inRange:range
                                 options:0
                              usingBlock:^(NSString *literal, NSRange part, BOOL *stop) {
                                [text appendString:literal ?: [self->_stack.textStorage.string substringWithRange:part]];
                              }];
  UIPasteboard.generalPasteboard.string = [text stringByReplacingOccurrencesOfString:kPlaceholder withString:@""];
}

- (NSInteger)linkIndexAtPoint:(CGPoint)point {
  const UIEdgeInsets inset = self.textContainerInset;
  return linkIndexInStack(self.layoutManager, self.textContainer,
                          CGPointMake(point.x - inset.left, point.y - inset.top));
}

- (NSArray *)accessibilityElements {
  if (_cachedAccessibilityElements == nil) {
    _cachedAccessibilityElements = [self buildAccessibilityElements];
  }
  return _cachedAccessibilityElements;
}

- (NSArray *)buildAccessibilityElements {
  if (_segment == nil) {
    return @[];
  }
  DSLayoutManager *layoutManager = (DSLayoutManager *)self.layoutManager;
  [layoutManager ensureLayoutForTextContainer:self.textContainer];
  NSTextStorage *storage = _stack.textStorage;
  const CGFloat top = self.textContainerInset.top;
  NSMutableArray *elements = [NSMutableArray arrayWithCapacity:_segment.blocks.count];

  [_segment.blocks enumerateObjectsUsingBlock:^(DSBlockInfo *info, NSUInteger index, BOOL *stop) {
    const NSRange range = self->_segment.blockRanges[index].rangeValue;
    DSBlockTextBounds bounds;
    if (info.decoration == DSBlockDecorationRule || ![layoutManager textBoundsForCharacterRange:range bounds:&bounds]) {
      return;
    }
    const CGRect blockFrame = CGRectMake(0, top + bounds.top, self.bounds.size.width, bounds.bottom - bounds.top);
    if (hasLink(storage, range)) {
      appendAccessibleSlices(elements, storage, range, self, layoutManager, self.textContainer,
                             CGPointMake(self.textContainerInset.left, top), blockFrame,
                             info.accessibilityPrefix,
                             info.headingLevel > 0 ? UIAccessibilityTraitHeader : 0, self.linkDelegate);
    } else {
      NSString *text = [storage.string substringWithRange:range];
      UIAccessibilityElement *element = [[UIAccessibilityElement alloc] initWithAccessibilityContainer:self];
      element.accessibilityLabel = info.accessibilityPrefix != nil
          ? [NSString stringWithFormat:@"%@, %@", info.accessibilityPrefix, text] : text;
      element.accessibilityFrameInContainerSpace = blockFrame;
      element.accessibilityTraits = info.headingLevel > 0 ? UIAccessibilityTraitHeader : UIAccessibilityTraitStaticText;
      [elements addObject:element];
    }
  }];
  return elements;
}

@end
