#import "DSSegmentViews.h"

#import "DSLayoutManager.h"

#include <cmath>

namespace {

constexpr CGFloat kLinkHitSlop = 4;
constexpr CGFloat kBarWidth = 3;
constexpr CGFloat kMarkerGap = 6;

NSString *const kPlaceholder = @"\u200b";

/// VoiceOver actions for the links in a range, labeled with their text.
NSArray<UIAccessibilityCustomAction *> *linkActions(NSAttributedString *text, NSRange range,
                                                    id<DSLinkDelegate> delegate) {
  NSMutableArray<UIAccessibilityCustomAction *> *actions = [NSMutableArray new];
  __weak id<DSLinkDelegate> weakDelegate = delegate;
  [text enumerateAttribute:DSLinkIndexAttributeName
                   inRange:range
                   options:0
                usingBlock:^(NSNumber *link, NSRange linkRange, BOOL *stop) {
                  if (link == nil) {
                    return;
                  }
                  const NSInteger index = link.integerValue;
                  NSString *name =
                      [NSString stringWithFormat:@"Link: %@", [text.string substringWithRange:linkRange]];
                  [actions addObject:[[UIAccessibilityCustomAction alloc]
                                          initWithName:name
                                         actionHandler:^BOOL(UIAccessibilityCustomAction *action) {
                                           id<DSLinkDelegate> strongDelegate = weakDelegate;
                                           [strongDelegate didPressLinkAtIndex:index];
                                           return strongDelegate != nil;
                                         }]];
                }];
  return actions;
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
  _cachedAccessibilityElements = nil;
  [_codeBackground setNeedsDisplay];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  if (!CGRectEqualToRect(_codeBackground.frame, self.bounds)) {
    _codeBackground.frame = self.bounds;
    [_codeBackground setNeedsDisplay];
  }
  [self sendSubviewToBack:_codeBackground];
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
    NSString *text = [storage.string substringWithRange:range];
    UIAccessibilityElement *element = [[UIAccessibilityElement alloc] initWithAccessibilityContainer:self];
    element.accessibilityLabel =
        info.accessibilityPrefix != nil ? [NSString stringWithFormat:@"%@, %@", info.accessibilityPrefix, text] : text;
    element.accessibilityFrameInContainerSpace =
        CGRectMake(0, top + bounds.top, self.bounds.size.width, bounds.bottom - bounds.top);
    element.accessibilityTraits = info.headingLevel > 0 ? UIAccessibilityTraitHeader : UIAccessibilityTraitStaticText;
    NSArray *actions = linkActions(storage, range, self.linkDelegate);
    if (actions.count > 0) {
      element.accessibilityCustomActions = actions;
    }
    [elements addObject:element];
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
  _cachedAccessibilityElements = nil;
  [self setNeedsDisplay];
}

- (CGRect)contentRectForRow:(NSUInteger)row column:(NSUInteger)column {
  CGFloat x = 0;
  for (NSUInteger i = 0; i < column; i++) {
    x += _segment.columnWidths[i].doubleValue;
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
  UIRectFill(CGRectMake(0, 0, hairline, y));
  for (NSNumber *width in _segment.columnWidths) {
    x += width.doubleValue;
    UIRectFill(CGRectMake(x, 0, hairline, y));
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
  NSMutableArray *elements = [NSMutableArray arrayWithCapacity:_segment.rows.count];
  CGFloat y = 0;
  for (NSUInteger row = 0; row < _segment.rows.count; row++) {
    NSMutableArray<NSString *> *cells = [NSMutableArray new];
    NSMutableArray<UIAccessibilityCustomAction *> *actions = [NSMutableArray new];
    for (DSTableCell *cell in _segment.rows[row]) {
      [cells addObject:cell.text.string];
      [actions addObjectsFromArray:linkActions(cell.text, NSMakeRange(0, cell.text.length), self.linkDelegate)];
    }
    const CGFloat height = _segment.rowHeights[row].doubleValue;
    UIAccessibilityElement *element = [[UIAccessibilityElement alloc] initWithAccessibilityContainer:self];
    element.accessibilityLabel = [cells componentsJoinedByString:@", "];
    element.accessibilityFrameInContainerSpace = CGRectMake(0, y, _segment.tableSize.width, height);
    element.accessibilityTraits = row == 0 && _segment.hasHeader ? UIAccessibilityTraitHeader
                                                                 : UIAccessibilityTraitStaticText;
    if (actions.count > 0) {
      element.accessibilityCustomActions = actions;
    }
    [elements addObject:element];
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
  _segment = segment;
  _grid.segment = segment;
  _scrollView.contentOffset = CGPointZero;
  [self setNeedsLayout];
  [self setNeedsDisplay];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const CGFloat leading = _segment.contentLeading;
  _scrollView.frame = CGRectMake(leading, 0, std::max<CGFloat>(0, self.bounds.size.width - leading),
                                 self.bounds.size.height);
  _grid.frame = CGRectMake(0, 0, _segment.tableSize.width, _segment.tableSize.height);
  _scrollView.contentSize = _segment.tableSize;
}

// Quote bars and the list marker live in the non-scrolling margin.
- (void)drawRect:(CGRect)rect {
  if (_segment == nil) {
    return;
  }
  for (DSContainerInfo *container in _segment.containers) {
    if (container.barColor != nil) {
      [container.barColor setFill];
      UIRectFill(CGRectMake(container.leading + 2, 0, kBarWidth, self.bounds.size.height));
    }
    if (container.marker != nil) {
      UIFont *font = [container.marker attribute:NSFontAttributeName atIndex:0 effectiveRange:NULL];
      const CGSize markerSize = container.marker.size;
      const CGFloat baseline = _segment.cellPadding.top + font.ascender;
      [container.marker drawAtPoint:CGPointMake(container.leading + container.width - kMarkerGap - markerSize.width,
                                                baseline - font.ascender)];
    }
  }
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
