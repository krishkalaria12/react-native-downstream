#import "DSSegmentViews.h"

#import "DSLayoutManager.h"
#import "DSRenderedDocument.h"

#include "Controls.h"

#include <algorithm>
#include <vector>

using namespace downstream::ControlBits;
// The slot names a themed drawing reads. The layout helpers they sit beside live
// in `theme_layout`, which needs no `using` here: the metrics arrive on the
// segment rather than as a rule to apply.
using namespace downstream::theme;

#pragma mark - Tables

@interface DSTableGridView : UIView
@property (nonatomic, nullable) DSTableSegment *segment;
@property (nonatomic, weak, nullable) id<DSLinkDelegate> linkDelegate;
/// Where the rows' fading alphas come from. A row is one reveal unit, so a new
/// row brings its fill, its cells, and its grid lines with it.
@property (nonatomic, weak, nullable) id<DSRevealProvider> revealProvider;
/// The document block this table's first row is.
@property (nonatomic) NSInteger blockOffset;
- (NSInteger)linkIndexAtPoint:(CGPoint)point;
@end

@implementation DSTableGridView {
  NSArray *_cachedAccessibilityElements;
  std::vector<CGFloat> _rowOffsets;
  std::vector<CGFloat> _columnOffsets;
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
  if (segment == _segment) {
    return;
  }
  _segment = segment;
  _rowOffsets.assign(1, 0);
  _rowOffsets.reserve(segment.rowHeights.count + 1);
  for (NSNumber *height in segment.rowHeights) {
    _rowOffsets.push_back(_rowOffsets.back() + height.doubleValue);
  }
  _columnOffsets.assign(1, 0);
  _columnOffsets.reserve(segment.columnWidths.count + 1);
  for (NSNumber *width in segment.columnWidths) {
    _columnOffsets.push_back(_columnOffsets.back() + width.doubleValue);
  }
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
  const CGFloat x = _segment.rightToLeft
      ? _columnOffsets.back() - _columnOffsets[column + 1]
      : _columnOffsets[column];
  const CGFloat y = _rowOffsets[row];
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
  const NSStringDrawingOptions options = NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingUsesFontLeading;
  // A theme's border width is in points, and a hairline is the default: one
  // physical pixel, so a grid stays one line on every screen density.
  const CGFloat hairline = _segment.borderWidth > 0 ? _segment.borderWidth
                                                     : 1.0 / self.traitCollection.displayScale;

  if (_segment.hasHeader && _segment.rowHeights.count > 0) {
    [[_segment.theme colorForSlot:kSlotTableHeaderBackgroundColor fallback:UIColor.tertiarySystemFillColor] setFill];
    UIRectFill(CGRectMake(0, 0, size.width - hairline, _segment.rowHeights[0].doubleValue));
  }

  // The zebra fill is drawn under the cells, per row rather than per cell, so
  // an empty trailing cell in a short row still gets the row's fill.
  //
  // A row is one reveal unit, so the whole row draws at one alpha: its fill, its
  // cells, and the grid lines under it. A table row is a block, and its cells are
  // laid out by this grid rather than by a text run, so there is nothing finer to
  // fade within one.
  NSUInteger firstDataRow = _segment.hasHeader ? 1 : 0;
  [_segment.rows enumerateObjectsUsingBlock:^(NSArray<DSTableCell *> *row, NSUInteger rowIndex, BOOL *stop) {
    if (self->_rowOffsets[rowIndex] > CGRectGetMaxY(rect)) {
      *stop = YES;
      return;
    }
    if (self->_rowOffsets[rowIndex + 1] < CGRectGetMinY(rect)) {
      return;
    }
    const CGFloat alpha = self.revealProvider == nil
        ? 1
        : [self.revealProvider revealAlphaAtBlock:self.blockOffset
                                            offset:(NSUInteger)rowIndex
                                               now:CACurrentMediaTime()];
    if (alpha <= 0) {
      return;
    }
    CGContextRef context = UIGraphicsGetCurrentContext();
    CGContextSaveGState(context);
    CGContextSetAlpha(context, alpha);
    if (rowIndex >= firstDataRow) {
      UIColor *stripe = [self->_segment.theme colorForSlot:(rowIndex - firstDataRow) % 2 == 0
                                                          ? kSlotTableRowEvenBackgroundColor
                                                          : kSlotTableRowOddBackgroundColor
                                                   fallback:nil];
      if (stripe != nil) {
        [stripe setFill];
        UIRectFill(CGRectMake(0, self->_rowOffsets[rowIndex], size.width,
                              self->_rowOffsets[rowIndex + 1] - self->_rowOffsets[rowIndex]));
      }
    }
    [row enumerateObjectsUsingBlock:^(DSTableCell *cell, NSUInteger column, BOOL *innerStop) {
      const CGRect content = [self contentRectForRow:rowIndex column:column];
      if (CGRectIntersectsRect(content, rect)) {
        [cell.text drawWithRect:content options:options context:nil];
      }
    }];
    CGContextRestoreGState(context);
  }];

  [[_segment.theme colorForSlot:kSlotTableBorderColor fallback:UIColor.separatorColor] setFill];
  CGFloat y = 0;
  UIRectFill(CGRectMake(0, 0, size.width - hairline, hairline));
  for (NSNumber *height in _segment.rowHeights) {
    y += height.doubleValue;
    UIRectFill(CGRectMake(0, y - hairline, size.width - hairline, hairline));
  }
  CGFloat x = 0;
  UIRectFill(CGRectMake(_segment.rightToLeft ? size.width - hairline : 0, 0, hairline, y));
  for (NSNumber *width in _segment.columnWidths) {
    x += width.doubleValue;
    UIRectFill(CGRectMake(_segment.rightToLeft ? size.width - hairline - x : x, 0, hairline, y));
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
  UIView *_toolbar;
  UIStackView *_buttons;
  UIScrollView *_scrollView;
  DSTableGridView *_grid;
  NSArray<NSString *> *_labels;
  /// The labels the current buttons and menu items were built from, so a changed
  /// `translations` prop rebuilds them instead of leaving stale strings up.
  NSArray<NSString *> *_builtLabels;
  BOOL _needsScrollRestore;
  BOOL _preserveScrollPosition;
  BOOL _forceScrollReset;
  CGFloat _savedScrollPosition;
  /// The document block this table's first row is, which a reveal unit's own block
  /// index is relative to.
  NSInteger _blockOffset;
  /// Where the rows' fading alphas come from. Weak, because the provider is the
  /// Markdown view's and this card is recycled.
  __weak id<DSRevealProvider> _revealProvider;
}

- (instancetype)initWithFrame:(CGRect)frame {
  if (self = [super initWithFrame:frame]) {
    self.opaque = NO;
    self.backgroundColor = UIColor.clearColor;
    self.contentMode = UIViewContentModeRedraw;
    _toolbar = [UIView new];
    _buttons = [UIStackView new];
    _buttons.axis = UILayoutConstraintAxisHorizontal;
    _buttons.spacing = 2;
    [_toolbar addSubview:_buttons];
    _scrollView = [UIScrollView new];
    _scrollView.showsVerticalScrollIndicator = NO;
    _scrollView.alwaysBounceVertical = NO;
    _scrollView.directionalLockEnabled = YES;
    _grid = [DSTableGridView new];
    [_scrollView addSubview:_grid];
    [self addSubview:_toolbar];
    [self addSubview:_scrollView];
  }
  return self;
}

- (NSArray<NSString *> *)labels {
  static NSArray<NSString *> *fallback;
  static dispatch_once_t once;
  dispatch_once(&once, ^{
    fallback = @[
      @"Copy Code", @"Download file", @"Copied", @"Wrap lines", @"Don't wrap lines", @"Show all lines",
      @"Show fewer lines", @"Copy table", @"Copy table as Markdown", @"Copy table as CSV",
      @"Copy table as TSV", @"Download table", @"Download table as CSV", @"Download table as Markdown",
      @"Markdown", @"CSV", @"TSV",
    ];
  });
  return _labels.count >= DSControlLabelCount ? _labels : fallback;
}

- (NSString *)label:(DSControlLabel)index {
  return self.labels[index];
}

- (void)setControlLabels:(NSArray<NSString *> *)labels {
  _labels = [labels copy];
  [self updateButtons];
}

- (void)setShowsToolbar:(BOOL)showsToolbar {
  if (showsToolbar == _showsToolbar) {
    return;
  }
  _showsToolbar = showsToolbar;
  _toolbar.hidden = !self.showsToolbar;
  [self setNeedsLayout];
}

- (void)setControls:(uint32_t)controls {
  if (controls == _controls) {
    return;
  }
  _controls = controls;
  [self updateButtons];
}

- (void)setControlsEnabled:(BOOL)enabled {
  if (enabled == _controlsEnabled) {
    return;
  }
  _controlsEnabled = enabled;
  [self updateButtons];
}

/// The formats a table can be copied or saved in. Copy offers every format the
/// engine serializes; save offers the two the file dialog names well, and both
/// platforms offer exactly these, so a reader sees one menu on either.
- (NSArray<NSArray<NSString *> *> *)menuEntriesForAction:(NSString *)action {
  if ([action isEqualToString:@"copy"]) {
    return @[
      @[ [self label:DSControlLabelTableFormatMarkdown], @"markdown" ],
      @[ [self label:DSControlLabelTableFormatCsv], @"csv" ],
      @[ [self label:DSControlLabelTableFormatTsv], @"tsv" ],
    ];
  }
  return @[
    @[ [self label:DSControlLabelTableFormatCsv], @"csv" ],
    @[ [self label:DSControlLabelTableFormatMarkdown], @"markdown" ],
  ];
}

/// A menu button: a glyph that opens a native menu of the formats it can
/// produce. Built once, because a table's controls do not change shape.
- (UIButton *)menuButtonWithSymbol:(NSString *)symbol label:(NSString *)label action:(NSString *)action {
  UIButton *button = makeControlButton(symbol, label, action, @"");
  NSMutableArray<UIMenuElement *> *children = [NSMutableArray array];
  for (NSArray<NSString *> *entry in [self menuEntriesForAction:action]) {
    NSString *format = entry[1];
    [children addObject:[UIAction actionWithTitle:entry[0]
                                           image:nil
                                      identifier:nil
                                         handler:^(__kindof UIAction *_Nonnull inner) {
                                           [self pressFormat:format action:action];
                                         }]];
  }
  button.menu = [UIMenu menuWithTitle:@"" children:children];
  button.showsMenuAsPrimaryAction = YES;
  return button;
}

- (void)buildButtonsIfNeeded {
  if (_buttons.arrangedSubviews.count > 0 && [_builtLabels isEqualToArray:_labels]) {
    return;
  }
  for (UIView *view in [_buttons.arrangedSubviews copy]) {
    [_buttons removeArrangedSubview:view];
    [view removeFromSuperview];
  }
  _builtLabels = [_labels copy];
  UIButton *copy = [self menuButtonWithSymbol:kSymbolCopy
                                        label:[self label:DSControlLabelCopyTable]
                                       action:@"copy"];
  UIButton *download = [self menuButtonWithSymbol:kSymbolDownload
                                            label:[self label:DSControlLabelDownloadTable]
                                           action:@"download"];
  copy.tag = 0;
  download.tag = 1;
  [_buttons addArrangedSubview:copy];
  [_buttons addArrangedSubview:download];
  for (UIButton *button in @[copy, download]) {
    button.accessibilityIdentifier = [NSString stringWithFormat:@"ds.table.%@", button.accessibilityLabel];
  }
}

- (void)updateButtons {
  if (!self.showsToolbar) {
    _buttons.hidden = YES;
    return;
  }
  _buttons.hidden = NO;
  [self buildButtonsIfNeeded];
  NSArray<UIButton *> *all = _buttons.arrangedSubviews;
  ((UIButton *)all[0]).hidden = (_controls & TableCopy) == 0;
  ((UIButton *)all[1]).hidden = (_controls & TableDownload) == 0;
  for (UIButton *button in all) {
    button.enabled = _controlsEnabled && !button.hidden;
  }
}

- (void)pressFormat:(NSString *)format action:(NSString *)action {
  if (_segment == nil) {
    return;
  }
  if ([action isEqualToString:@"copy"]) {
    [self.controlDelegate didPressControlOfKind:DSBlockKindTableRow
                                   blockIndex:_segment.blockIndex
                                   blockCount:_segment.blockCount
                                     language:@""
                                      action:@"copy"
                                      format:format
                                    filename:@""];
    return;
  }
  [self.controlDelegate didPressControlOfKind:DSBlockKindTableRow
                                   blockIndex:_segment.blockIndex
                                   blockCount:_segment.blockCount
                                     language:@""
                                      action:@"download"
                                      format:format
                                    filename:@""];
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
  _grid.revealProvider = _revealProvider;
  _grid.blockOffset = _blockOffset;
  // The toolbar's buttons take the theme's own toolbar color when it set one.
  UIColor *toolbar = segment.toolbarButtonColor ?: UIColor.secondaryLabelColor;
  for (UIView *button in _buttons.arrangedSubviews) {
    [(UIControl *)button setTintColor:toolbar];
  }
  [self updateButtons];
  [self setNeedsLayout];
  [self setNeedsDisplay];
}

- (void)resetScrollPosition {
  _forceScrollReset = YES;
  _preserveScrollPosition = NO;
  _needsScrollRestore = YES;
  [self setNeedsLayout];
}

/// The document block this table's first row is.
- (void)setBlockOffset:(NSInteger)blockOffset {
  if (_blockOffset == blockOffset) {
    return;
  }
  _blockOffset = blockOffset;
  _grid.blockOffset = blockOffset;
  [_grid setNeedsDisplay];
}

/// A table row is one reveal unit and the grid draws every row from its own
/// offsets, so a tick has nothing to invalidate here: the grid redraws from the
/// alphas the provider gives it.
- (void)invalidateRevealCharacterRange:(NSRange)range
                            blockOffset:(NSInteger)blockOffset
                                     now:(double)now {
  (void)range;
  (void)blockOffset;
  (void)now;
  [_grid setNeedsDisplay];
}

- (void)setRevealProvider:(id<DSRevealProvider>)provider {
  if (_revealProvider == provider) {
    return;
  }
  _revealProvider = provider;
  _grid.revealProvider = provider;
  [_grid setNeedsDisplay];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  if (_segment == nil) {
    return;
  }
  const CGFloat toolbarHeight = self.showsToolbar ? kChromeRowHeight : 0;
  // A toolbar above the grid takes layout space rather than floating over the
  // rows, so a reader never taps a button instead of a cell.
  _toolbar.frame = CGRectMake(0, 0, self.bounds.size.width, toolbarHeight);
  // The buttons keep their own width at the trailing edge, as the code header's
  // do; a row-wide stack would stretch them apart.
  const CGFloat buttonsWidth =
      [_buttons systemLayoutSizeFittingSize:UILayoutFittingCompressedSize].width;
  const BOOL rightToLeft =
      self.effectiveUserInterfaceLayoutDirection == UIUserInterfaceLayoutDirectionRightToLeft;
  _buttons.frame = CGRectMake(rightToLeft ? 4 : self.bounds.size.width - buttonsWidth - 4, 0, buttonsWidth,
                              toolbarHeight);
  const CGFloat leading = _segment.contentLeading;
  _scrollView.frame = CGRectMake(_segment.rightToLeft ? 0 : leading, toolbarHeight,
                                 std::max<CGFloat>(0, self.bounds.size.width - leading),
                                 self.bounds.size.height - toolbarHeight);
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
  const CGFloat toolbarHeight = self.showsToolbar ? kChromeRowHeight : 0;
  for (DSContainerInfo *container in _segment.containers) {
    if (container.barColor != nil && container.barWidth > 0) {
      [container.barColor setFill];
      // The bar sits two points inside the container, and its width is the
      // theme's, so a theme that sets a wider bar draws a wider bar here and in
      // a plain text block alike.
      const CGFloat x = _segment.rightToLeft
          ? self.bounds.size.width - container.leading - 2 - container.barWidth
          : container.leading + 2;
      UIRectFill(CGRectMake(x, toolbarHeight, container.barWidth, self.bounds.size.height - toolbarHeight));
    }
    if (container.marker != nil) {
      UIFont *font = [container.marker attribute:NSFontAttributeName atIndex:0 effectiveRange:NULL];
      const CGSize markerSize = container.marker.size;
      const CGFloat baseline = toolbarHeight + _segment.cellPadding.top + font.ascender;
      const CGFloat x = _segment.rightToLeft
          ? self.bounds.size.width - container.leading - container.width + container.markerGap
          : container.leading + container.width - container.markerGap - markerSize.width;
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

- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent *)event {
  return CGRectContainsPoint(CGRectInset(self.bounds, -6, 0), point);
}

- (NSArray *)accessibilityElements {
  if (_segment == nil) {
    return @[];
  }
  // The buttons come before the rows, as in Streamdown's DOM.
  NSMutableArray *elements = [NSMutableArray new];
  if (self.showsToolbar) {
    for (UIView *view in _buttons.arrangedSubviews) {
      if (!view.hidden && [view isAccessibilityElement]) {
        [elements addObject:view];
      }
    }
  }
  // The grid is listed as a container, not flattened: its rows' frames are in
  // the grid's space, which the toolbar and the horizontal scroll move.
  [elements addObject:_grid];
  return elements;
}

@end
