#import "DSSegmentViews.h"

#import "DSLayoutManager.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr CGFloat kLinkHitSlop = 4;
constexpr CGFloat kBarWidth = 3;
constexpr CGFloat kMarkerGap = 6;

NSString *const kPlaceholder = @"\u200b";

BOOL hasLink(NSAttributedString *text, NSRange range) {
  __block BOOL found = NO;
  [text enumerateAttribute:DSLinkIndexAttributeName
                   inRange:range
                   options:0
                usingBlock:^(NSNumber *link, NSRange effectiveRange, BOOL *stop) {
                  if (link != nil) {
                    found = YES;
                    *stop = YES;
                  }
                }];
  return found;
}

/// Link index under a point in a laid-out TextKit stack, or -1.
NSInteger linkIndexInStack(NSLayoutManager *layoutManager, NSTextContainer *container, CGPoint location) {
  if (layoutManager.numberOfGlyphs == 0) {
    return -1;
  }
  const NSUInteger glyph = [layoutManager glyphIndexForPoint:location
                                             inTextContainer:container
                              fractionOfDistanceThroughGlyph:NULL];
  if (glyph >= layoutManager.numberOfGlyphs) {
    return -1;
  }
  const CGRect glyphRect = [layoutManager boundingRectForGlyphRange:NSMakeRange(glyph, 1) inTextContainer:container];
  if (!CGRectContainsPoint(CGRectInset(glyphRect, -kLinkHitSlop, -kLinkHitSlop), location)) {
    return -1;
  }
  const NSUInteger character = [layoutManager characterIndexForGlyphAtIndex:glyph];
  NSNumber *link = [layoutManager.textStorage attribute:DSLinkIndexAttributeName atIndex:character effectiveRange:NULL];
  return link != nil ? link.integerValue : -1;
}

} // namespace

@interface DSLinkAccessibilityElement : UIAccessibilityElement
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
@property (nonatomic) NSInteger linkIndex;
@end

@implementation DSLinkAccessibilityElement

- (BOOL)accessibilityActivate {
  id<DSLinkDelegate> delegate = self.linkDelegate;
  if (delegate == nil) {
    return NO;
  }
  [delegate didPressLinkAtIndex:self.linkIndex];
  return YES;
}

@end

namespace {

void invalidateAccessibleLinks(NSArray<UIAccessibilityElement *> *elements) {
  for (UIAccessibilityElement *element in elements) {
    if ([element isKindOfClass:DSLinkAccessibilityElement.class]) {
      ((DSLinkAccessibilityElement *)element).linkDelegate = nil;
    }
  }
}

/// Split only ranges containing links. Each link gets a focusable element in
/// source order; plain text remains readable without repeating link labels.
void appendAccessibleSlices(NSMutableArray<UIAccessibilityElement *> *elements, NSAttributedString *text,
                            NSRange range, UIView *container, NSLayoutManager *layoutManager,
                            NSTextContainer *textContainer, CGPoint origin, CGRect fallbackFrame,
                            NSString *prefix, UIAccessibilityTraits firstTraits,
                            id<DSLinkDelegate> delegate) {
  NSUInteger cursor = range.location;
  const NSUInteger end = NSMaxRange(range);
  BOOL first = YES;
  while (cursor < end) {
    NSRange slice;
    NSNumber *link = [text attribute:DSLinkIndexAttributeName
                           atIndex:cursor
             longestEffectiveRange:&slice
                           inRange:range];
    if (slice.length == 0) {
      break;
    }
    UIAccessibilityElement *element;
    if (link != nil) {
      DSLinkAccessibilityElement *linkElement =
          [[DSLinkAccessibilityElement alloc] initWithAccessibilityContainer:container];
      linkElement.linkDelegate = delegate;
      linkElement.linkIndex = link.integerValue;
      element = linkElement;
    } else {
      element = [[UIAccessibilityElement alloc] initWithAccessibilityContainer:container];
    }
    NSString *label = [text.string substringWithRange:slice];
    if (first && prefix.length > 0) {
      label = [NSString stringWithFormat:@"%@, %@", prefix, label];
    }
    element.accessibilityLabel = label;
    element.accessibilityTraits = (link != nil ? UIAccessibilityTraitLink : UIAccessibilityTraitStaticText) |
        (first ? firstTraits : 0);
    const NSRange glyphs = [layoutManager glyphRangeForCharacterRange:slice actualCharacterRange:NULL];
    const CGRect glyphFrame = [layoutManager boundingRectForGlyphRange:glyphs inTextContainer:textContainer];
    element.accessibilityFrameInContainerSpace = CGRectIsEmpty(glyphFrame)
        ? fallbackFrame
        : CGRectOffset(glyphFrame, origin.x, origin.y);
    [elements addObject:element];
    first = NO;
    cursor = NSMaxRange(slice);
  }
}

} // namespace

#pragma mark - Text segments

/// Draws code block backgrounds behind a UITextView's text. UITextView clips
/// its text drawing to the text container, which excludes the insets that
/// hold a code block's top and bottom padding.
@interface DSCodeBackgroundView : UIView
@property (nonatomic, weak) UITextView *textView;
@end

@implementation DSCodeBackgroundView

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    self.opaque = NO;
    self.backgroundColor = UIColor.clearColor;
    self.userInteractionEnabled = NO;
    self.contentMode = UIViewContentModeRedraw;
  }
  return self;
}

- (void)drawRect:(CGRect)rect {
  UITextView *textView = self.textView;
  const UIEdgeInsets inset = textView.textContainerInset;
  [(DSLayoutManager *)textView.layoutManager drawCodeBackgroundsAtPoint:CGPointMake(inset.left, inset.top)];
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  [self setNeedsDisplay];
}

@end

@implementation DSTextSegmentView {
  DSTextKitStack *_stack;
  DSTextSegment *_segment;
  DSCodeBackgroundView *_codeBackground;
  NSArray *_cachedAccessibilityElements;
}

- (instancetype)init {
  DSLayoutManager *layoutManager = [DSLayoutManager new];
  layoutManager.drawsCodeBackgrounds = NO;
  DSTextKitStack *stack = [[DSTextKitStack alloc] initWithLayoutManager:layoutManager];
  if (self = [super initWithFrame:CGRectZero textContainer:stack.textContainer]) {
    _stack = stack;
    _codeBackground = [DSCodeBackgroundView new];
    _codeBackground.textView = self;
    [self insertSubview:_codeBackground atIndex:0];
    stack.textContainer.widthTracksTextView = YES;
    self.editable = NO;
    self.selectable = NO;
    self.scrollEnabled = NO;
    self.backgroundColor = UIColor.clearColor;
    self.textContainerInset = UIEdgeInsetsZero;
    self.dataDetectorTypes = UIDataDetectorTypeNone;
    self.isAccessibilityElement = NO;
    self.textDragInteraction.enabled = NO;
  }
  return self;
}

- (void)setSegment:(DSTextSegment *)segment {
  if (segment == _segment) {
    return;
  }
  _segment = segment;
  self.textContainerInset = UIEdgeInsetsMake(segment.topInset, 0, segment.bottomInset, 0);
  [_stack.textStorage setAttributedString:segment.attributedString];
  self.selectedRange = NSMakeRange(0, 0);
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
  [_codeBackground setNeedsDisplay];
}

- (void)setLinkDelegate:(id<DSLinkDelegate>)linkDelegate {
  _linkDelegate = linkDelegate;
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
}

- (void)layoutSubviews {
  [super layoutSubviews];
  if (!CGRectEqualToRect(_codeBackground.frame, self.bounds)) {
    _codeBackground.frame = self.bounds;
    [_codeBackground setNeedsDisplay];
  }
  [self sendSubviewToBack:_codeBackground];
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
  NSString *text = [[_stack.textStorage.string substringWithRange:range] stringByReplacingOccurrencesOfString:kPlaceholder
                                                                                                   withString:@""];
  UIPasteboard.generalPasteboard.string = text;
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

#pragma mark - Tables

@interface DSTableGridView : UIView
@property (nonatomic, nullable) DSTableSegment *segment;
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
- (NSInteger)linkIndexAtPoint:(CGPoint)point;
@end

@implementation DSTableGridView {
  NSArray *_cachedAccessibilityElements;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    self.opaque = NO;
    self.backgroundColor = UIColor.clearColor;
    self.contentMode = UIViewContentModeRedraw;
  }
  return self;
}

- (void)setSegment:(DSTableSegment *)segment {
  _segment = segment;
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
  [self setNeedsDisplay];
}

- (void)setLinkDelegate:(id<DSLinkDelegate>)linkDelegate {
  _linkDelegate = linkDelegate;
  invalidateAccessibleLinks(_cachedAccessibilityElements);
  _cachedAccessibilityElements = nil;
}

- (CGRect)contentRectForRow:(NSUInteger)row column:(NSUInteger)column {
  CGFloat x = 0;
  if (_segment.rightToLeft) {
    for (NSUInteger i = column + 1; i < _segment.columnWidths.count; i++) {
      x += _segment.columnWidths[i].doubleValue;
    }
  } else {
    for (NSUInteger i = 0; i < column; i++) {
      x += _segment.columnWidths[i].doubleValue;
    }
  }
  CGFloat y = 0;
  for (NSUInteger i = 0; i < row; i++) {
    y += _segment.rowHeights[i].doubleValue;
  }
  const UIEdgeInsets padding = _segment.cellPadding;
  return CGRectMake(x + padding.left, y + padding.top,
                    _segment.columnWidths[column].doubleValue - padding.left - padding.right,
                    _segment.rowHeights[row].doubleValue - padding.top - padding.bottom);
}

- (void)drawRect:(CGRect)rect {
  if (_segment == nil) {
    return;
  }
  const CGSize size = _segment.tableSize;
  const CGFloat hairline = 1.0 / self.traitCollection.displayScale;
  const NSStringDrawingOptions options = NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingUsesFontLeading;

  if (_segment.hasHeader && _segment.rowHeights.count > 0) {
    [UIColor.tertiarySystemFillColor setFill];
    UIRectFill(CGRectMake(0, 0, size.width - 1, _segment.rowHeights[0].doubleValue));
  }

  [_segment.rows enumerateObjectsUsingBlock:^(NSArray<DSTableCell *> *row, NSUInteger rowIndex, BOOL *stop) {
    [row enumerateObjectsUsingBlock:^(DSTableCell *cell, NSUInteger column, BOOL *innerStop) {
      [cell.text drawWithRect:[self contentRectForRow:rowIndex column:column] options:options context:nil];
    }];
  }];

  [UIColor.separatorColor setFill];
  CGFloat y = 0;
  UIRectFill(CGRectMake(0, 0, size.width - 1, hairline));
  for (NSNumber *height in _segment.rowHeights) {
    y += height.doubleValue;
    UIRectFill(CGRectMake(0, y, size.width - 1, hairline));
  }
  CGFloat x = 0;
  UIRectFill(CGRectMake(_segment.rightToLeft ? size.width - 1 : 0, 0, hairline, y));
  for (NSNumber *width in _segment.columnWidths) {
    x += width.doubleValue;
    UIRectFill(CGRectMake(_segment.rightToLeft ? size.width - 1 - x : x, 0, hairline, y));
  }
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  [self setNeedsDisplay];
}

- (NSInteger)linkIndexAtPoint:(CGPoint)point {
  if (_segment == nil) {
    return -1;
  }
  for (NSUInteger row = 0; row < _segment.rows.count; row++) {
    for (NSUInteger column = 0; column < _segment.rows[row].count; column++) {
      const CGRect content = [self contentRectForRow:row column:column];
      if (!CGRectContainsPoint(CGRectInset(content, -kLinkHitSlop, -kLinkHitSlop), point)) {
        continue;
      }
      DSTextKitStack *stack = [[DSTextKitStack alloc] initWithLayoutManager:[NSLayoutManager new]];
      stack.textContainer.size = CGSizeMake(content.size.width, CGFLOAT_MAX);
      [stack.textStorage setAttributedString:_segment.rows[row][column].text];
      [stack.layoutManager ensureLayoutForTextContainer:stack.textContainer];
      return linkIndexInStack(stack.layoutManager, stack.textContainer,
                              CGPointMake(point.x - content.origin.x, point.y - content.origin.y));
    }
  }
  return -1;
}

- (BOOL)isAccessibilityElement {
  return NO;
}

- (NSArray *)accessibilityElements {
  if (_cachedAccessibilityElements != nil || _segment == nil) {
    return _cachedAccessibilityElements ?: @[];
  }
  NSMutableArray<UIAccessibilityElement *> *elements = [NSMutableArray arrayWithCapacity:_segment.rows.count];
  CGFloat y = 0;
  for (NSUInteger row = 0; row < _segment.rows.count; row++) {
    NSMutableArray<NSString *> *cells = [NSMutableArray new];
    BOOL rowHasLinks = NO;
    for (DSTableCell *cell in _segment.rows[row]) {
      [cells addObject:cell.text.string];
      rowHasLinks |= hasLink(cell.text, NSMakeRange(0, cell.text.length));
    }
    const CGFloat height = _segment.rowHeights[row].doubleValue;
    if (!rowHasLinks) {
      UIAccessibilityElement *element = [[UIAccessibilityElement alloc] initWithAccessibilityContainer:self];
      element.accessibilityLabel = [cells componentsJoinedByString:@", "];
      element.accessibilityFrameInContainerSpace = CGRectMake(0, y, _segment.tableSize.width, height);
      element.accessibilityTraits = row == 0 && _segment.hasHeader ? UIAccessibilityTraitHeader
                                                                   : UIAccessibilityTraitStaticText;
      [elements addObject:element];
    } else {
      const NSUInteger rowStart = elements.count;
      for (NSUInteger column = 0; column < _segment.rows[row].count; column++) {
        DSTableCell *cell = _segment.rows[row][column];
        if (cell.text.length == 0) {
          continue;
        }
        const CGRect content = [self contentRectForRow:row column:column];
        DSTextKitStack *stack = [[DSTextKitStack alloc] initWithLayoutManager:[NSLayoutManager new]];
        stack.textContainer.size = CGSizeMake(content.size.width, CGFLOAT_MAX);
        [stack.textStorage setAttributedString:cell.text];
        [stack.layoutManager ensureLayoutForTextContainer:stack.textContainer];
        const BOOL firstCell = elements.count == rowStart;
        NSString *prefix = firstCell ? [NSString stringWithFormat:@"Row %lu", (unsigned long)row + 1] : nil;
        appendAccessibleSlices(elements, cell.text, NSMakeRange(0, cell.text.length), self,
                               stack.layoutManager, stack.textContainer, content.origin, content, prefix,
                               firstCell && row == 0 && _segment.hasHeader ? UIAccessibilityTraitHeader : 0,
                               self.linkDelegate);
      }
    }
    y += height;
  }
  _cachedAccessibilityElements = elements;
  return elements;
}

@end

@implementation DSTableSegmentView {
  DSTableSegment *_segment;
  UIScrollView *_scrollView;
  DSTableGridView *_grid;
  BOOL _needsScrollRestore;
  BOOL _preserveScrollPosition;
  BOOL _forceScrollReset;
  CGFloat _savedScrollPosition;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    self.opaque = NO;
    self.backgroundColor = UIColor.clearColor;
    self.contentMode = UIViewContentModeRedraw;
    _scrollView = [UIScrollView new];
    _scrollView.showsVerticalScrollIndicator = NO;
    _scrollView.alwaysBounceVertical = NO;
    _scrollView.directionalLockEnabled = YES;
    _grid = [DSTableGridView new];
    [_scrollView addSubview:_grid];
    [self addSubview:_scrollView];
  }
  return self;
}

- (void)setLinkDelegate:(id<DSLinkDelegate>)linkDelegate {
  _linkDelegate = linkDelegate;
  _grid.linkDelegate = linkDelegate;
}

- (void)setSegment:(DSTableSegment *)segment {
  if (segment == _segment) {
    return;
  }
  _preserveScrollPosition = !_forceScrollReset && _segment != nil && segment != nil &&
      _segment.tableIdentifier == segment.tableIdentifier &&
      _segment.rightToLeft == segment.rightToLeft &&
      [_segment.firstRowText isEqualToString:segment.firstRowText];
  const CGFloat oldMaximum = std::max<CGFloat>(0, _scrollView.contentSize.width - _scrollView.bounds.size.width);
  _savedScrollPosition = _segment.rightToLeft
      ? oldMaximum - _scrollView.contentOffset.x
      : _scrollView.contentOffset.x;
  _needsScrollRestore = YES;
  _segment = segment;
  _grid.segment = segment;
  [self setNeedsLayout];
  [self setNeedsDisplay];
}

- (void)resetScrollPosition {
  _forceScrollReset = YES;
  _preserveScrollPosition = NO;
  _needsScrollRestore = YES;
  [self setNeedsLayout];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  if (_segment == nil) {
    return;
  }
  const CGFloat leading = _segment.contentLeading;
  _scrollView.frame = CGRectMake(_segment.rightToLeft ? 0 : leading, 0,
                                 std::max<CGFloat>(0, self.bounds.size.width - leading),
                                 self.bounds.size.height);
  _grid.frame = CGRectMake(0, 0, _segment.tableSize.width, _segment.tableSize.height);
  _scrollView.contentSize = _segment.tableSize;
  if (_needsScrollRestore) {
    const CGFloat maximum = std::max<CGFloat>(0, _segment.tableSize.width - _scrollView.bounds.size.width);
    const CGFloat position = _preserveScrollPosition ? _savedScrollPosition : 0;
    _scrollView.contentOffset = CGPointMake(_segment.rightToLeft
        ? std::clamp(maximum - position, 0.0, maximum)
        : std::clamp(position, 0.0, maximum), 0);
    _needsScrollRestore = NO;
    _forceScrollReset = NO;
  }
}

// Quote bars and the list marker live in the non-scrolling margin.
- (void)drawRect:(CGRect)rect {
  if (_segment == nil) {
    return;
  }
  for (DSContainerInfo *container in _segment.containers) {
    if (container.barColor != nil) {
      [container.barColor setFill];
      const CGFloat x = _segment.rightToLeft
          ? self.bounds.size.width - container.leading - 2 - kBarWidth
          : container.leading + 2;
      UIRectFill(CGRectMake(x, 0, kBarWidth, self.bounds.size.height));
    }
    if (container.marker != nil) {
      UIFont *font = [container.marker attribute:NSFontAttributeName atIndex:0 effectiveRange:NULL];
      const CGSize markerSize = container.marker.size;
      const CGFloat baseline = _segment.cellPadding.top + font.ascender;
      const CGFloat x = _segment.rightToLeft
          ? self.bounds.size.width - container.leading - container.width + kMarkerGap
          : container.leading + container.width - kMarkerGap - markerSize.width;
      [container.marker drawAtPoint:CGPointMake(x, baseline - font.ascender)];
    }
  }
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  [self setNeedsDisplay];
}

- (NSInteger)linkIndexAtPoint:(CGPoint)point {
  return [_grid linkIndexAtPoint:[self convertPoint:point toView:_grid]];
}

@end

#pragma mark - Document

@implementation DSDocumentView {
  DSRenderedDocument *_rendered;
  NSMutableArray<UIView<DSSegmentView> *> *_segmentViews;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    self.backgroundColor = UIColor.clearColor;
    _segmentViews = [NSMutableArray new];
  }
  return self;
}

- (void)setRendered:(DSRenderedDocument *)rendered {
  if (rendered == _rendered) {
    return;
  }
  _rendered = rendered;
  NSArray *segments = rendered.segments ?: @[];

  // Reuse views of the same kind in place, so unchanged segments keep their
  // selection and scroll position across updates.
  for (NSUInteger index = 0; index < segments.count; index++) {
    id segment = segments[index];
    const BOOL isText = [segment isKindOfClass:DSTextSegment.class];
    UIView<DSSegmentView> *view = index < _segmentViews.count ? _segmentViews[index] : nil;
    const BOOL reusable = view != nil && [view isKindOfClass:isText ? DSTextSegmentView.class : DSTableSegmentView.class];
    if (!reusable) {
      [view removeFromSuperview];
      if (isText) {
        DSTextSegmentView *textView = [DSTextSegmentView new];
        textView.selectable = _selectable;
        textView.linkDelegate = _linkDelegate;
        view = textView;
      } else {
        DSTableSegmentView *tableView = [DSTableSegmentView new];
        tableView.linkDelegate = _linkDelegate;
        view = tableView;
      }
      if (index < _segmentViews.count) {
        _segmentViews[index] = view;
      } else {
        [_segmentViews addObject:view];
      }
      [self addSubview:view];
    }
    if (isText) {
      [(DSTextSegmentView *)view setSegment:segment];
    } else {
      [(DSTableSegmentView *)view setSegment:segment];
    }
  }
  while (_segmentViews.count > segments.count) {
    [_segmentViews.lastObject removeFromSuperview];
    [_segmentViews removeLastObject];
  }
  [self setNeedsLayout];
}

- (void)setSelectable:(BOOL)selectable {
  _selectable = selectable;
  for (UIView *view in _segmentViews) {
    if ([view isKindOfClass:DSTextSegmentView.class]) {
      ((DSTextSegmentView *)view).selectable = selectable;
    }
  }
}

- (void)resetTableScrollPositions {
  for (UIView *view in _segmentViews) {
    if ([view isKindOfClass:DSTableSegmentView.class]) {
      [(DSTableSegmentView *)view resetScrollPosition];
    }
  }
}

- (void)setLinkDelegate:(id<DSLinkDelegate>)linkDelegate {
  _linkDelegate = linkDelegate;
  for (UIView *view in _segmentViews) {
    [(id)view setLinkDelegate:linkDelegate];
  }
}

- (void)layoutSubviews {
  [super layoutSubviews];
  if (_rendered == nil) {
    return;
  }
  NSArray<NSValue *> *frames = [_rendered segmentFramesForWidth:self.bounds.size.width];
  for (NSUInteger index = 0; index < _segmentViews.count && index < frames.count; index++) {
    _segmentViews[index].frame = frames[index].CGRectValue;
  }
}

- (NSInteger)linkIndexAtPoint:(CGPoint)point {
  for (UIView<DSSegmentView> *view in _segmentViews) {
    if (CGRectContainsPoint(view.frame, point)) {
      return [view linkIndexAtPoint:[self convertPoint:point toView:view]];
    }
  }
  return -1;
}

@end
